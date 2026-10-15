// Explicit sequence duration and work area: trailing gaps survive ripples until asked to go.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Ticks durationOf(const ProjectState& s, SequenceId seq) { return s.findSequence(seq)->duration; }

} // namespace

TEST_CASE("Edits that extend the content grow the sequence", "[model][extent]") {
    Timeline tl(R"(
        V1 tgt |Aa..|
    )");
    CHECK(durationOf(tl.state, tl.seq) == f(2)); // the fixture starts at its content end
    const edit::SourceClip source{tl.media('S'), {f(0), f(3)}, {{{StreamKind::Video, 0}, tl.track("V1")}}};
    const auto r = ok(edit::overwriteEdit(tl.state, tl.seq, source, f(4)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Aa..Sss|
    )");
    CHECK(durationOf(r.state, tl.seq) == f(7));
}

TEST_CASE("Ripple removals leave a trailing gap instead of shrinking the sequence", "[model][extent]") {
    Timeline tl(R"(
        V1 tgt |AaBbCc|
    )");
    const auto r = ok(edit::rippleDeleteClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |BbCc..|
    )");
    CHECK(r.state.findSequence(tl.seq)->contentEnd() == f(4));
    CHECK(durationOf(r.state, tl.seq) == f(6));

    // Asked to, the sequence drops its trailing gap.
    const auto fitted = ok(edit::fitSequenceDurationToContent(r.state, tl.seq));
    CHECK(durationOf(fitted.state, tl.seq) == f(4));
    const auto again = ok(edit::fitSequenceDurationToContent(fitted.state, tl.seq));
    CHECK(isSameRevision(again.state, fitted.state));
}

TEST_CASE("The duration can be set, but never shorter than the content", "[model][extent]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    CHECK(durationOf(ok(edit::setSequenceDuration(tl.state, tl.seq, f(20))).state, tl.seq) == f(20));
    const auto tooShort = edit::setSequenceDuration(tl.state, tl.seq, f(3));
    REQUIRE_FALSE(tooShort);
    CHECK(tooShort.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("The work area lies inside the sequence", "[model][extent]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa|
    )");
    const auto set = ok(edit::setWorkArea(tl.state, tl.seq, TimeRange{f(2), f(5)}));
    CHECK(set.state.findSequence(tl.seq)->workArea == TimeRange{f(2), f(5)});
    CHECK_FALSE(edit::setWorkArea(tl.state, tl.seq, TimeRange{f(8), f(5)}));  // past the end
    CHECK_FALSE(edit::setWorkArea(tl.state, tl.seq, TimeRange{f(2), f(0)}));  // empty
    CHECK_FALSE(edit::setSequenceDuration(set.state, tl.seq, f(6)));         // would cut into it... and the clip
    const auto cleared = ok(edit::setWorkArea(set.state, tl.seq, std::nullopt));
    CHECK_FALSE(cleared.state.findSequence(tl.seq)->workArea);

    // Fitting to a shorter content end trims the work area with it.
    const auto longer = ok(edit::setSequenceDuration(tl.state, tl.seq, f(20)));
    const auto wide = ok(edit::setWorkArea(longer.state, tl.seq, TimeRange{f(5), f(10)}));
    const auto fitted = ok(edit::fitSequenceDurationToContent(wide.state, tl.seq));
    CHECK(fitted.state.findSequence(tl.seq)->workArea == TimeRange::fromStartEnd(f(5), f(10)));
}

TEST_CASE("validate() rejects a duration shorter than the content", "[model][extent]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    auto seq = std::make_shared<Sequence>(tl.sequence());
    seq->duration = f(2);
    const auto valid = validate(tl.state.withSequence(seq));
    REQUIRE_FALSE(valid);
    CHECK(valid.error().message.find("shorter than its content") != std::string::npos);
}
