// 0.5 audio scrubbing (playback::AudioScrubber through the AudioEngine's overlay), on the Null-backend engine pulled
// block by block and a synthetic feed (no audio device): the window is the export mix at the playhead with its fades,
// one frame or at least 60 ms long; the rate limit and a newer request replacing the queued one; a newer window cutting
// a sounding one without a click; mute and solo heard as the mix has them; nothing while playing; and the transport
// clock (so the picture) never moving for it. The worker thread renders off the calling thread.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/AudioScrubber.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal constant(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

SyntheticSignal ramp(float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 2.5; // no zero crossing inside the windows these tests hear
    s.amplitude = amplitude;
    s.channelScale = 0.5f;
    return s;
}

float sampleAt(const std::vector<float>& stream, std::int64_t frame, int channel = 0) {
    return stream[static_cast<std::size_t>(frame) * 2 + static_cast<std::size_t>(channel)];
}

/// The scrubber's defaults, rendered by hand (manual mode).
AudioScrubOptions manual() {
    AudioScrubOptions o;
    o.workerThread = false;
    return o;
}

/// The export mix of `p` from `start`, `frames` long, with the scrubber's pre-rolls.
std::vector<float> mixAt(const Project& p, Ticks start, std::size_t frames) {
    OfflineMixdownOptions options;
    const AudioScrubOptions scrub;
    options.renderer.maxPreroll = scrub.effectPreroll;
    options.renderer.maxClipPreroll = scrub.clipPreroll;
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(frames * 2);
    mix.render(out);
    return out;
}

/// The window's fade gain at frame `i` of `n` (fade frames `f`).
double fadeGain(std::size_t i, std::size_t n, std::size_t f) {
    if (i < f) {
        return static_cast<double>(i + 1) / static_cast<double>(f + 1);
    }
    if (i >= n - f) {
        return static_cast<double>(n - i) / static_cast<double>(f + 1);
    }
    return 1.0;
}

/// The first frame from `from` on whose left or right sample is not 0, or -1.
std::int64_t firstSound(const std::vector<float>& stream, std::size_t from = 0) {
    for (std::size_t i = from * 2; i < stream.size(); ++i) {
        if (stream[i] != 0.0f) {
            return static_cast<std::int64_t>(i / 2);
        }
    }
    return -1;
}

/// Frames from `from` on that carry sound.
std::size_t soundingFrames(const std::vector<float>& stream, std::size_t from = 0) {
    std::size_t n = 0;
    for (std::size_t f = from; f * 2 + 1 < stream.size(); ++f) {
        if (stream[f * 2] != 0.0f || stream[f * 2 + 1] != 0.0f) {
            ++n;
        }
    }
    return n;
}

/// A feed that remembers the threads that read it (the scrubber's own, not the caller's).
class ThreadRecordingFeed final : public AudioFeed {
public:
    explicit ThreadRecordingFeed(std::shared_ptr<SyntheticAudioFeed> inner) : m_inner(std::move(inner)) {}
    std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime, std::span<float> interleaved,
                                           int channels, Rate rate) override {
        if (std::this_thread::get_id() == m_caller) {
            m_onCaller.store(true);
        } else {
            m_elsewhere.store(true);
        }
        return m_inner->read(media, streamOrdinal, sourceTime, interleaved, channels, rate);
    }
    std::thread::id m_caller = std::this_thread::get_id();
    std::atomic<bool> m_onCaller{false};
    std::atomic<bool> m_elsewhere{false};

private:
    std::shared_ptr<SyntheticAudioFeed> m_inner;
};

const auto kT0 = std::chrono::steady_clock::now();

std::chrono::steady_clock::time_point at(int milliseconds) {
    return kT0 + std::chrono::milliseconds(milliseconds);
}

} // namespace

