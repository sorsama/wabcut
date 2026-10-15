// Audible speed changes (A.10, TrackRenderer.h "Speed", audio/dsp/SpeedRender.h): constant speeds, remap ramps,
// reverse and holds through the track renderer - pitch with and without Maintain Audio Pitch, click positions,
// preview == export (also after a seek), silent holds with faded edges, click-free crossfades across ramps.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>
#include <wabcut/playback/audio/TrackAudio.h>

#include <wabcut/model/TimeRemap.h>

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <tuple>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal sineAt(double hz, float amplitude = 0.5f) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}

SyntheticSignal level(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

Keyframe key(std::int64_t time, std::int64_t source, Interpolation mode = Interpolation::Linear) {
    return Keyframe{frames(time), static_cast<double>(frames(source).value), mode};
}

/// Track 0's first clip changed by `change`.
template <class Change>
void changeClip(Project& p, Change change, std::size_t index = 0) {
    Track track = p.track(0);
    change(track.clips.mutableAt(index));
    p.state = Project::withTrack(p.state, 0, std::move(track));
}

void remap(Clip& clip, std::vector<Keyframe> keys) {
    ClipContent content = clip.content();
    content.timeRemap = Curve{std::move(keys)};
    clip.setContent(std::move(content));
}

/// The export path from `start`, in uneven blocks; left channel and both channels.
std::vector<float> exported(const Project& p, Ticks start, std::size_t frameCount, Ticks edgeFade = {}) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = edgeFade;
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(frameCount * 2);
    for (std::size_t done = 0; done < frameCount;) {
        const std::size_t n = std::min<std::size_t>(frameCount - done, 211 + done % 1013);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

std::vector<float> left(const std::vector<float>& stereo, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    std::vector<float> out;
    for (std::size_t k = from; k < std::min(to, stereo.size() / 2); ++k) {
        out.push_back(stereo[2 * k]);
    }
    return out;
}

/// Frequency from rising zero crossings (interpolated).
double frequencyOf(const std::vector<float>& x) {
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double at = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0) {
                first = at;
            } else {
                ++crossings;
            }
            last = at;
        }
    }
    return crossings > 0 ? crossings * 48000.0 / (last - first) : 0.0;
}

double meanAbs(const std::vector<float>& stereo, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t k = from; k < to; ++k) {
        sum += std::fabs(stereo[2 * k]);
    }
    return sum / static_cast<double>(to - from);
}

} // namespace

