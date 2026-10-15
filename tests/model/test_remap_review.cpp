// Regression tests for the review-wave-4 findings on time remapping (docs/internal/reviews-wave4.md): each failed before its
// fix.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

std::vector<std::int64_t> shown(const Clip& c, std::int64_t from, std::int64_t to) {
    std::vector<std::int64_t> out;
    for (std::int64_t k = from; k < to; ++k) {
        out.push_back(ticksToFrames(c.sourceTimeAt(f(k)), kRate));
    }
    return out;
}

std::vector<std::int64_t> repeat(std::int64_t value, std::size_t count) { return std::vector<std::int64_t>(count, value); }

Keyframe rk(std::int64_t time, std::int64_t source, Interpolation interpolation = Interpolation::Linear) {
    return Keyframe{f(time), static_cast<double>(f(source).value), interpolation};
}

} // namespace

// <P1-6>
TEST_CASE("Review P1-6: a hold or reverse inserted inside a Hold step keeps the step", "[model][remap][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    // Holds frame 5 until 20, then steps to 30.
    const ProjectState stepped =
        ok(edit::setTimeRemap(tl.state, tl.seq, a, Curve{{rk(0, 5, Interpolation::Hold), rk(20, 30)}}, Ticks{})).state;
    const auto held = ok(edit::addTimeRemapHold(stepped, tl.seq, a, f(10), f(5)));
    std::vector<std::int64_t> expected = repeat(5, 10);
    expected.insert(expected.end(), {30, 30});
    CHECK(shown(clipIn(held.state, tl.seq, "V1@0"), 15, 27) == expected); // still the step, 5 frames later
    const auto reversed = ok(edit::addTimeRemapReverse(stepped, tl.seq, a, f(10), f(3)));
    expected = repeat(5, 10);
    expected.insert(expected.end(), {30, 30});
    CHECK(shown(clipIn(reversed.state, tl.seq, "V1@0"), 16, 28) == expected);
}
// </P1-6>

// <P2-18>
TEST_CASE("Review P2-18: a Hold step has no speed to set", "[model][remap][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    const ProjectState stepped =
        ok(edit::setTimeRemap(tl.state, tl.seq, a, Curve{{rk(0, 5, Interpolation::Hold), rk(20, 30)}}, Ticks{})).state;
    const auto r = edit::setTimeRemapSpeed(stepped, tl.seq, a, 0, Speed{1, 2});
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("holds a frame") != std::string::npos);
}
// </P2-18>

// <P2-19>
TEST_CASE("Review P2-19: tail room behind a leading hold does not overflow", "[model][remap][review]") {
    // An invalid state (the media got shorter after a relink): the clip shows the hold before the first key, at the
    // media's end. The tail cannot grow at all.
    Clip c;
    c.timelineRange = {f(0), f(10)};
    ClipContent content;
    content.timeRemap = Curve{{rk(100, 50)}};
    c.setContent(std::move(content));
    CHECK(c.tailRoom(f(50)) == Ticks{});
    CHECK(c.headRoom(f(50)) == Ticks{});
}
// </P2-19>

// <P2-20>
TEST_CASE("Review P2-20: turning remapping on keeps a retimed clip's frames beyond its ends", "[model][remap][review]") {
    Timeline tl(R"(
        V1 tgt |A10aaaaaaa..........|
    )");
    const ClipId a = tl.clip("V1@0");
    const auto check = [&](const ProjectState& before) {
        const Clip original = clipIn(before, tl.seq, "V1@0");
        const auto on = ok(edit::setTimeRemapEnabled(before, tl.seq, a, true));
        const Clip& remapped = clipIn(on.state, tl.seq, "V1@0");
        // Transition handles and trims read past the clip's ends: the same frames as the constant speed there.
        for (const std::int64_t k : {-3, -1, 0, 4, 5, 7, 9}) {
            const Ticks t = original.start() + f(k);
            INFO(k);
            CHECK(remapped.sourceTimeAt(t) == original.sourceTimeAt(t));
        }
    };
    SECTION("200 %") {
        edit::SpeedChange twice;
        twice.speed = Speed{2, 1};
        check(ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, twice)).state);
    }
    SECTION("50 %") {
        edit::SpeedChange half;
        half.speed = Speed{1, 2};
        check(ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, half)).state);
    }
    SECTION("reversed") {
        edit::SpeedChange back;
        back.speed = Speed{1, 1};
        back.reversed = true;
        check(ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, back)).state);
    }
}
// </P2-20>

// <P2-26>
TEST_CASE("Review P2-26: a remap holds at most kMaxRemapKeys keys", "[model][remap][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    Curve many;
    for (std::int64_t i = 0; i <= static_cast<std::int64_t>(kMaxRemapKeys); ++i) {
        many.keys.push_back(Keyframe{Ticks{i}, static_cast<double>(i)});
    }
    CHECK(remapProblem(many).has_value());
    CHECK_FALSE(edit::setTimeRemap(tl.state, tl.seq, tl.clip("V1@0"), many, Ticks{}));
    many.keys.pop_back();
    CHECK_FALSE(remapProblem(many).has_value());
}
// </P2-26>

// <P1-5>
TEST_CASE("Review P1-5: linked partners with streams of different lengths get the same remap", "[model][remap][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    tl.linkMatching();
    // Real files: the audio stream ends before the video stream.
    {
        auto media = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media('A')));
        media->info.audio[0].duration = f(900);
        for (MediaPtr& m : tl.state.media) {
            if (m->id == media->id) {
                m = media;
            }
        }
    }
    ProjectState s = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, tl.clip("V1@0"), true)).state;
    s = ok(edit::setTimeRemapEnabled(s, tl.seq, tl.clip("A1@0"), true)).state;
    CHECK(*clipIn(s, tl.seq, "V1@0").timeRemap() == *clipIn(s, tl.seq, "A1@0").timeRemap());
}
// </P1-5>
// <P1-7>
TEST_CASE("Review P1-7: the media a remapped clip shows and where it shows a media time", "[model][remap][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    ProjectState s = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, a, true)).state;
    // A boomerang at 8: 0..8 forwards, 8 -> 5 backwards, 5 -> 8 forwards, then on (the clip keeps its 20 frames).
    s = ok(edit::addTimeRemapReverse(s, tl.seq, a, f(8), f(3))).state;
    const Clip& c = clipIn(s, tl.seq, "V1@0");
    CHECK(c.shownSourceRange(f(9), f(12)) == TimeRange::fromStartEnd(f(5), f(7))); // reverse shows the frame before
    CHECK(c.shownSourceRange(c.start(), c.end()) == TimeRange::fromStartEnd(Ticks{}, f(14)));
    CHECK(c.timelineTimeOfSourceTime(f(6)) == f(6));   // the first time it shows it
    CHECK(c.timelineTimeOfSourceTime(f(12)) == f(18));
    CHECK(c.timelineTimeOfSourceTime(f(15)) == std::nullopt);
    // Not remapped: the clip's own window (the old behaviour).
    const Clip& plain = clipIn(tl.state, tl.seq, "V1@0");
    CHECK(plain.shownSourceRange(f(2), f(5)) == TimeRange::fromStartEnd(f(2), f(5)));
    CHECK(plain.timelineTimeOfSourceTime(f(7)) == f(7));
    CHECK(plain.timelineTimeOfSourceTime(f(20)) == std::nullopt);
}
// </P1-7>

