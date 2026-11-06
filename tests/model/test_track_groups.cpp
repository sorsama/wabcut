// 0.7 (batch A9, card 124): track groups - a named set of video or audio tracks (Sequence::trackGroups). The operations keep
// a group's tracks next to each other, a track is in at most one group, every change is one edit (one undo step), the
// switches apply to every track of the group, track reordering and deleting keep the groups right, and the groups are saved
// with the project (an optional key, written only when there are some).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/SubmixOps.h>
#include <wabcut/model/TrackGroups.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V2 |..Bbb...|
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
    A2 |.Bbbbbb.|
    A3 |..Cccc..|
    A4 |........|
)";

std::vector<TrackId> audioOrder(const Sequence& s) {
    std::vector<TrackId> out;
    for (const TrackPtr& t : s.audioTracks) {
        out.push_back(t->id);
    }
    return out;
}

std::vector<TrackId> ids(const Timeline& tl, std::initializer_list<const char*> labels) {
    std::vector<TrackId> out;
    for (const char* label : labels) {
        out.push_back(tl.track(label));
    }
    return out;
}

const TrackGroup& groupOf(const ProjectState& s, const Timeline& tl, TrackGroupId id) {
    const TrackGroup* group = s.findSequence(tl.seq)->findTrackGroup(id);
    REQUIRE(group != nullptr);
    return *group;
}

const Track& trackOf(const ProjectState& s, const Timeline& tl, const char* label) {
    return *s.findSequence(tl.seq)->findTrack(tl.track(label));
}

/// A1 and A3 grouped as "Dialogue", colour 4.
struct Grouped {
    Timeline tl{kPicture};
    ProjectState state;
    TrackGroupId group;

    Grouped() {
        const std::vector<TrackId> tracks = ids(tl, {"A1", "A3"});
        const edit::EditResult made = ok(edit::createTrackGroup(tl.state, tl.seq, tracks, "Dialogue", 4));
        state = made.state;
        group = made.createdTrackGroup;
    }
};

} // namespace

TEST_CASE("Group Tracks gathers the tracks at the first one, in stack order, and names the group", "[model][trackgroup]") {
    Timeline tl(kPicture);
    const std::vector<TrackId> tracks = ids(tl, {"A3", "A1"}); // the order asked for does not matter
    const edit::EditResult made = ok(edit::createTrackGroup(tl.state, tl.seq, tracks, "Dialogue", 4));
    REQUIRE(made.createdTrackGroup.isValid());
    const Sequence& seq = *made.state.findSequence(tl.seq);
    REQUIRE(seq.trackGroups.size() == 1);
    const TrackGroup& group = seq.trackGroups.front();
    CHECK(group.id == made.createdTrackGroup);
    CHECK(group.name == "Dialogue");
    CHECK(group.kind == TrackKind::Audio);
    CHECK(group.colorIndex == 4);
    CHECK_FALSE(group.collapsed);
    CHECK(group.members == ids(tl, {"A1", "A3"}));
    // A3 came up next to A1; A2 and A4 kept their order; no track was lost.
    CHECK(audioOrder(seq) == ids(tl, {"A1", "A3", "A2", "A4"}));
    CHECK(seq.trackGroupOf(tl.track("A3")) == &group);
    CHECK(seq.trackGroupOf(tl.track("A2")) == nullptr);
    // The clips did not move, and the original state is untouched.
    CHECK(trackOf(made.state, tl, "A3").clips.size() == 1);
    CHECK(tl.sequence().trackGroups.empty());
    // The ChangeSet says the track list changed (the headers redraw).
    const SequenceChange* change = made.changes.sequence(tl.seq);
    REQUIRE(change != nullptr);
    CHECK(change->trackListChanged);

    // A video group is a group too; names are "Group N" by default, the first number not taken.
    const std::vector<TrackId> video = ids(tl, {"V1", "V2"});
    const edit::EditResult second = ok(edit::createTrackGroup(made.state, tl.seq, video));
    CHECK(groupOf(second.state, tl, second.createdTrackGroup).name == "Group 1");
    CHECK(groupOf(second.state, tl, second.createdTrackGroup).kind == TrackKind::Video);
    const std::vector<TrackId> a4 = ids(tl, {"A4"});
    const edit::EditResult third = ok(edit::createTrackGroup(second.state, tl.seq, a4, "  ")); // blank: the default
    CHECK(groupOf(third.state, tl, third.createdTrackGroup).name == "Group 2");
}

