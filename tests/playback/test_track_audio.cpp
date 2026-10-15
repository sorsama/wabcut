// renderTrack(): the pure, sample-accurate track renderer.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/TrackAudio.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_approx.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal ramp() {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 1.0; // one sawtooth per second: every sample value is unique
    s.amplitude = 0.5f;
    s.channelScale = 0.5f;
    return s;
}

SyntheticSignal constant(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

std::vector<float> render(const Project& p, std::size_t track, Ticks start, std::size_t frameCount,
                          TrackRenderOptions options = {Ticks{}}, TrackRenderStats* statsOut = nullptr) {
    std::vector<float> out(frameCount * 2, -9.0f);
    TrackRenderStats stats;
    renderTrack(p.track(track), start, out, 2, k48k, *p.feed, options, stats);
    if (statsOut != nullptr) {
        *statsOut = stats;
    }
    return out;
}

} // namespace

TEST_CASE("Remapped audio reads unity ranges straight and plays the rest with preview export parity",
          "[playback][track][remap]") {
    Project p;
    p.media('A', ramp());
    p.addTrack({{'A', frames(100), frames(600), frames(500)}});
    Track track = p.track(0);
    Clip& clip = track.clips.mutableAt(0);
    ClipContent content = clip.content();
    const auto key = [](std::int64_t time, std::int64_t source, Interpolation mode = Interpolation::Linear) {
        return Keyframe{frames(time), static_cast<double>(frames(source).value), mode};
    };
    content.timeRemap = Curve{{key(500, 1000), key(600, 1100), key(700, 1300), key(800, 1300),
                               key(900, 1200, Interpolation::EaseOut), key(1000, 1250), key(1100, 1350)}};
    clip.setContent(std::move(content));
    REQUIRE(clip.unitySpeedRanges() == std::vector<TimeRange>{{frames(100), frames(100)}, {frames(600), frames(100)}});
    p.state = Project::withTrack(p.state, 0, track);
    const auto expected = render(p, 0, {}, 960);
    const auto sample = [&](std::int64_t source, int ch) { return p.feed->sample(p.mediaId('A'), 0, source, ch, k48k); };
    // A.10: the 100 % spans [100, 200) and [600, 700) read the media sample for sample (within 32 frames of a speed
    // span they crossfade with it, which at whole-frame positions changes only the rounding); the 2x span [200, 300),
    // the 100 % reverse [400, 500) (the time-reversed source: exact where its speed gain is whole) and the ramp
    // [500, 600) sound; the hold is silent; nothing plays outside the clip. The reverse sits between the hold and a
    // ramp that eases out of a standstill, so its speed gain (averaged over +-48 frames, so a speed that jumps out of the
    // audible range fades instead of clicking) ramps over its first and last 48 frames.
    for (std::size_t k = 0; k < 960; ++k) {
        const auto f = static_cast<std::int64_t>(k);
        for (int ch = 0; ch < 2; ++ch) {
            const float got = expected[2 * k + static_cast<std::size_t>(ch)];
            if ((k >= 100 && k < 168) || (k >= 632 && k < 700)) {
                CHECK(got == sample(k < 200 ? 1000 + f - 100 : 1250 + f - 600, ch));
            } else if (k >= 168 && k < 200) {
                CHECK(got == Approx(sample(1000 + f - 100, ch)).margin(1e-6));
            } else if (k >= 600 && k < 632) {
                CHECK(got == Approx(sample(1250 + f - 600, ch)).margin(1e-6));
            } else if (k >= 400 && k < 500) {
                const float reversed = sample(1299 - (f - 400), ch); // frame 400 plays the frame before position 1300
                if (k >= 448 && k < 452) {
                    CHECK(got == reversed);
                } else { // the same sample, faded
                    CHECK(std::fabs(got) <= std::fabs(reversed) + 1e-6f);
                    CHECK(got * reversed >= 0.0f);
                }
            } else if ((k >= 300 && k < 400) || k < 100 || k >= 700) {
                CHECK(got == 0.0f);
            }
        }
    }
    double fast = 0.0;
    for (std::size_t k = 200; k < 300; ++k) {
        fast += std::fabs(expected[2 * k]);
    }
    CHECK(fast / 100.0 > 0.1); // the 2x span plays (the sawtooth at twice the rate, band-limited)
    OfflineMixdownOptions options;
    options.renderer.edgeFade = {};
    OfflineMixdown offline(p.state.sequences.front(), 2, 48000, {}, *p.feed, options);
    std::vector<float> exported(1920);
    offline.render(std::span<float>(exported.data(), 274));
    offline.render(std::span<float>(exported.data() + 274, exported.size() - 274));
    CHECK(exported == expected);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(20);
    CHECK(rig.stream == exported);
    CHECK(rig.player->stats().feedErrors == 0);

    // A seek inside the second 1x segment uses the remapped source offset, independent of block boundaries.
    const auto seek = render(p, 0, frames(625), 100);
    CHECK(std::equal(seek.begin(), seek.end(), expected.begin() + 1250));
    Clip next = clip;
    next.timelineRange.start = clip.end();
    next.sourceIn = clip.sourceOut();
    CHECK_FALSE(continuesSeamlessly(clip, next));
}

