#include <wabcut/ai/dub/Dub.h>

#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/ai/roughcut/RoughCut.h>
#include <wabcut/ai/roughcut/Script.h>
#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>

namespace wabcut::ai::dub {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

/// Something to say: a letter or digit (any non-ASCII byte counts: Thai, CJK, accented letters).
bool hasWords(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
    });
}

/// A long line split at a space (or anywhere in a word-less run) so no request is longer than `limit` bytes.
std::vector<std::string> capped(std::string text, std::size_t limit) {
    std::vector<std::string> out;
    while (text.size() > limit) {
        std::size_t cut = text.rfind(' ', limit);
        if (cut == std::string::npos || cut < limit / 2) {
            cut = limit;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
                --cut; // not inside a UTF-8 sequence
            }
        }
        out.push_back(text.substr(0, cut));
        text.erase(0, cut);
        while (!text.empty() && text.front() == ' ') {
            text.erase(0, 1);
        }
    }
    if (!text.empty()) {
        out.push_back(std::move(text));
    }
    return out;
}

} // namespace

std::string speakableText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (std::size_t i = 0; i < text.size();) {
        if (text.substr(i, kRedactedWord.size()) == kRedactedWord) {
            i += kRedactedWord.size();
            pendingSpace = true;
            continue;
        }
        const char c = text[i++];
        if (isSpace(c)) {
            pendingSpace = true;
            continue;
        }
        if (pendingSpace && !out.empty() && c != ',' && c != '.' && c != '!' && c != '?' && c != ';' && c != ':') {
            out.push_back(' ');
        }
        pendingSpace = false;
        out.push_back(c);
    }
    return hasWords(out) ? out : std::string();
}

std::vector<DubLine> linesFromCaptions(const CaptionTrack& track) {
    std::vector<DubLine> lines;
    for (std::size_t i = 0; i < track.cues.size(); ++i) {
        const CaptionCue& cue = track.cues[i];
        std::string text = speakableText(cue.text);
        if (text.empty() || cue.duration <= Ticks{}) {
            continue;
        }
        lines.push_back(DubLine{std::move(text), cue.start, cue.duration, cue.speaker, i});
    }
    std::stable_sort(lines.begin(), lines.end(), [](const DubLine& a, const DubLine& b) { return a.start < b.start; });
    return lines;
}

std::vector<DubLine> narrationLines(std::string_view text) {
    std::vector<DubLine> lines;
    roughcut::ScriptOptions options;
    options.split = roughcut::ScriptSplit::Auto;
    auto script = roughcut::parseScript(text, options);
    if (!script) {
        return lines;
    }
    for (const roughcut::ScriptBeat& beat : script->beats) {
        std::string spoken = speakableText(beat.text);
        if (spoken.empty()) {
            continue;
        }
        for (std::string& part : capped(std::move(spoken), 1500)) {
            lines.push_back(DubLine{std::move(part), {}, {}, {}, beat.index});
        }
    }
    return lines;
}

