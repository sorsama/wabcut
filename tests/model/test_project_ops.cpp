// Project-level operations: bins, media, sequences, tracks and clip attributes.

#include "TimelineFixture.h"

#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("A new project starts empty and valid", "[model][project]") {
    const ProjectState empty;
    requireValid(empty);
    CHECK(empty.media.empty());
    CHECK(empty.sequences.empty());
}

TEST_CASE("Bins nest and must have an existing parent", "[model][project]") {
    ProjectState s;
    const auto footage = edit::addBin(s, "Footage");
    REQUIRE(footage);
    const auto dayOne = edit::addBin(footage->state, "Day 1", footage->id);
    REQUIRE(dayOne);
    requireValid(dayOne->state);
    CHECK(dayOne->state.findBin(dayOne->id)->parent == footage->id);
    CHECK_FALSE(edit::addBin(s, "Lost", BinId{12345}));
}

TEST_CASE("Media items get fresh sorted ids and cannot be removed while used", "[model][project]") {
    Timeline tl(R"(
        V1 |Aa|
    )");
    MediaItem item;
    item.name = "interview.mov";
    item.locator.path = "D:/footage/interview.mov";
    item.id = MediaId{1}; // ignored
    const auto added = edit::addMedia(tl.state, item);
    REQUIRE(added);
    CHECK(added->id.value >= tl.state.ids.next);
    CHECK(added->state.media.back()->id == added->id);
    requireValid(added->state);

    const auto used = edit::removeMedia(added->state, tl.media('A'));
    REQUIRE_FALSE(used);
    CHECK(used.error().code == ErrorCode::InvalidArgument);

    const auto removed = edit::removeMedia(added->state, added->id);
    REQUIRE(removed);
    CHECK(removed->findMedia(added->id) == nullptr);
    CHECK_FALSE(edit::removeMedia(*removed, added->id));
}

TEST_CASE("New sequences get Premiere's default tracks", "[model][project]") {
    edit::SequenceSettings settings;
    settings.frameRate = kRate29_97;
    settings.dropFrame = true;
    const auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    requireValid(created->state);
    const Sequence& s = *created->state.findSequence(created->id);
    CHECK(s.videoTracks.size() == 3);
    CHECK(s.audioTracks.size() == 3);
    CHECK(s.videoTracks[0]->targeted);
    CHECK(s.audioTracks[0]->targeted);
    CHECK_FALSE(s.videoTracks[1]->targeted);
    CHECK(s.videoTracks[0]->syncLocked);
    CHECK(s.dropFrame);

    settings.frameRate = Rate{13, 1}; // 1/13 s is not a whole number of ticks
    CHECK_FALSE(edit::addSequence(ProjectState{}, settings));
    settings.frameRate = kRate25;
    settings.width = 0;
    CHECK_FALSE(edit::addSequence(ProjectState{}, settings));

    const auto removed = edit::removeSequence(created->state, created->id);
    REQUIRE(removed);
    CHECK(removed->sequences.empty());
}

TEST_CASE("Adding and removing tracks", "[model][project]") {
    Timeline tl(R"(
        V1 |Aaaa|
        A1 |Aaaa|
    )");
    tl.linkMatching();

    const auto added = edit::addTrack(tl.state, tl.seq, TrackKind::Video, "Titles");
    REQUIRE(added);
    requireValid(added->state);
    const Sequence& withV2 = *added->state.findSequence(tl.seq);
    REQUIRE(withV2.videoTracks.size() == 2);
    CHECK(withV2.videoTracks[1]->id == added->id);
    CHECK(withV2.videoTracks[1]->name == "Titles");

    // Deleting A1 deletes its clip and unlinks the video clip it was linked to.
    const auto removed = ok(edit::removeTrack(tl.state, tl.seq, tl.track("A1")));
    CHECK(removed.state.findSequence(tl.seq)->audioTracks.empty());
    CHECK(removed.removed.size() == 1);
    CHECK(links(removed.state, tl.seq).empty());
}

TEST_CASE("Track header toggles", "[model][project]") {
    Timeline tl(R"(
        V1 |AaBb|
    )");
    edit::TrackStateChange lock;
    lock.locked = true;
    lock.targeted = true;
    lock.name = "Main";
    const auto locked = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), lock));
    const Track& t = *locked.state.findSequence(tl.seq)->videoTracks[0];
    CHECK(t.locked);
    CHECK(t.targeted);
    CHECK(t.name == "Main");
    CHECK(t.syncLocked); // untouched fields keep their value

    // Locked: edits are refused, but the lock itself can be toggled off.
    CHECK_FALSE(edit::deleteClips(locked.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}));
    edit::TrackStateChange unlock;
    unlock.locked = false;
    const auto unlocked = ok(edit::setTrackState(locked.state, tl.seq, tl.track("V1"), unlock));
    CHECK(ok(edit::deleteClips(unlocked.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")})).removed.size() == 1);
}

TEST_CASE("Clip attributes: name, label colour, enable and effect properties", "[model][project]") {
    Timeline tl(R"(
        V1 |AaBb|
    )");
    const std::vector<ClipId> a{tl.clip("V1@0")};
    ClipProperties props;
    props.set(ParamId::Opacity, 0.5);
    props.set(ParamId::PositionX, 120.0);
    props.set(ParamId::PositionY, -40.5);
    props.set(ParamId::VolumeDb, -6.0);
    edit::ClipAttributeChange change;
    change.name = "Wide shot";
    change.colorIndex = 3;
    change.enabled = false;
    change.properties = props;
    const auto r = ok(edit::setClipAttributes(tl.state, tl.seq, a, change));
    const Clip& c = r.state.findSequence(tl.seq)->findClip(a[0])->clip();
    CHECK(c.name() == "Wide shot");
    CHECK(c.colorIndex == 3);
    CHECK_FALSE(c.enabled);
    CHECK(c.properties() == props);
    CHECK(c.properties().value(ParamId::Opacity, f(1)) == 0.5);

    edit::ClipAttributeChange bad;
    bad.properties = ClipProperties{};
    bad.properties->set(ParamId::Pan, 2.0);
    CHECK_FALSE(edit::setClipAttributes(tl.state, tl.seq, a, bad));
}
