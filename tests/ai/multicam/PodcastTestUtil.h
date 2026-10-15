#pragma once

// The raw recordings of the synthetic podcast (MulticamTestUtil.h) with every service the AI-13 podcast workflow uses:
// audio sync from a table, a fixed loudness meter, a fake Enhance Speech, transcripts on two topics (coffee, then
// mountains: for the chapters; the host says "um" now and then) and the speaker signals. No transcription service: the microphones have transcripts.

#include "MulticamTestUtil.h"

#include <wabcut/ai/workflow/Podcast.h>
#include <wabcut/audio/edit/AudioAnalysis.h>

namespace wabcut::ai::multicam::test {

/// Two topics: coffee for the first half, mountains for the second (for the chapters).
inline const std::vector<std::string> kCoffee{"coffee", "beans", "roast", "espresso", "grinder", "brew", "crema", "barista"};
inline const std::vector<std::string> kMountains{"mountain", "trail", "summit", "hiking", "ridge", "glacier", "climb", "valley"};

inline std::shared_ptr<const Transcript> topicalWords(const std::vector<Talk>& script, int who) {
    auto t = std::make_shared<Transcript>();
    t->language = "en";
    int n = 0;
    for (const Talk& k : script) {
        if (k.who != who) {
            continue;
        }
        for (double s = k.start; s + 0.3 <= k.end; s += 0.4, ++n) {
            const auto& words = s < 17.2 ? kCoffee : kMountains;
            TranscriptWord w;
            // The host says "um" now and then (for the clean-up).
            w.text = who == 0 && n % 13 == 5 ? std::string("um") : words[static_cast<std::size_t>(n) % words.size()] + ((n % 8 == 7) ? "." : "");
            w.t0 = secs(s);
            w.t1 = secs(s + 0.3);
            w.confidence = 0.9f;
            w.speakerId = 0;
            t->words.push_back(std::move(w));
        }
    }
    return t;
}

class FixedLoudness final : public wabcut::audio::edit::LoudnessAnalyzer {
public:
    std::expected<wabcut::audio::edit::LoudnessStats, Error> measure(const ProjectState&, SequenceId, const wabcut::audio::edit::LoudnessTarget&,
                                                             std::optional<TimeRange>, command::ExecutionControl*) override {
        ++calls;
        wabcut::audio::edit::LoudnessStats s;
        // -24 LUFS, then (audio.normalize measures again after its gain) the target it asked for.
        s.integratedLufs = calls % 2 == 1 ? -24.0 : -16.0;
        s.truePeakDbtp = -20.0;
        s.samplePeakDbfs = -20.0;
        s.duration = secs(40);
        return s;
    }
    int calls = 0;
};

class FakeEnhancer final : public wabcut::audio::edit::SpeechEnhancer {
public:
    std::string modelVersion() const override { return "fake-enhance-1"; }
    std::expected<Take, Error> render(const MediaItem& item, int, std::stop_token, const std::function<void(double)>& progress) override {
        ++renders;
        if (progress) {
            progress(1.0);
        }
        Take t;
        t.path = "C:/cache/enhanced-" + item.name + ".wav";
        t.model = modelVersion();
        t.seconds = 40.0;
        t.elapsed = 0.5;
        t.device = "CPU";
        return t;
    }
    int renders = 0;
};

/// The raw recordings and every service the workflow uses (audio sync from a table, the fake loudness meter and
/// Enhance Speech, the transcripts, the speaker signals). No transcription service: the mics have transcripts.
struct PodcastStudio {
    PodcastStudio() : studio(podcastScript(), true) {
        studio.transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
        studio.transcripts->add(studio.tl.media('D'), 0, topicalWords(podcastScript(), 0));
        studio.transcripts->add(studio.tl.media('E'), 0, topicalWords(podcastScript(), 1));
        loudness = std::make_shared<FixedLoudness>();
        enhancer = std::make_shared<FakeEnhancer>();
        auto audioServices = std::make_shared<wabcut::audio::edit::AudioServices>(loudness, nullptr);
        audioServices->enhancer = enhancer;
        std::vector<std::pair<MediaId, Ticks>> offsets;
        for (const MediaId m : studio.cameras()) {
            offsets.emplace_back(m, Ticks{});
        }
        for (const MediaId m : studio.mics()) {
            offsets.emplace_back(m, Ticks{});
        }
        services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<ai::edit::TranscriptServices>(studio.transcripts), std::make_shared<ai::multicam::SpeakerServices>(studio.angleSignals),
            std::make_shared<command::MulticamServices>(std::make_shared<wabcut::OffsetTableAudioSync>(offsets)), audioServices});
    }

    workflow::PodcastOptions options() const {
        workflow::PodcastOptions o;
        o.cameras = studio.cameras();
        o.mics = studio.mics();
        o.name = "Episode 12";
        o.minChapterSeconds = 10.0;
        return o;
    }

    Studio studio;
    std::shared_ptr<FixedLoudness> loudness;
    std::shared_ptr<FakeEnhancer> enhancer;
    std::shared_ptr<command::ServiceBundle> services;
};

} // namespace wabcut::ai::multicam::test