TEST_CASE("Group Tracks refuses what cannot be a group, and changes nothing", "[model][trackgroup]") {
    Timeline tl(kPicture);
    const std::vector<TrackId> none;
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, none).has_value());
    const std::vector<TrackId> mixed = ids(tl, {"V1", "A1"});
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, mixed).has_value());
    const std::vector<TrackId> twice = ids(tl, {"A1", "A1"});
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, twice).has_value());
    const std::array<TrackId, 1> unknown{TrackId{987654}};
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, unknown).has_value());
    const std::vector<TrackId> a1 = ids(tl, {"A1"});
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, a1, "x", 16).has_value());   // colour outside the labels
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, a1, std::string(129, 'x')).has_value()); // name too long
    CHECK_FALSE(edit::createTrackGroup(tl.state, tl.seq, a1, "bad \xC3\x28 utf8").has_value());

    // A submix track cannot be grouped.
    const ProjectState withSubmix = ok(edit::addSubmixTrack(tl.state, tl.seq, ChannelFormat::Stereo, "Bus")).state;
    const std::array<TrackId, 1> bus{withSubmix.findSequence(tl.seq)->submixTracks.front()->id};
    CHECK_FALSE(edit::createTrackGroup(withSubmix, tl.seq, bus).has_value());

    // A track already in a group: take it out of that one first.
    Grouped g;
    const std::vector<TrackId> again = ids(g.tl, {"A3", "A4"});
    const auto refused = edit::createTrackGroup(g.state, g.tl.seq, again);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("Dialogue") != std::string::npos);
}

TEST_CASE("A track group is renamed, coloured and collapsed in one edit each", "[model][trackgroup]") {
    Grouped g;
    edit::TrackGroupChange change;
    change.name = "  Voices  ";
    change.colorIndex = 9;
    change.collapsed = true;
    const edit::EditResult changed = ok(edit::setTrackGroup(g.state, g.tl.seq, g.group, change));
    const TrackGroup& group = groupOf(changed.state, g.tl, g.group);
    CHECK(group.name == "Voices");
    CHECK(group.colorIndex == 9);
    CHECK(group.collapsed);
    CHECK(group.members == ids(g.tl, {"A1", "A3"})); // the tracks are untouched
    CHECK(changed.changes.sequence(g.tl.seq) != nullptr);

    // The same values again, or nothing: no change at all.
    const edit::EditResult same = ok(edit::setTrackGroup(changed.state, g.tl.seq, g.group, change));
    CHECK(diff(changed.state, same.state).empty());
    CHECK(diff(g.state, ok(edit::setTrackGroup(g.state, g.tl.seq, g.group, edit::TrackGroupChange{})).state).empty());

    // Refused: an empty name, a colour outside the labels, an unknown group.
    edit::TrackGroupChange bad;
    bad.name = "   ";
    CHECK_FALSE(edit::setTrackGroup(g.state, g.tl.seq, g.group, bad).has_value());
    bad = {};
    bad.colorIndex = 16;
    CHECK_FALSE(edit::setTrackGroup(g.state, g.tl.seq, g.group, bad).has_value());
    CHECK_FALSE(edit::setTrackGroup(g.state, g.tl.seq, TrackGroupId{424242}, change).has_value());

    // Colour -1 clears it.
    bad = {};
    bad.colorIndex = -1;
    CHECK(groupOf(ok(edit::setTrackGroup(changed.state, g.tl.seq, g.group, bad)).state, g.tl, g.group).colorIndex == -1);
}

