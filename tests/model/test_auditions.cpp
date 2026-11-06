// 0.6 (card #74): auditions - take stacks at a cut (Auditions.h). Creating one from clips, switching takes in place,
// trims / ripples / razor cuts / moves keeping every take in step, short takes, linked picture and sound, a take's own
// effects, a multicam take, remove / reorder / rename / duplicate / flatten, undo, project files and the clipboard.

#include "NestedTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) {
    const auto loc = s.findSequence(seq)->findClip(id);
    REQUIRE(loc.has_value());
    return loc->clip();
}

std::vector<std::string> names(const Clip& c) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < takeCount(c); ++i) {
        out.push_back(takeName(c, i));
    }
    return out;
}

/// V1 holds the audition A (6 frames from 0) with takes B (from source frame 5) and C (from 2), V2/V3 now empty.
struct Three {
    Timeline tl{R"(
        V3 |C2cccc....|
        V2 |B5bbbb....|
        V1 |Aaaaaa..Dd|
    )"};
    ProjectState state;
    ClipId audition;

    Three() {
        audition = tl.clip("V1@0");
        const std::vector<ClipId> ids{tl.clip("V3@0"), audition, tl.clip("V2@0")};
        state = ok(edit::createAudition(tl.state, tl.seq, ids)).state;
    }
};

} // namespace

TEST_CASE("Auditions 0.6: three clips become one audition and switch takes in place", "[model][auditions]") {
    Three t;
    CHECK_TIMELINE(t.state, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |Aaaaaa..Dd|
    )");
    const Clip& a = clipOf(t.state, t.tl.seq, t.audition);
    REQUIRE(a.isAudition());
    CHECK(takeCount(a) == 3);
    CHECK(a.audition()->active == 0);
    CHECK(names(a) == std::vector<std::string>{"A", "B", "C"});

    // Use take 2: the clip keeps its id, place and length and plays B from its own In; the playhead is not touched.
    const edit::EditResult b = ok(edit::setActiveTake(t.state, t.tl.seq, t.audition, 1));
    CHECK_FALSE(b.playhead.has_value());
    CHECK_TIMELINE(b.state, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |B5bbbb..Dd|
    )");
    CHECK(clipOf(b.state, t.tl.seq, t.audition).audition()->active == 1);
    CHECK(names(clipOf(b.state, t.tl.seq, t.audition)) == std::vector<std::string>{"A", "B", "C"});
    // Take 3, then back to take 1: exactly the clip it was.
    const ProjectState c = ok(edit::setActiveTake(b.state, t.tl.seq, t.audition, 2)).state;
    CHECK_TIMELINE(c, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |C2cccc..Dd|
    )");
    const ProjectState back = ok(edit::setActiveTake(c, t.tl.seq, t.audition, 0)).state;
    CHECK(clipOf(back, t.tl.seq, t.audition) == clipOf(t.state, t.tl.seq, t.audition));
    // The active take again changes nothing; a take that does not exist is refused.
    CHECK(ok(edit::setActiveTake(back, t.tl.seq, t.audition, 0)).changes.sequences.empty());
    CHECK_FALSE(edit::setActiveTake(back, t.tl.seq, t.audition, 3).has_value());
}

