// TimelineAudioPlayer through the Null-backend engine, block by block.

#include "PlaybackFixture.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal ramp(float amplitude = 0.5f) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 1.0;
    s.amplitude = amplitude;
    s.channelScale = 0.5f;
    return s;
}
SyntheticSignal sine(double hz, float amplitude = 0.4f) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}
SyntheticSignal constant(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

/// What a track should contribute at timeline frame k on `channel` (no fades).
float expectedTrack(const Project& p, std::size_t trackIndex, std::int64_t k, int channel) {
    const Track& track = p.track(trackIndex);
    const Ticks t = frames(k);
    const auto index = track.clipIndexAt(t);
    if (!index) {
        return 0.0f;
    }
    const Clip& clip = track.clips[*index];
    const std::int64_t n = ticksToSamples(clip.sourceTimeAt(t), k48k);
    const float x = p.feed->sample(clip.media, clip.stream.index, n, channel, k48k);
    const float gain = audio::dbToLinear(static_cast<float>(clip.properties()[ParamId::VolumeDb].constant()));
    const audio::StereoGains balance = audio::stereoBalance(static_cast<float>(clip.properties()[ParamId::Pan].constant()));
    return x * (gain * (channel == 0 ? balance.left : balance.right));
}

float sampleAt(const std::vector<float>& stream, std::int64_t frame, int channel = 0) {
    return stream[static_cast<std::size_t>(frame) * 2 + static_cast<std::size_t>(channel)];
}

} // namespace

