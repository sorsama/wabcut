#include <wabcut/ai/multicam/Speakers.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace wabcut::ai::multicam {

namespace {

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

std::string normalizedText(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u)) {
            out.push_back(static_cast<char>(u < 0x80 ? std::tolower(u) : u));
        }
    }
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// The p-th percentile (0..1) of `values` (a copy is sorted).
float percentile(std::vector<float> values, double p) {
    if (values.empty()) {
        return 0.0f;
    }
    const auto k = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

double pearson(const std::vector<float>& a, const std::vector<float>& b, const std::vector<bool>& use) {
    double n = 0, sa = 0, sb = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (use[i]) {
            n += 1;
            sa += a[i];
            sb += b[i];
        }
    }
    if (n < 10) {
        return 0.0;
    }
    const double ma = sa / n, mb = sb / n;
    double cov = 0, va = 0, vb = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (use[i]) {
            cov += (a[i] - ma) * (b[i] - mb);
            va += (a[i] - ma) * (a[i] - ma);
            vb += (b[i] - mb) * (b[i] - mb);
        }
    }
    return va > 1e-12 && vb > 1e-12 ? cov / std::sqrt(va * vb) : 0.0;
}

struct Frames {
    Ticks frame;
    std::size_t count = 0;
    std::size_t of(Ticks t) const {
        if (t.value <= 0) {
            return 0;
        }
        return std::min(count == 0 ? 0 : count - 1, static_cast<std::size_t>(t.value / frame.value));
    }
    Ticks at(std::size_t f) const { return Ticks{frame.value * static_cast<std::int64_t>(f)}; }
};

/// A mic level on the analysis frames, normalised between its noise floor and its speech level (0..1).
struct NormalisedMic {
    std::size_t angle = 0;
    std::vector<float> level;
};

std::optional<NormalisedMic> normalise(const MicEnvelope& env, const Frames& frames, std::vector<std::string>& notes,
                                       const std::string& angleName) {
    if (env.db.empty() || env.frame.value <= 0) {
        return std::nullopt;
    }
    std::vector<float> finite;
    finite.reserve(env.db.size());
    for (const float d : env.db) {
        finite.push_back(std::isfinite(d) ? std::max(d, -100.0f) : -100.0f);
    }
    const float floor = percentile(finite, 0.15);
    const float speech = percentile(finite, 0.95);
    if (speech - floor < 6.0f) {
        notes.push_back(angleName + "'s microphone hardly changes level: not used to tell who speaks");
        return std::nullopt;
    }
    NormalisedMic m;
    m.angle = env.angle;
    m.level.assign(frames.count, 0.0f);
    for (std::size_t i = 0; i < finite.size(); ++i) {
        const Ticks t = env.start + Ticks{env.frame.value * static_cast<std::int64_t>(i)};
        if (t.value < 0) {
            continue;
        }
        const std::size_t f = frames.of(t);
        if (f >= frames.count) {
            continue;
        }
        const float v = std::clamp((finite[i] - floor) / (speech - floor), 0.0f, 1.0f);
        m.level[f] = std::max(m.level[f], v);
    }
    return m;
}

/// Hysteresis, then bridging short gaps and dropping short runs.
std::vector<bool> speaking(const std::vector<float>& score, double on, double off, std::size_t bridge, std::size_t minRun) {
    std::vector<bool> s(score.size(), false);
    bool active = false;
    for (std::size_t i = 0; i < score.size(); ++i) {
        active = active ? score[i] >= off : score[i] >= on;
        s[i] = active;
    }
    // Bridge gaps.
    std::size_t i = 0;
    while (i < s.size()) {
        if (!s[i]) {
            std::size_t j = i;
            while (j < s.size() && !s[j]) {
                ++j;
            }
            if (i > 0 && j < s.size() && j - i <= bridge) {
                std::fill(s.begin() + static_cast<std::ptrdiff_t>(i), s.begin() + static_cast<std::ptrdiff_t>(j), true);
            }
            i = j;
        } else {
            ++i;
        }
    }
    // Drop short runs.
    i = 0;
    while (i < s.size()) {
        if (s[i]) {
            std::size_t j = i;
            while (j < s.size() && s[j]) {
                ++j;
            }
            if (j - i < minRun) {
                std::fill(s.begin() + static_cast<std::ptrdiff_t>(i), s.begin() + static_cast<std::ptrdiff_t>(j), false);
            }
            i = j;
        } else {
            ++i;
        }
    }
    return s;
}

} // namespace

