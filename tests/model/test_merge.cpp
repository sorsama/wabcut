// C.9: three-way project merge (Merge.h) - clean merges of disjoint edits, conflicts for overlapping ones with mine /
// theirs resolution, renumbered ids, determinism, and the speed on a large project.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |....................|
    V1 tgt |AaaaBbbbCccc........|
    A1 tgt |AaaaBbbbCccc........|
)";

edit::SourceClip clipOf(const Timeline& tl, char letter, const char* track, std::int64_t frames) {
    edit::SourceClip c;
    c.media = tl.media(letter);
    c.sourceRange = {f(0), f(frames)};
    c.patches = {{{StreamKind::Video, 0}, tl.track(track)}};
    return c;
}

MergeResult merged(const ProjectState& base, const ProjectState& mine, const ProjectState& theirs,
                   const std::map<std::string, MergeChoice>& choices = {}) {
    auto r = mergeProjects(base, mine, theirs, choices);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    requireValid(r->merged);
    return std::move(*r);
}

} // namespace

TEST_CASE("Merge: edits of different tracks and clips merge without conflicts", "[model][merge]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const ProjectState& base = tl.state;
    // Mine trims B on V1/A1; theirs adds a title-like clip on V2 and a marker.
    const ProjectState mine = ok(edit::trimEdge(base, tl.seq, tl.clip("V1@4"), edit::Edge::Tail, -f(1))).state;
    ProjectState theirs = ok(edit::overwriteEdit(base, tl.seq, clipOf(tl, 'D', "V2", 6), f(2))).state;
    theirs = ok(edit::addMarker(theirs, tl.seq, Marker{f(10), Ticks{}, "Music in"})).state;
    const MergeResult r = merged(base, mine, theirs);
    CHECK(r.conflicts.empty());
    CHECK(r.fromTheirs >= 2);
    CHECK_TIMELINE(r.merged, tl.seq, R"(
        V2 |..Dddddd............|
        V1 |AaaaBbb.Cccc........|
        A1 |AaaaBbb.Cccc........|
    )");
    CHECK(r.merged.findSequence(tl.seq)->markers.size() == 1);
    CHECK(links(r.merged, tl.seq) == "V1@0+A1@0 V1@4+A1@4 V1@8+A1@8");
}

TEST_CASE("Merge: both sides adding clips get their own ids", "[model][merge]") {
    Timeline tl(kPicture);
    const ProjectState& base = tl.state;
    // Both place a clip with the next free id: theirs is renumbered above mine.
    const ProjectState mine = ok(edit::overwriteEdit(base, tl.seq, clipOf(tl, 'D', "V1", 3), f(13))).state;
    const ProjectState theirs = ok(edit::overwriteEdit(base, tl.seq, clipOf(tl, 'E', "V2", 3), f(1))).state;
    REQUIRE(clipAt(mine, tl.seq, "V1@13") == clipAt(theirs, tl.seq, "V2@1")); // the same number on both sides
    const MergeResult r = merged(base, mine, theirs);
    CHECK(r.conflicts.empty());
    CHECK_TIMELINE(r.merged, tl.seq, R"(
        V2 |.Eee................|
        V1 |AaaaBbbbCccc.Ddd....|
        A1 |AaaaBbbbCccc........|
    )");
    CHECK(clipAt(r.merged, tl.seq, "V1@13") != clipAt(r.merged, tl.seq, "V2@1"));
    CHECK(r.merged.ids.next > mine.ids.next);
}