TEST_CASE("A 2-track timeline renders sample-accurately", "[playback][player]") {
    Project p;
    p.media('A', ramp(0.5f));
    p.media('B', sine(440.0, 0.4f));
    p.addTrack({{'A', ms(20), ms(500), Ticks::fromSeconds(1), -6.0}});      // A1
    p.addTrack({{'B', ms(300), ms(600), Ticks{}, 0.0, 0.5}});               // A2, balanced right
    Rig rig(p);
    rig.player->play();
    rig.pullFor(1000);

    std::size_t mismatches = 0;
    for (std::int64_t k = 0; k < 48000; ++k) {
        for (int c = 0; c < 2; ++c) {
            const float expected = expectedTrack(p, 0, k, c) + expectedTrack(p, 1, k, c);
            if (sampleAt(rig.stream, k, c) != expected) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(rig.player->stats().underrunFrames == 0);
    CHECK(rig.player->stats().activeTracks == 2);
    // Gaps are silent: before A1, and after A2 ends.
    CHECK(sampleAt(rig.stream, 959) == 0.0f);
    CHECK(sampleAt(rig.stream, 47999) == 0.0f);
    CHECK(sampleAt(rig.stream, 960) != 0.0f); // A1's first sample (frame 960 = 20 ms)
}

TEST_CASE("Gaps between clips are exact silence", "[playback][player]") {
    Project p;
    p.media('A', constant(0.3f));
    p.addTrack({{'A', ms(0), ms(100), Ticks{}}, {'A', ms(250), ms(100), Ticks{}}, {'A', ms(600), ms(50), Ticks{}}});
    Rig rig(p, Rig::Options{Ticks{}, ms(2)}); // with clip-edge fades: only the clips themselves are touched
    rig.player->play();
    rig.pullFor(800);
    for (const auto& [from, to] : std::vector<std::pair<double, double>>{{100, 250}, {350, 600}, {650, 800}}) {
        bool silent = true;
        for (std::int64_t k = static_cast<std::int64_t>(from * 48); k < static_cast<std::int64_t>(to * 48); ++k) {
            silent = silent && sampleAt(rig.stream, k) == 0.0f && sampleAt(rig.stream, k, 1) == 0.0f;
        }
        CHECK(silent);
    }
    CHECK(sampleAt(rig.stream, 48 * 50) == 0.3f);
    CHECK(sampleAt(rig.stream, 48 * 300) == 0.3f);
}

TEST_CASE("Track mute, solo and enable act through the mixer; meters follow", "[playback][player][mixer]") {
    Project p;
    p.media('A', constant(0.1f));
    p.media('B', constant(0.2f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(10), Ticks{}}});
    p.addTrack({{'B', Ticks{}, Ticks::fromSeconds(10), Ticks{}}});
    Rig rig(p);
    rig.player->play();
    auto settle = [&](auto change) {
        const ProjectSnapshot now = rig.session.snapshot();
        Track a = *now->sequences.front()->audioTracks[0];
        Track b = *now->sequences.front()->audioTracks[1];
        change(a, b);
        ProjectState next = Project::withTrack(*now, 0, a);
        next = Project::withTrack(next, 1, b);
        REQUIRE(rig.session.commit("flags", next));
        rig.player->feedOnce();
        rig.pull(); // the gain ramps over this block
        return rig.pull()[0];
    };
    rig.pull();
    CHECK(rig.pull()[0] == Approx(0.3f));
    CHECK(settle([](Track& a, Track&) { a.muted = true; }) == Approx(0.2f));
    CHECK(settle([](Track& a, Track& b) { a.muted = false; b.solo = true; }) == Approx(0.2f));
    CHECK(settle([](Track& a, Track& b) { a.solo = true; b.solo = false; }) == Approx(0.1f));
    CHECK(settle([](Track& a, Track& b) { a.solo = true; b.solo = true; }) == Approx(0.3f));
    CHECK(settle([](Track& a, Track& b) { a.solo = false; b.solo = false; b.enabled = false; }) == Approx(0.1f));
    CHECK(rig.player->stats().editFlushes == 0); // flags only: nothing was refilled

    CHECK(rig.player->trackMeter(0).peak(0) == Approx(0.1f));
    CHECK(rig.player->trackMeter(1).peak(0) < 0.2f); // disabled: falling back
    CHECK(rig.player->masterMeter().peak(0) >= 0.1f);
    CHECK(rig.player->trackMeter(5).peak(0) == 0.0f);
}

TEST_CASE("Seeking mid-clip and across clip edges lands sample-accurately without clicks",
          "[playback][player][seek]") {
    Project p;
    p.media('A', sine(440.0, 0.5f));
    p.media('B', sine(660.0, 0.5f));
    p.addTrack({{'A', Ticks{}, ms(1000), Ticks{}}, {'B', ms(1000), ms(1000), Ticks{}}}); // a cut at 1 s
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(50)}); // de-click + edge fades on, 50 ms preroll
    rig.player->play();
    rig.pullFor(200);

    // Seek mid-clip, then right before the cut so playback runs across it, then back into A.
    for (const double target : {500.0, 990.0, 120.0}) {
        rig.player->seek(ms(target));
        const auto before = static_cast<std::int64_t>(rig.stream.size() / 2);
        rig.pullFor(200);
        // The seek lands on the next block, the feeder refills from the target after it, and the block after that
        // plays the target (P.1: the preroll ends as soon as every track is refilled, well before its 50 ms), so
        // the output is the timeline at the target from one block (480 frames) after the seek, sample-accurate once
        // the 2 ms fades are over.
        std::size_t mismatches = 0;
        for (std::int64_t i = 200; i < 2000; ++i) {
            const auto timelineFrame = static_cast<std::int64_t>(target * 48) + i;
            if (std::abs(timelineFrame - 48000) < 100) {
                continue; // the cut's own fades
            }
            if (std::fabs(sampleAt(rig.stream, before + 480 + i) - expectedTrack(p, 0, timelineFrame, 0)) > 1e-6f) {
                ++mismatches;
            }
        }
        INFO("seek to " << target << " ms");
        CHECK(mismatches == 0);
    }
    // No clicks anywhere: steps never exceed the sines' own slope (2*pi*660/48000*0.5 = 0.043) by much.
    CHECK(maxStep(rig.stream) < 0.06);
    CHECK(rig.player->stats().seeks == 3);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("A seek's preroll ends as soon as the tracks are refilled, holds while they are not, at most its length",
          "[playback][player][seek][preroll]") {
    // P.1: play from a new position used to wait out the whole preroll (100 ms by default) even when the feeder had
    // the target's audio queued in a millisecond; the picture follows the audio clock, so it waited too.
    Project p;
    p.media('A', sine(440.0, 0.5f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(4), Ticks{}}});
    Rig rig(p, Rig::Options{Ticks{}, Ticks{}, false, ms(100)}); // manual feeding, the default 100 ms preroll
    rig.player->play();
    rig.pullFor(100);
    std::vector<float> block(960);
    const auto pullOnly = [&](int blocks) { // 10 ms device blocks, the feeder not run
        for (int b = 0; b < blocks; ++b) {
            REQUIRE(rig.engine.pull(block, rig.simulatedTime()));
            rig.m_elapsedFrames += 480;
        }
    };
    const auto position = [&] { return rig.engine.clockAt(rig.simulatedTime()); };

    SECTION("refilled: the target plays one block later, sample-accurate") {
        rig.player->seek(ms(2000));
        const auto before = static_cast<std::int64_t>(rig.stream.size() / 2);
        rig.pullFor(100);
        std::size_t mismatches = 0;
        for (std::int64_t i = 0; i < 3000; ++i) {
            if (std::fabs(sampleAt(rig.stream, before + 480 + i) - expectedTrack(p, 0, 96000 + i, 0)) > 1e-6f) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        CHECK(sampleAt(rig.stream, before + 240) == 0.0f); // the seek's own block: held
        CHECK(rig.player->stats().underrunFrames == 0);
    }
    SECTION("not refilled: the transport holds at the target, and moves once the feeder catches up") {
        rig.player->seek(ms(2000));
        pullOnly(5); // 50 ms, the feeder never ran
        CHECK(position() == ms(2000));
        rig.player->feedOnce(); // refills from the target: ready
        pullOnly(2);
        CHECK(position() > ms(2000));
        CHECK(position() <= ms(2000) + ms(20));
    }
    SECTION("never refilled: the preroll still ends after its 100 ms") {
        rig.player->seek(ms(2000));
        pullOnly(9);
        CHECK(position() == ms(2000));
        pullOnly(3);
        CHECK(position() > ms(2000));
    }
}