TEST_CASE("Auditions 0.6: trims ripples moves and razor cuts keep every take in step", "[model][auditions]") {
    Three t;
    // Trim the head 1 frame later: every take loses its first frame.
    ProjectState s = ok(edit::trimEdge(t.state, t.tl.seq, t.audition, edit::Edge::Head, f(1))).state;
    CHECK_TIMELINE(s, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |.A1aaa..Dd|
    )");
    CHECK_TIMELINE(ok(edit::setActiveTake(s, t.tl.seq, t.audition, 1)).state, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |.B6bbb..Dd|
    )");
    // Ripple-trim the tail 1 frame shorter: D follows, the takes end one frame earlier.
    s = ok(edit::rippleTrim(s, t.tl.seq, t.audition, edit::Edge::Tail, f(-1))).state;
    CHECK_TIMELINE(s, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |.A1aa..Dd.|
    )");
    CHECK_TIMELINE(ok(edit::setActiveTake(s, t.tl.seq, t.audition, 2)).state, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |.C3cc..Dd.|
    )");
    // Move it 1 frame earlier: the takes go with it.
    const ClipId one[] = {t.audition};
    s = ok(edit::moveClips(s, t.tl.seq, one, f(-1))).state;
    CHECK_TIMELINE(ok(edit::setActiveTake(s, t.tl.seq, t.audition, 1)).state, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |B6bb...Dd.|
    )");
    // A razor cut at frame 2: both pieces are auditions with every take, each over its own part.
    const TrackId v1 = t.tl.track("V1");
    s = ok(edit::addEdit(s, t.tl.seq, f(2), std::span(&v1, 1))).state;
    const ClipId left = clipAt(s, t.tl.seq, "V1@0");
    const ClipId right = clipAt(s, t.tl.seq, "V1@2");
    CHECK(takeCount(clipOf(s, t.tl.seq, left)) == 3);
    CHECK(takeCount(clipOf(s, t.tl.seq, right)) == 3);
    s = ok(edit::setActiveTake(s, t.tl.seq, right, 2)).state;
    CHECK_TIMELINE(s, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |A1C5...Dd.|
    )");
    s = ok(edit::setActiveTake(s, t.tl.seq, left, 1)).state;
    CHECK_TIMELINE(s, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |B6C5...Dd.|
    )");
    requireValid(s);
}

TEST_CASE("Auditions 0.6: a shorter take leaves the rest empty and a longer one fills it again", "[model][auditions]") {
    Timeline tl(R"(
        V2 |B2........|
        V1 |Aaaaaa....|
    )",
                {{'B', 4}});
    const ClipId a = tl.clip("V1@0");
    const ClipId ids[] = {a, tl.clip("V2@0")};
    ProjectState s = ok(edit::createAudition(tl.state, tl.seq, ids)).state;
    // B has 4 frames: from its In (2) it fills 2 frames of the 6-frame audition and is 4 short at the tail.
    const TakeFit fit = takeFit(s, clipOf(s, tl.seq, a), 1, kRate);
    CHECK(fit.head == Ticks{});
    CHECK(fit.tail == f(4));
    CHECK(takeFit(s, clipOf(s, tl.seq, a), 0, kRate).fills());
    s = ok(edit::setActiveTake(s, tl.seq, a, 1)).state;
    CHECK_TIMELINE(s, tl.seq, R"(
        V2 |..........|
        V1 |B2........|
    )");
    CHECK(auditionRange(clipOf(s, tl.seq, a)) == TimeRange{f(0), f(6)});
    // Take 1 again: the full cut comes back.
    const ProjectState full = ok(edit::setActiveTake(s, tl.seq, a, 0)).state;
    CHECK_TIMELINE(full, tl.seq, R"(
        V2 |..........|
        V1 |Aaaaaa....|
    )");
    // With a clip in the gap meanwhile, take 1 grows only up to it (the rest is kept for later).
    edit::SourceClip e;
    e.media = tl.media('E');
    e.sourceRange = {f(0), f(2)};
    e.patches = {{StreamRef{StreamKind::Video, 0}, tl.track("V1")}};
    s = ok(edit::overwriteEdit(s, tl.seq, e, f(4))).state;
    const ProjectState blocked = ok(edit::setActiveTake(s, tl.seq, a, 0)).state;
    CHECK_TIMELINE(blocked, tl.seq, R"(
        V2 |..........|
        V1 |AaaaEe....|
    )");
    CHECK(auditionRange(clipOf(blocked, tl.seq, a)) == TimeRange{f(0), f(6)});
    requireValid(blocked);
}

