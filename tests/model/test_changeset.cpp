// ChangeSets: what each edit, undo and redo reports, so views can update only what changed.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::vector<ClipId> sorted(std::vector<ClipId> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<ClipId> refs(const ProjectState& state, SequenceId seq, std::initializer_list<const char*> clipRefs) {
    std::vector<ClipId> out;
    for (const char* r : clipRefs) {
        out.push_back(clipAt(state, seq, r));
    }
    return sorted(std::move(out));
}

const SequenceChange& only(const edit::EditResult& r, SequenceId seq) {
    REQUIRE(r.changes.sequences.size() == 1);
    const SequenceChange* change = r.changes.sequence(seq);
    REQUIRE(change);
    return *change;
}

} // namespace

TEST_CASE("An insert reports new clips, moved clips and the affected time per track", "[model][changeset]") {
    Timeline tl(R"(
        V2     |..Cc..|
        V1 tgt |AaaaBb|
        A1 tgt |AaaaBb|
    )");
    tl.linkMatching();
    edit::SourceClip source{tl.media('S'), {f(0), f(2)},
                            {{{StreamKind::Video, 0}, tl.track("V1")}, {{StreamKind::Audio, 0}, tl.track("A1")}}};
    const auto r = ok(edit::insertEdit(tl.state, tl.seq, source, f(2)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |....Cc..|
        V1 |AaSsA2Bb|
        A1 |AaSsA2Bb|
    )");
    const SequenceChange& c = only(r, tl.seq);
    // New: the inserted clips and the right halves of the cut clips. Changed: A (shorter), B and C (moved).
    CHECK(sorted(c.addedClips) == refs(r.state, tl.seq, {"V1@2", "A1@2", "V1@4", "A1@4"}));
    CHECK(sorted(c.modifiedClips) == refs(r.state, tl.seq, {"V1@0", "A1@0", "V1@6", "A1@6", "V2@4"}));
    CHECK(c.removedClips.empty());
    CHECK(c.roles == PropertyRoles(PropertyRole::Timing));
    REQUIRE(c.track(tl.track("V1")));
    CHECK(c.track(tl.track("V1"))->ranges == std::vector<TimeRange>{TimeRange::fromStartEnd(f(0), f(8))});
    CHECK(c.track(tl.track("V2"))->ranges == std::vector<TimeRange>{TimeRange::fromStartEnd(f(2), f(6))});
    CHECK(c.affectedTracks().size() == 3);
    CHECK(c.extentChanged); // the sequence grew from 6 to 8 frames
}

TEST_CASE("A delete reports removed clips and leaves untouched tracks out", "[model][changeset]") {
    Timeline tl(R"(
        V2 |Cccccc|
        V1 |AaBbDd|
    )");
    const ClipId b = tl.clip("V1@2");
    const auto r = ok(edit::deleteClips(tl.state, tl.seq, std::vector<ClipId>{b}));
    const SequenceChange& c = only(r, tl.seq);
    CHECK(c.removedClips == std::vector<ClipId>{b});
    CHECK(c.addedClips.empty());
    CHECK(c.modifiedClips.empty());
    CHECK(c.affectedTracks() == std::vector<TrackId>{tl.track("V1")});
    CHECK(c.track(tl.track("V1"))->ranges == std::vector<TimeRange>{{f(2), f(2)}});
    CHECK_FALSE(c.extentChanged); // a gap is left; the duration stays
}

TEST_CASE("Property roles say what kind of change happened", "[model][changeset]") {
    Timeline tl(R"(
        V2 |......|
        V1 |AaaBbb|
        A1 |AaaBbb|
    )");
    const ClipId b = tl.clip("V1@3");
    const std::vector<ClipId> sel{b};
    const auto rolesOf = [&](const edit::EditOutcome& outcome) { return only(ok(outcome), tl.seq).roles; };

    CHECK(rolesOf(edit::trimEdge(tl.state, tl.seq, b, edit::Edge::Head, f(1), edit::LinkMode::IgnoreLinks)) ==
          (PropertyRole::Timing | PropertyRole::Source));
    CHECK(rolesOf(edit::slip(tl.state, tl.seq, sel, f(1))) == PropertyRoles(PropertyRole::Source));

    edit::ClipAttributeChange opacity;
    opacity.properties = ClipProperties{};
    opacity.properties->set(ParamId::Opacity, 0.5);
    CHECK(rolesOf(edit::setClipAttributes(tl.state, tl.seq, sel, opacity)) == PropertyRoles(PropertyRole::Opacity));

    edit::ClipAttributeChange audio;
    audio.properties = ClipProperties{};
    audio.properties->set(ParamId::VolumeDb, -3.0);
    audio.properties->set(ParamId::Pan, 0.5);
    CHECK(rolesOf(edit::setClipAttributes(tl.state, tl.seq, sel, audio)) == PropertyRoles(PropertyRole::Audio));

    edit::ClipAttributeChange motion;
    motion.properties = ClipProperties{};
    motion.properties->set(ParamId::Rotation, 90.0);
    CHECK(rolesOf(edit::setClipAttributes(tl.state, tl.seq, sel, motion)) == PropertyRoles(PropertyRole::Motion));

    edit::ClipAttributeChange label;
    label.name = "Hero shot";
    label.colorIndex = 4;
    CHECK(rolesOf(edit::setClipAttributes(tl.state, tl.seq, sel, label)) == PropertyRoles(PropertyRole::Label));

    edit::ClipAttributeChange disable;
    disable.enabled = false;
    CHECK(rolesOf(edit::setClipAttributes(tl.state, tl.seq, sel, disable)) == PropertyRoles(PropertyRole::Enabled));

    const std::vector<ClipId> pair{b, tl.clip("A1@3")};
    CHECK(rolesOf(edit::linkClips(tl.state, tl.seq, pair)) == PropertyRoles(PropertyRole::Link));
}

TEST_CASE("The 0.2 clip settings report the clip modified with their role (p02 integration)", "[model][changeset]") {
    // Scale to Frame Size, Hold Filters, Time Interpolation and the input colour space live in the clip's shared
    // content: a change to any of them alone must still list the clip, so views and caches gated on the ChangeSet
    // follow it.
    Timeline tl(R"(
        V1 tgt |..A10aaaaa..|
    )");
    const ClipId clip = tl.clip("V1@2");
    const std::vector<ClipId> sel{clip};
    const auto check = [&](const edit::EditResult& r, PropertyRole role) {
        const SequenceChange& c = only(r, tl.seq);
        CHECK(c.modifiedClips == sel);
        CHECK(c.addedClips.empty());
        CHECK(c.removedClips.empty());
        CHECK(c.roles == PropertyRoles(role));
    };
    SECTION("Scale to Frame Size") {
        check(ok(edit::setScaleToFrameSize(tl.state, tl.seq, sel, true)), PropertyRole::Motion);
    }
    SECTION("Hold Filters on a frame hold") {
        edit::FrameHoldOptions hold;
        const auto held = ok(edit::setFrameHoldOptions(tl.state, tl.seq, clip, hold));
        hold.holdFilters = true;
        check(ok(edit::setFrameHoldOptions(held.state, tl.seq, clip, hold)), PropertyRole::Source);
    }
    SECTION("Time Interpolation") {
        check(ok(edit::setTimeInterpolation(tl.state, tl.seq, sel, TimeInterpolation::OpticalFlow)), PropertyRole::Source);
    }
    SECTION("Input colour space") {
        check(ok(edit::setClipInputColorSpace(tl.state, tl.seq, sel, InputColorSpace::SLog3SGamut3)), PropertyRole::Effects);
    }
    SECTION("Clip group (0.2 timeline)") {
        Timeline two(R"(
            V1 tgt |AaBb|
        )");
        const std::vector<ClipId> pair{two.clip("V1@0"), two.clip("V1@2")};
        const auto grouped = ok(edit::groupClips(two.state, two.seq, pair));
        REQUIRE(grouped.changes.sequences.size() == 1);
        const SequenceChange* c = grouped.changes.sequence(two.seq);
        REQUIRE(c);
        CHECK(std::is_permutation(c->modifiedClips.begin(), c->modifiedClips.end(), pair.begin(), pair.end()));
        CHECK(c->roles == PropertyRoles(PropertyRole::Link));
    }
}

TEST_CASE("A clip moved to another track is modified, not removed and added", "[model][changeset]") {
    Timeline tl(R"(
        V2 |......|
        V1 |AaaBbb|
    )");
    const ClipId b = tl.clip("V1@3");
    const auto r = ok(edit::moveClips(tl.state, tl.seq, std::vector<ClipId>{b}, f(0), {1, 0}));
    const SequenceChange& c = only(r, tl.seq);
    CHECK(c.modifiedClips == std::vector<ClipId>{b});
    CHECK(c.addedClips.empty());
    CHECK(c.removedClips.empty());
    CHECK(c.roles.has(PropertyRole::Track));
    CHECK(c.track(tl.track("V1"))->ranges == std::vector<TimeRange>{{f(3), f(3)}});
    CHECK(c.track(tl.track("V2"))->ranges == std::vector<TimeRange>{{f(3), f(3)}});
}

TEST_CASE("Header-only changes report the header", "[model][changeset]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    const auto marks = ok(edit::markIn(tl.state, tl.seq, f(1)));
    CHECK(only(marks, tl.seq).marksChanged);
    CHECK(only(marks, tl.seq).tracks.empty());

    edit::TrackStateChange lock;
    lock.locked = true;
    const auto locked = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), lock));
    const SequenceChange& c = only(locked, tl.seq);
    REQUIRE(c.tracks.size() == 1);
    CHECK(c.tracks[0].headerChanged);
    CHECK(c.tracks[0].ranges.empty());

    const auto longer = ok(edit::setSequenceDuration(tl.state, tl.seq, f(10)));
    CHECK(only(longer, tl.seq).extentChanged);

    const auto marker = ok(edit::addMarker(tl.state, tl.seq, {f(2), {}, "m", "", 0}));
    CHECK(only(marker, tl.seq).markersChanged);

    const auto noop = ok(edit::addEditToAllTracks(tl.state, tl.seq, f(0)));
    CHECK(noop.changes.empty());
}