TEST_CASE("Remapped audio fades at its 100 % span edges inside the clip, not only at its ends (review P2-21)",
          "[playback][track][remap][fade]") {
    Project p;
    p.media('A', constant(0.5f));
    p.addTrack({{'A', frames(0), frames(3000), frames(0)}});
    Track track = p.track(0);
    Clip& clip = track.clips.mutableAt(0);
    ClipContent content = clip.content();
    const auto key = [](std::int64_t time, std::int64_t source) {
        return Keyframe{frames(time), static_cast<double>(frames(source).value), Interpolation::Linear};
    };
    // 1x for 1000 frames, a hold for 1000 (silent), 1x again: two cuts inside the clip.
    content.timeRemap = Curve{{key(0, 0), key(1000, 1000), key(2000, 1000), key(3000, 2000)}};
    clip.setContent(std::move(content));
    REQUIRE(clip.unitySpeedRanges() == std::vector<TimeRange>{{frames(0), frames(1000)}, {frames(2000), frames(1000)}});
    p.state = Project::withTrack(p.state, 0, track);

    OfflineMixdownOptions options; // the player's 2 ms cut fades (96 frames)
    OfflineMixdown offline(p.state.sequences.front(), 2, 48000, {}, *p.feed, options);
    std::vector<float> out(3200 * 2);
    offline.render(std::span<float>(out.data(), 1234));
    offline.render(std::span<float>(out.data() + 1234, out.size() - 1234));
    // Before the fix the level jumped 0.5 -> 0 at frame 1000 and 0 -> 0.5 at frame 2000.
    CHECK(maxStep(out) < 0.5 / 96.0 * 1.5);
    CHECK(out[500 * 2] == 0.5f);
    CHECK(out[999 * 2] < 0.01f);  // faded out by the end of the first span
    CHECK(out[1500 * 2] == 0.0f); // the hold is silent
    CHECK(out[2000 * 2] < 0.01f); // fading in from the start of the second
    CHECK(out[2500 * 2] == 0.5f);
    // Preview renders the same samples (after the first frames, which the track streams' start de-click also fades).
    Rig::Options faded;
    faded.edgeFade = ms(2);
    Rig rig(p, faded);
    rig.player->play();
    rig.pullFor(70);
    REQUIRE(rig.stream.size() >= out.size());
    const auto [mine, theirs] = std::mismatch(out.begin() + 400, out.end(), rig.stream.begin() + 400);
    if (mine != out.end()) {
        UNSCOPED_INFO("first difference at sample " << (mine - out.begin()) << ": export " << *mine << ", preview " << *theirs);
    }
    CHECK(mine == out.end());
}

TEST_CASE("renderTrack places clip edges on the exact sample and leaves gaps silent", "[playback][track]") {
    Project p;
    p.media('A', ramp());
    // Clip from frame 100 to frame 300 (exclusive), source from frame 48000 (1 s).
    p.addTrack({{'A', frames(100), frames(200), frames(48000)}});
    const auto out = render(p, 0, Ticks{}, 400);
    for (std::size_t k = 0; k < 400; ++k) {
        const bool inside = k >= 100 && k < 300;
        const float left = inside ? p.feed->sample(p.mediaId('A'), 0, 48000 + static_cast<std::int64_t>(k) - 100, 0, k48k) : 0.0f;
        const float right = inside ? p.feed->sample(p.mediaId('A'), 0, 48000 + static_cast<std::int64_t>(k) - 100, 1, k48k) : 0.0f;
        CHECK(out[2 * k] == left);
        CHECK(out[2 * k + 1] == right);
    }

    // A clip edge between two samples: the first sample at or after the edge is the clip's first.
    Project q;
    q.media('A', constant(0.25f));
    q.addTrack({{'A', frames(10) + Ticks{1}, frames(10), Ticks{}}});
    const auto odd = render(q, 0, Ticks{}, 30);
    CHECK(odd[2 * 10] == 0.0f);   // frame 10 starts 1 tick before the clip
    CHECK(odd[2 * 11] == 0.25f);  // frame 11 is the clip's first sample
    CHECK(odd[2 * 20] == 0.25f);  // frame 20 at 10 frames + 0 ticks < end (20 frames + 1 tick)
    CHECK(odd[2 * 21] == 0.0f);
}