TEST_CASE("Auditions 0.6: linked picture and sound become one audition and switch together", "[model][auditions]") {
    Timeline tl(R"(
        V2 |..B3bb....|
        V1 |Aaaa......|
        A1 |Aaaa......|
        A2 |..B3bb....|
    )");
    tl.link("V1@0 A1@0").link("V2@2 A2@2");
    const ClipId picture = tl.clip("V1@0");
    const ClipId sound = tl.clip("A1@0");
    const ClipId pick[] = {picture, tl.clip("V2@2")};
    ProjectState s = ok(edit::createAudition(tl.state, tl.seq, pick)).state;
    CHECK_TIMELINE(s, tl.seq, R"(
        V2 |..........|
        V1 |Aaaa......|
        A1 |Aaaa......|
        A2 |..........|
    )");
    CHECK(takeCount(clipOf(s, tl.seq, sound)) == 2);
    CHECK(auditionPartners(*s.findSequence(tl.seq), picture) == std::vector<ClipId>{picture, sound});
    // Using take 2 on the picture switches the sound too, in sync.
    s = ok(edit::setActiveTake(s, tl.seq, picture, 1)).state;
    CHECK_TIMELINE(s, tl.seq, R"(
        V2 |..........|
        V1 |B3bb......|
        A1 |B3bb......|
        A2 |..........|
    )");
    CHECK(links(s, tl.seq) == "V1@0+A1@0");
    // A take of another shape is refused with the reason.
    Timeline odd(R"(
        V2 |B3bb......|
        V1 |Aaaa......|
        A1 |Aaaa......|
    )");
    odd.link("V1@0 A1@0");
    const ClipId mixed[] = {odd.clip("V1@0"), odd.clip("V2@0")};
    const auto refused = edit::createAudition(odd.state, odd.seq, mixed);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("alike") != std::string::npos);
}

TEST_CASE("Auditions 0.6: a take keeps its own effects and a multicam clip can be a take", "[model][auditions]") {
    Timeline tl(R"(
        V2 |Bbbb......|
        V1 |Aaaa......|
    )");
    const ClipId b = tl.clip("V2@0");
    const ClipId a = tl.clip("V1@0");
    ProjectState s = ok(edit::addEffect(tl.state, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian",
                                        {EffectParam{"radius", ParamValue(9.0)}}))
                         .state;
    const ClipId ids[] = {a, b};
    s = ok(edit::createAudition(s, tl.seq, ids)).state;
    CHECK(clipOf(s, tl.seq, a).effects().empty());
    s = ok(edit::setActiveTake(s, tl.seq, a, 1)).state;
    REQUIRE(clipOf(s, tl.seq, a).effects().size() == 1);
    CHECK(clipOf(s, tl.seq, a).effects().front().effect == "wabcut.blur.gaussian");
    // Effects added while a take is active belong to that take.
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.color.tint")).state;
    s = ok(edit::setActiveTake(s, tl.seq, a, 0)).state;
    CHECK(clipOf(s, tl.seq, a).effects().empty());
    s = ok(edit::setActiveTake(s, tl.seq, a, 1)).state;
    CHECK(clipOf(s, tl.seq, a).effects().size() == 2);

    // A multicam clip as a take: it keeps its source and angle.
    Timeline mc(R"(
        V1 |Aaaa......|
        A1 |Aaaa......|
    )");
    mc.linkMatching();
    PlacedMulticam placed = placeMulticam(mc.state, mc.seq, {mc.media('M'), mc.media('N')}, {f(0), f(0)},
                                          {f(0), f(4)}, f(6), 1, 1);
    const ClipId plain = clipAt(placed.state, mc.seq, "V1@0");
    const ClipId both[] = {plain, placed.video};
    ProjectState m = ok(edit::createAudition(placed.state, mc.seq, both)).state;
    m = ok(edit::setActiveTake(m, mc.seq, plain, 1)).state;
    const Clip& active = clipOf(m, mc.seq, plain);
    CHECK(active.sequence == placed.multicam);
    CHECK(active.angles == AngleSet::of(1));
    CHECK(clipOf(m, mc.seq, clipAt(m, mc.seq, "A1@0")).sequence == placed.multicam);
    // The multicam source plays inside the audition: it cannot be removed.
    CHECK_FALSE(edit::removeSequence(ok(edit::setActiveTake(m, mc.seq, plain, 0)).state, placed.multicam).has_value());
    requireValid(m);
}