TEST_CASE("A group's switches reach every track of the group in one edit", "[model][trackgroup]") {
    Grouped g;
    edit::TrackStateChange mute;
    mute.muted = true;
    const edit::EditResult muted = ok(edit::setTrackGroupTracksState(g.state, g.tl.seq, g.group, mute));
    CHECK(trackOf(muted.state, g.tl, "A1").muted);
    CHECK(trackOf(muted.state, g.tl, "A3").muted);
    CHECK_FALSE(trackOf(muted.state, g.tl, "A2").muted); // not in the group
    CHECK_FALSE(trackOf(muted.state, g.tl, "A4").muted);

    // Already muted: nothing changes (no empty undo step).
    CHECK(diff(muted.state, ok(edit::setTrackGroupTracksState(muted.state, g.tl.seq, g.group, mute)).state).empty());

    // Solo, lock and target work the same way, and a locked track takes the change (that is how it gets unlocked).
    edit::TrackStateChange lock;
    lock.locked = true;
    lock.solo = true;
    lock.targeted = true;
    const edit::EditResult locked = ok(edit::setTrackGroupTracksState(muted.state, g.tl.seq, g.group, lock));
    CHECK(trackOf(locked.state, g.tl, "A1").locked);
    CHECK(trackOf(locked.state, g.tl, "A3").solo);
    CHECK(trackOf(locked.state, g.tl, "A3").targeted);
    lock.locked = false;
    CHECK_FALSE(trackOf(ok(edit::setTrackGroupTracksState(locked.state, g.tl.seq, g.group, lock)).state, g.tl, "A3").locked);

    // A change the tracks refuse is refused whole: the first track's error, no partial result.
    edit::TrackStateChange role;
    role.role = TrackRole::Dialogue;
    CHECK(ok(edit::setTrackGroupTracksState(g.state, g.tl.seq, g.group, role)).state.findSequence(g.tl.seq)->findTrack(g.tl.track("A3"))->role ==
          TrackRole::Dialogue);
    const std::vector<TrackId> videoTracks = ids(g.tl, {"V1", "V2"});
    const edit::EditResult videoGroup = ok(edit::createTrackGroup(g.state, g.tl.seq, videoTracks, "Picture"));
    CHECK_FALSE(edit::setTrackGroupTracksState(videoGroup.state, g.tl.seq, videoGroup.createdTrackGroup, role).has_value());
}

TEST_CASE("Add to Group and Remove from Group keep the group together", "[model][trackgroup]") {
    Grouped g; // order: A1 A3 A2 A4, group A1 A3
    const std::vector<TrackId> a4 = ids(g.tl, {"A4"});
    const edit::EditResult added = ok(edit::addTracksToTrackGroup(g.state, g.tl.seq, g.group, a4));
    CHECK(groupOf(added.state, g.tl, g.group).members == ids(g.tl, {"A1", "A3", "A4"}));
    CHECK(audioOrder(*added.state.findSequence(g.tl.seq)) == ids(g.tl, {"A1", "A3", "A4", "A2"})); // A4 moved next to A3
    // A track already in the group, a video track and an unknown one.
    const std::vector<TrackId> a1 = ids(g.tl, {"A1"});
    CHECK(diff(g.state, ok(edit::addTracksToTrackGroup(g.state, g.tl.seq, g.group, a1)).state).empty());
    const std::vector<TrackId> v1 = ids(g.tl, {"V1"});
    CHECK_FALSE(edit::addTracksToTrackGroup(g.state, g.tl.seq, g.group, v1).has_value());
    CHECK_FALSE(edit::addTracksToTrackGroup(g.state, g.tl.seq, TrackGroupId{424242}, a4).has_value());

    // A track of another group leaves that group when it joins this one.
    const std::vector<TrackId> a2a4 = ids(g.tl, {"A2", "A4"});
    const edit::EditResult other = ok(edit::createTrackGroup(g.state, g.tl.seq, a2a4, "SFX"));
    const edit::EditResult moved = ok(edit::addTracksToTrackGroup(other.state, g.tl.seq, g.group, a4));
    CHECK(groupOf(moved.state, g.tl, other.createdTrackGroup).members == ids(g.tl, {"A2"}));
    CHECK(groupOf(moved.state, g.tl, g.group).members == ids(g.tl, {"A1", "A3", "A4"}));

    // Remove from Group: a track at the end just leaves; one from the middle moves to the end so the rest stay together.
    const std::vector<TrackId> a3 = ids(g.tl, {"A3"});
    const edit::EditResult left = ok(edit::removeTracksFromTrackGroups(added.state, g.tl.seq, a3));
    CHECK(groupOf(left.state, g.tl, g.group).members == ids(g.tl, {"A1", "A4"}));
    CHECK(left.state.findSequence(g.tl.seq)->trackGroupOf(g.tl.track("A3")) == nullptr);
    CHECK(audioOrder(*left.state.findSequence(g.tl.seq)) == ids(g.tl, {"A1", "A4", "A3", "A2"}));
    // The last track out dissolves the group; tracks in no group are ignored.
    const std::vector<TrackId> both = ids(g.tl, {"A1", "A3", "A2"});
    const edit::EditResult empty = ok(edit::removeTracksFromTrackGroups(g.state, g.tl.seq, both));
    CHECK(empty.state.findSequence(g.tl.seq)->trackGroups.empty());
    const std::vector<TrackId> a2 = ids(g.tl, {"A2"});
    CHECK(diff(g.state, ok(edit::removeTracksFromTrackGroups(g.state, g.tl.seq, a2)).state).empty());
}