TEST_CASE("Merge: the same clip changed differently is a conflict; mine or theirs resolves it", "[model][merge]") {
    Timeline tl(kPicture);
    const ProjectState& base = tl.state;
    const ClipId b = tl.clip("V1@4");
    const ProjectState mine = ok(edit::trimEdge(base, tl.seq, b, edit::Edge::Tail, -f(1), edit::LinkMode::IgnoreLinks)).state;
    const ProjectState theirs = ok(edit::trimEdge(base, tl.seq, b, edit::Edge::Tail, -f(2), edit::LinkMode::IgnoreLinks)).state;
    const MergeResult r = merged(base, mine, theirs);
    REQUIRE(r.conflicts.size() == 1);
    const std::string id = "clip:" + std::to_string(b.value);
    CHECK(r.conflicts[0].id == id);
    CHECK(r.conflicts[0].sequence == tl.seq);
    CHECK(r.conflicts[0].what.find("'B'") != std::string::npos);
    CHECK(r.conflicts[0].resolved == MergeChoice::Mine);
    CHECK(r.merged.findSequence(tl.seq)->findClip(b)->clip().duration() == f(3));
    const MergeResult t = merged(base, mine, theirs, {{id, MergeChoice::Theirs}});
    CHECK(t.conflicts[0].resolved == MergeChoice::Theirs);
    CHECK(t.merged.findSequence(tl.seq)->findClip(b)->clip().duration() == f(2));
    // Removed on one side, changed on the other.
    const ProjectState gone = ok(edit::deleteClips(base, tl.seq, std::vector<ClipId>{b}, edit::LinkMode::IgnoreLinks)).state;
    const MergeResult g = merged(base, gone, theirs);
    REQUIRE(g.conflicts.size() == 1);
    CHECK_FALSE(g.merged.findSequence(tl.seq)->findClip(b));
    CHECK(merged(base, gone, theirs, {{id, MergeChoice::Theirs}}).merged.findSequence(tl.seq)->findClip(b));
}

TEST_CASE("Merge: clips both sides put over the same time conflict; the other side's change goes back", "[model][merge]") {
    Timeline tl(kPicture);
    const ProjectState& base = tl.state;
    const ProjectState mine = ok(edit::overwriteEdit(base, tl.seq, clipOf(tl, 'D', "V1", 4), f(12))).state;
    const ProjectState theirs = ok(edit::overwriteEdit(base, tl.seq, clipOf(tl, 'E', "V1", 4), f(14))).state;
    const MergeResult r = merged(base, mine, theirs);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].id.starts_with("overlap:" + std::to_string(tl.track("V1").value) + ":"));
    CHECK_TIMELINE(r.merged, tl.seq, R"(
        V2 |....................|
        V1 |AaaaBbbbCcccDddd....|
        A1 |AaaaBbbbCccc........|
    )");
    const MergeResult t = merged(base, mine, theirs, {{r.conflicts[0].id, MergeChoice::Theirs}});
    CHECK_TIMELINE(t.merged, tl.seq, R"(
        V2 |....................|
        V1 |AaaaBbbbCccc..Eeee..|
        A1 |AaaaBbbbCccc........|
    )");
    // Moving an existing clip onto the other side's new clip: back to where it was.
    const ProjectState moved =
        ok(edit::moveClips(base, tl.seq, std::vector<ClipId>{tl.clip("V1@8")}, f(5), {}, edit::MoveMode::Overwrite,
                           edit::LinkMode::IgnoreLinks))
            .state;
    const MergeResult m = merged(base, mine, moved);
    REQUIRE(m.conflicts.size() == 1);
    CHECK_TIMELINE(m.merged, tl.seq, R"(
        V2 |....................|
        V1 |AaaaBbbbCcccDddd....|
        A1 |AaaaBbbbCccc........|
    )");
}

TEST_CASE("Merge: results are deterministic and other sequences merge whole", "[model][merge]") {
    Timeline tl(kPicture);
    ProjectState base = tl.state;
    edit::SequenceSettings settings;
    settings.name = "Teaser";
    const auto teaser = edit::addSequence(base, settings);
    REQUIRE(teaser);
    base = teaser->state;
    const ProjectState mine = ok(edit::trimEdge(base, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, -f(1), edit::LinkMode::IgnoreLinks)).state;
    auto renamed = std::make_shared<Sequence>(*base.findSequence(teaser->id));
    renamed->name = "Teaser (client)";
    ProjectState theirs = base.withSequence(renamed);
    theirs = ok(edit::trimEdge(theirs, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, -f(2), edit::LinkMode::IgnoreLinks)).state;
    const MergeResult a = merged(base, mine, theirs);
    const MergeResult b = merged(base, mine, theirs);
    CHECK(a.merged == b.merged);
    CHECK(a.conflicts == b.conflicts);
    CHECK(a.merged.findSequence(teaser->id)->name == "Teaser (client)");
    // Nothing on one side: the other side's project, exactly.
    CHECK(merged(base, base, theirs).merged == theirs);
    CHECK(merged(base, mine, base).merged == mine);
}