std::string_view toString(SegmentKind kind) {
    switch (kind) {
    case SegmentKind::Silence:
        return "silence";
    case SegmentKind::Single:
        return "single";
    case SegmentKind::Overlap:
        return "overlap";
    case SegmentKind::NonSpeech:
        return "nonSpeech";
    }
    return "silence";
}

std::string_view toString(AngleRole role) {
    switch (role) {
    case AngleRole::Unknown:
        return "unknown";
    case AngleRole::CloseUp:
        return "closeUp";
    case AngleRole::Wide:
        return "wide";
    }
    return "unknown";
}

std::optional<std::size_t> SpeakerTimeline::closeUpOf(std::size_t speaker) const {
    std::optional<std::size_t> best;
    double confidence = -1.0;
    for (const AngleAssignment& a : angles) {
        if (a.role == AngleRole::CloseUp && std::ranges::find(a.speakers, speaker) != a.speakers.end()) {
            const double c = a.confidence + (a.confirmed ? 1.0 : 0.0);
            if (c > confidence) {
                confidence = c;
                best = a.angle;
            }
        }
    }
    return best;
}

std::optional<std::size_t> SpeakerTimeline::wide() const {
    std::optional<std::size_t> best;
    double confidence = -1.0;
    for (const AngleAssignment& a : angles) {
        if (a.role == AngleRole::Wide) {
            const double c = a.confidence + (a.confirmed ? 1.0 : 0.0);
            if (c > confidence) {
                confidence = c;
                best = a.angle;
            }
        }
    }
    return best;
}

const SpeakerSegment* SpeakerTimeline::segmentAt(Ticks t) const {
    const auto it = std::upper_bound(segments.begin(), segments.end(), t,
                                     [](Ticks v, const SpeakerSegment& s) { return v < s.range.start; });
    if (it == segments.begin()) {
        return nullptr;
    }
    const SpeakerSegment& s = *(it - 1);
    return s.range.contains(t) ? &s : nullptr;
}