TEST_CASE("An edit during playback takes effect within one lead window, without a gap",
          "[playback][player][edit]") {
    Project p;
    p.media('A', constant(0.25f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(5), Ticks{}}});
    Rig rig(p, Rig::Options{Ticks{}, ms(2)});
    rig.player->play();
    rig.pullFor(200);

    // Turn the clip down by 12 dB while it plays.
    Track turned = p.track(0);
    ClipProperties props = turned.clips[0].properties();
    props.set(ParamId::VolumeDb, -12.0);
    turned.clips.mutableAt(0).setProperties(props);
    REQUIRE(rig.session.commit("volume", Project::withTrack(*rig.session.snapshot(), 0, turned)));
    const std::int64_t editFrame = static_cast<std::int64_t>(rig.stream.size() / 2);
    rig.player->feedOnce();
    rig.pullFor(400);

    const float quiet = 0.25f * audio::dbToLinear(-12.0f);
    std::int64_t switched = -1;
    bool gap = false;
    for (std::int64_t k = editFrame; k < static_cast<std::int64_t>(rig.stream.size() / 2); ++k) {
        const float v = sampleAt(rig.stream, k);
        gap = gap || v < quiet * 0.5f;
        if (switched < 0 && std::fabs(v - quiet) < 1e-6f) {
            switched = k;
        }
    }
    REQUIRE(switched >= 0);
    const double latencyMs = static_cast<double>(switched - editFrame) / 48.0;
    INFO("the edit was heard " << latencyMs << " ms after it was committed");
    CHECK(latencyMs <= 250.0); // within one lead window (options.leadTime)
    CHECK(latencyMs >= 40.0);  // and not before the cutover: the audio already queued was used, not cut
    CHECK_FALSE(gap);          // the take-over is continuous: no dropout
    CHECK(rig.player->stats().editFlushes == 1);
    CHECK(maxStep(rig.stream) < 0.2f * (1.0f / 96.0f) + 0.01f); // the level change itself is de-clicked
}

