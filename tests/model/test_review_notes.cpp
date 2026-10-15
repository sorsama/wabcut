// C.9: review notes - add, thread, resolve, remove, validation, project files, notes to markers (ReviewOps.h).

#include "TimelineFixture.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ReviewOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

ReviewNote note(SequenceId seq, std::int64_t frame, std::string text, std::int64_t frames = 0) {
    ReviewNote n;
    n.sequence = seq;
    n.start = f(frame);
    n.duration = f(frames);
    n.author = "Client";
    n.text = std::move(text);
    n.createdAt = "2026-10-12T09:30:00Z";
    return n;
}

} // namespace

TEST_CASE("Review notes: add, reply, resolve, remove, saved in the project", "[model][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    ReviewNote first = note(tl.seq, 4, "Logo too small", 6);
    first.mark = ReviewMark{0.7, 0.05, 0.2, 0.1};
    auto a = edit::addReviewNote(tl.state, first);
    REQUIRE(a);
    auto b = edit::addReviewNote(a->state, note(tl.seq, 12, "Colour pops here"));
    REQUIRE(b);
    requireValid(b->state);
    ProjectState s = b->state;
    REQUIRE(s.reviewNotes.size() == 2);
    CHECK(s.reviewNotes[0].id == a->id);
    CHECK(s.reviewNotes[0].mark->width == 0.2);

    s = *edit::replyToReviewNote(s, a->id, ReviewReply{"Editor", "Bigger in v3", "2026-10-12T10:00:00Z"});
    s = *edit::replyToReviewNote(s, a->id, ReviewReply{"Client", "Thanks!", ""});
    s = *edit::setReviewNoteStatus(s, a->id, ReviewStatus::Resolved);
    const ReviewNote& thread = *s.findReviewNote(a->id);
    REQUIRE(thread.replies.size() == 2);
    CHECK(thread.replies[1].text == "Thanks!");
    CHECK(thread.status == ReviewStatus::Resolved);

    const auto json = io::toJson(s);
    REQUIRE(json);
    CHECK(json->find("\"reviewNotes\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == s);

    s = *edit::removeReviewNote(s, b->id);
    CHECK(s.reviewNotes.size() == 1);
    CHECK_FALSE(edit::removeReviewNote(s, b->id));
    // Removing the sequence removes its notes.
    const auto gone = edit::removeSequence(s, tl.seq);
    REQUIRE(gone);
    CHECK(gone->reviewNotes.empty());
    requireValid(*gone);
}

TEST_CASE("Review notes are validated (bounded, on the frame, on an existing sequence)", "[model][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    ReviewNote outside = note(tl.seq, 2, "x");
    outside.mark = ReviewMark{0.9, 0.5, 0.2, 0.1};
    CHECK_FALSE(edit::addReviewNote(tl.state, outside));
    CHECK_FALSE(edit::addReviewNote(tl.state, note(SequenceId{999}, 2, "x")));
    CHECK_FALSE(edit::addReviewNote(tl.state, note(tl.seq, 20, "after the end")));
    CHECK_FALSE(edit::addReviewNote(tl.state, note(tl.seq, 2, std::string(kMaxReviewTextBytes + 1, 'x'))));
    CHECK_FALSE(edit::addReviewNote(tl.state, note(tl.seq, 2, "\xff\xfe not UTF-8")));
    ReviewNote badTime = note(tl.seq, 2, "x");
    badTime.createdAt = "yesterday";
    CHECK_FALSE(edit::addReviewNote(tl.state, badTime));
    // A hand-edited file with a note on a missing sequence does not load.
    auto ok = edit::addReviewNote(tl.state, note(tl.seq, 2, "fine"));
    REQUIRE(ok);
    std::string text = *io::toJson(ok->state);
    const std::string seq = "\"sequence\": " + std::to_string(tl.seq.value);
    text.replace(text.rfind(seq), seq.size(), "\"sequence\": 999999");
    CHECK_FALSE(io::fromJson(text));
}

TEST_CASE("Review notes become sequence markers", "[model][review]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    auto a = edit::addReviewNote(tl.state, note(tl.seq, 12, "Cut earlier\nand tighten the pause", 3));
    REQUIRE(a);
    auto b = edit::addReviewNote(a->state, note(tl.seq, 4, "Fix the colour"));
    REQUIRE(b);
    ProjectState s = *edit::replyToReviewNote(b->state, b->id, ReviewReply{"Editor", "Done", ""});
    s = *edit::setReviewNoteStatus(s, b->id, ReviewStatus::Resolved);
    const auto r = ok(edit::reviewNotesToMarkers(s, tl.seq, {}, true));
    const Sequence& seq = *r.state.findSequence(tl.seq);
    REQUIRE(seq.markers.size() == 2);
    CHECK(seq.markers[0].start == f(4));
    CHECK(seq.markers[0].name == "Fix the colour");
    CHECK(seq.markers[0].colorIndex == 0); // resolved: green
    CHECK(seq.markers[0].comment == "Fix the colour\n- Client\nEditor: Done");
    CHECK(seq.markers[1].name == "Cut earlier");
    CHECK(seq.markers[1].duration == f(3));
    CHECK(seq.markers[1].colorIndex == 1); // open: red
    CHECK(r.state.reviewNotes.empty());
    CHECK(r.changes.reviewNotesChanged);
    // Only some, keeping the notes; a note of another sequence is refused.
    const std::vector<ReviewNoteId> one{a->id};
    const auto kept = ok(edit::reviewNotesToMarkers(s, tl.seq, one));
    CHECK(kept.state.reviewNotes.size() == 2);
    CHECK(kept.state.findSequence(tl.seq)->markers.size() == 1);
    CHECK_FALSE(edit::reviewNotesToMarkers(s, tl.seq, std::vector<ReviewNoteId>{ReviewNoteId{424242}}));
}

TEST_CASE("Review notes, caption presets and transcript edits are shared between snapshots until changed (C.10)",
          "[model][review]") {
    // A thousand notes copied by every edit and undo step cost ~300 KB a step (the C.10 scale test); snapshots now
    // share the list until it changes.
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    ProjectState s = tl.state;
    for (int i = 0; i < 20; ++i) {
        s = edit::addReviewNote(s, note(tl.seq, i % 20, "note " + std::to_string(i)))->state;
    }
    const ProjectState before = s;
    const ProjectState trimmed = ok(edit::trimEdge(before, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, -f(1))).state;
    CHECK(trimmed.reviewNotes.sharesWith(before.reviewNotes));
    CHECK(trimmed.captionPresets.sharesWith(before.captionPresets));
    CHECK(trimmed.transcriptEdits.sharesWith(before.transcriptEdits));
    // Changing a note copies the list: the earlier snapshot keeps its own.
    const ProjectState resolved = *edit::setReviewNoteStatus(trimmed, before.reviewNotes[3].id, ReviewStatus::Resolved);
    CHECK_FALSE(resolved.reviewNotes.sharesWith(trimmed.reviewNotes));
    CHECK(trimmed.reviewNotes[3].status == ReviewStatus::Open);
    CHECK(resolved.reviewNotes[3].status == ReviewStatus::Resolved);
    CHECK(resolved.reviewNotes.size() == 20);
    // Equal contents compare equal whether shared or not.
    ProjectState copy = before;
    copy.reviewNotes = std::vector<ReviewNote>(before.reviewNotes.begin(), before.reviewNotes.end());
    CHECK_FALSE(copy.reviewNotes.sharesWith(before.reviewNotes));
    CHECK(copy == before);
}