TEST_CASE("Merge: a sequence of another version comes back as a new sequence", "[model][merge][history]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const ProjectState version = tl.state;
    // Since that version: C's clips were deleted and C removed from the project, B trimmed.
    const ClipId c = tl.clip("V1@8");
    ProjectState now = ok(edit::deleteClips(version, tl.seq, std::span(&c, 1))).state;
    auto removed = edit::removeMedia(now, tl.media('C'));
    REQUIRE(removed);
    now = std::move(*removed);
    now = ok(edit::trimEdge(now, tl.seq, tl.clip("V1@4"), edit::Edge::Tail, -f(2))).state;
    const std::uint64_t nextBefore = now.ids.next;

    const auto copy = edit::copySequenceFrom(now, version, tl.seq, "Seq (old)");
    REQUIRE(copy);
    requireValid(copy->state);
    REQUIRE(copy->state.sequences.size() == 2);
    const Sequence* restored = copy->state.findSequence(copy->id);
    REQUIRE(restored);
    CHECK(restored->name == "Seq (old)");
    CHECK(copy->id.value >= nextBefore); // everything in the copy has a new id
    CHECK(clipAt(copy->state, copy->id, "V1@0").value >= nextBefore);
    CHECK_TIMELINE(copy->state, copy->id, R"(
        V2 |....................|
        V1 |AaaaBbbbCccc........|
        A1 |AaaaBbbbCccc........|
    )");
    CHECK(links(copy->state, copy->id) == "V1@0+A1@0 V1@4+A1@4 V1@8+A1@8");
    // The project's own sequence is untouched; A and B are the project's media, C comes back under its own id (the
    // source's identity: its redactions keep applying, review wave 5 E5-P1-3).
    CHECK(copy->state.findSequence(tl.seq) == now.findSequence(tl.seq));
    const auto mediaAt = [&](std::string_view ref) {
        return restored->findClip(clipAt(copy->state, copy->id, ref))->clip().media;
    };
    CHECK(mediaAt("V1@0") == tl.media('A'));
    CHECK(mediaAt("A1@4") == tl.media('B'));
    const MediaId c2 = mediaAt("V1@8");
    CHECK(c2 == tl.media('C'));
    CHECK(copy->state.media.size() == now.media.size() + 1);
    CHECK(copy->state.ids.next > c2.value);

    // A sequence the version does not have.
    CHECK(edit::copySequenceFrom(now, version, SequenceId{9999}, "x").error().code == ErrorCode::NotFound);
}

TEST_CASE("Merge: a restored sequence plays the project's media ids, so their redactions keep applying (review wave 5 "
          "E5-P1-3)",
          "[model][merge][history][redaction]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto redact = [](const ProjectState& s, MediaId media, const char* name) {
        Redaction r;
        r.media = media;
        r.name = name;
        r.source = {f(0), f(100)};
        r.audio = RedactionAudio::Mute;
        r.region.shape = MaskShape::Rectangle;
        auto added = edit::addRedaction(s, r);
        REQUIRE(added);
        return std::move(*added);
    };
    const auto faceA = redact(tl.state, tl.media('A'), "Face on A");
    const auto faceC = redact(faceA.state, tl.media('C'), "Face on C");
    const ProjectState version = faceC.state; // the checkpoint
    // Since that version: A renamed in the Library; C's clips deleted and C removed (its redaction stays, orphaned).
    edit::MediaAttributeChange rename;
    rename.name = "A (interview)";
    const MediaId a = tl.media('A');
    ProjectState now = *edit::setMediaAttributes(version, std::span(&a, 1), rename);
    const ClipId c = tl.clip("V1@8");
    now = ok(edit::deleteClips(now, tl.seq, std::span(&c, 1))).state;
    auto removed = edit::removeMedia(now, tl.media('C'));
    REQUIRE(removed);
    now = std::move(*removed);
    REQUIRE(now.findRedaction(faceC.id));

    const auto copy = edit::copySequenceFrom(now, version, tl.seq, "Seq (old)");
    REQUIRE(copy);
    requireValid(copy->state);
    const auto redactedAt = [&](StreamKind kind, std::int64_t frame) {
        const auto leaves = resolveSequenceLeaves(copy->state, copy->id, kind, f(frame));
        REQUIRE(leaves.size() == 1);
        return leaves.front().redactions;
    };
    // The renamed item is the same source: A's face stays blurred and muted in the restored sequence.
    CHECK(redactedAt(StreamKind::Video, 1) == std::vector<RedactionId>{faceA.id});
    CHECK(redactedAt(StreamKind::Audio, 1) == std::vector<RedactionId>{faceA.id});
    // The removed item comes back under its own id: its redaction applies again.
    CHECK(copy->state.findMedia(tl.media('C')));
    CHECK(redactedAt(StreamKind::Video, 9) == std::vector<RedactionId>{faceC.id});
    CHECK(redactedAt(StreamKind::Audio, 9) == std::vector<RedactionId>{faceC.id});
}

