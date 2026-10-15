#pragma once

// A synthetic 3-camera podcast for the AI-13 tests (no media, no model): Cam A sees the host, Cam B the guest, Cam C
// both; Mic 1 is the host's microphone and Mic 2 the guest's (audio-only recordings). A 40 s script:
//    0.5 -  8.0  host          8.5 - 17.0  guest         15.0 - 17.0  the host too (crosstalk)
//   17.5 - 30.0  host (a long stretch; the guest says "Wow!" at 22.0 and laughs at 25.0 - 26.0 without words)
//   30.5 - 38.0  guest         38.0 - 40.0  silence
// Words are 0.3 s long every 0.4 s (so cuts have gaps to land in); each mic hears its speaker at -18 dBFS and the other
// at -38 (bleed); faces move their lips while their person speaks.

#include "EditTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/multicam/Evidence.h>
#include <wabcut/ai/multicam/MulticamAiCommands.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Nesting.h>

#include <cmath>
#include <map>
#include <memory>

namespace wabcut::ai::multicam::test {

using namespace wabcut::test;

struct Talk {
    double start;
    double end;
    int who; ///< 0 host, 1 guest
};

inline std::vector<Talk> podcastScript() {
    return {{0.5, 8.0, 0}, {8.5, 17.0, 1}, {15.0, 17.0, 0}, {17.5, 30.0, 0}, {30.5, 38.0, 1}};
}

inline Ticks secs(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

inline bool talking(const std::vector<Talk>& script, int who, double t) {
    for (const Talk& k : script) {
        if (k.who == who && t >= k.start && t < k.end) {
            return true;
        }
    }
    return false;
}

/// Words of `who`: 0.3 s every 0.4 s in their talks (plus extra words), sentences of 8 words.
inline std::shared_ptr<const Transcript> wordsOf(const std::vector<Talk>& script, int who, std::vector<std::pair<double, std::string>> extra = {},
                                                 const std::vector<std::string>& vocabulary = {}) {
    auto t = std::make_shared<Transcript>();
    t->language = "en";
    int n = 0;
    for (const Talk& k : script) {
        if (k.who != who) {
            continue;
        }
        for (double s = k.start; s + 0.3 <= k.end; s += 0.4) {
            TranscriptWord w;
            const std::string base = vocabulary.empty() ? (who == 0 ? "host" : "guest") + std::to_string(n) : vocabulary[static_cast<std::size_t>(n) % vocabulary.size()];
            w.text = base + ((n % 8 == 7) ? "." : "");
            w.t0 = secs(s);
            w.t1 = secs(s + 0.3);
            w.confidence = 0.9f;
            w.speakerId = 0;
            t->words.push_back(std::move(w));
            ++n;
        }
    }
    for (const auto& [at, text] : extra) {
        TranscriptWord w;
        w.text = text;
        w.t0 = secs(at);
        w.t1 = secs(at + 0.4);
        w.confidence = 0.9f;
        w.speakerId = 0;
        t->words.push_back(std::move(w));
    }
    std::sort(t->words.begin(), t->words.end(), [](const TranscriptWord& a, const TranscriptWord& b) { return a.t0 < b.t0; });
    return t;
}

/// Mic levels and faces from the script.
class ScriptSignals final : public AngleSignals {
public:
    ScriptSignals(std::vector<Talk> script, std::map<std::uint64_t, int> micOf, std::map<std::uint64_t, std::vector<int>> facesOf,
                  double seconds = 40.0)
        : m_script(std::move(script)), m_micOf(std::move(micOf)), m_facesOf(std::move(facesOf)), m_seconds(seconds) {}

    std::vector<std::pair<double, double>> laughter; ///< (start, end) on the guest's mic
    int levelCalls = 0;
    int faceCalls = 0;

    std::expected<std::vector<float>, Error> levels(const MediaItem& media, int, TimeRange source, Ticks frame, std::stop_token) override {
        ++levelCalls;
        const auto it = m_micOf.find(media.id.value);
        if (it == m_micOf.end()) {
            return std::unexpected(Error{ErrorCode::NotFound, "no level for " + media.name});
        }
        const int who = it->second;
        std::vector<float> out;
        for (Ticks t = source.start; t < source.end(); t = t + frame) {
            const double s = static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond);
            float db = -60.0f;
            if (talking(m_script, who, s)) {
                db = -18.0f + 2.0f * static_cast<float>(std::sin(s * 7.0));
            } else if (talking(m_script, 1 - who, s)) {
                db = -38.0f;
            }
            if (who == 1) {
                for (const auto& [a, b] : laughter) {
                    if (s >= a && s < b) {
                        db = -15.0f;
                    }
                }
            }
            out.push_back(db);
        }
        return out;
    }

    std::expected<vision::SubjectAnalysis, Error> faces(const MediaItem& media, int, const std::function<bool(double)>&) override {
        ++faceCalls;
        const auto it = m_facesOf.find(media.id.value);
        if (it == m_facesOf.end()) {
            return std::unexpected(Error{ErrorCode::NotFound, "no faces for " + media.name});
        }
        vision::SubjectAnalysis a;
        a.frameWidth = 1920;
        a.frameHeight = 1080;
        a.start = Ticks{};
        a.end = secs(m_seconds);
        int k = 0;
        for (double s = 0.0; s < m_seconds; s += 0.2, ++k) {
            vision::SubjectSample sample;
            sample.time = secs(s);
            for (std::size_t f = 0; f < it->second.size(); ++f) {
                const int who = it->second[f];
                vision::SubjectBox box;
                box.box = vision::Box{0.2f + 0.4f * static_cast<float>(f), 0.2f, 0.3f, 0.45f};
                box.score = 0.9f;
                box.track = static_cast<int>(f);
                box.activity = talking(m_script, who, s) ? ((k % 2) ? 0.5f : 0.3f) : 0.03f;
                sample.faces.push_back(box);
            }
            a.samples.push_back(std::move(sample));
        }
        return a;
    }

private:
    std::vector<Talk> m_script;
    std::map<std::uint64_t, int> m_micOf;
    std::map<std::uint64_t, std::vector<int>> m_facesOf;
    double m_seconds = 40.0;
};

/// The studio's recordings: media A, B, C are the cameras, D and E the microphones, each `seconds` long.
inline Timeline studioTimeline(double seconds) {
    const auto frames = static_cast<std::int64_t>(std::llround(seconds * 25.0));
    std::map<char, std::int64_t> lengths;
    for (char c = 'A'; c <= 'E'; ++c) {
        lengths[c] = frames;
    }
    return Timeline("V1 tgt |" + std::string(40, '.') + "|\nA1 tgt |" + std::string(40, '.') + "|", lengths);
}

/// The project with the recordings named as a podcast's ("Cam A.mov"... ; "Host mic.wav", "Guest mic.wav" without a
/// picture). The timeline's own state keeps the letters, so tl.media('A') still finds them.
inline ProjectState studioState(const Timeline& tl) {
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id == tl.media('D') || m->id == tl.media('E')) {
            auto audioOnly = std::make_shared<MediaItem>(*m);
            audioOnly->info.video.clear();
            audioOnly->name = m->id == tl.media('D') ? "Host mic.wav" : "Guest mic.wav";
            m = audioOnly;
        }
        if (m->id == tl.media('A') || m->id == tl.media('B') || m->id == tl.media('C')) {
            auto named = std::make_shared<MediaItem>(*m);
            named->name = m->id == tl.media('A') ? "Cam A.mov" : m->id == tl.media('B') ? "Cam B.mov" : "Cam C.mov";
            m = named;
        }
    }
    return state;
}