TEST_CASE("Ungroup removes the group and keeps its tracks where they are", "[model][trackgroup]") {
    Grouped g;
    const edit::EditResult gone = ok(edit::ungroupTracks(g.state, g.tl.seq, g.group));
    CHECK(gone.state.findSequence(g.tl.seq)->trackGroups.empty());
    CHECK(audioOrder(*gone.state.findSequence(g.tl.seq)) == audioOrder(*g.state.findSequence(g.tl.seq)));
    CHECK_FALSE(edit::ungroupTracks(gone.state, g.tl.seq, g.group).has_value());
}

TEST_CASE("Moving and deleting tracks keep the groups right", "[model][trackgroup]") {
    Timeline tl(kPicture);
    const std::vector<TrackId> three = ids(tl, {"A1", "A2", "A3"});
    const edit::EditResult made = ok(edit::createTrackGroup(tl.state, tl.seq, three, "Dialogue"));
    const TrackGroupId group = made.createdTrackGroup;

    // A member moved inside the group stays in it (A1 to third place: A2 A3 A1 A4).
    const edit::EditResult inside = ok(edit::moveTrack(made.state, tl.seq, tl.track("A1"), 3));
    CHECK(audioOrder(*inside.state.findSequence(tl.seq)) == ids(tl, {"A2", "A3", "A1", "A4"}));
    CHECK(groupOf(inside.state, tl, group).members == ids(tl, {"A2", "A3", "A1"})); // listed in stack order
    // Dragged away from its group (A1 to the end: A4 sits between), it leaves; the others stay together.
    const edit::EditResult away = ok(edit::moveTrack(made.state, tl.seq, tl.track("A1"), 5)); // 5: past the end is the end
    CHECK(audioOrder(*away.state.findSequence(tl.seq)) == ids(tl, {"A2", "A3", "A4", "A1"}));
    CHECK(groupOf(away.state, tl, group).members == ids(tl, {"A2", "A3"}));
    CHECK(away.state.findSequence(tl.seq)->trackGroupOf(tl.track("A1")) == nullptr);
    // A track dropped between two members joins the group.
    const edit::EditResult into = ok(edit::moveTrack(away.state, tl.seq, tl.track("A4"), 2));
    CHECK(audioOrder(*into.state.findSequence(tl.seq)) == ids(tl, {"A2", "A4", "A3", "A1"}));
    CHECK(groupOf(into.state, tl, group).members == ids(tl, {"A2", "A4", "A3"}));
    CHECK(into.state.findSequence(tl.seq)->trackGroupOf(tl.track("A4")) != nullptr);
    // A track dropped next to the group but not between its tracks does not.
    const edit::EditResult beside = ok(edit::moveTrack(away.state, tl.seq, tl.track("A4"), 1));
    CHECK(beside.state.findSequence(tl.seq)->trackGroupOf(tl.track("A4")) == nullptr);
    CHECK(groupOf(beside.state, tl, group).members == ids(tl, {"A2", "A3"}));

    // Add Tracks next to a group's tracks: one inserted between two of them joins the group, one at its edge does not.
    const auto between = edit::insertTrack(made.state, tl.seq, TrackKind::Audio, 1, "Between");
    REQUIRE(between);
    CHECK(groupOf(between->state, tl, group).members.size() == 4);
    CHECK(between->state.findSequence(tl.seq)->trackGroupOf(between->id) != nullptr);
    const auto edge = edit::insertTrack(made.state, tl.seq, TrackKind::Audio, 0, "Edge");
    REQUIRE(edge);
    CHECK(groupOf(edge->state, tl, group).members.size() == 3);
    CHECK(edge->state.findSequence(tl.seq)->trackGroupOf(edge->id) == nullptr);

    // Deleting a member takes it out; deleting the last one dissolves the group.
    const edit::EditResult deleted = ok(edit::removeTrack(made.state, tl.seq, tl.track("A2")));
    CHECK(groupOf(deleted.state, tl, group).members == ids(tl, {"A1", "A3"}));
    Timeline solo(kPicture);
    const std::vector<TrackId> a4 = ids(solo, {"A4"});
    const edit::EditResult single = ok(edit::createTrackGroup(solo.state, solo.seq, a4, "One"));
    CHECK(ok(edit::removeTrack(single.state, solo.seq, solo.track("A4"))).state.findSequence(solo.seq)->trackGroups.empty());
}