TEST_CASE("Loop playback wraps the timeline audio exactly where the clock wraps", "[playback][player][loop]") {
    Project p;
    p.media('A', ramp(0.5f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(2), Ticks{}}});
    Rig rig(p);
    rig.player->setLoop(TimeRange{ms(100), ms(200)}); // [100 ms, 300 ms)
    rig.player->feedOnce();
    rig.player->play();
    rig.pullFor(700);
    // Timeline frame for output frame k: 0..14399 straight, then wrapping every 9600 frames.
    std::size_t mismatches = 0;
    for (std::int64_t k = 0; k < 48 * 700; ++k) {
        std::int64_t t = k;
        if (t >= 14400) {
            t = 4800 + (t - 14400) % 9600;
        }
        if (sampleAt(rig.stream, k) != expectedTrack(p, 0, t, 0)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK(rig.player->stats().resyncs == 0);
}

TEST_CASE("Changing a loop just before its end plays on without a gap", "[playback][player][loop]") {
    // The audio queued 20 ms before the wrap has wrapped already. Refilled from the edit cutover (40 ms ahead), the
    // 20 ms between the old wrap and the cutover were missing. And with the loop (200 ms) shorter than the lead
    // (250 ms) the rings were full of repeats, so a refill while paused had no room: a block of silence on play.
    Project p;
    p.media('A', ramp(0.5f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(2), Ticks{}}});
    Rig rig(p);
    rig.player->setLoop(TimeRange{ms(100), ms(200)}); // [100 ms, 300 ms)
    rig.player->feedOnce();
    rig.player->play();
    rig.pullFor(280);
    const auto straight = [&](std::int64_t from, std::int64_t to) {
        std::size_t mismatches = 0;
        for (std::int64_t k = from; k < to; ++k) {
            mismatches += sampleAt(rig.stream, k) != expectedTrack(p, 0, k, 0) ? 1 : 0;
        }
        return mismatches;
    };
    SECTION("the loop removed") {
        rig.player->setLoop(std::nullopt);
        rig.pullFor(300);
        CHECK(straight(0, 48 * 580) == 0); // the transport never wrapped: frame k plays timeline frame k
    }
    SECTION("the loop now ending later") {
        rig.player->setLoop(TimeRange{ms(100), ms(500)}); // [100 ms, 600 ms)
        rig.pullFor(300);
        CHECK(straight(0, 48 * 580) == 0);
    }
    SECTION("a loop set to end just ahead wraps there exactly") {
        rig.player->setLoop(TimeRange{ms(100), ms(190)}); // [100 ms, 290 ms): 10 ms ahead
        rig.pullFor(300);
        std::size_t mismatches = 0;
        for (std::int64_t k = 0; k < 48 * 580; ++k) {
            const std::int64_t t = k < 13920 ? k : 4800 + (k - 13920) % 9120;
            mismatches += sampleAt(rig.stream, k) != expectedTrack(p, 0, t, 0) ? 1 : 0;
        }
        CHECK(mismatches == 0);
    }
    SECTION("the loop removed, then paused and played") {
        rig.player->setLoop(std::nullopt);
        rig.player->pause();
        rig.pullFor(50);
        rig.player->play();
        rig.pullFor(100);
    }
    CHECK(rig.player->stats().underrunFrames == 0);
    CHECK(rig.player->stats().resyncs == 0);
}

TEST_CASE("Shuttle: 2x is heard, 8x is muted; back at 1x the audio continues where the transport is",
          "[playback][player][shuttle]") {
    Project p;
    p.media('A', ramp(0.5f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(10), Ticks{}}});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(100);
    rig.player->setSpeed({2, 1});
    rig.pullFor(100);
    // 2x is time-stretched audio (after the stretcher's 10 ms fade-in).
    bool heard = false;
    for (std::size_t k = 4800 + 960; k < 9600; ++k) {
        heard = heard || sampleAt(rig.stream, static_cast<std::int64_t>(k)) != 0.0f;
    }
    CHECK(heard);
    rig.player->setSpeed({8, 1});
    rig.pullFor(100);
    bool silent = true;
    for (std::size_t k = 9600; k < 14400; ++k) {
        silent = silent && sampleAt(rig.stream, static_cast<std::int64_t>(k)) == 0.0f;
    }
    CHECK(silent);
    rig.player->setSpeed(audio::TransportSpeed::normal());
    rig.player->feedOnce();
    rig.pullFor(100);
    // 100 ms at 1x + 100 ms at 2x + 100 ms at 8x = timeline 1100 ms when 1x resumes.
    CHECK(sampleAt(rig.stream, 14400 + 100) == expectedTrack(p, 0, 48 * 1100 + 100, 0));
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("Transport jumps the player was not told about are resynced", "[playback][player][resync]") {
    Project p;
    p.media('A', ramp(0.5f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(30), Ticks{}}});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(300);
    rig.engine.setTimeline(Ticks::fromSeconds(10)); // forward, behind the player's back
    rig.pullFor(300);
    CHECK(sampleAt(rig.stream, 48 * 590) == expectedTrack(p, 0, 48 * 10'290, 0));
    rig.engine.setTimeline(Ticks::fromSeconds(2)); // and backwards
    rig.pullFor(300);
    CHECK(sampleAt(rig.stream, 48 * 890) == expectedTrack(p, 0, 48 * 2'290, 0));
    CHECK(rig.player->stats().resyncs >= 1);
}

TEST_CASE("A slow feed underruns into silence, is counted, and recovers", "[playback][player][underrun]") {
    Project p;
    p.media('A', constant(0.25f));
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(60), Ticks{}}});
    Rig rig(p, Rig::Options{Ticks{}, Ticks{}, true}); // feeder thread
    rig.player->play();
    p.feed->setReadDelay(std::chrono::milliseconds(40)); // 512 frames (10.7 ms) per 40 ms: 4x too slow
    for (int i = 0; i < 100; ++i) {
        rig.pull(); // one second of audio, as fast as the test can pull
    }
    const auto stats = rig.player->stats();
    CHECK(stats.underrunFrames > 0);
    CHECK(std::ranges::all_of(rig.stream, [](float x) { return std::isfinite(x) && (x == 0.0f || x == 0.25f); }));

    p.feed->setReadDelay(std::chrono::microseconds(0));
    bool recovered = false;
    for (int i = 0; i < 300 && !recovered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto block = rig.pull();
        recovered = std::ranges::all_of(block, [](float x) { return x == 0.25f; });
    }
    CHECK(recovered);
}