TEST_CASE("Audio scrubbing: the window length is one frame and at least 60 ms", "[playback][scrub]") {
    Project p;
    audio::AudioEngine engine;
    AudioScrubber scrubber(engine, p.feed, manual());
    CHECK(scrubber.windowLength(kRate30) == ms(60));
    CHECK(scrubber.windowLength(kRate24) == ms(60));
    CHECK(scrubber.windowLength(kRate59_94) == ms(60));
    CHECK(scrubber.windowLength(Rate{10, 1}) == ms(100)); // one frame of a 10 fps sequence
    CHECK(scrubber.windowLength(Rate{2, 1}) == ms(200));  // at most 200 ms
    // Not started: nothing is asked for, and start() needs a running engine.
    scrubber.scrub(ms(1000));
    CHECK(scrubber.stats().requests == 0);
    CHECK_FALSE(scrubber.start());
}

TEST_CASE("Audio scrubbing: a faded window of the mix at the playhead without moving the clock", "[playback][scrub]") {
    Project p;
    p.media('A', ramp(0.6f));
    p.addTrack({{'A', Ticks{}, ms(10000), ms(500)}});
    Rig rig(p);
    // Stopped at 2 s, the transport holds there.
    rig.player->seek(ms(2000));
    rig.pullFor(50);
    const Ticks parked = rig.engine.clockAt(rig.simulatedTime());
    REQUIRE(parked == ms(2000));
    REQUIRE(rig.engine.isPaused());

    AudioScrubber scrubber(rig.engine, p.feed, manual());
    scrubber.setSession(&rig.session);
    scrubber.setSequence(p.sequence);
    REQUIRE(scrubber.start());
    REQUIRE(rig.engine.overlay() != nullptr);

    // A scrub to 5 s: one window of 60 ms (30 fps: a frame is shorter), heard from the next block on.
    scrubber.scrub(ms(5000));
    REQUIRE(scrubber.process(at(0)));
    const std::size_t from = rig.m_elapsedFrames;
    rig.pullFor(200);
    const std::size_t frames = 60 * 48;
    const std::size_t fade = 5 * 48;
    CHECK(scrubber.stats().rendered == 1);
    CHECK(scrubber.stats().started == 1);
    CHECK(scrubber.stats().framesPlayed == frames);
    CHECK(scrubber.stats().lastWindowFrames == static_cast<std::int64_t>(frames));
    CHECK(scrubber.stats().lastWindowStart == ms(5000));
    const std::int64_t first = firstSound(rig.stream, from);
    REQUIRE(first == static_cast<std::int64_t>(from)); // the next block starts with it
    CHECK(soundingFrames(rig.stream, from) == frames);

    // What it plays is the export mix at 5 s with 5 ms fades at both ends.
    const std::vector<float> mix = mixAt(p, ms(5000), frames);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            const double expected = mix[i * 2 + static_cast<std::size_t>(c)] * fadeGain(i, frames, fade);
            const double got = sampleAt(rig.stream, static_cast<std::int64_t>(from + i), c);
            if (std::fabs(expected - got) > 1e-6) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(std::fabs(sampleAt(rig.stream, first)) < 0.01);                                      // fades in
    CHECK(std::fabs(sampleAt(rig.stream, first + static_cast<std::int64_t>(frames) - 1)) < 0.01); // and out

    // The transport and the clock did not move: the picture stays on the frame it shows.
    CHECK(rig.engine.isPaused());
    CHECK(rig.engine.clockAt(rig.simulatedTime()) == parked);
    CHECK(rig.player->position() == parked);
    scrubber.stop();
    CHECK(rig.engine.overlay() == nullptr);
}