TEST_CASE("renderTrack applies clip volume and balance", "[playback][track]") {
    Project p;
    p.media('A', constant(0.5f));
    p.addTrack({{'A', Ticks{}, frames(100), Ticks{}, -6.0206, -1.0}});
    const auto out = render(p, 0, Ticks{}, 10);
    CHECK(out[0] == Approx(0.25f).epsilon(1e-4)); // -6 dB
    CHECK(out[1] == 0.0f);                         // hard left balance

    Project q;
    q.media('A', constant(0.5f));
    q.addTrack({{'A', Ticks{}, frames(100), Ticks{}, 0.0, 0.5}});
    const auto half = render(q, 0, Ticks{}, 10);
    CHECK(half[0] == Approx(0.5f * audio::stereoBalance(0.5f).left));
    CHECK(half[1] == 0.5f);
}

TEST_CASE("renderTrack fades cuts but not through edits", "[playback][track][fade]") {
    Project p;
    p.media('A', constant(0.5f));
    p.media('B', constant(0.5f));
    // A [0,1000) and A continuing [1000,2000) (through edit), then B [2000,3000) (a cut).
    p.addTrack({{'A', Ticks{}, frames(1000), Ticks{}}, {'A', frames(1000), frames(1000), frames(1000)},
                {'B', frames(2000), frames(1000), Ticks{}}});
    const auto out = render(p, 0, Ticks{}, 3000, TrackRenderOptions{});
    constexpr int kFade = 96; // 2 ms
    CHECK(out[0] == Approx(0.5f * 1.0f / (kFade + 1)));        // fade in at the start
    CHECK(out[2 * kFade] == 0.5f);                              // full level after the fade
    CHECK(out[2 * 999] == 0.5f);                                // through edit: no dip
    CHECK(out[2 * 1000] == 0.5f);
    CHECK(out[2 * 1999] == Approx(0.5f * 1.0f / (kFade + 1))); // cut A|B: A fades out...
    CHECK(out[2 * 2000] == Approx(0.5f * 1.0f / (kFade + 1))); // ...and B fades in
    CHECK(out[2 * 1900] == 0.5f); // before the 96-frame fade-out
}

TEST_CASE("renderTrack renders disabled, frozen and unreadable clips as silence", "[playback][track]") {
    Project p;
    p.media('A', constant(0.5f));
    p.addTrack({{'A', Ticks{}, frames(100), Ticks{}, 0.0, 0.0, false}});
    CHECK(render(p, 0, Ticks{}, 100) == std::vector<float>(200, 0.0f));

    // A freeze frame holds a picture: its audio is silent (A.10: other speeds play, test_speed_audio.cpp).
    Project q;
    q.media('A', constant(0.5f));
    q.addTrack({{'A', Ticks{}, frames(100), Ticks{}}});
    Track frozen = q.track(0);
    frozen.clips.mutableAt(0).speed = {0, 1};
    q.state = Project::withTrack(q.state, 0, frozen);
    CHECK(render(q, 0, Ticks{}, 50) == std::vector<float>(100, 0.0f));

    Project r;
    r.media('A', constant(0.5f));
    r.addTrack({{'A', Ticks{}, frames(100), Ticks{}}});
    Track missing = r.track(0);
    missing.clips.mutableAt(0).media = MediaId{999}; // the feed does not know it
    r.state = Project::withTrack(r.state, 0, missing);
    TrackRenderStats stats;
    CHECK(render(r, 0, Ticks{}, 50, TrackRenderOptions{Ticks{}}, &stats) == std::vector<float>(100, 0.0f));
    CHECK(stats.feedErrors == 1);

    // Past the end of the media: the rest of the clip is silence.
    Project s;
    SyntheticSignal shortSignal = constant(0.5f);
    shortSignal.duration = frames(30);
    s.media('A', shortSignal);
    s.addTrack({{'A', Ticks{}, frames(100), Ticks{}}});
    const auto out = render(s, 0, Ticks{}, 100, TrackRenderOptions{Ticks{}}, &stats);
    CHECK(out[2 * 29] == 0.5f);
    CHECK(out[2 * 30] == 0.0f);
    CHECK(stats.shortReadFrames == 70);
}
