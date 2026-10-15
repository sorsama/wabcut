// C.9: what changed between two revisions (ProjectDiff.h): structure and the human-readable summary of scripted edits.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectDiff.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ReviewOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

bool hasLine(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l == text; });
}

} // namespace

TEST_CASE("The diff names clips added, removed, moved and trimmed, with timecodes", "[model][diff]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....Cccc|
        A1 tgt |AaaaBbbb....Cccc|
    )");
    const ProjectState before = tl.state;
    ProjectState s = ok(edit::trimEdge(tl.state, tl.seq, tl.clip("V1@4"), edit::Edge::Tail, -f(2), edit::LinkMode::IgnoreLinks)).state;
    s = ok(edit::moveClips(s, tl.seq, std::vector<ClipId>{tl.clip("V1@12")}, f(-3), {}, edit::MoveMode::Overwrite,
                           edit::LinkMode::IgnoreLinks))
            .state;
    s = ok(edit::deleteClips(s, tl.seq, std::vector<ClipId>{tl.clip("A1@0")}, edit::LinkMode::IgnoreLinks)).state;
    edit::SourceClip d;
    d.media = tl.media('D');
    d.sourceRange = {f(0), f(2)};
    d.patches = {{{StreamKind::Video, 0}, tl.track("V1")}};
    s = ok(edit::overwriteEdit(s, tl.seq, d, f(14))).state;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::addEffect(s, tl.seq, std::span<const ClipId>(&a, 1), "wabcut.blur.gaussian")).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(1), 0.5})).state;
    s = edit::addReviewNote(s, ReviewNote{{}, tl.seq, f(1), {}, std::nullopt, "C", "note", "", ReviewStatus::Open, {}})->state;

    const ProjectDiff diff = diffProjects(before, s);
    REQUIRE(diff.sequences.size() == 1);
    const SequenceDiff& seq = diff.sequences[0];
    REQUIRE(seq.clips.size() == 5);
    const auto lines = diff.summary();
    INFO(lines.size());
    for (const std::string& l : lines) {
        UNSCOPED_INFO(l);
    }
    const std::string name = "'" + tl.sequence().name + "'";
    CHECK(hasLine(lines, name + ": V1 'A' + effect wabcut.blur.gaussian, keyframes of Opacity changed"));
    CHECK(hasLine(lines, name + ": V1 'B' trimmed 00:00:00:04-00:00:00:08 -> 00:00:00:04-00:00:00:06"));
    CHECK(hasLine(lines, name + ": V1 'C' moved 00:00:00:12 -> 00:00:00:09"));
    CHECK(hasLine(lines, name + ": V1 'D' added at 00:00:00:14-00:00:00:16"));
    CHECK(hasLine(lines, name + ": A1 'A' removed from 00:00:00:00-00:00:00:04"));
    CHECK(hasLine(lines, "Review notes: 1 added, 0 removed, 0 changed"));
    // Structure: kinds and flags.
    const auto find = [&](const char* clip, ClipDiff::Kind kind) {
        return std::find_if(seq.clips.begin(), seq.clips.end(),
                            [&](const ClipDiff& c) { return c.name == clip && c.kind == kind; });
    };
    REQUIRE(find("B", ClipDiff::Kind::Changed) != seq.clips.end());
    CHECK(find("B", ClipDiff::Kind::Changed)->trimmed);
    CHECK_FALSE(find("B", ClipDiff::Kind::Changed)->moved);
    CHECK(find("C", ClipDiff::Kind::Changed)->moved);
    CHECK(find("A", ClipDiff::Kind::Removed)->trackBefore == "A1");
    // Nothing changed, nothing to say; a sequence and media added are named.
    CHECK(diffProjects(s, s).empty());
    edit::SequenceSettings settings;
    settings.name = "Teaser";
    const auto added = edit::addSequence(s, settings);
    REQUIRE(added);
    const auto more = diffProjects(s, added->state).summary();
    CHECK(hasLine(more, "Added sequence 'Teaser'"));
}