TEST_CASE("Audio scrubbing: the rate limit and a newer request replacing the queued one", "[playback][scrub]") {
    Project p;
    p.media('A', constant(0.4f));
    p.addTrack({{'A', Ticks{}, ms(20000), Ticks{}}});
    Rig rig(p);
    AudioScrubber scrubber(rig.engine, p.feed, manual());
    scrubber.setSession(&rig.session);
    scrubber.setSequence(p.sequence);
    REQUIRE(scrubber.start());

    // The first window goes at once.
    scrubber.scrub(ms(1000));
    REQUIRE(scrubber.process(at(0)));
    // A fast drag: 2 s is replaced by 3 s before it renders; 10 ms later is too soon for another window.
    scrubber.scrub(ms(2000));
    scrubber.scrub(ms(3000));
    CHECK(scrubber.stats().replaced == 1);
    CHECK_FALSE(scrubber.process(at(10)));
    CHECK(scrubber.stats().rendered == 1);
    scrubber.scrub(ms(4000));
    CHECK(scrubber.stats().replaced == 2);
    // 30 ms after the first: the latest position (4 s) is rendered, the ones in between never are.
    REQUIRE(scrubber.process(at(30)));
    CHECK(scrubber.stats().rendered == 2);
    CHECK(scrubber.stats().lastWindowStart == ms(4000));
    CHECK_FALSE(scrubber.process(at(31))); // nothing queued
    CHECK(scrubber.stats().requests == 4);

    // Both windows were handed over before the audio thread ran: it plays only the newer one.
    const std::size_t from = rig.m_elapsedFrames;
    rig.pull(480);
    CHECK(scrubber.stats().started == 1);
    CHECK(scrubber.stats().skipped == 1);

    // A newer window while that one sounds: it fades out under the new one (constant level: no step, no click).
    scrubber.scrub(ms(6000));
    REQUIRE(scrubber.process(at(70)));
    rig.pullFor(200);
    CHECK(scrubber.stats().started == 2);
    CHECK(scrubber.stats().cutShort == 1);
    std::vector<float> heard(rig.stream.begin() + static_cast<std::ptrdiff_t>(from * 2), rig.stream.end());
    const std::int64_t start = firstSound(heard);
    REQUIRE(start == 0);
    // Inside the two windows (past the first fade-in, before the last fade-out) the level never jumps.
    const std::size_t cut = 480;
    const std::vector<float> across(heard.begin() + static_cast<std::ptrdiff_t>((cut - 100) * 2),
                                    heard.begin() + static_cast<std::ptrdiff_t>((cut + 400) * 2));
    CHECK(maxStep(across) < 0.005);
    CHECK(sampleAt(heard, static_cast<std::int64_t>(cut + 120)) == Approx(sampleAt(heard, 300)).margin(0.002));
    CHECK(soundingFrames(heard) == cut + 60 * 48);
}

TEST_CASE("Audio scrubbing: muted and soloed tracks are heard as the mix has them", "[playback][scrub]") {
    const auto plateau = [](const auto& setup) {
        Project p;
        p.media('A', constant(0.25f));
        p.media('B', constant(0.125f));
        p.addTrack({{'A', Ticks{}, ms(5000), Ticks{}}});
        p.addTrack({{'B', Ticks{}, ms(5000), Ticks{}}});
        setup(p);
        Rig rig(p);
        AudioScrubber scrubber(rig.engine, p.feed, manual());
        scrubber.setSession(&rig.session);
        scrubber.setSequence(p.sequence);
        REQUIRE(scrubber.start());
        scrubber.scrub(ms(1000));
        REQUIRE(scrubber.process(at(0)));
        const std::size_t from = rig.m_elapsedFrames;
        rig.pullFor(100);
        const double level = sampleAt(rig.stream, static_cast<std::int64_t>(from + 30 * 48));
        CHECK(level == Approx(mixAt(p, ms(1000), 30 * 48 + 1)[30 * 48 * 2]).margin(1e-6));
        return level;
    };
    const auto edit = [](Project& p, std::size_t index, auto change) {
        Track t = p.track(index);
        change(t);
        p.state = Project::withTrack(p.state, index, std::move(t));
    };
    const double both = plateau([](Project&) {});
    const double a1Muted = plateau([&](Project& p) { edit(p, 0, [](Track& t) { t.muted = true; }); });
    const double a2Solo = plateau([&](Project& p) { edit(p, 1, [](Track& t) { t.solo = true; }); });
    const double allMuted = plateau([&](Project& p) {
        edit(p, 0, [](Track& t) { t.muted = true; });
        edit(p, 1, [](Track& t) { t.muted = true; });
    });
    CHECK(both > a1Muted);
    CHECK(a1Muted == Approx(a2Solo).margin(1e-6)); // only A2 either way
    CHECK(a1Muted > 0.0);
    CHECK(allMuted == 0.0);
}