TEST_CASE("Merge: 5,000 clips merge in well under a second", "[model][merge][perf]") {
    Timeline tl(R"(
        V1 tgt |A|
        A1 tgt |A|
    )");
    ProjectState base = tl.state;
    auto seq = std::make_shared<Sequence>(*base.findSequence(tl.seq));
    for (int t = 0; t < 10; ++t) {
        auto v = std::make_shared<Track>();
        v->id = base.ids.allocate<TrackId>();
        v->kind = TrackKind::Video;
        auto a = std::make_shared<Track>();
        a->id = base.ids.allocate<TrackId>();
        a->kind = TrackKind::Audio;
        for (int i = 0; i < 250; ++i) {
            const MediaId media = tl.media(static_cast<char>('A' + i % 26));
            const LinkGroupId group = base.ids.allocate<LinkGroupId>();
            for (Track* track : {v.get(), a.get()}) {
                Clip c;
                c.id = base.ids.allocate<ClipId>();
                c.media = media;
                c.stream = {track == v.get() ? StreamKind::Video : StreamKind::Audio, 0};
                c.timelineRange = {f(i * 10), f(10)};
                c.sourceIn = f(i % 40);
                c.linkGroup = group;
                c.setContent(base.findMedia(media)->defaultClipContent());
                track->clips.push_back(std::move(c));
            }
        }
        seq->videoTracks.push_back(std::move(v));
        seq->audioTracks.push_back(std::move(a));
    }
    seq->duration = f(2500);
    base = base.withSequence(seq);
    requireValid(base);
    // Each side trims ten clips on different tracks.
    ProjectState mine = base;
    ProjectState theirs = base;
    for (int k = 0; k < 10; ++k) {
        const Sequence& s = *base.findSequence(tl.seq);
        const ClipId m = s.videoTracks[2]->clips[static_cast<std::size_t>(10 + 20 * k)].id;
        const ClipId t = s.videoTracks[7]->clips[static_cast<std::size_t>(10 + 20 * k)].id;
        mine = ok(edit::trimEdge(mine, tl.seq, m, edit::Edge::Tail, -f(1), edit::LinkMode::IgnoreLinks)).state;
        theirs = ok(edit::trimEdge(theirs, tl.seq, t, edit::Edge::Tail, -f(2), edit::LinkMode::IgnoreLinks)).state;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto r = mergeProjects(base, mine, theirs);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    REQUIRE(r);
    CHECK(r->conflicts.empty());
    std::printf("merge, 5,002 clips, 10 + 10 edits: %.1f ms\n", ms);
    CHECK(ms < 1000.0);
    const Sequence& out = *r->merged.findSequence(tl.seq);
    CHECK(out.videoTracks[2]->clips[10].duration() == f(9));
    CHECK(out.videoTracks[7]->clips[10].duration() == f(8));
}

TEST_CASE("Merge: a clip moved on one side and changed or moved on the other is placed once (E5-P2-5)", "[model][merge]") {
    Timeline tl(R"(
        V3     |....................|
        V2     |....................|
        V1 tgt |AaaaBbbbCccc........|
    )");
    const ProjectState& base = tl.state;
    const ClipId x = tl.clip("V1@4");
    const std::string conflict = "clip:" + std::to_string(x.value);
    const auto conflicted = [&](const MergeResult& r) {
        return std::any_of(r.conflicts.begin(), r.conflicts.end(), [&](const MergeConflict& k) { return k.id == conflict; });
    };
    const ProjectState trimmed = ok(edit::trimEdge(base, tl.seq, x, edit::Edge::Tail, -f(1))).state;
    const ProjectState movedUp = ok(edit::moveClips(base, tl.seq, std::span(&x, 1), f(0), {1, 0})).state;
    const ProjectState movedTop = ok(edit::moveClips(base, tl.seq, std::span(&x, 1), f(0), {2, 0})).state;

    // Theirs moves B up to V2 while mine trims it: one conflict; either way B is there once.
    const MergeResult keepMine = merged(base, trimmed, movedUp);
    CHECK(conflicted(keepMine));
    CHECK_TIMELINE(keepMine.merged, tl.seq, R"(
        V3 |....................|
        V2 |....................|
        V1 |AaaaBbb.Cccc........|
    )");
    const MergeResult keepTheirs = merged(base, trimmed, movedUp, {{conflict, MergeChoice::Theirs}});
    CHECK_TIMELINE(keepTheirs.merged, tl.seq, R"(
        V3 |....................|
        V2 |....Bbbb............|
        V1 |Aaaa....Cccc........|
    )");

    // Both move it, to different tracks (no conflict was raised for this before).
    const MergeResult both = merged(base, movedTop, movedUp);
    CHECK(conflicted(both));
    CHECK_TIMELINE(both.merged, tl.seq, R"(
        V3 |....Bbbb............|
        V2 |....................|
        V1 |Aaaa....Cccc........|
    )");

    // Theirs nests B (the nest keeps the clip's id) while mine trims it.
    const auto nest = edit::nestClips(base, tl.seq, std::span(&x, 1), "N");
    REQUIRE(nest);
    const MergeResult nested = merged(base, trimmed, nest->edit.state);
    CHECK(conflicted(nested));
    std::size_t copies = 0;
    for (const SequencePtr& s : nested.merged.sequences) {
        s->forEachTrack([&](const Track& t) { copies += static_cast<std::size_t>(std::count_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.id == x; })); });
    }
    CHECK(copies == 1);
}