TEST_CASE("The ChangeSet narrows edit refills: outside the queued audio nothing is refilled",
          "[playback][player][edit][changeset]") {
    Project p;
    p.media('A', constant(0.25f));
    p.media('B', constant(0.25f));
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}, {'B', ms(3000), ms(1000), Ticks{}}});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(200); // audio is queued up to ~450 ms

    Track edited = p.track(0);
    ClipProperties props = edited.clips[1].properties();
    props.set(ParamId::VolumeDb, -12.0); // the clip at 3 s, far beyond the queued audio
    edited.clips.mutableAt(1).setProperties(props);
    REQUIRE(rig.session.commit("volume later", Project::withTrack(*rig.session.snapshot(), 0, edited)));
    rig.player->feedOnce();
    CHECK(rig.player->stats().editFlushes == 0); // nothing queued was affected

    rig.pullFor(3300);
    const float quiet = 0.25f * audio::dbToLinear(-12.0f);
    CHECK(sampleAt(rig.stream, 48 * 2900) == 0.25f);
    CHECK(sampleAt(rig.stream, 48 * 3200) == quiet); // the edit is heard when playback gets there
}

TEST_CASE("The ChangeSet narrows edit refills: inside the queued audio the refill starts at the edited range",
          "[playback][player][edit][changeset]") {
    Project p;
    p.media('A', constant(0.25f));
    p.media('B', constant(0.25f));
    p.addTrack({{'A', Ticks{}, ms(350), Ticks{}}, {'B', ms(350), ms(2000), Ticks{}}});
    Rig rig(p); // no fades: the take-over point is visible sample-exactly
    rig.player->play();
    rig.pullFor(200);

    Track edited = p.track(0);
    ClipProperties props = edited.clips[1].properties();
    props.set(ParamId::VolumeDb, -12.0); // B starts 150 ms ahead, inside the queued audio, after the cutover
    edited.clips.mutableAt(1).setProperties(props);
    REQUIRE(rig.session.commit("volume ahead", Project::withTrack(*rig.session.snapshot(), 0, edited)));
    rig.player->feedOnce();
    CHECK(rig.player->stats().editFlushes == 1);
    rig.pullFor(400);

    const float quiet = 0.25f * audio::dbToLinear(-12.0f);
    bool before = true;
    bool after = true;
    for (std::int64_t k = 48 * 200; k < 48 * 350; ++k) {
        before = before && sampleAt(rig.stream, k) == 0.25f; // untouched: no refill, no crossfade before B
    }
    for (std::int64_t k = 48 * 350; k < 48 * 600; ++k) {
        after = after && sampleAt(rig.stream, k) == quiet; // from B's first sample on, the edited level
    }
    CHECK(before);
    CHECK(after);
}
