#pragma once

// Test helpers: a project with audio tracks built clip by clip (sample-accurate positions), a Null-backend
// engine driven block by block, and a TimelineAudioPlayer in manual-feed mode.

#include <wabcut/playback/audio/SyntheticAudioFeed.h>
#include <wabcut/playback/audio/TimelineAudioPlayer.h>

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Gain.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace wabcut::test {

inline constexpr Rate k48k{48000, 1};
inline constexpr std::int64_t kTicksPerFrame = 5'292'000;

/// Milliseconds as ticks (exact for multiples of 1 µs).
inline Ticks ms(double v) {
    return Ticks{static_cast<std::int64_t>(std::llround(v * 1000.0)) * (kTicksPerSecond / 1'000'000)};
}
inline Ticks frames(std::int64_t n) { return Ticks{n * kTicksPerFrame}; }

struct ClipSpec {
    char media = 'A';
    Ticks start;
    Ticks duration;
    Ticks sourceIn;
    double volumeDb = 0.0;
    double pan = 0.0;
    bool enabled = true;
};

/// A project with one sequence; media 'A'.. are synthetic streams defined on the fixture's feed.
class Project {
public:
    Project() {
        Sequence seq;
        seq.id = state.ids.allocate<SequenceId>();
        seq.name = "Sequence";
        sequence = seq.id;
        state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    }

    MediaId media(char letter, playback::SyntheticSignal signal) {
        MediaItem item;
        item.id = state.ids.allocate<MediaId>();
        item.name = std::string(1, letter);
        item.locator.path = std::string("synthetic:") + letter;
        item.info.duration = Ticks::fromSeconds(3600);
        AudioStreamInfo stream;
        stream.index = 0;
        stream.channels = 2;
        stream.duration = Ticks::fromSeconds(3600);
        item.info.audio.push_back(stream);
        const MediaId id = item.id;
        state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
        letters[letter - 'A'] = id;
        feed->setSignal(id, 0, signal);
        return id;
    }
    MediaId mediaId(char letter) const { return letters[letter - 'A']; }

    Clip makeClip(const ClipSpec& spec) {
        Clip clip;
        clip.id = state.ids.allocate<ClipId>();
        clip.media = mediaId(spec.media);
        clip.stream = {StreamKind::Audio, 0};
        clip.timelineRange = {spec.start, spec.duration};
        clip.sourceIn = spec.sourceIn;
        clip.enabled = spec.enabled;
        ClipProperties props;
        props.set(ParamId::VolumeDb, spec.volumeDb);
        props.set(ParamId::Pan, spec.pan);
        clip.setProperties(props);
        return clip;
    }

    /// Appends audio track A(n+1) with the given clips (in timeline order).
    void addTrack(const std::vector<ClipSpec>& clips) {
        Track track;
        track.id = state.ids.allocate<TrackId>();
        track.kind = TrackKind::Audio;
        for (const ClipSpec& spec : clips) {
            track.clips.push_back(makeClip(spec));
        }
        Sequence seq = *state.sequences.front();
        seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
        state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    }

    const Sequence& sequenceRef() const { return *state.sequences.front(); }
    const Track& track(std::size_t index) const { return *sequenceRef().audioTracks[index]; }

    /// `state` with audio track `index` replaced.
    static ProjectState withTrack(const ProjectState& from, std::size_t index, Track track) {
        Sequence seq = *from.sequences.front();
        seq.audioTracks[index] = std::make_shared<const Track>(std::move(track));
        return from.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }

    ProjectState state;
    SequenceId sequence;
    std::shared_ptr<playback::SyntheticAudioFeed> feed = std::make_shared<playback::SyntheticAudioFeed>();
    MediaId letters[26]{};
};

/// The engine (Null backend) + player, driven 10 ms block by 10 ms block.
class Rig {
public:
    struct Options {
        Ticks declick{};    // engine de-click (off: bit-exact output)
        Ticks edgeFade{};   // player clip-edge fades (off: bit-exact output)
        bool feederThread = false;
        Ticks preroll = Ticks{};
        int outputLatencyFrames = 0; // the Null device's output latency (A.8: voice-over placement)
    };

    Rig(Project& project, Options options) : m_project(project), session(project.state) {
        audio::AudioDeviceConfig config;
        config.backend = audio::AudioBackend::Null;
        config.declickTime = options.declick;
        config.nullLatencyFrames = options.outputLatencyFrames;
        REQUIRE(engine.start(config));
        playback::TimelineAudioOptions playerOptions;
        playerOptions.feederThread = options.feederThread;
        playerOptions.edgeFade = options.edgeFade;
        playerOptions.preroll = options.preroll;
        player = std::make_unique<playback::TimelineAudioPlayer>(engine, project.feed, playerOptions);
        player->setSession(&session);
        player->setSequence(project.sequence);
        m_threaded = options.feederThread;
        REQUIRE(player->start());
    }
    Rig(Project& project) : Rig(project, Options{}) {}

    ~Rig() {
        player->stop();
        engine.stop();
    }

    /// Pulls one block (default 10 ms) at the next simulated time, then (manual mode) runs a feeder pass.
    std::vector<float> pull(std::size_t frameCount = 480) {
        std::vector<float> block(frameCount * 2);
        REQUIRE(engine.pull(block, simulatedTime()));
        m_elapsedFrames += static_cast<std::int64_t>(frameCount);
        stream.insert(stream.end(), block.begin(), block.end());
        if (!m_threaded) {
            player->feedOnce();
        }
        return block;
    }
    void pullFor(double milliseconds) {
        const auto blocks = static_cast<int>(std::llround(milliseconds / 10.0));
        for (int i = 0; i < blocks; ++i) {
            pull();
        }
    }

    std::chrono::steady_clock::time_point simulatedTime() const {
        static const auto origin = std::chrono::steady_clock::now() - std::chrono::minutes(30);
        return origin + std::chrono::nanoseconds(m_elapsedFrames * 1'000'000'000LL / 48000);
    }

    Project& m_project;
    EditSession session;
    audio::AudioEngine engine;
    std::unique_ptr<playback::TimelineAudioPlayer> player;
    std::vector<float> stream; ///< everything pulled, interleaved stereo
    std::int64_t m_elapsedFrames = 0;
    bool m_threaded = false;
};

/// Largest |x[i] - x[i-1]| on one channel of an interleaved stereo stream.
inline double maxStep(const std::vector<float>& stream, int channel = 0) {
    double worst = 0.0;
    for (std::size_t i = 2 + static_cast<std::size_t>(channel); i < stream.size(); i += 2) {
        worst = std::max(worst, std::fabs(static_cast<double>(stream[i]) - stream[i - 2]));
    }
    return worst;
}

} // namespace wabcut::test