TEST_CASE("Auditions 0.6: remove reorder rename duplicate and flatten takes", "[model][auditions]") {
    Three t;
    ProjectState s = ok(edit::renameAuditionTake(t.state, t.tl.seq, t.audition, 1, "Angry read")).state;
    CHECK(names(clipOf(s, t.tl.seq, t.audition)) == std::vector<std::string>{"A", "Angry read", "C"});
    CHECK_FALSE(edit::renameAuditionTake(s, t.tl.seq, t.audition, 1, "").has_value());
    // Move take 3 to the front: take 1 (A) stays active, now as take 2.
    s = ok(edit::moveAuditionTake(s, t.tl.seq, t.audition, 2, 0)).state;
    CHECK(names(clipOf(s, t.tl.seq, t.audition)) == std::vector<std::string>{"C", "A", "Angry read"});
    CHECK(clipOf(s, t.tl.seq, t.audition).audition()->active == 1);
    CHECK_TIMELINE(s, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |Aaaaaa..Dd|
    )");
    // Duplicate the active take: the copy follows it and becomes active.
    s = ok(edit::duplicateAuditionTake(s, t.tl.seq, t.audition, 1)).state;
    CHECK(names(clipOf(s, t.tl.seq, t.audition)) == std::vector<std::string>{"C", "A", "A copy", "Angry read"});
    CHECK(clipOf(s, t.tl.seq, t.audition).audition()->active == 2);
    // Remove the active copy: the next take (Angry read = B) plays.
    s = ok(edit::removeAuditionTake(s, t.tl.seq, t.audition, 2)).state;
    CHECK(names(clipOf(s, t.tl.seq, t.audition)) == std::vector<std::string>{"C", "A", "Angry read"});
    CHECK_TIMELINE(s, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |B5bbbb..Dd|
    )");
    // Remove an inactive one, then another: an audition left with one take is a plain clip.
    s = ok(edit::removeAuditionTake(s, t.tl.seq, t.audition, 0)).state;
    CHECK(takeCount(clipOf(s, t.tl.seq, t.audition)) == 2);
    s = ok(edit::removeAuditionTake(s, t.tl.seq, t.audition, 0)).state;
    CHECK_FALSE(clipOf(s, t.tl.seq, t.audition).isAudition());
    CHECK(clipOf(s, t.tl.seq, t.audition).name() == "Angry read");
    // Flatten keeps the active take only.
    const ClipId one[] = {t.audition};
    const ProjectState flat = ok(edit::flattenAuditions(ok(edit::setActiveTake(t.state, t.tl.seq, t.audition, 2)).state,
                                                        t.tl.seq, one))
                                  .state;
    CHECK_FALSE(clipOf(flat, t.tl.seq, t.audition).isAudition());
    CHECK_TIMELINE(flat, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |C2cccc..Dd|
    )");
    CHECK_FALSE(edit::flattenAuditions(flat, t.tl.seq, one).has_value());
    // One clip alone: Duplicate as Audition (the copy is active and plays the same).
    Timeline single(R"(
        V1 |Aaaa......|
    )");
    const ClipId only = single.clip("V1@0");
    const ProjectState dup = ok(edit::createAudition(single.state, single.seq, std::span(&only, 1))).state;
    CHECK(names(clipOf(dup, single.seq, only)) == std::vector<std::string>{"A", "A copy"});
    CHECK(clipOf(dup, single.seq, only).audition()->active == 1);
    CHECK_TIMELINE(dup, single.seq, "V1 |Aaaa......|");
}

TEST_CASE("Auditions 0.6: a source becomes a take and media used by a take stays in the project", "[model][auditions]") {
    Three t;
    edit::TakeSource source;
    source.media = t.tl.media('E');
    source.sourceIn = f(7);
    source.name = "Wide";
    ProjectState s = ok(edit::addAuditionSource(t.state, t.tl.seq, t.audition, source)).state;
    CHECK(names(clipOf(s, t.tl.seq, t.audition)) == std::vector<std::string>{"A", "B", "C", "Wide"});
    CHECK_TIMELINE(ok(edit::setActiveTake(s, t.tl.seq, t.audition, 3)).state, t.tl.seq, R"(
        V3 |..........|
        V2 |..........|
        V1 |E7eeee..Dd|
    )");
    // B plays only as a take now: removing its media is refused.
    const auto removed = edit::removeMedia(s, t.tl.media('B'));
    REQUIRE_FALSE(removed.has_value());
    CHECK(clipUsesMedia(clipOf(s, t.tl.seq, t.audition), t.tl.media('B')));
    // Add to Audition: clips on the timeline join after the last take and leave the timeline.
    Timeline more(R"(
        V2 |....Ff....|
        V1 |Aaaa......|
    )");
    const ClipId f1[] = {more.clip("V2@4")};
    const ProjectState joined = ok(edit::addAuditionTakes(more.state, more.seq, more.clip("V1@0"), f1)).state;
    CHECK(names(clipOf(joined, more.seq, more.clip("V1@0"))) == std::vector<std::string>{"A", "F"});
    CHECK_TIMELINE(joined, more.seq, R"(
        V2 |..........|
        V1 |Aaaa......|
    )");
}

