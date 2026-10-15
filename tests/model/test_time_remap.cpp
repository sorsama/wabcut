// Time remapping (C.6): remap curves (exact frames at keys, monotonic segments, holds, reverse, ramps), turning
// it on and off, the remap operations, trims and razor cuts on remapped clips, audio ranges, validation and files.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <limits>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

/// Source frames the clip shows at timeline frames [from, to).
std::vector<std::int64_t> shown(const Clip& c, std::int64_t from, std::int64_t to) {
    std::vector<std::int64_t> out;
    for (std::int64_t k = from; k < to; ++k) {
        out.push_back(ticksToFrames(c.sourceTimeAt(f(k)), kRate));
    }
    return out;
}

std::vector<std::int64_t> range(std::int64_t from, std::int64_t to, std::int64_t step = 1) {
    std::vector<std::int64_t> out;
    for (std::int64_t v = from; step > 0 ? v < to : v > to; v += step) {
        out.push_back(v);
    }
    return out;
}

std::vector<std::int64_t> repeat(std::int64_t value, std::size_t count) { return std::vector<std::int64_t>(count, value); }

std::vector<std::int64_t> join(std::initializer_list<std::vector<std::int64_t>> parts) {
    std::vector<std::int64_t> out;
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

/// A remap key at curve frame `time` holding source frame `source`.
Keyframe rk(std::int64_t time, std::int64_t source, Interpolation interpolation = Interpolation::Linear) {
    return Keyframe{f(time), static_cast<double>(f(source).value), interpolation};
}

std::vector<std::pair<std::int64_t, std::int64_t>> keysOf(const Clip& c) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    for (const Keyframe& k : c.timeRemap()->keys) {
        out.emplace_back(ticksToFrames(k.time, kRate), ticksToFrames(Ticks{static_cast<std::int64_t>(k.value)}, kRate));
    }
    return out;
}

std::string errorOf(const edit::EditOutcome& outcome) { return outcome ? std::string() : outcome.error().message; }

/// Every tick time in [from, to) sampled `samples` times, and the segment ends: the shown source never turns back.
bool monotonic(const Clip& c, Ticks from, Ticks to, int direction, int samples = 500) {
    Ticks previous = c.sourceTimeAt(from);
    for (int i = 1; i <= samples; ++i) {
        const Ticks t = from + Ticks{(to - from).value * i / samples} - Ticks{i == samples ? 1 : 0};
        const Ticks now = c.sourceTimeAt(t);
        if ((direction > 0 && now < previous) || (direction < 0 && now > previous) ||
            (direction == 0 && now != previous)) {
            return false;
        }
        previous = now;
    }
    return true;
}

} // namespace