SpeakerTimeline analyzeSpeakers(const SpeakerEvidence& evidence, const SpeakerOptions& options) {
    SpeakerTimeline out;
    out.notes = evidence.notes;
    out.duration = std::max(evidence.duration, Ticks{});
    out.frame = options.frame.value > 0 ? options.frame : Ticks{kTicksPerSecond / 10};
    Frames frames{out.frame, static_cast<std::size_t>((out.duration.value + out.frame.value - 1) / out.frame.value)};
    if (frames.count == 0) {
        out.notes.push_back("the multicam source is empty");
        return out;
    }
    const auto angleName = [&](std::size_t angle) {
        for (const AngleDescription& a : evidence.angles) {
            if (a.angle == angle && !a.name.empty()) {
                return a.name;
            }
        }
        return "Angle " + std::to_string(angle + 1);
    };

    // ---- Microphones ----
    std::vector<NormalisedMic> mics;
    for (const MicEnvelope& env : evidence.mics) {
        if (auto m = normalise(env, frames, out.notes, angleName(env.angle))) {
            mics.push_back(std::move(*m));
        }
    }
    const auto micLevel = [&](std::size_t angle, std::size_t f) -> float {
        for (const NormalisedMic& m : mics) {
            if (m.angle == angle) {
                return m.level[f];
            }
        }
        return 0.0f;
    };

    // ---- Words: one per utterance (the same word heard by two mics: the louder mic's) ----
    std::vector<SpokenWord> words = evidence.words;
    std::sort(words.begin(), words.end(), [](const SpokenWord& a, const SpokenWord& b) { return a.range.start < b.range.start; });
    std::vector<bool> drop(words.size(), false);
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (drop[i]) {
            continue;
        }
        const std::string a = normalizedText(words[i].text);
        for (std::size_t j = i + 1; j < words.size() && words[j].range.start < words[i].range.end(); ++j) {
            if (drop[j] || words[j].angle == words[i].angle || normalizedText(words[j].text) != a) {
                continue;
            }
            const Ticks overlap = std::min(words[i].range.end(), words[j].range.end()) - std::max(words[i].range.start, words[j].range.start);
            const Ticks shorter = std::min(words[i].range.duration, words[j].range.duration);
            if (overlap.value * 2 < shorter.value) {
                continue;
            }
            const std::size_t fi = frames.of(words[i].range.start + Ticks{words[i].range.duration.value / 2});
            const std::size_t fj = frames.of(words[j].range.start + Ticks{words[j].range.duration.value / 2});
            const float li = micLevel(words[i].angle, fi);
            const float lj = micLevel(words[j].angle, fj);
            const bool keepI = li != lj ? li > lj : words[i].confidence >= words[j].confidence;
            drop[keepI ? j : i] = true;
            if (!keepI) {
                break;
            }
        }
    }
    {
        std::vector<SpokenWord> kept;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (!drop[i]) {
                kept.push_back(std::move(words[i]));
            }
        }
        words = std::move(kept);
    }

    // ---- Speakers ----
    std::map<std::string, std::size_t> index;
    const auto addSpeaker = [&](const std::string& key, const std::string& name, SpeakerId project) {
        if (const auto it = index.find(key); it != index.end()) {
            return it->second;
        }
        Speaker s;
        s.key = key;
        s.name = name;
        s.project = project;
        out.speakers.push_back(s);
        index[key] = out.speakers.size() - 1;
        return out.speakers.size() - 1;
    };
    std::map<std::size_t, std::set<int>> labelsPerAngle;
    for (const SpokenWord& w : words) {
        if (!w.speaker.isValid() && w.label) {
            labelsPerAngle[w.angle].insert(*w.label);
        }
    }
    const auto projectName = [&](SpeakerId id) -> std::string {
        for (const auto& [sid, name] : evidence.speakerNames) {
            if (sid == id) {
                return name;
            }
        }
        return "Speaker " + std::to_string(id.value);
    };
    std::vector<std::size_t> wordSpeaker(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        const SpokenWord& w = words[i];
        if (w.speaker.isValid()) {
            wordSpeaker[i] = addSpeaker("speaker:" + std::to_string(w.speaker.value), projectName(w.speaker), w.speaker);
        } else if (w.label) {
            const bool several = labelsPerAngle[w.angle].size() > 1;
            wordSpeaker[i] = addSpeaker("a" + std::to_string(w.angle) + ":L" + std::to_string(*w.label),
                                        several ? angleName(w.angle) + " speaker " + std::to_string(*w.label + 1) : angleName(w.angle), {});
        } else {
            wordSpeaker[i] = addSpeaker("mic:" + std::to_string(w.angle), angleName(w.angle), {});
        }
    }
    const bool haveWords = !words.empty();
    // With dedicated microphones (angles without a picture), the cameras' own sound is the room's: it hears everyone.
    const auto isCamera = [&](std::size_t angle) {
        return std::ranges::any_of(evidence.angles, [&](const AngleDescription& a) { return a.angle == angle && a.video; });
    };
    const bool dedicatedMics = std::ranges::any_of(mics, [&](const NormalisedMic& m) { return !isCamera(m.angle); });
    // Whose microphone is each angle's: its words' dominant speaker (a personal mic: >= 70 % one speaker); without a
    // transcript at all, each dedicated mic (else each mic) is one speaker.
    for (const NormalisedMic& m : mics) {
        std::map<std::size_t, std::int64_t> heard;
        std::int64_t total = 0;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i].angle == m.angle) {
                heard[wordSpeaker[i]] += words[i].range.duration.value;
                total += words[i].range.duration.value;
            }
        }
        if (total > 0) {
            const auto best = std::max_element(heard.begin(), heard.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            if (best->second * 10 >= total * 7) {
                Speaker& s = out.speakers[best->first];
                if (!s.mic) {
                    s.mic = m.angle;
                }
            } else {
                out.notes.push_back(angleName(m.angle) + " hears several speakers (a room or camera microphone): its level is not "
                                                         "used for any one of them");
            }
        } else if (!haveWords && dedicatedMics && isCamera(m.angle)) {
            out.notes.push_back(angleName(m.angle) + "'s own sound is the room's (there are dedicated microphones): not a speaker");
        } else if (!haveWords) {
            const std::size_t s = addSpeaker("mic:" + std::to_string(m.angle), angleName(m.angle), {});
            out.speakers[s].mic = m.angle;
        } else {
            out.notes.push_back(angleName(m.angle) + " has no transcript: its level is not used");
        }
    }

    // Pictures only: every camera that mostly sees one face is one speaker.
    if (!haveWords && mics.empty()) {
        std::map<std::size_t, std::pair<double, double>> oneOfAll;
        for (const FaceCount& c : evidence.faceCounts) {
            auto& [one, all] = oneOfAll[c.angle];
            one += c.faces == 1 ? 1 : 0;
            all += 1;
        }
        for (const auto& [angle, counts] : oneOfAll) {
            if (counts.second > 0 && counts.first / counts.second >= 0.4) {
                addSpeaker("cam:" + std::to_string(angle), angleName(angle), {});
            }
        }
        if (!out.speakers.empty()) {
            out.notes.push_back("no transcript and no usable microphone: who speaks comes from the faces alone");
        }
    }

    // ---- Cues per speaker per frame ----
    const std::size_t S0 = out.speakers.size();
    std::vector<std::vector<float>> transcriptCue(S0, std::vector<float>(frames.count, 0.0f));
    const Ticks pad{kTicksPerSecond * 6 / 100};
    for (std::size_t i = 0; i < words.size(); ++i) {
        const Ticks a = std::max(Ticks{}, words[i].range.start - pad);
        const Ticks b = std::min(out.duration, words[i].range.end() + pad);
        for (std::size_t f = frames.of(a); f < frames.count && frames.at(f) < b; ++f) {
            const Ticks fs = frames.at(f), fe = fs + frames.frame;
            const double cover = seconds(std::min(fe, b) - std::max(fs, a)) / seconds(frames.frame);
            transcriptCue[wordSpeaker[i]][f] = std::min(1.0f, transcriptCue[wordSpeaker[i]][f] + static_cast<float>(std::max(0.0, cover)));
        }
    }
    std::vector<std::vector<float>> micCue(S0, std::vector<float>(frames.count, 0.0f));
    std::vector<bool> hasMic(S0, false);
    for (std::size_t s = 0; s < S0; ++s) {
        if (!out.speakers[s].mic) {
            continue;
        }
        hasMic[s] = true;
        const std::size_t own = *out.speakers[s].mic;
        for (std::size_t f = 0; f < frames.count; ++f) {
            float v = micLevel(own, f);
            float loudest = 0.0f;
            for (const NormalisedMic& m : mics) {
                if (m.angle != own) {
                    loudest = std::max(loudest, m.level[f]);
                }
            }
            if (v < loudest - static_cast<float>(options.bleed)) {
                v *= 0.3f; // mostly someone else's voice bleeding in
            }
            micCue[s][f] = v;
        }
    }
    std::vector<bool> hasTranscript(S0, false);
    for (std::size_t i = 0; i < words.size(); ++i) {
        hasTranscript[wordSpeaker[i]] = true;
    }

    // ---- Faces: per angle, the main track's activity per frame ----
    struct AngleFaces {
        std::vector<float> activity; ///< normalised, the main face
        std::vector<bool> visible;
        int mainTrack = -1;
        std::optional<FacePoint> face;
        double one = 0, many = 0, none = 0, samples = 0;
        double area = 0;
    };
    std::map<std::size_t, AngleFaces> faces;
    for (const FaceCount& c : evidence.faceCounts) {
        AngleFaces& af = faces[c.angle];
        af.samples += 1;
        (c.faces == 0 ? af.none : c.faces == 1 ? af.one : af.many) += 1;
    }
    for (auto& [angle, af] : faces) {
        // The main face: the track seen longest.
        std::size_t longest = 0;
        const FaceTrack* main = nullptr;
        for (const FaceTrack& t : evidence.faces) {
            if (t.angle == angle && t.points.size() > longest) {
                longest = t.points.size();
                main = &t;
            }
        }
        af.activity.assign(frames.count, 0.0f);
        af.visible.assign(frames.count, false);
        if (!main) {
            continue;
        }
        af.mainTrack = main->track;
        std::vector<float> acts;
        for (const FacePoint& p : main->points) {
            acts.push_back(p.activity);
            af.area += static_cast<double>(p.w) * p.h;
        }
        af.area /= static_cast<double>(main->points.size());
        const float scale = std::max(1e-4f, percentile(acts, 0.9));
        for (std::size_t k = 0; k < main->points.size(); ++k) {
            const FacePoint& p = main->points[k];
            const Ticks next = k + 1 < main->points.size() ? main->points[k + 1].time : p.time + Ticks{kTicksPerSecond / 5};
            const Ticks until = std::min(next, p.time + Ticks{kTicksPerSecond * 4 / 10});
            for (std::size_t f = frames.of(std::max(p.time, Ticks{})); f < frames.count && frames.at(f) < until; ++f) {
                af.activity[f] = std::max(af.activity[f], std::min(1.0f, p.activity / scale));
                af.visible[f] = true;
            }
        }
        af.face = main->points[main->points.size() / 2];
    }

    // ---- Pass 1 scores (no faces) ----
    const auto fuse = [&](std::size_t s, std::size_t f, float face, bool hasFace) {
        double num = 0, den = 0;
        if (hasTranscript[s]) {
            num += options.transcriptWeight * transcriptCue[s][f];
            den += options.transcriptWeight;
        }
        if (hasMic[s]) {
            num += options.micWeight * micCue[s][f];
            den += options.micWeight;
        }
        if (hasFace) {
            num += options.faceWeight * face;
            den += options.faceWeight;
        }
        return den > 0 ? static_cast<float>(num / den) : 0.0f;
    };
    std::vector<std::vector<float>> score1(S0, std::vector<float>(frames.count, 0.0f));
    for (std::size_t s = 0; s < S0; ++s) {
        for (std::size_t f = 0; f < frames.count; ++f) {
            score1[s][f] = fuse(s, f, 0.0f, false);
        }
    }

    // ---- Angles: close-up, wide or unknown; whom a close-up shows ----
    std::vector<std::size_t> claimed(S0, 0);
    for (const AngleDescription& a : evidence.angles) {
        if (!a.video) {
            continue;
        }
        AngleAssignment as;
        as.angle = a.angle;
        const auto it = faces.find(a.angle);
        if (it != faces.end() && it->second.samples > 0) {
            const AngleFaces& af = it->second;
            const double one = af.one / af.samples, many = af.many / af.samples;
            as.face = af.face;
            if (many >= 0.35) {
                as.role = AngleRole::Wide;
                as.confidence = std::min(1.0, many);
                as.reason = "sees two or more faces " + std::to_string(static_cast<int>(std::lround(many * 100))) + " % of the time";
            } else if (one >= 0.4) {
                as.role = AngleRole::CloseUp;
                as.confidence = std::min(1.0, one) * 0.6;
                // Whose: the speaker whose speech the face's motion follows best.
                double best = 0.1;
                std::optional<std::size_t> who;
                for (std::size_t s = 0; s < S0; ++s) {
                    const double r = pearson(af.activity, score1[s], af.visible) - 0.05 * static_cast<double>(claimed[s]);
                    if (r > best) {
                        best = r;
                        who = s;
                    }
                }
                if (who) {
                    as.confidence = std::min(1.0, as.confidence + best);
                    as.reason = "one face, whose lips move when " + out.speakers[*who].name + " speaks";
                } else {
                    for (std::size_t s = 0; s < S0 && !who; ++s) {
                        if (out.speakers[s].mic == a.angle) {
                            who = s;
                            as.reason = "one face; its own microphone hears " + out.speakers[s].name;
                        } else if (out.speakers[s].key == "cam:" + std::to_string(a.angle)) {
                            who = s;
                            as.reason = "one face, whose lips are the only cue";
                        }
                    }
                }
                if (who) {
                    as.speakers = {*who};
                    ++claimed[*who];
                } else {
                    as.reason = "one face, not matched to a voice";
                }
            } else {
                as.reason = "no steady face";
            }
        }
        out.angles.push_back(std::move(as));
    }
    // Without faces: a camera with its own (personal) microphone is that speaker's close-up; one camera left over with
    // several speakers is the wide shot.
    const bool anyFaces = std::ranges::any_of(out.angles, [](const AngleAssignment& a) { return a.role != AngleRole::Unknown; });
    if (!anyFaces) {
        std::vector<AngleAssignment*> rest;
        for (AngleAssignment& a : out.angles) {
            for (std::size_t s = 0; s < S0; ++s) {
                if (out.speakers[s].mic == a.angle && claimed[s] == 0) {
                    a.role = AngleRole::CloseUp;
                    a.speakers = {s};
                    a.confidence = 0.4;
                    a.reason = "its own microphone hears " + out.speakers[s].name;
                    ++claimed[s];
                    break;
                }
            }
            if (a.role == AngleRole::Unknown) {
                rest.push_back(&a);
            }
        }
        if (rest.size() == 1 && S0 > 1) {
            rest.front()->role = AngleRole::Wide;
            rest.front()->confidence = 0.35;
            rest.front()->reason = "the camera that is nobody's close-up";
        }
    }
    // The user's assignments win.
    for (const AngleOverride& o : options.overrides) {
        auto a = std::ranges::find_if(out.angles, [&](const AngleAssignment& x) { return x.angle == o.angle; });
        if (a == out.angles.end()) {
            continue;
        }
        a->role = o.role;
        a->speakers.clear();
        for (const std::string& who : o.speakers) {
            for (std::size_t s = 0; s < S0; ++s) {
                if (out.speakers[s].key == who || lower(out.speakers[s].name) == lower(who)) {
                    a->speakers.push_back(s);
                }
            }
        }
        a->confirmed = true;
        a->confidence = 1.0;
        a->reason = "set by you";
    }

    // ---- Pass 2: the face cue of each speaker's close-up ----
    out.activity.assign(S0, std::vector<float>(frames.count, 0.0f));
    for (std::size_t s = 0; s < S0; ++s) {
        const AngleFaces* face = nullptr;
        for (const AngleAssignment& a : out.angles) {
            if (a.role == AngleRole::CloseUp && a.speakers.size() == 1 && a.speakers[0] == s) {
                if (const auto it = faces.find(a.angle); it != faces.end() && it->second.mainTrack >= 0) {
                    face = &it->second;
                }
            }
        }
        for (std::size_t f = 0; f < frames.count; ++f) {
            const bool hasFace = face && face->visible[f];
            out.activity[s][f] = fuse(s, f, hasFace ? face->activity[f] : 0.0f, hasFace);
        }
    }

    // ---- Speech per speaker ----
    const auto framesOf = [&](Ticks t) { return static_cast<std::size_t>(std::max<std::int64_t>(1, (t.value + frames.frame.value - 1) / frames.frame.value)); };
    std::vector<std::vector<bool>> talk(S0);
    for (std::size_t s = 0; s < S0; ++s) {
        talk[s] = speaking(out.activity[s], options.on, options.off, framesOf(options.bridge), framesOf(options.minSpeech));
    }

    // ---- Backchannels: a listener's quick "wow" or "right?" (up to 3 words, 1.2 s) while someone else holds the floor
    // is a reaction, not a turn: no cut to them, no crosstalk ----
    std::vector<Reaction> backchannels;
    {
        const Ticks gapMax{kTicksPerSecond / 2};
        const Ticks around{kTicksPerSecond * 4 / 10};
        for (std::size_t s = 0; s < S0; ++s) {
            std::vector<std::size_t> mine;
            for (std::size_t i = 0; i < words.size(); ++i) {
                if (wordSpeaker[i] == s) {
                    mine.push_back(i);
                }
            }
            for (std::size_t g = 0; g < mine.size();) {
                std::size_t h = g + 1;
                while (h < mine.size() && words[mine[h]].range.start - words[mine[h - 1]].range.end() <= gapMax) {
                    ++h;
                }
                const Ticks a = words[mine[g]].range.start;
                const Ticks b = words[mine[h - 1]].range.end();
                // Before the group and after it, the gaps to this speaker's other words are long too.
                const bool alone = (g == 0 || a - words[mine[g - 1]].range.end() > Ticks{gapMax.value * 2}) &&
                                   (h == mine.size() || words[mine[h]].range.start - b > Ticks{gapMax.value * 2});
                if (alone && h - g <= 3 && b - a <= Ticks{kTicksPerSecond * 12 / 10} && a >= around && b + around < out.duration) {
                    const std::size_t fa = frames.of(a - around);
                    const std::size_t fb = std::min(frames.count - 1, frames.of(b + around));
                    std::optional<std::size_t> floor;
                    for (std::size_t o = 0; o < S0 && !floor; ++o) {
                        if (o != s && talk[o][fa] && talk[o][fb]) {
                            floor = o;
                        }
                    }
                    if (floor) {
                        backchannels.push_back(Reaction{TimeRange::fromStartEnd(a, b), s, 0.7, "reacts"});
                        // Their speech run around the words is not a turn.
                        std::size_t lo = frames.of(a);
                        std::size_t hi = std::min(frames.count - 1, frames.of(b));
                        while (lo > 0 && talk[s][lo - 1] && lo > fa) {
                            --lo;
                        }
                        while (hi + 1 < frames.count && talk[s][hi + 1] && hi < fb) {
                            ++hi;
                        }
                        for (std::size_t f = lo; f <= hi; ++f) {
                            talk[s][f] = false;
                        }
                    }
                }
                g = h;
            }
        }
    }
    for (std::size_t s = 0; s < S0; ++s) {
        out.speakers[s].seconds = seconds(frames.frame) * static_cast<double>(std::count(talk[s].begin(), talk[s].end(), true));
    }

    // ---- Segments ----
    const auto loudMic = [&](std::size_t f) {
        float m = 0.0f;
        for (const NormalisedMic& x : mics) {
            m = std::max(m, x.level[f]);
        }
        return m;
    };
    struct FrameState {
        SegmentKind kind;
        std::vector<std::size_t> who;
        bool operator==(const FrameState&) const = default;
    };
    const auto stateAt = [&](std::size_t f) {
        FrameState st{SegmentKind::Silence, {}};
        for (std::size_t s = 0; s < S0; ++s) {
            if (talk[s][f]) {
                st.who.push_back(s);
            }
        }
        st.kind = st.who.empty() ? (loudMic(f) >= 0.6f ? SegmentKind::NonSpeech : SegmentKind::Silence)
                  : st.who.size() == 1 ? SegmentKind::Single
                                       : SegmentKind::Overlap;
        return st;
    };
    std::size_t f0 = 0;
    FrameState current = stateAt(0);
    double confidenceSum = 0;
    const auto frameConfidence = [&](const FrameState& st, std::size_t f) {
        std::vector<float> scores;
        for (std::size_t s = 0; s < S0; ++s) {
            scores.push_back(out.activity[s][f]);
        }
        std::sort(scores.rbegin(), scores.rend());
        const float top = scores.empty() ? 0.0f : scores[0];
        const float second = scores.size() > 1 ? scores[1] : 0.0f;
        switch (st.kind) {
        case SegmentKind::Single:
            return std::clamp(static_cast<double>(top - second) * 1.5, 0.0, 1.0);
        case SegmentKind::Overlap:
            return std::clamp(static_cast<double>(second) * 1.5, 0.0, 1.0);
        case SegmentKind::NonSpeech:
            return static_cast<double>(loudMic(f));
        case SegmentKind::Silence:
            return std::clamp(1.0 - static_cast<double>(top) * 1.5, 0.0, 1.0);
        }
        return 0.0;
    };
    for (std::size_t f = 0; f <= frames.count; ++f) {
        const bool end = f == frames.count;
        FrameState st = end ? FrameState{} : stateAt(f);
        if (end || !(st == current)) {
            SpeakerSegment seg;
            seg.range = TimeRange::fromStartEnd(frames.at(f0), std::min(out.duration, frames.at(f)));
            seg.kind = current.kind;
            seg.speakers = current.who;
            seg.confidence = confidenceSum / static_cast<double>(std::max<std::size_t>(1, f - f0));
            if (!seg.range.isEmpty()) {
                out.segments.push_back(std::move(seg));
            }
            f0 = f;
            current = st;
            confidenceSum = 0;
        }
        if (!end) {
            confidenceSum += frameConfidence(st, f);
        }
    }

    // ---- Reactions: a listener laughing or reacting while someone else speaks ----
    for (const SpeakerSegment& seg : out.segments) {
        if (seg.kind == SegmentKind::NonSpeech) {
            // The loudest personal mic is whose laughter it is.
            std::optional<std::size_t> who;
            float best = 0.0f;
            for (std::size_t s = 0; s < S0; ++s) {
                if (!out.speakers[s].mic) {
                    continue;
                }
                float peak = 0.0f;
                for (std::size_t f = frames.of(seg.range.start); f < frames.count && frames.at(f) < seg.range.end(); ++f) {
                    peak = std::max(peak, micLevel(*out.speakers[s].mic, f));
                }
                if (peak > best) {
                    best = peak;
                    who = s;
                }
            }
            if (who && seg.range.duration >= Ticks{kTicksPerSecond * 3 / 10}) {
                out.reactions.push_back(Reaction{seg.range, *who, best, "laughs"});
            }
            continue;
        }
        if (seg.kind != SegmentKind::Single) {
            continue;
        }
        const std::size_t main = seg.speakers.front();
        for (std::size_t s = 0; s < S0; ++s) {
            if (s == main) {
                continue;
            }
            // Laughter on the listener's own mic (loud for them, no words).
            if (out.speakers[s].mic) {
                const std::size_t mic = *out.speakers[s].mic;
                const std::size_t a = frames.of(seg.range.start);
                const std::size_t b = std::min(frames.count, frames.of(seg.range.end() - Ticks{1}) + 1);
                const auto loud = [&](std::size_t f) { return micLevel(mic, f) >= 0.6f && transcriptCue[s][f] < 0.1f; };
                for (std::size_t f = a; f < b;) {
                    if (!loud(f)) {
                        ++f;
                        continue;
                    }
                    std::size_t g = f;
                    float peak = 0.0f;
                    while (g < b && loud(g)) {
                        peak = std::max(peak, micLevel(mic, g));
                        ++g;
                    }
                    if (frames.frame.value * static_cast<std::int64_t>(g - f) >= kTicksPerSecond * 3 / 10) {
                        out.reactions.push_back(
                            Reaction{TimeRange{frames.at(f), Ticks{frames.frame.value * static_cast<std::int64_t>(g - f)}}, s, peak, "laughs"});
                    }
                    f = g;
                }
            }
        }
    }
    out.reactions.insert(out.reactions.end(), backchannels.begin(), backchannels.end());
    std::sort(out.reactions.begin(), out.reactions.end(), [](const Reaction& a, const Reaction& b) { return a.range.start < b.range.start; });
    if (S0 == 0) {
        out.notes.push_back("no speaker found: transcribe the microphones, or give the angles microphones or faces");
    }
    return out;
}

} // namespace wabcut::ai::multicam