TEST_CASE("Auditions 0.6: one undo step each and the takes survive save and reopen and the clipboard",
          "[model][auditions]") {
    Three t;
    EditSession session(t.tl.state);
    REQUIRE(session
                .apply("New Audition",
                       [&](const ProjectState& s) {
                           const std::vector<ClipId> ids{t.tl.clip("V1@0"), t.tl.clip("V2@0"), t.tl.clip("V3@0")};
                           return edit::createAudition(s, t.tl.seq, ids);
                       })
                .has_value());
    REQUIRE(session
                .apply("Use Take", [&](const ProjectState& s) { return edit::setActiveTake(s, t.tl.seq, t.audition, 2); })
                .has_value());
    CHECK(session.undoLabel() == std::optional<std::string>("Use Take"));
    REQUIRE(session.undo().has_value());
    CHECK(clipOf(*session.snapshot(), t.tl.seq, t.audition).audition()->active == 0);
    REQUIRE(session.undo().has_value());
    CHECK_FALSE(clipOf(*session.snapshot(), t.tl.seq, t.audition).isAudition());
    CHECK(*session.snapshot()->findSequence(t.tl.seq) == *t.tl.state.findSequence(t.tl.seq));

    // A short take with its uncovered range, saved and opened again: exactly the same project.
    Timeline tl(R"(
        V2 |B2........|
        V1 |Aaaaaa....|
    )",
                {{'B', 4}});
    const ClipId a = tl.clip("V1@0");
    const ClipId ids[] = {a, tl.clip("V2@0")};
    ProjectState s = ok(edit::createAudition(tl.state, tl.seq, ids)).state;
    s = ok(edit::renameAuditionTake(s, tl.seq, a, 0, "Close")).state;
    s = ok(edit::setActiveTake(s, tl.seq, a, 1)).state;
    const auto text = io::toJson(s);
    REQUIRE(text.has_value());
    CHECK(text->find("\"audition\"") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded.has_value());
    requireValid(*loaded);
    CHECK(clipOf(*loaded, tl.seq, a) == clipOf(s, tl.seq, a));
    CHECK_TIMELINE(ok(edit::setActiveTake(*loaded, tl.seq, a, 0)).state, tl.seq, R"(
        V2 |..........|
        V1 |Aaaaaa....|
    )");
    // A project without auditions writes no audition key (older projects load as they were).
    CHECK(io::toJson(tl.state)->find("\"audition\"") == std::string::npos);

    // Copy and paste: the copy is an audition with every take.
    const auto doc = copyClips(s, tl.seq, std::span(&a, 1));
    REQUIRE(doc.has_value());
    const auto json = clipboardToJson(*doc);
    REQUIRE(json.has_value());
    const auto back = clipboardFromJson(*json);
    REQUIRE(back.has_value());
    const auto pasted = pasteClips(s, tl.seq, *back, f(7));
    REQUIRE(pasted.has_value());
    requireValid(pasted->edit.state);
    REQUIRE(pasted->report.pasted.size() == 1);
    const Clip& copy = clipOf(pasted->edit.state, tl.seq, pasted->report.pasted.front());
    CHECK(takeCount(copy) == 2);
    CHECK(names(copy) == std::vector<std::string>{"Close", "B"});
}

// ---- Fix round 1 (ARGON-03): an inactive take is a use everywhere the project decides what is used ----------------