TEST_CASE("Time remapping on keeps the picture; off restores the clip", "[model][remap]") {
    Timeline tl(R"(
        V1 tgt |A10aaaaaaaB20bbbbbbbbbb|
        A1 tgt |A10aaaaaaa.............|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const auto roundTrip = [&](const ProjectState& before) {
        const Clip original = clipIn(before, tl.seq, "V1@0");
        const auto on = ok(edit::setTimeRemapEnabled(before, tl.seq, original.id, true));
        const Clip& remapped = clipIn(on.state, tl.seq, "V1@0");
        CHECK(remapped.isRemapped());
        CHECK(remapped.speed == Speed{1, 1});
        CHECK_FALSE(remapped.reversed);
        CHECK(shown(remapped, 0, ticksToFrames(original.duration(), kRate)) ==
              shown(original, 0, ticksToFrames(original.duration(), kRate)));
        REQUIRE(on.changes.sequences.size() == 1);
        const auto off = ok(edit::setTimeRemapEnabled(on.state, tl.seq, original.id, false));
        CHECK(clipIn(off.state, tl.seq, "V1@0") == original);
        return keysOf(remapped);
    };

    SECTION("100 %: the identity over the whole media") {
        CHECK(roundTrip(tl.state) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}, {1000, 1000}});
        const auto on = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, a, true));
        CHECK(clipIn(on.state, tl.seq, "V1@0").curveTimeAt(f(3)) == f(13)); // keyframes stay where they were
        CHECK_TIMELINE(on.state, tl.seq, R"(
            V1 |A10aaaaaaaB20bbbbbbbbbb|
            A1 |A10aaaaaaa.............|
        )");
        CHECK(edit::setTimeRemapEnabled(on.state, tl.seq, a, true)->changes.empty()); // already on
    }
    SECTION("200 %: one segment at that speed over the media, the same speed back") {
        edit::SpeedChange twice;
        twice.speed = Speed{2, 1};
        const auto fast = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, twice, edit::LinkMode::IgnoreLinks));
        CHECK(shown(clipIn(fast.state, tl.seq, "V1@0"), 0, 5) == range(10, 20, 2));
        CHECK(roundTrip(fast.state) == std::vector<std::pair<std::int64_t, std::int64_t>>{{5, 0}, {505, 1000}}); // the media's ends
    }
    SECTION("reversed: a reverse segment, reversed again") {
        edit::SpeedChange back;
        back.speed = Speed{1, 1};
        back.reversed = true;
        const auto reversed = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, back, edit::LinkMode::IgnoreLinks));
        CHECK(shown(clipIn(reversed.state, tl.seq, "V1@0"), 0, 10) == range(19, 9, -1));
        CHECK(roundTrip(reversed.state) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 30}, {30, 0}});
    }
    SECTION("a freeze frame: one key, a freeze frame again") {
        const auto held = ok(edit::frameHold(tl.state, tl.seq, a, f(0)));
        CHECK(roundTrip(held.state) == std::vector<std::pair<std::int64_t, std::int64_t>>{{10, 10}});
    }
}

TEST_CASE("Remap segments are exact at keys and monotonic: forward, hold, reverse", "[model][remap]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    // 0..10: 0 -> 20 at 200 %; 10..20: hold 20; 20..30: back to 10 at 100 %; 30..40: on to 20 at 100 %.
    const Curve remap{{rk(0, 0), rk(10, 20), rk(20, 20), rk(30, 10), rk(40, 20)}};
    const auto r = ok(edit::setTimeRemap(tl.state, tl.seq, a, remap, Ticks{}));
    const Clip& c = clipIn(r.state, tl.seq, "V1@0");
    CHECK(shown(c, 0, 42) == join({range(0, 20, 2), repeat(20, 10), range(19, 9, -1), range(10, 20), repeat(20, 2)}));

    // At a key: its position exactly (forward, hold), or the frame that ends there (reverse).
    CHECK(c.sourceTimeAt(f(0)) == f(0));
    CHECK(c.sourceTimeAt(f(10)) == f(20));
    CHECK(c.sourceTimeAt(f(20)) == f(20) - Ticks{1});
    CHECK(c.sourceTimeAt(f(30)) == f(10));
    CHECK(c.sourceTimeAt(f(40)) == f(20)); // after the last key it holds

    // Between keys, tick by tick, never turning back; a 200 % segment is exact rational time.
    CHECK(monotonic(c, f(0), f(10), +1));
    CHECK(monotonic(c, f(10), f(20), 0));
    CHECK(monotonic(c, f(20), f(30), -1));
    CHECK(monotonic(c, f(30), f(40), +1));
    CHECK(c.sourceTimeAt(f(3) + Ticks{7}) == f(6) + Ticks{14});
    const RemapSegment s = remapSegmentAt(remap, f(25));
    CHECK(s.motion == RemapMotion::Reverse);
    CHECK(s.straight);
    CHECK(remapShownRange(remap, f(0), f(40)) == std::pair{f(0), f(20)});

    // Effects keep animating at timeline rate through holds and reverse.
    CHECK(c.curveTimeAt(f(15)) == f(15));
    CHECK(c.timelineTimeOfCurveTime(f(25)) == f(25));
}