TEST_CASE("Audio scrubbing: nothing while playing and a cancel fades the window out", "[playback][scrub]") {
    Project p;
    p.media('A', constant(0.4f));
    p.addTrack({{'A', Ticks{}, ms(20000), Ticks{}}});
    Rig rig(p);
    AudioScrubber scrubber(rig.engine, p.feed, manual());
    scrubber.setSession(&rig.session);
    scrubber.setSequence(p.sequence);
    REQUIRE(scrubber.start());

    // Playing: the program is heard, a scrub is dropped.
    rig.player->play();
    rig.pullFor(30);
    scrubber.scrub(ms(3000));
    CHECK_FALSE(scrubber.process(at(0)));
    CHECK(scrubber.stats().dropped == 1);
    CHECK(scrubber.stats().rendered == 0);
    rig.player->pause();
    rig.pullFor(30);

    // Stopped: a window starts, then a cancel (playback starts, the sequence changes) fades it out within 5 ms.
    scrubber.scrub(ms(3000));
    REQUIRE(scrubber.process(at(100)));
    const std::size_t from = rig.m_elapsedFrames;
    rig.pull(480);
    scrubber.cancel();
    CHECK_FALSE(scrubber.process(at(101)));
    rig.pullFor(100);
    CHECK(scrubber.stats().cutShort == 1);
    CHECK(soundingFrames(rig.stream, from) == 480 + 5 * 48); // then silence
    std::vector<float> heard(rig.stream.begin() + static_cast<std::ptrdiff_t>(from * 2), rig.stream.end());
    CHECK(maxStep(std::vector<float>(heard.begin() + 400 * 2, heard.end())) < 0.005);

    // Another sequence (none here) drops what is queued: nothing renders for it.
    scrubber.scrub(ms(4000));
    scrubber.setSequence(SequenceId{});
    CHECK_FALSE(scrubber.process(at(200)));
    CHECK(scrubber.stats().rendered == 1);
}