TEST_CASE("Auditions 0.6: a bin whose media or sequence only an inactive take plays is not deleted", "[model][auditions]") {
    Timeline tl(R"(
        V3 |Cccc......|
        V2 |Bbbb......|
        V1 |Aaaa......|
    )");
    // C goes into a sequence of its own first: the audition's third take nests it.
    auto nested = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V3@0")}, "Nest");
    REQUIRE(nested.has_value());
    const ClipId nestClip = nested->clips.front();
    const ClipId a = tl.clip("V1@0");
    const std::vector<ClipId> ids{a, tl.clip("V2@0"), nestClip};
    ProjectState s = ok(edit::createAudition(nested->edit.state, tl.seq, ids)).state;
    requireValid(s);
    REQUIRE(takeCount(clipOf(s, tl.seq, a)) == 3);
    CHECK(clipOf(s, tl.seq, a).media == tl.media('A')); // A is active: B and Nest play only as takes

    // B's media in a bin: deleting the bin is refused (it used to go and leave the take pointing at nothing).
    auto bin = edit::addBin(s, "Alternates");
    REQUIRE(bin.has_value());
    const MediaId b = tl.media('B');
    s = *edit::moveToBin(bin->state, bin->id, {}, std::span(&b, 1), {});
    const BinId binB = bin->id;
    const auto refused = edit::removeBins(s, std::span(&binB, 1));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("'B'") != std::string::npos);
    // The nested sequence in a bin: refused too.
    auto bin2 = edit::addBin(s, "Nests");
    REQUIRE(bin2.has_value());
    const SequenceId nest = nested->nested;
    s = *edit::moveToBin(bin2->state, bin2->id, {}, {}, std::span(&nest, 1));
    const BinId binN = bin2->id;
    const auto refusedNest = edit::removeBins(s, std::span(&binN, 1));
    REQUIRE_FALSE(refusedNest.has_value());
    CHECK(refusedNest.error().message.find("'Nest'") != std::string::npos);
    // The nested sequence's last video track stays while a take plays its picture.
    const TrackId nestV1 = s.findSequence(nest)->videoTracks.front()->id;
    for (std::size_t i = s.findSequence(nest)->videoTracks.size(); i > 1; --i) {
        s = ok(edit::removeTrack(s, nest, s.findSequence(nest)->videoTracks.back()->id)).state;
    }
    CHECK_FALSE(edit::removeTrack(s, nest, nestV1).has_value());

    // Without the takes the bins go, and the project stays valid.
    s = ok(edit::flattenAuditions(s, tl.seq, std::span(&a, 1))).state;
    const BinId both[] = {binB, binN};
    const auto removed = edit::removeBins(s, both);
    REQUIRE(removed.has_value());
    requireValid(*removed);
    CHECK(removed->findMedia(b) == nullptr);
    CHECK(removed->findSequence(nest) == nullptr);
}