TEST_CASE("Speed ramps are monotonic and exact at their keys", "[model][remap]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    // Ease from 0 to 40 frames over 20 (a smooth 0 -> 400 % -> 0 ramp), then reverse eased back to 10.
    const Curve remap{{rk(0, 0, Interpolation::EaseInOut), rk(20, 40, Interpolation::EaseInOut),
                       rk(30, 10, Interpolation::EaseInOut)}};
    const auto r = ok(edit::setTimeRemap(tl.state, tl.seq, tl.clip("V1@0"), remap, Ticks{}));
    const Clip& c = clipIn(r.state, tl.seq, "V1@0");
    CHECK(c.sourceTimeAt(f(0)) == f(0));
    CHECK(c.sourceTimeAt(f(20)) == f(40) - Ticks{1});
    CHECK(std::llabs((c.sourceTimeAt(f(10)) - f(20)).value) <= 1); // the ease's midpoint
    CHECK(monotonic(c, f(0), f(20), +1, 4000));
    CHECK(monotonic(c, f(20), f(30), -1, 4000));
    CHECK(c.sourceTimeAt(f(20) - Ticks{1}) < f(40)); // never reaches the next key early
    CHECK_FALSE(remapSegmentAt(remap, f(5)).straight);
    // Bezier handles that would turn the ramp back are refused.
    Curve bad = remap;
    bad.keys[0].interpolation = Interpolation::Bezier;
    bad.keys[0].out = BezierHandle{-100.0, 0.5};
    CHECK(remapProblem(bad).has_value());
    CHECK(errorOf(edit::setTimeRemap(tl.state, tl.seq, tl.clip("V1@0"), bad, Ticks{})).find("turn it back") !=
          std::string::npos);
}