TEST_CASE("Speed audio: a constant speed plays the media at that speed with or without its pitch",
          "[playback][track][speed]") {
    for (const bool keepPitch : {false, true}) {
        for (const auto& [num, den, reversed] : {std::tuple{1, 2, false}, std::tuple{2, 1, false}, std::tuple{2, 1, true}}) {
            const double speed = static_cast<double>(num) / den;
            INFO("speed " << speed << (reversed ? " reversed" : "") << (keepPitch ? ", maintain pitch" : ", varispeed"));
            Project p;
            p.media('A', sineAt(1000.0));
            p.addTrack({{'A', frames(0), frames(96000), frames(200000)}});
            changeClip(p, [&](Clip& c) {
                c.speed = {num, den};
                c.reversed = reversed;
                c.maintainPitch = keepPitch;
            });
            const auto out = exported(p, Ticks{}, 96000);
            const double hz = frequencyOf(left(out, 4800, 91200));
            CHECK(hz == Approx(keepPitch ? 1000.0 : 1000.0 * speed).epsilon(0.01));
            CHECK(meanAbs(out, 4800, 91200) == Approx(0.5 * 2.0 / std::numbers::pi).epsilon(0.05)); // a full-level sine
        }
    }
    // Past the clip nothing plays, and a through edit of a slowed clip renders exactly like the whole clip.
    Project whole;
    whole.media('A', sineAt(440.0));
    whole.addTrack({{'A', frames(0), frames(48000), frames(1000)}});
    changeClip(whole, [](Clip& c) { c.speed = {1, 2}; });
    Project split;
    split.media('A', sineAt(440.0));
    split.addTrack({{'A', frames(0), frames(20000), frames(1000)}, {'A', frames(20000), frames(28000), frames(11000)}});
    changeClip(split, [](Clip& c) { c.speed = {1, 2}; }, 0);
    changeClip(split, [](Clip& c) { c.speed = {1, 2}; }, 1);
    REQUIRE(continuesSeamlessly(split.track(0).clips[0], split.track(0).clips[1]));
    const auto a = exported(whole, Ticks{}, 50000);
    CHECK(exported(split, Ticks{}, 50000) == a);
    CHECK(std::all_of(a.begin() + 2 * 48000, a.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Speed audio: remap ramps and reverse sound alike in preview and export also after a seek",
          "[playback][track][speed][remap]") {
    for (const bool keepPitch : {false, true}) {
        INFO((keepPitch ? "maintain pitch" : "varispeed"));
        Project p;
        p.media('A', sineAt(523.25, 0.4f));
        p.addTrack({{'A', frames(0), frames(96000), frames(0)}});
        // 100 % for 0.25 s, an eased slow-mo ramp (0.5x on average), 100 %, a hold, a 100 % reverse, a straight 3x,
        // then 100 % again.
        changeClip(p, [&](Clip& c) {
            remap(c, {key(0, 48000), key(12000, 60000, Interpolation::EaseOut), key(36000, 72000, Interpolation::EaseIn),
                      key(48000, 84000), key(54000, 84000), key(66000, 72000), key(78000, 108000), key(96000, 126000)});
            c.maintainPitch = keepPitch;
        });
        const auto expected = exported(p, Ticks{}, 100000);
        // Preview: the player's feeder renders in its own blocks from 0.
        Rig rig(p);
        rig.player->play();
        rig.pullFor(2090);
        REQUIRE(rig.stream.size() >= expected.size());
        const auto [mine, theirs] = std::mismatch(expected.begin(), expected.end(), rig.stream.begin());
        if (mine != expected.end()) {
            UNSCOPED_INFO("first difference at sample " << (mine - expected.begin()) << ": export " << *mine << ", preview " << *theirs);
        }
        CHECK(mine == expected.end());
        // A start inside the slow-mo, inside the hold's edge, inside the reverse and inside the 3x span.
        for (const std::int64_t at : {std::int64_t{20000}, std::int64_t{48010}, std::int64_t{60001}, std::int64_t{70500}}) {
            INFO("from frame " << at);
            const auto seek = exported(p, frames(at), 6000);
            CHECK(std::equal(seek.begin(), seek.end(), expected.begin() + 2 * at));
        }
        // It sounds through the ramp and the reverse; the hold is silent.
        CHECK(meanAbs(expected, 20000, 28000) > 0.1);  // the middle of the ramp
        CHECK(meanAbs(expected, 56000, 64000) > 0.2);  // the reverse
        CHECK(meanAbs(expected, 68000, 76000) > 0.2);  // 3x
        CHECK(meanAbs(expected, 49000, 53000) == 0.0); // the hold
    }
}

TEST_CASE("Speed audio: holds are silent and their edges fade like cuts", "[playback][track][speed][fade]") {
    for (const bool keepPitch : {false, true}) {
        INFO((keepPitch ? "maintain pitch" : "varispeed"));
        Project p;
        p.media('A', level(0.5f));
        p.addTrack({{'A', frames(0), frames(3000), frames(0)}});
        // 1x for 1000 frames, a hold for 1000, then 50 % for 1000.
        changeClip(p, [&](Clip& c) {
            remap(c, {key(0, 0), key(1000, 1000), key(2000, 1000), key(3000, 1500)});
            c.maintainPitch = keepPitch;
        });
        const auto out = exported(p, Ticks{}, 3200, ms(2));
        CHECK(maxStep(out) < 0.5 / 96.0 * 1.5);
        CHECK(out[500 * 2] == 0.5f);
        CHECK(out[1500 * 2] == 0.0f);
        CHECK(out[999 * 2] < 0.01f);
        CHECK(out[2000 * 2] < 0.01f);
        CHECK(out[2500 * 2] == Approx(0.5f).margin(1e-4)); // the slowed constant is still 0.5
        CHECK(out[3100 * 2] == 0.0f);
    }
}

TEST_CASE("Speed audio: a crossfade across a ramp does not click", "[playback][track][speed][transition]") {
    for (const bool keepPitch : {false, true}) {
        INFO((keepPitch ? "maintain pitch" : "varispeed"));
        Project p;
        p.media('A', sineAt(440.0));
        p.media('B', sineAt(660.0));
        p.addTrack({{'A', frames(0), frames(48000), frames(0)}, {'B', frames(48000), frames(48000), frames(10000)}});
        // A slows from 100 % to about 50 % over its last half second, the curve going on past its end (the transition's
        // tail handle reads it there).
        changeClip(p, [&](Clip& c) {
            remap(c, {key(0, 0), key(24000, 24000, Interpolation::EaseOut), key(72000, 48000)});
            c.maintainPitch = keepPitch;
        });
        Track track = p.track(0);
        Transition t;
        t.id = p.state.ids.allocate<TransitionId>();
        t.outgoing = track.clips[0].id;
        t.incoming = track.clips[1].id;
        t.before = frames(4800);
        t.after = frames(4800);
        t.effect = "constantPower";
        track.transitions.push_back(t);
        p.state = Project::withTrack(p.state, 0, std::move(track));
        const auto out = exported(p, Ticks{}, 96000, ms(2));
        // Derivative peak detector: no sample steps further than the two sines' own slopes allow.
        const double slope = 2.0 * std::numbers::pi * (440.0 + 660.0) * 0.5 / 48000.0;
        CHECK(maxStep(out) < slope * 1.2);
        CHECK(meanAbs(out, 43200, 52800) > 0.15); // both sides sound through the crossfade
    }
}

TEST_CASE("Speed audio: clicks land within a millisecond of their remapped time", "[playback][track][speed]") {
    // A sawtooth that jumps down every 4800 source frames: each jump is a click whose position is exact.
    SyntheticSignal saw;
    saw.kind = SyntheticSignal::Kind::Ramp;
    saw.frequency = 10.0;
    saw.amplitude = 0.4f;
    Project p;
    p.media('A', saw);
    p.addTrack({{'A', frames(0), frames(144000), frames(0)}});
    const std::vector<Keyframe> keys{key(0, 1000), key(48000, 49000, Interpolation::EaseOut), key(120000, 85000),
                                     key(144000, 73000)};
    changeClip(p, [&](Clip& c) { remap(c, keys); });
    const auto out = exported(p, Ticks{}, 144000);
    const Curve curve{keys};
    int checked = 0;
    double worst = 0.0;
    for (std::int64_t f = 1000; f < 143000; ++f) {
        // Where the position (as the renderer reads it: the frame before it in reverse) passes a jump.
        const auto positionAt = [&](std::int64_t frame) {
            const RemapSegment s = remapSegmentAt(curve, frames(frame));
            const double q = static_cast<double>(remapPositionAt(curve, frames(frame)).value) / kTicksPerFrame;
            return s.motion == RemapMotion::Reverse ? q - 1.0 : q;
        };
        const double q0 = positionAt(f);
        const double q1 = positionAt(f + 1);
        const double lo = std::min(q0, q1);
        const double hi = std::max(q0, q1);
        const double jump = std::ceil(lo / 4800.0) * 4800.0;
        if (jump <= lo || jump > hi || std::fabs(q1 - q0) < 0.2) {
            continue;
        }
        const double expected = static_cast<double>(f) + (jump - q0) / (q1 - q0);
        std::int64_t steepest = f;
        for (std::int64_t g = f - 96; g <= f + 96; ++g) {
            const auto step = [&](std::int64_t k) { return std::fabs(out[2 * k] - out[2 * (k - 1)]); };
            if (step(g) > step(steepest)) {
                steepest = g;
            }
        }
        worst = std::max(worst, std::fabs(static_cast<double>(steepest) - expected));
        ++checked;
    }
    std::printf("speed audio: varispeed clicks within %.3f ms of their remapped time (%d clicks)\n", worst / 48.0, checked);
    CHECK(checked >= 12);
    CHECK(worst <= 48.0);
}