/// The studio: the recordings, their transcripts and signals, and (unless `raw`) a multicam source of the five and a
/// sequence with its multicam clip.
struct Studio {
    explicit Studio(std::vector<Talk> script = podcastScript(), bool raw = false, double seconds = 40.0,
                    const std::vector<std::string>& vocabulary = {})
        : tl(studioTimeline(seconds)) {
        length = secs(seconds);
        transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
        transcripts->add(tl.media('D'), 0, wordsOf(script, 0, {}, vocabulary));
        transcripts->add(tl.media('E'), 0, wordsOf(script, 1, seconds >= 23.0 ? std::vector<std::pair<double, std::string>>{{22.0, "Wow!"}}
                                                                              : std::vector<std::pair<double, std::string>>{},
                                                   vocabulary));
        angleSignals = std::make_shared<ScriptSignals>(script, std::map<std::uint64_t, int>{{tl.media('D').value, 0}, {tl.media('E').value, 1}},
                                                  std::map<std::uint64_t, std::vector<int>>{{tl.media('A').value, {0}},
                                                                                            {tl.media('B').value, {1}},
                                                                                            {tl.media('C').value, {0, 1}}},
                                                  seconds);
        angleSignals->laughter = {{25.0, 26.0}};
        state = studioState(tl);
        if (raw) {
            return;
        }
        wabcut::edit::MulticamSpec spec;
        spec.name = "Podcast (multicam)";
        spec.angles = cameras();
        spec.angles.push_back(tl.media('D'));
        spec.angles.push_back(tl.media('E'));
        spec.angleNames = {"Cam A", "Cam B", "Cam C", "Mic 1", "Mic 2"};
        spec.sync = MulticamSync::Manual;
        spec.offsets = std::vector<Ticks>(5, Ticks{});
        auto made = wabcut::edit::createMulticam(state, spec);
        REQUIRE(made);
        state = std::move(made->state);
        multicam = made->id;
        wabcut::edit::SourceClip clip;
        clip.sequence = multicam;
        clip.sourceRange = {Ticks{}, length};
        clip.videoAngle = AngleSet::of(0);
        clip.patches.push_back(wabcut::edit::SourcePatch{StreamRef{StreamKind::Video, 0}, tl.track("V1")});
        auto placed = wabcut::edit::overwriteEdit(state, tl.seq, clip, Ticks{});
        REQUIRE(placed);
        state = std::move(placed->state);
        picture = placed->placed.front();
    }

    std::vector<MediaId> cameras() const { return {tl.media('A'), tl.media('B'), tl.media('C')}; }
    std::vector<MediaId> mics() const { return {tl.media('D'), tl.media('E')}; }

    std::shared_ptr<command::ServiceBundle> services(bool withSignals = true) const {
        std::vector<std::shared_ptr<command::CommandServices>> parts{std::make_shared<ai::edit::TranscriptServices>(transcripts)};
        if (withSignals) {
            parts.push_back(std::make_shared<SpeakerServices>(angleSignals));
        }
        return std::make_shared<command::ServiceBundle>(std::move(parts));
    }

    Timeline tl;
    Ticks length{};
    ProjectState state;
    SequenceId multicam;
    ClipId picture;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    std::shared_ptr<ScriptSignals> angleSignals;
};

} // namespace wabcut::ai::multicam::test