TEST_CASE("Remap operations: keys, speeds, holds and reverse segments", "[model][remap]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    const ProjectState on = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, a, true)).state; // keys (0,0) (1000,1000)
    const auto clip = [&](const ProjectState& s) -> const Clip& { return clipIn(s, tl.seq, "V1@0"); };

    SECTION("a new key keeps the picture") {
        const auto r = ok(edit::addTimeRemapKey(on, tl.seq, a, f(20)));
        CHECK(keysOf(clip(r.state)) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}, {20, 20}, {1000, 1000}});
        CHECK(shown(clip(r.state), 0, 40) == range(0, 40));
        CHECK(edit::addTimeRemapKey(r.state, tl.seq, a, f(20))->changes.empty());
    }
    SECTION("a new key in a hold preserves the step to the next key") {
        const auto held = ok(edit::setTimeRemap(tl.state, tl.seq, a,
                                               Curve{{rk(0, 5, Interpolation::Hold), rk(20, 30)}}, Ticks{})).state;
        const auto r = ok(edit::addTimeRemapKey(held, tl.seq, a, f(10)));
        CHECK(shown(clip(r.state), 0, 25) == shown(clip(held), 0, 25));
        CHECK(clip(r.state).timeRemap()->keys[1].interpolation == Interpolation::Hold);
    }
    SECTION("moving a key between its neighbours changes both speeds") {
        const ProjectState keyed = ok(edit::addTimeRemapKey(on, tl.seq, a, f(20))).state;
        const auto r = ok(edit::moveTimeRemapKey(keyed, tl.seq, a, 1, f(10)));
        CHECK(shown(clip(r.state), 0, 12) == join({range(0, 20, 2), {20, 20}})); // 200 %, then 98/99 %
        CHECK(errorOf(edit::moveTimeRemapKey(keyed, tl.seq, a, 1, f(0))).find("before it") != std::string::npos);
        CHECK(errorOf(edit::moveTimeRemapKey(keyed, tl.seq, a, 1, f(1000))).find("after it") != std::string::npos);
    }
    SECTION("ripple: later keys move with it, so only the segment before it changes speed") {
        ProjectState keyed = ok(edit::addTimeRemapKey(on, tl.seq, a, f(20))).state;
        keyed = ok(edit::addTimeRemapKey(keyed, tl.seq, a, f(30))).state;
        const auto r = ok(edit::moveTimeRemapKey(keyed, tl.seq, a, 1, f(10), true));
        CHECK(keysOf(clip(r.state)) ==
              std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}, {10, 20}, {20, 30}, {990, 1000}});
        CHECK(shown(clip(r.state), 0, 25) == join({range(0, 20, 2), range(20, 35)}));
    }
    SECTION("a segment's speed moves its end key and the keys after it") {
        const ProjectState keyed = ok(edit::addTimeRemapKey(on, tl.seq, a, f(10))).state;
        const auto r = ok(edit::setTimeRemapSpeed(keyed, tl.seq, a, 0, Speed{1, 2}));
        CHECK(keysOf(clip(r.state)) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}, {20, 10}, {1010, 1000}});
        CHECK(shown(clip(r.state), 18, 24) == std::vector<std::int64_t>{9, 9, 10, 11, 12, 13});
        CHECK(errorOf(edit::setTimeRemapSpeed(keyed, tl.seq, a, 5, Speed{1, 2})).find("no remap segment") !=
              std::string::npos);
    }
    SECTION("hold interpolation steps; a key's position can change") {
        const ProjectState keyed = ok(edit::addTimeRemapKey(on, tl.seq, a, f(10))).state;
        edit::TimeRemapKeyChange hold;
        hold.interpolation = Interpolation::Hold;
        hold.source = f(5);
        const auto r = ok(edit::setTimeRemapKey(keyed, tl.seq, a, 1, hold));
        CHECK(shown(clip(r.state), 8, 14) == std::vector<std::int64_t>{4, 4, 5, 5, 5, 5}); // 50 %, then the step
    }
    SECTION("removing keys; the last one stays") {
        const auto r = ok(edit::removeTimeRemapKey(on, tl.seq, a, 1));
        CHECK(shown(clip(r.state), 0, 3) == std::vector<std::int64_t>{0, 0, 0}); // one key: a hold
        CHECK(errorOf(edit::removeTimeRemapKey(r.state, tl.seq, a, 0)).find("at least one key") != std::string::npos);
    }
    SECTION("a hold segment holds the frame shown there and plays the rest later") {
        const auto r = ok(edit::addTimeRemapHold(on, tl.seq, a, f(10), f(5)));
        CHECK(shown(clip(r.state), 0, 20) == join({range(0, 10), repeat(10, 5), range(10, 15)}));
        CHECK(clip(r.state).duration() == f(40)); // the clip keeps its length
    }
    SECTION("a reverse segment plays back and forwards again") {
        const auto r = ok(edit::addTimeRemapReverse(on, tl.seq, a, f(20), f(5)));
        CHECK(shown(clip(r.state), 18, 32) == join({{18, 19}, range(19, 14, -1), range(15, 20), {20, 21}}));
        CHECK(keysOf(clip(r.state)) ==
              std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}, {20, 20}, {25, 15}, {30, 20}, {1010, 1000}});
        CHECK(errorOf(edit::addTimeRemapReverse(on, tl.seq, a, f(2), f(5))).find("as much media before") !=
              std::string::npos);
        CHECK(errorOf(edit::addTimeRemapReverse(r.state, tl.seq, a, f(22), f(2))).find("already plays in reverse") !=
              std::string::npos);
    }
    SECTION("only remapped clips") {
        CHECK(errorOf(edit::addTimeRemapKey(tl.state, tl.seq, a, f(3))).find("time remapping is off") !=
              std::string::npos);
    }
}