std::vector<TimeRange> roughCutBeats(const Sequence& sequence) {
    std::vector<TimeRange> out;
    auto record = roughcut::recordOfSequence(sequence);
    if (!record) {
        return out;
    }
    for (const roughcut::RecordedBeat& beat : record->beats) {
        if (beat.chosen >= beat.takes.size()) {
            continue;
        }
        const roughcut::MediaRange& take = beat.takes[beat.chosen].media;
        std::optional<TimeRange> found;
        for (const auto* tracks : {&sequence.audioTracks, &sequence.videoTracks}) {
            for (const TrackPtr& track : *tracks) {
                for (const Clip& clip : track->clips) {
                    if (clip.media != take.media || clip.isRemapped() || clip.reversed || !clip.speed.isUnity()) {
                        continue;
                    }
                    const Ticks from = std::max(clip.sourceIn, take.source.start);
                    const Ticks to = std::min(clip.sourceOut(), take.source.end());
                    if (to <= from) {
                        continue;
                    }
                    const TimeRange r = TimeRange::fromStartEnd(clip.start() + (from - clip.sourceIn), clip.start() + (to - clip.sourceIn));
                    found = found ? TimeRange::fromStartEnd(std::min(found->start, r.start), std::max(found->end(), r.end())) : r;
                }
            }
            if (found) {
                break;
            }
        }
        if (found) {
            out.push_back(*found);
        }
    }
    std::sort(out.begin(), out.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    return out;
}

NarrationLayout layoutNarration(std::span<const DubLine> lines, std::span<const Ticks> lengths, Ticks at, Ticks gap,
                                std::span<const TimeRange> clips) {
    NarrationLayout layout;
    const std::size_t n = std::min(lines.size(), lengths.size());
    if (!clips.empty() && clips.size() == n) {
        layout.onClips = true;
        for (const TimeRange& clip : clips) {
            layout.targets.push_back(Slot{clip.start, clip.duration});
        }
        return layout;
    }
    if (!clips.empty()) {
        layout.warning = "the script has " + std::to_string(n) + " lines and the rough cut " + std::to_string(clips.size()) +
                         " beats: the narration runs line after line instead";
    }
    Ticks t = std::max(at, Ticks{});
    for (std::size_t i = 0; i < n; ++i) {
        layout.targets.push_back(Slot{t, lengths[i]});
        t = t + lengths[i] + gap;
    }
    return layout;
}

std::expected<Assembly, Error> assembleDub(const ProjectState& state, SequenceId sequence, const RenderedDub& audio,
                                           std::span<const FittedLine> fitted, const AssemblyOptions& options) {
    const auto fail = [](std::string message) { return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)}); };
    if (fitted.size() != audio.segments.size() || fitted.empty()) {
        return fail("the generated audio has " + std::to_string(audio.segments.size()) + " lines, the fit " + std::to_string(fitted.size()));
    }
    if (!audio.item.generated || audio.item.info.audio.empty()) {
        return fail("generated speech must be an audio item with provenance");
    }
    if (state.findSequence(sequence) == nullptr) {
        return std::unexpected(Error{ErrorCode::NotFound, "no sequence " + std::to_string(sequence.value)});
    }
    Assembly out;
    auto media = wabcut::edit::addMedia(state, audio.item);
    if (!media) {
        return std::unexpected(media.error());
    }
    out.media = media->id;
    auto track = wabcut::edit::addTrack(media->state, sequence, TrackKind::Audio, options.trackName);
    if (!track) {
        return std::unexpected(track.error());
    }
    out.track = track->id;
    wabcut::edit::TrackStateChange role;
    role.role = TrackRole::Dialogue;
    auto roled = wabcut::edit::setTrackState(track->state, sequence, track->id, role);
    if (!roled) {
        return std::unexpected(roled.error());
    }
    ProjectState s = std::move(roled->state);

    std::vector<TimeRange> spoken;
    for (std::size_t i = 0; i < fitted.size(); ++i) {
        wabcut::edit::SourceClip source;
        source.media = out.media;
        source.sourceRange = audio.segments[i];
        source.patches.push_back(wabcut::edit::SourcePatch{StreamRef{StreamKind::Audio, 0}, out.track});
        auto placed = wabcut::edit::overwriteEdit(s, sequence, source, fitted[i].start);
        if (!placed) {
            return std::unexpected(placed.error());
        }
        s = std::move(placed->state);
        out.clips.insert(out.clips.end(), placed->placed.begin(), placed->placed.end());
        spoken.push_back(TimeRange{fitted[i].start, audio.segments[i].duration});
    }

    // The original dialogue goes down under the new lines: audio.autoDuck's envelope.
    if (options.duckDb < 0.0) {
        wabcut::audio::edit::DuckingOptions ducking;
        ducking.amountDb = options.duckDb;
        ducking.minSpeech = Ticks{};
        const std::vector<wabcut::audio::edit::DuckRegion> regions = wabcut::audio::edit::duckRegions(spoken, ducking);
        const Sequence& seq = *s.findSequence(sequence);
        std::vector<TrackId> targets;
        if (options.duckTracks) {
            targets = *options.duckTracks;
        } else {
            for (const TrackPtr& t : seq.audioTracks) {
                if (t->id != out.track && std::find(options.duckRoles.begin(), options.duckRoles.end(), t->role) != options.duckRoles.end()) {
                    targets.push_back(t->id);
                }
            }
        }
        for (const TrackId id : targets) {
            const Track* t = s.findSequence(sequence)->findTrack(id);
            if (t == nullptr || t->kind != TrackKind::Audio || t->id == out.track || t->locked) {
                continue;
            }
            std::vector<ClipId> ids;
            for (const Clip& clip : t->clips) {
                ids.push_back(clip.id);
            }
            for (const ClipId clipId : ids) {
                const auto location = s.findSequence(sequence)->findClip(clipId);
                if (!location) {
                    continue;
                }
                const Clip& clip = location->clip();
                const auto volume = wabcut::audio::edit::duckedVolume(clip, regions, ducking);
                if (!volume || *volume == clip.properties()[ParamId::VolumeDb]) {
                    continue;
                }
                auto ducked = wabcut::edit::setParameter(s, sequence, clipId, wabcut::edit::ParamRef::of(ParamId::VolumeDb), *volume);
                if (!ducked) {
                    return std::unexpected(ducked.error());
                }
                s = std::move(ducked->state);
                ++out.duckedClips;
            }
        }
    }
    out.state = std::move(s);
    return out;
}

} // namespace wabcut::ai::dub