TEST_CASE("An audio group routes its tracks to the Mix or to a new submix", "[model][trackgroup]") {
    Grouped g;
    const edit::EditResult routed = ok(edit::routeTrackGroupToNewSubmix(g.state, g.tl.seq, g.group));
    REQUIRE(routed.createdTracks.size() == 1);
    const Sequence& seq = *routed.state.findSequence(g.tl.seq);
    const Track* submix = seq.findSubmix(routed.createdTracks.front());
    REQUIRE(submix != nullptr);
    CHECK(submix->name == "Dialogue"); // named after the group
    CHECK(trackOf(routed.state, g.tl, "A1").output == submix->id);
    CHECK(trackOf(routed.state, g.tl, "A3").output == submix->id);
    CHECK(trackOf(routed.state, g.tl, "A2").output == TrackId{}); // not in the group: it still goes to the Mix

    const edit::EditResult back = ok(edit::routeTrackGroupToSubmix(routed.state, g.tl.seq, g.group, TrackId{}));
    CHECK(trackOf(back.state, g.tl, "A1").output == TrackId{});
    CHECK(diff(back.state, ok(edit::routeTrackGroupToSubmix(back.state, g.tl.seq, g.group, TrackId{})).state).empty());

    const std::vector<TrackId> video = ids(g.tl, {"V1", "V2"});
    const edit::EditResult videoGroup = ok(edit::createTrackGroup(g.state, g.tl.seq, video, "Picture"));
    CHECK_FALSE(edit::routeTrackGroupToNewSubmix(videoGroup.state, g.tl.seq, videoGroup.createdTrackGroup).has_value());
    CHECK_FALSE(edit::routeTrackGroupToSubmix(videoGroup.state, g.tl.seq, videoGroup.createdTrackGroup, TrackId{}).has_value());
}

TEST_CASE("Track groups are saved only when there are some, and load back", "[model][trackgroup][io]") {
    Timeline tl(kPicture);
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("trackGroups") == std::string::npos);

    Grouped g;
    edit::TrackGroupChange fold;
    fold.collapsed = true;
    const ProjectState folded = ok(edit::setTrackGroup(g.state, g.tl.seq, g.group, fold)).state;
    const auto saved = io::toJson(folded);
    REQUIRE(saved);
    CHECK(saved->find("\"trackGroups\"") != std::string::npos);
    CHECK(saved->find("\"Dialogue\"") != std::string::npos);

    const auto back = io::fromJson(*saved);
    REQUIRE(back);
    const TrackGroup& group = groupOf(*back, g.tl, g.group);
    CHECK(group.name == "Dialogue");
    CHECK(group.colorIndex == 4);
    CHECK(group.collapsed);
    CHECK(group.kind == TrackKind::Audio);
    CHECK(group.members == ids(g.tl, {"A1", "A3"}));
    CHECK(*io::toJson(*back) == *saved);

    // Ungrouped again: the key is gone and the file is the one it was.
    const ProjectState ungrouped = ok(edit::ungroupTracks(folded, g.tl.seq, g.group)).state;
    CHECK(io::toJson(ungrouped)->find("trackGroups") == std::string::npos);

    // A file whose group names a missing track, a track of the other kind, one track twice or no track at all is refused.
    const auto loads = [&](const std::vector<TrackId>& members) {
        Sequence copy = *g.state.findSequence(g.tl.seq);
        copy.trackGroups.front().members = members;
        const ProjectState changed = g.state.withSequence(std::make_shared<const Sequence>(std::move(copy)));
        const auto text = io::toJson(changed);
        return text && io::fromJson(*text).has_value();
    };
    CHECK_FALSE(loads({TrackId{987654}}));
    CHECK_FALSE(loads({g.tl.track("V1")}));
    CHECK_FALSE(loads({g.tl.track("A1"), g.tl.track("A1")}));
    CHECK_FALSE(loads({}));
    CHECK(loads({g.tl.track("A1")}));
}