TEST_CASE("Undo and redo report the inverse and the repeat of an edit", "[model][changeset][session]") {
    Timeline tl(R"(
        V1 tgt |AaaaBb|
        A1 tgt |AaaaBb|
    )");
    EditSessionOptions options;
    options.validateEdits = true;
    EditSession session(tl.state, options);
    std::vector<ChangeSet> heard;
    session.addListener([&](const ChangeEvent& e) { heard.push_back(e.changes); });

    const auto applied = session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(2)); });
    REQUIRE(applied);
    const SequenceChange& forward = *applied->changes.sequence(tl.seq);
    REQUIRE(heard.size() == 1);
    REQUIRE(heard[0].sequence(tl.seq));
    CHECK(sorted(heard[0].sequence(tl.seq)->addedClips) == sorted(forward.addedClips));

    const auto undone = session.undo();
    REQUIRE(undone);
    const SequenceChange& back = *undone->sequence(tl.seq);
    CHECK(sorted(back.removedClips) == sorted(forward.addedClips));
    CHECK(sorted(back.modifiedClips) == sorted(forward.modifiedClips));
    CHECK(back.addedClips.empty());

    const auto redone = session.redo();
    REQUIRE(redone);
    CHECK(sorted(redone->sequence(tl.seq)->addedClips) == sorted(forward.addedClips));
    CHECK(heard.size() == 3);
}

TEST_CASE("diff covers media, bins and sequences", "[model][changeset]") {
    const ProjectState empty;
    CHECK(diff(empty, empty).empty());

    auto bin = edit::addBin(empty, "Footage");
    REQUIRE(bin);
    CHECK(diff(empty, bin->state).binsChanged);

    MediaItem item;
    item.name = "a.mov";
    auto media = edit::addMedia(bin->state, item);
    REQUIRE(media);
    const ChangeSet addedMedia = diff(bin->state, media->state);
    CHECK(addedMedia.addedMedia == std::vector<MediaId>{media->id});
    CHECK_FALSE(addedMedia.binsChanged);

    auto seq = edit::addSequence(media->state, {});
    REQUIRE(seq);
    CHECK(diff(media->state, seq->state).addedSequences == std::vector<SequenceId>{seq->id});
    const auto removed = edit::removeSequence(seq->state, seq->id);
    REQUIRE(removed);
    CHECK(diff(seq->state, *removed).removedSequences == std::vector<SequenceId>{seq->id});
    CHECK(diff(media->state, bin->state).removedMedia == std::vector<MediaId>{media->id});
}