TEST_CASE("Audio scrubbing: the worker renders off the calling thread and reports its latency", "[playback][scrub]") {
    Project p;
    p.media('A', constant(0.4f));
    p.addTrack({{'A', Ticks{}, ms(20000), Ticks{}}});
    Rig rig(p);
    auto feed = std::make_shared<ThreadRecordingFeed>(p.feed);
    AudioScrubber scrubber(rig.engine, feed, AudioScrubOptions{});
    scrubber.setSession(&rig.session);
    scrubber.setSequence(p.sequence);
    REQUIRE(scrubber.start());
    scrubber.scrub(ms(2500));
    for (int i = 0; i < 500 && scrubber.stats().rendered == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(scrubber.stats().rendered == 1);
    CHECK(feed->m_elsewhere.load());
    CHECK_FALSE(feed->m_onCaller.load());
    const std::int64_t latency = scrubber.stats().lastLatencyMicros;
    CHECK(latency > 0);
    CHECK(latency < 500'000);
    std::printf("audio scrubbing: request to hand-over %lld us (worker render, Null engine)\n", static_cast<long long>(latency));
    // The window plays at the next block of the device.
    rig.pullFor(100);
    CHECK(scrubber.stats().framesPlayed == 60 * 48);
    scrubber.stop();
    CHECK(rig.engine.overlay() == nullptr);
}

TEST_CASE("Audio scrubbing: a backward drag plays the window before the playhead backwards with its fades",
          "[playback][scrub]") {
    Project p;
    p.media('A', ramp(0.6f));
    p.addTrack({{'A', Ticks{}, ms(10000), ms(500)}});
    Rig rig(p);
    rig.player->seek(ms(6000));
    rig.pullFor(30);
    const Ticks parked = rig.engine.clockAt(rig.simulatedTime());
    AudioScrubber scrubber(rig.engine, p.feed, manual());
    scrubber.setSession(&rig.session);
    scrubber.setSequence(p.sequence);
    REQUIRE(scrubber.start());

    // Dragged back to 5 s: the 60 ms before it, from 5 s back to 4.94 s.
    scrubber.scrub(ms(5000), -1);
    REQUIRE(scrubber.process(at(0)));
    CHECK(scrubber.stats().lastWindowReversed);
    CHECK(scrubber.stats().lastWindowStart == ms(4940));
    const std::size_t from = rig.m_elapsedFrames;
    rig.pullFor(200);
    const std::size_t frames = 60 * 48;
    const std::size_t fade = 5 * 48;
    CHECK(firstSound(rig.stream, from) == static_cast<std::int64_t>(from));
    CHECK(soundingFrames(rig.stream, from) == frames);
    const std::vector<float> mix = mixAt(p, ms(4940), frames);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            // Frame i plays the mix's frame (frames - 1 - i), the fades on the reversed order.
            const double expected = mix[(frames - 1 - i) * 2 + static_cast<std::size_t>(c)] * fadeGain(i, frames, fade);
            if (std::fabs(expected - sampleAt(rig.stream, static_cast<std::int64_t>(from + i), c)) > 1e-6) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
    // The ramp rises with time, so backwards it falls: the sound runs back from the playhead.
    CHECK(sampleAt(rig.stream, static_cast<std::int64_t>(from + fade + 10)) >
          sampleAt(rig.stream, static_cast<std::int64_t>(from + frames - fade - 10)));
    CHECK(rig.engine.clockAt(rig.simulatedTime()) == parked);

    // Back at the very start: the window cannot reach before 0, it plays 0 to 60 ms backwards.
    scrubber.scrub(ms(20), -1);
    REQUIRE(scrubber.process(at(100)));
    CHECK(scrubber.stats().lastWindowStart == Ticks{});
    CHECK(scrubber.stats().lastWindowReversed);
    // Forward again: not reversed.
    scrubber.scrub(ms(3000), 1);
    REQUIRE(scrubber.process(at(200)));
    CHECK_FALSE(scrubber.stats().lastWindowReversed);
    CHECK(scrubber.stats().lastWindowStart == ms(3000));
}

TEST_CASE("Audio scrubbing: the worker waits out the rate limit without spinning", "[playback][scrub]") {
    Project p;
    p.media('A', constant(0.4f));
    p.addTrack({{'A', Ticks{}, ms(20000), Ticks{}}});
    Rig rig(p);
    AudioScrubOptions options;
    options.minInterval = std::chrono::milliseconds(150); // a long wait, so a spin would show
    AudioScrubber scrubber(rig.engine, p.feed, options);
    scrubber.setSession(&rig.session);
    scrubber.setSequence(p.sequence);
    REQUIRE(scrubber.start());
    scrubber.scrub(ms(1000));
    for (int i = 0; i < 500 && scrubber.stats().rendered == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(scrubber.stats().rendered == 1);
    // The next request waits for its turn (150 ms after the first window): one wait, then one pass that renders it.
    const std::uint64_t passes = scrubber.stats().workerPasses;
    scrubber.scrub(ms(2000));
    for (int i = 0; i < 1000 && scrubber.stats().rendered == 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(scrubber.stats().rendered == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    INFO("worker passes for the rate-limited request: " << scrubber.stats().workerPasses - passes);
    CHECK(scrubber.stats().workerPasses - passes <= 3); // one in practice (slack for a loaded machine)
    scrubber.stop();
}
