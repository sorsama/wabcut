#include <wabcut/ai/multicam/Evidence.h>

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/model/Nesting.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace wabcut::ai::multicam {

std::optional<SequenceId> multicamSourceOf(const ProjectState& state, const Clip& clip) {
    if (!clip.isNested()) {
        return std::nullopt;
    }
    const Sequence* source = state.findSequence(clip.sequence);
    return source && source->multicam ? std::optional(clip.sequence) : std::nullopt;
}

std::vector<TimeRange> sourceWords(const SpeakerEvidence& evidence) {
    std::vector<TimeRange> out;
    out.reserve(evidence.words.size());
    for (const SpokenWord& w : evidence.words) {
        out.push_back(w.range);
    }
    return out;
}

std::expected<SpeakerEvidence, Error> gatherEvidence(const ProjectState& state, SequenceId multicam, const edit::TranscriptProvider* transcripts,
                                                     AngleSignals* angleSignals, const EvidenceOptions& options, const EvidenceProgress& progress,
                                                     std::stop_token stop) {
    const Sequence* seq = state.findSequence(multicam);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "no sequence " + std::to_string(multicam.value)});
    }
    if (!seq->multicam) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + seq->name + "' is not a multicam source"});
    }
    const MulticamInfo& info = *seq->multicam;
    SpeakerEvidence ev;
    ev.duration = std::max(seq->duration, seq->contentEnd());
    for (std::size_t i = 0; i < info.angles.size(); ++i) {
        const MulticamAngle& a = info.angles[i];
        ev.angles.push_back(AngleDescription{i, a.name.empty() ? "Angle " + std::to_string(i + 1) : a.name, a.video.isValid(), !a.audio.empty()});
    }
    for (const wabcut::Speaker& s : state.speakers) {
        ev.speakerNames.emplace_back(s.id, s.name);
    }
    const auto cancelled = [&] { return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"}); };
    const auto report = [&](double fraction, const std::string& what) { return !progress || progress(fraction, what); };

    // ---- Words, through the angles' audio tracks ----
    if (transcripts) {
        const edit::SequenceTranscript st = edit::SequenceTranscript::build(state, multicam, *transcripts);
        for (std::size_t i = 0; i < st.size(); ++i) {
            const edit::TimelineWord& w = st[i];
            const auto angle = multicamAngleOfTrack(info, w.track);
            if (!angle) {
                continue;
            }
            SpokenWord sw;
            sw.range = w.timeline;
            sw.angle = *angle;
            sw.label = w.speaker;
            sw.speaker = w.speakerId;
            sw.text = std::string(st.text(i));
            sw.confidence = w.confidence;
            ev.words.push_back(std::move(sw));
        }
        if (ev.words.empty()) {
            ev.notes.push_back("no transcript of the angles' audio: transcribe the microphones for the best result");
        }
    } else {
        ev.notes.push_back("no transcripts here: who speaks comes from the levels and the faces");
    }

    if (!angleSignals) {
        ev.notes.push_back("no media analysis here: microphone levels and faces are not used");
        return ev;
    }
    const std::size_t steps = std::max<std::size_t>(1, info.angles.size() * 2);
    std::size_t step = 0;
    for (std::size_t i = 0; i < info.angles.size(); ++i) {
        const MulticamAngle& a = info.angles[i];
        const std::string& name = ev.angles[i].name;
        // ---- The mic: the angle's first audio track ----
        if (options.levels && !a.audio.empty()) {
            if (!report(static_cast<double>(step) / static_cast<double>(steps), "Levels of " + name)) {
                return cancelled();
            }
            if (const Track* track = seq->findTrack(a.audio.front())) {
                MicEnvelope env;
                env.angle = i;
                env.start = Ticks{};
                env.frame = options.frame;
                env.db.assign(static_cast<std::size_t>((ev.duration.value + options.frame.value - 1) / options.frame.value), -100.0f);
                bool any = false;
                for (const Clip& clip : track->clips) {
                    const MediaItem* media = state.findMedia(clip.media);
                    if (!media || clip.isNested()) {
                        continue;
                    }
                    auto levels = angleSignals->levels(*media, clip.stream.index, TimeRange::fromStartEnd(clip.sourceIn, clip.sourceOut()), options.frame, stop);
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    if (!levels) {
                        ev.notes.push_back(name + ": no level (" + levels.error().message + ")");
                        continue;
                    }
                    const std::size_t first = static_cast<std::size_t>(std::max<std::int64_t>(0, clip.start().value / options.frame.value));
                    for (std::size_t k = 0; k < levels->size() && first + k < env.db.size(); ++k) {
                        env.db[first + k] = (*levels)[k];
                    }
                    any = true;
                }
                if (any) {
                    ev.mics.push_back(std::move(env));
                }
            }
        }
        ++step;
        // ---- The faces ----
        if (options.faces && a.video.isValid()) {
            if (!report(static_cast<double>(step) / static_cast<double>(steps), "Faces in " + name)) {
                return cancelled();
            }
            if (const Track* track = seq->findTrack(a.video)) {
                int trackBase = 0;
                for (const Clip& clip : track->clips) {
                    const MediaItem* media = state.findMedia(clip.media);
                    if (!media || clip.isNested() || clip.isFreeze()) {
                        continue;
                    }
                    const double base = static_cast<double>(step) / static_cast<double>(steps);
                    auto subjects = angleSignals->faces(*media, clip.stream.index, [&](double f) {
                        return report(base + f / static_cast<double>(steps), "Faces in " + name) && !stop.stop_requested();
                    });
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    if (!subjects) {
                        ev.notes.push_back(name + ": no faces (" + subjects.error().message + ")");
                        continue;
                    }
                    int maxTrack = 0;
                    std::map<int, FaceTrack> tracks;
                    for (const vision::SubjectSample& sample : subjects->samples) {
                        if (sample.time < clip.sourceIn || sample.time >= clip.sourceOut()) {
                            continue;
                        }
                        const Ticks t = clip.start() + (sample.time - clip.sourceIn);
                        ev.faceCounts.push_back(FaceCount{i, t, static_cast<int>(sample.faces.size())});
                        for (const vision::SubjectBox& f : sample.faces) {
                            if (f.track < 0) {
                                continue;
                            }
                            maxTrack = std::max(maxTrack, f.track);
                            FaceTrack& ft = tracks[f.track];
                            ft.angle = i;
                            ft.track = trackBase + f.track;
                            ft.points.push_back(FacePoint{t, f.box.x, f.box.y, f.box.w, f.box.h, f.activity});
                        }
                    }
                    for (auto& [id, ft] : tracks) {
                        ev.faces.push_back(std::move(ft));
                    }
                    trackBase += maxTrack + 1;
                }
            }
        }
        ++step;
    }
    report(1.0, "Done");
    return ev;
}

} // namespace wabcut::ai::multicam