TEST_CASE("Remapped clips: razor cuts share the remap, trims and holds stay inside the media", "[model][remap]") {
    // Media A has 50 frames; the clip plays 10..50, so its tail is at the end of the media.
    Timeline tl(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa....|
    )",
                {{'A', 50}});
    const ClipId a = tl.clip("V1@0");
    const ProjectState on = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, a, true)).state; // (0,0) (50,50)
    const ProjectState slow = ok(edit::setTimeRemapSpeed(on, tl.seq, a, 0, Speed{1, 2})).state; // (0,0) (100,50)

    SECTION("razor: both halves share the curve and continue seamlessly") {
        const auto cut = ok(edit::addEdit(slow, tl.seq, f(15), std::vector<TrackId>{tl.track("V1")}));
        const Clip& left = clipIn(cut.state, tl.seq, "V1@0");
        const Clip& right = clipIn(cut.state, tl.seq, "V1@15");
        CHECK(left.sharesContentWith(right));
        CHECK(join({shown(left, 0, 15), shown(right, 15, 30)}) == shown(clipIn(slow, tl.seq, "V1@0"), 0, 30));
    }
    SECTION("the tail stops before holding past the media's end") {
        // At 100 % from 10 the clip ends at media frame 50: no further.
        CHECK(clipIn(on, tl.seq, "V1@0").tailRoom(f(50)) == Ticks{});
        CHECK(ok(edit::trimEdge(on, tl.seq, a, edit::Edge::Tail, f(4))).changes.empty());
        // At 50 % it shows 5..25 over its 40 frames, and can grow to the end of the curve.
        CHECK(clipIn(slow, tl.seq, "V1@0").tailRoom(f(50)) == f(50));
        const auto longer = ok(edit::trimEdge(slow, tl.seq, a, edit::Edge::Tail, f(4)));
        CHECK(shown(clipIn(longer.state, tl.seq, "V1@0"), 42, 44) == std::vector<std::int64_t>{26, 26});
        // A key past the end of the media is refused.
        edit::TimeRemapKeyChange past;
        past.source = f(60);
        CHECK(errorOf(edit::setTimeRemapKey(on, tl.seq, a, 1, past)).find("past the end") != std::string::npos);
    }
    SECTION("speed changes and frame holds are refused; slip moves along the curve") {
        edit::SpeedChange twice;
        twice.speed = Speed{2, 1};
        CHECK(errorOf(edit::setSpeed(slow, tl.seq, std::vector<ClipId>{a}, twice)).find("time-remapped") !=
              std::string::npos);
        CHECK(errorOf(edit::frameHold(slow, tl.seq, a, f(3))).find("time-remapped") != std::string::npos);
        const auto slipped = ok(edit::slip(slow, tl.seq, std::vector<ClipId>{a}, f(-4)));
        CHECK(clipIn(slipped.state, tl.seq, "V1@0").sourceIn == f(6));
        CHECK(slipped.appliedDelta == f(-4));
    }
}

TEST_CASE("Remapped audio is heard only at 100 % forwards", "[model][remap]") {
    Timeline tl(R"(
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("A1@0");
    CHECK(clipIn(tl.state, tl.seq, "A1@0").unitySpeedRanges() == std::vector<TimeRange>{TimeRange{f(0), f(40)}});
    // 100 % to 10, 200 % to 20, hold to 25, 100 % (continuing) to the end.
    const Curve remap{{rk(0, 0), rk(10, 10), rk(20, 30), rk(25, 30), rk(100, 105)}};
    const auto r = ok(edit::setTimeRemap(tl.state, tl.seq, a, remap, Ticks{}));
    CHECK(clipIn(r.state, tl.seq, "A1@0").unitySpeedRanges() ==
          std::vector<TimeRange>{TimeRange{f(0), f(10)}, TimeRange::fromStartEnd(f(25), f(40))});
    edit::SpeedChange twice;
    twice.speed = Speed{2, 1};
    const auto fast = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, twice));
    CHECK(clipIn(fast.state, tl.seq, "A1@0").unitySpeedRanges().empty());
}