TEST_CASE("Merge: sequences nested into each other across the sides are a conflict (E5-P2-5)", "[model][merge]") {
    Timeline tl(kPicture);
    // A second sequence Q with a clip of its own.
    edit::SequenceSettings settings;
    settings.name = "Q";
    settings.frameRate = kRate;
    settings.videoTracks = 1;
    settings.audioTracks = 1;
    const auto q = edit::addSequence(tl.state, settings);
    REQUIRE(q);
    edit::SourceClip d;
    d.media = tl.media('D');
    d.sourceRange = {f(0), f(4)};
    d.patches = edit::defaultSourcePatches(*q->state.findSequence(q->id), *q->state.findMedia(tl.media('D')));
    const ProjectState base = ok(edit::overwriteEdit(q->state, q->id, d, f(0))).state;
    const auto nestOf = [&](SequenceId from, SequenceId into) {
        edit::SourceClip s;
        s.sequence = from;
        s.sourceRange = {f(0), f(4)};
        s.patches = edit::defaultSourcePatches(*base.findSequence(into), *base.findSequence(from));
        return ok(edit::overwriteEdit(base, into, s, f(14))).state;
    };
    const ProjectState mine = nestOf(q->id, tl.seq);   // mine plays Q in P
    const ProjectState theirs = nestOf(tl.seq, q->id); // theirs plays P in Q
    REQUIRE(validate(mine, Validation::Structure).has_value());
    REQUIRE(validate(theirs, Validation::Structure).has_value());

    const auto r = mergeProjects(base, mine, theirs);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r.has_value());
    requireValid(r->merged);
    CHECK_FALSE(r->invalid.has_value());
    CHECK_FALSE(nestingCycle(r->merged).has_value());
    REQUIRE(std::any_of(r->conflicts.begin(), r->conflicts.end(), [](const MergeConflict& k) { return k.id.starts_with("nesting:"); }));
    CHECK(sequenceContains(r->merged, tl.seq, q->id)); // mine's nest stays by default
    for (const MergeConflict& k : r->conflicts) {
        if (k.id.starts_with("nesting:")) {
            const auto other = mergeProjects(base, mine, theirs, {{k.id, MergeChoice::Theirs}});
            REQUIRE(other.has_value());
            requireValid(other->merged);
            CHECK(sequenceContains(other->merged, q->id, tl.seq));
            CHECK_FALSE(sequenceContains(other->merged, tl.seq, q->id));
        }
    }
}