TEST_CASE("A copy of a sequence keeps its track groups, with the copy's own track ids", "[model][trackgroup]") {
    Grouped g;
    const auto copy = edit::duplicateSequence(g.state, g.tl.seq, "Copy");
    REQUIRE(copy);
    requireValid(copy->state);
    const Sequence* duplicate = copy->state.findSequence(copy->id);
    REQUIRE(duplicate != nullptr);
    REQUIRE(duplicate->trackGroups.size() == 1);
    CHECK(duplicate->trackGroups.front().name == "Dialogue");
    CHECK(duplicate->trackGroups.front().members.size() == 2);
    CHECK(duplicate->trackGroups.front().id != g.group); // ids are the project's: never shared between sequences
    for (const TrackId member : duplicate->trackGroups.front().members) {
        CHECK(duplicate->findTrack(member) != nullptr);
        CHECK(g.state.findSequence(g.tl.seq)->findTrack(member) == nullptr);
    }
}

TEST_CASE("pruneTrackGroups and trackGroupsProblem judge a sequence's groups", "[model][trackgroup]") {
    Grouped g;
    Sequence seq = *g.state.findSequence(g.tl.seq);
    CHECK_FALSE(edit::trackGroupsProblem(seq).has_value());
    CHECK_FALSE(edit::pruneTrackGroups(seq));

    Sequence missing = seq;
    missing.trackGroups.front().members.push_back(TrackId{987654});
    CHECK(edit::trackGroupsProblem(missing).has_value());
    CHECK(edit::pruneTrackGroups(missing));
    CHECK_FALSE(edit::trackGroupsProblem(missing).has_value());
    CHECK(missing.trackGroups.front().members == ids(g.tl, {"A1", "A3"}));

    Sequence emptied = seq;
    emptied.trackGroups.front().members = {TrackId{987654}};
    CHECK(edit::pruneTrackGroups(emptied));
    CHECK(emptied.trackGroups.empty());

    Sequence shared = seq;
    TrackGroup second = seq.trackGroups.front();
    second.id = TrackGroupId{seq.trackGroups.front().id.value + 1000};
    shared.trackGroups.push_back(second); // the same tracks in two groups
    CHECK(edit::trackGroupsProblem(shared).has_value());

    Sequence unnamed = seq;
    unnamed.trackGroups.front().name.clear();
    CHECK(edit::trackGroupsProblem(unnamed).has_value());
    Sequence wrongKind = seq;
    wrongKind.trackGroups.front().kind = TrackKind::Video;
    CHECK(edit::trackGroupsProblem(wrongKind).has_value());
}

TEST_CASE("Group edits are single steps in the edit session: undo and redo restore them", "[model][trackgroup][undo]") {
    Timeline tl(kPicture);
    EditSession session(tl.state);
    const std::vector<TrackId> tracks = ids(tl, {"A1", "A2"});
    TrackGroupId group;
    REQUIRE(session.apply("Group Tracks", [&](const ProjectState& state) {
        auto made = edit::createTrackGroup(state, tl.seq, tracks, "Dialogue");
        if (made) {
            group = made->createdTrackGroup;
        }
        return made;
    }));
    CHECK(session.snapshot()->findSequence(tl.seq)->trackGroups.size() == 1);
    edit::TrackGroupChange fold;
    fold.collapsed = true;
    REQUIRE(session.apply("Collapse Group", [&](const ProjectState& state) { return edit::setTrackGroup(state, tl.seq, group, fold); }));
    CHECK(session.snapshot()->findSequence(tl.seq)->trackGroups.front().collapsed);
    REQUIRE(session.undo());
    CHECK_FALSE(session.snapshot()->findSequence(tl.seq)->trackGroups.front().collapsed);
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findSequence(tl.seq)->trackGroups.empty());
    REQUIRE(session.redo());
    REQUIRE(session.redo());
    CHECK(session.snapshot()->findSequence(tl.seq)->trackGroups.front().collapsed);
}