TEST_CASE("Remap edits refuse overflowing times and extreme segment indices", "[model][remap]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    const auto on = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, a, true)).state;
    CHECK_FALSE(edit::setTimeRemapSpeed(on, tl.seq, a, std::numeric_limits<std::size_t>::max(), Speed{1, 1}));
    CHECK_FALSE(edit::moveTimeRemapKey(on, tl.seq, a, 0, Ticks::max(), true));
    CHECK_FALSE(edit::moveTimeRemapKey(on, tl.seq, a, 1, Ticks::min(), true));
    CHECK_FALSE(edit::addTimeRemapHold(on, tl.seq, a, f(1), Ticks::max()));
    // The inserted hold fits, but the later key would wrap when spliced.
    CHECK_FALSE(edit::addTimeRemapHold(on, tl.seq, a, Ticks{}, Ticks::max() - f(1)));
    CHECK_FALSE(edit::addTimeRemapReverse(on, tl.seq, a, Ticks::max() - Ticks{1}, f(1)));
    CHECK_FALSE(edit::setTimeRemap(on, tl.seq, a, Curve{{rk(0, 0)}}, Ticks::max()));

    const Curve tooLong{{Keyframe{Ticks::min(), 0.0}, Keyframe{Ticks::max(), 1.0}}};
    CHECK(remapProblem(tooLong).has_value());
    CHECK_FALSE(edit::setTimeRemap(on, tl.seq, a, tooLong, Ticks{}));

    // A huge source span slowed down must fail, not fall back to a one-tick segment.
    Curve longSource{{Keyframe{Ticks{}, 0.0}, Keyframe{Ticks{kMaxRemapSource}, static_cast<double>(kMaxRemapSource)}}};
    CHECK_FALSE(remapProblem(longSource));
    ProjectState largeMedia = tl.state;
    auto media = std::make_shared<MediaItem>(*largeMedia.findMedia(clipIn(tl.state, tl.seq, "V1@0").media));
    media->info.duration = Ticks{kMaxRemapSource};
    media->info.video[0].duration = Ticks{kMaxRemapSource};
    largeMedia.media[0] = media;
    const auto mapped = ok(edit::setTimeRemap(largeMedia, tl.seq, a, longSource, Ticks{})).state;
    CHECK_FALSE(edit::setTimeRemapSpeed(mapped, tl.seq, a, 0, Speed{1, std::numeric_limits<std::int32_t>::max()}));
}

TEST_CASE("Remapped clips are validated, saved and diffed", "[model][remap][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )",
                {{'A', 30}});
    const ClipId a = tl.clip("V1@0");
    const auto r = ok(edit::setTimeRemap(tl.state, tl.seq, a, Curve{{rk(0, 0), rk(10, 20, Interpolation::EaseIn)}},
                                         f(2)));
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].modifiedClips == std::vector<ClipId>{a});
    CHECK(r.changes.sequences[0].roles.has(PropertyRole::Source));

    // Saved and loaded as it was.
    const auto json = io::toJson(r.state);
    REQUIRE(json);
    CHECK(json->find("\"timeRemap\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == r.state);
    CHECK(clipIn(*loaded, tl.seq, "V1@0").timeRemap()->keys[1].interpolation == Interpolation::EaseIn);

    // Keys past the media, a curve with no keys, and a remapped clip with a speed are invalid.
    CHECK(errorOf(edit::setTimeRemap(tl.state, tl.seq, a, Curve{{rk(0, 0), rk(10, 40)}}, Ticks{}))
              .find("past the end of the media") != std::string::npos);
    CHECK_FALSE(edit::setTimeRemap(tl.state, tl.seq, a, Curve{}, Ticks{}));
    CHECK_FALSE(edit::setTimeRemap(tl.state, tl.seq, a, Curve{{rk(0, 0)}}, f(-1)));
    ProjectState broken = r.state;
    {
        auto seq = std::make_shared<Sequence>(*broken.findSequence(tl.seq));
        auto v1 = std::make_shared<Track>(*seq->videoTracks[0]);
        v1->clips.mutableAt(0).speed = Speed{2, 1};
        seq->videoTracks[0] = v1;
        broken = broken.withSequence(seq);
    }
    const auto valid = validate(broken, Validation::Structure);
    REQUIRE_FALSE(valid);
    CHECK(valid.error().message.find("not at speed 100 %") != std::string::npos);
}