TEST_CASE("Auditions 0.6: removing an asset finds it in a take's effects and in Master Clip Effects", "[model][auditions]") {
    Timeline tl(R"(
        V2 |Bbbb......|
        V1 |Aaaa......|
    )");
    ProjectAsset lut{{}, std::string(kAssetKindLut), "Look.cube", {}};
    lut.locator.path = "C:/luts/Look.cube";
    auto asset = edit::addAsset(tl.state, lut);
    REQUIRE(asset.has_value());
    const ClipId bClip = tl.clip("V2@0");
    auto graded = edit::addEffect(asset->state, tl.seq, std::span(&bClip, 1), "wabcut.color.grade");
    REQUIRE(graded.has_value());
    edit::EffectChange look;
    look.data = {EffectData{"creative.look", assetReference(asset->id)}};
    ProjectState s = ok(edit::setEffect(graded->state, tl.seq, bClip, graded->createdEffects.at(0), look)).state;
    const ClipId a = tl.clip("V1@0");
    const std::vector<ClipId> ids{a, bClip};
    s = ok(edit::createAudition(s, tl.seq, ids)).state; // B (with the look) is now the inactive take
    requireValid(s);
    CHECK(clipOf(s, tl.seq, a).effects().empty());

    // Refused while the take refers to it; with clearReferences the take loses the look and keeps its grade.
    const auto refused = edit::removeAsset(s, asset->id);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("Look.cube") != std::string::npos);
    const auto cleared = edit::removeAsset(s, asset->id, true);
    REQUIRE(cleared.has_value());
    requireValid(*cleared);
    CHECK(cleared->assets.empty());
    const Audition* take = clipOf(*cleared, tl.seq, a).audition();
    REQUIRE(take != nullptr);
    REQUIRE(take->others.front().contentOrDefault().effects.size() == 1);
    CHECK(take->others.front().contentOrDefault().effects.front().data.empty());
    CHECK(assetReferenceWarnings(*cleared).empty());

    // Master Clip Effects (card #71) of A refer to it: refused, then cleared.
    ProjectState m = *edit::removeAsset(s, asset->id, true);
    auto readded = edit::addAsset(m, lut);
    REQUIRE(readded.has_value());
    m = readded->state;
    const AssetId again = m.assets.front().id;
    {
        const auto slot = std::find_if(m.media.begin(), m.media.end(), [&](const MediaPtr& p) { return p->id == tl.media('A'); });
        REQUIRE(slot != m.media.end());
        MediaItem item = **slot;
        EffectInstance grade = take->others.front().contentOrDefault().effects.front();
        grade.data = {EffectData{"creative.look", assetReference(again)}};
        item.masterEffects = {grade};
        *slot = std::make_shared<const MediaItem>(std::move(item));
    }
    const auto refusedMaster = edit::removeAsset(m, again);
    REQUIRE_FALSE(refusedMaster.has_value());
    CHECK(refusedMaster.error().message.find("Master Clip Effects") != std::string::npos);
    const auto clearedMaster = edit::removeAsset(m, again, true);
    REQUIRE(clearedMaster.has_value());
    CHECK(clearedMaster->findMedia(tl.media('A'))->masterEffects.front().data.empty());
    CHECK(assetReferenceWarnings(*clearedMaster).empty());
}

TEST_CASE("Auditions 0.6: a merge keeps the media and sequences the merged takes play", "[model][auditions]") {
    Timeline tl(R"(
        V2 |Bbbb......|
        V1 |Aaaa......|
    )");
    const ClipId a = tl.clip("V1@0");
    const std::vector<ClipId> ids{a, tl.clip("V2@0")};
    const ProjectState base = tl.state;
    // Mine makes the audition (B plays only as a take); theirs deletes B's media, which no clip of theirs uses.
    const ProjectState mine = ok(edit::createAudition(base, tl.seq, ids)).state;
    ProjectState theirs = ok(edit::deleteClips(base, tl.seq, std::vector<ClipId>{tl.clip("V2@0")})).state;
    theirs = *edit::removeMedia(theirs, tl.media('B'));
    const auto merged = mergeProjects(base, mine, theirs);
    REQUIRE(merged.has_value());
    REQUIRE_FALSE(merged->invalid.has_value()); // it used to drop B and give up on the merge
    requireValid(merged->merged);
    CHECK(merged->merged.findMedia(tl.media('B')) != nullptr);
    CHECK(clipUsesMedia(clipOf(merged->merged, tl.seq, a), tl.media('B')));
}

// ---- Card 092: a copy of an audition has effect instance ids of its own on every take --------------------------------

namespace {

std::vector<std::uint64_t> takeEffectIds(const Clip& clip) {
    std::vector<std::uint64_t> ids;
    if (const Audition* a = clip.audition()) {
        for (const AuditionTake& take : a->others) {
            if (take.content) {
                for (const EffectInstance& e : take.content->effects) {
                    ids.push_back(e.id.value);
                }
            }
        }
    }
    return ids;
}

/// An audition A (V1) whose other take B carries a blur; the id generator is already far on.
struct BlurTake {
    Timeline tl{R"(
        V2 |Bbbb......|
        V1 |Aaaa......|
    )"};
    ProjectState state;
    ClipId audition;

    BlurTake() {
        tl.state.ids.next = 900;
        const ClipId b = tl.clip("V2@0");
        audition = tl.clip("V1@0");
        state = ok(edit::addEffect(tl.state, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian",
                                   {EffectParam{"radius", ParamValue(9.0)}}))
                    .state;
        const ClipId ids[] = {audition, b};
        state = ok(edit::createAudition(state, tl.seq, ids)).state;
    }
};

} // namespace

TEST_CASE("Auditions 0.6: pasting an audition into another project renews the effect ids of its other takes", "[model][auditions]") {
    BlurTake src;
    const auto original = takeEffectIds(clipOf(src.state, src.tl.seq, src.audition));
    REQUIRE(original.size() == 1);
    CHECK(original.front() >= 900);

    // The destination project has issued far fewer ids than the source did.
    Timeline dest(R"(
        V1 |..........|
    )");
    REQUIRE(dest.state.ids.next < original.front());
    const auto doc = copyClips(src.state, src.tl.seq, std::span(&src.audition, 1));
    REQUIRE(doc.has_value());
    const auto json = clipboardToJson(*doc);
    REQUIRE(json.has_value());
    const auto back = clipboardFromJson(*json);
    REQUIRE(back.has_value());
    const auto pasted = pasteClips(dest.state, dest.seq, *back, f(2));
    REQUIRE(pasted.has_value());
    requireValid(pasted->edit.state); // before the fix: "effect N has an id that is not below the id generator"
    REQUIRE(pasted->report.pasted.size() == 1);
    const Clip& copy = clipOf(pasted->edit.state, dest.seq, pasted->report.pasted.front());
    REQUIRE(takeCount(copy) == 2);
    const auto ids = takeEffectIds(copy);
    REQUIRE(ids.size() == 1);
    CHECK(ids.front() < pasted->edit.state.ids.next);
    // The take kept its effect (name and parameter), only the instance id is new; the new id is reported as created.
    const AuditionTake& take = copy.audition()->others.front();
    REQUIRE(take.content != nullptr);
    CHECK(take.content->effects.front().effect == "wabcut.blur.gaussian");
    CHECK(std::ranges::find(pasted->edit.createdEffects, EffectInstanceId{ids.front()}) != pasted->edit.createdEffects.end());
    // ... and the project saves and loads again.
    const auto saved = io::toJson(pasted->edit.state);
    REQUIRE(saved.has_value());
    CHECK(io::fromJson(*saved).has_value());
}

TEST_CASE("Auditions 0.6: a paste in the same project and an Alt-drag copy do not share effect ids with the original", "[model][auditions]") {
    BlurTake src;
    const auto original = takeEffectIds(clipOf(src.state, src.tl.seq, src.audition));
    REQUIRE(original.size() == 1);

    // Paste (clipboard round trip) in the same project.
    const auto doc = copyClips(src.state, src.tl.seq, std::span(&src.audition, 1));
    REQUIRE(doc.has_value());
    const auto pasted = pasteClips(src.state, src.tl.seq, *doc, f(5));
    REQUIRE(pasted.has_value());
    requireValid(pasted->edit.state);
    REQUIRE(pasted->report.pasted.size() == 1);
    const auto pastedIds = takeEffectIds(clipOf(pasted->edit.state, src.tl.seq, pasted->report.pasted.front()));
    REQUIRE(pastedIds.size() == 1);
    CHECK(pastedIds.front() != original.front());
    CHECK(takeEffectIds(clipOf(pasted->edit.state, src.tl.seq, src.audition)) == original); // the original is untouched

    // Alt-drag copy (MoveOps duplicateClips).
    const auto dup = ok(edit::duplicateClips(src.state, src.tl.seq, std::span(&src.audition, 1), f(5)));
    REQUIRE(dup.created.size() == 1);
    const auto dupIds = takeEffectIds(clipOf(dup.state, src.tl.seq, dup.created.front()));
    REQUIRE(dupIds.size() == 1);
    CHECK(dupIds.front() != original.front());
    CHECK(dupIds.front() < dup.state.ids.next);
    CHECK(takeEffectIds(clipOf(dup.state, src.tl.seq, src.audition)) == original);
    CHECK(std::ranges::find(dup.createdEffects, EffectInstanceId{dupIds.front()}) != dup.createdEffects.end());
    // The copy's other take plays what the original's does: only the id differs.
    const Clip& copyClip = clipOf(dup.state, src.tl.seq, dup.created.front());
    CHECK(copyClip.audition()->others.front().content->effects.front().effect == "wabcut.blur.gaussian");
}
