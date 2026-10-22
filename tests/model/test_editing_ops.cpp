// P.2 (E5): Replace Edit and Replace Footage, Make Offline, Make Subsequence, Join Through Edits, Go to Gap and Close Gap,
// Frame Hold Options, Merge Clips, Interpret Footage and Modify > Timecode (EditingOps.h).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& onTrack(const ProjectState& state, SequenceId seq, std::string_view track, std::size_t index = 0) {
    const Sequence& s = *state.findSequence(seq);
    const bool video = track.front() == 'V';
    const std::size_t n = static_cast<std::size_t>(std::stoi(std::string(track.substr(1)))) - 1;
    return s.tracks(video ? TrackKind::Video : TrackKind::Audio)[n]->clips[index];
}

ProjectState withItem(ProjectState state, MediaId id, const std::function<void(MediaItem&)>& change) {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            change(*item);
            m = std::move(item);
        }
    }
    return state;
}

} // namespace

// ---- Replace Edit ---------------------------------------------------------------------------------------------------

TEST_CASE("Replace Edit puts another item in a clip's place and keeps its attributes", "[model][replace]") {
    Timeline tl(R"(
        V1 tgt |..A10aaaaa..|
        A1 tgt |..A10aaaaa..|
    )");
    tl.linkMatching();
    ProjectState state = tl.state;
    // A blur on the video clip stays after the replace.
    const ClipId video = tl.clip("V1@2");
    state = ok(edit::addEffect(state, tl.seq, std::span(&video, 1), "wabcut.blur.gaussian")).state;
    const std::array<ClipId, 1> clips{video};
    const auto r = ok(edit::replaceClips(state, tl.seq, clips, edit::ReplaceSource{tl.media('B'), f(40)}));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |..B40bbbbb..|
        A1 |..B40bbbbb..|
    )");
    const Clip& v = onTrack(r.state, tl.seq, "V1");
    CHECK(v.media == tl.media('B'));
    CHECK(v.name() == "B");
    CHECK(v.effects().size() == 1);
    CHECK(onTrack(r.state, tl.seq, "A1").media == tl.media('B')); // the linked audio follows, in sync
    CHECK(onTrack(r.state, tl.seq, "A1").linkGroup == v.linkGroup);

    // Not enough media from there: refused (Premiere's insufficient media).
    CHECK_FALSE(edit::replaceClips(state, tl.seq, clips, edit::ReplaceSource{tl.media('B'), f(998)}));
    // Only the video, not following links.
    const auto alone = ok(edit::replaceClips(state, tl.seq, clips, edit::ReplaceSource{tl.media('C'), f(0)},
                                             edit::LinkMode::IgnoreLinks));
    CHECK(onTrack(alone.state, tl.seq, "A1").media == tl.media('A'));
}

TEST_CASE("Replace Edit with Match Frame lines the source frame up with the playhead", "[model][replace]") {
    Timeline tl(R"(
        V1 tgt |..A10aaaaa..|
    )");
    const Clip& clip = onTrack(tl.state, tl.seq, "V1");
    // The Source Monitor shows frame 50 and the playhead is at sequence frame 4 (2 frames into the clip): source In 48.
    const auto in = edit::replaceMatchFrameIn(clip, f(50), f(4));
    REQUIRE(in);
    CHECK(*in == f(48));
    const std::array<ClipId, 1> clips{clip.id};
    const auto r = ok(edit::replaceClips(tl.state, tl.seq, clips, edit::ReplaceSource{tl.media('B'), *in}));
    CHECK(onTrack(r.state, tl.seq, "V1").sourceTimeAt(f(4)) == f(50));
}

// ---- Replace Footage, Make Offline ----------------------------------------------------------------------------------

TEST_CASE("Replace Footage re-points an item and keeps its clips", "[model][replaceFootage]") {
    Timeline tl(R"(
        V1 tgt |A5aaaa|
    )");
    MediaItem next = *tl.state.findMedia(tl.media('A'));
    next.name = "A_v2.mov";
    next.locator.path = "D:/footage/A_v2.mov";
    next.locator.fingerprint = SourceFingerprint{1234, 1, "00ff00ff00ff00ff"};
    const auto replaced = edit::replaceFootage(tl.state, tl.media('A'), next, true);
    REQUIRE(replaced);
    requireValid(*replaced);
    const MediaItem& item = *replaced->findMedia(tl.media('A'));
    CHECK(item.locator.path == "D:/footage/A_v2.mov");
    CHECK(item.name == "A_v2.mov");
    CHECK(onTrack(*replaced, tl.seq, "V1").media == tl.media('A'));
    CHECK(onTrack(*replaced, tl.seq, "V1").sourceIn == f(5));
    const auto kept = edit::replaceFootage(tl.state, tl.media('A'), next, false);
    REQUIRE(kept);
    CHECK(kept->findMedia(tl.media('A'))->name == "A");

    // A shorter file than the clip plays: refused.
    MediaItem shorter = next;
    shorter.info.duration = f(6);
    shorter.info.video[0].duration = f(6);
    shorter.info.audio[0].duration = f(6);
    shorter.info.audio[1].duration = f(6);
    CHECK_FALSE(edit::replaceFootage(tl.state, tl.media('A'), shorter, true));
}

TEST_CASE("Make Offline keeps an item offline until it is linked again", "[model][makeOffline]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::array<MediaId, 1> items{tl.media('A')};
    const auto offline = edit::makeOffline(tl.state, items);
    REQUIRE(offline);
    requireValid(*offline);
    CHECK(offline->findMedia(tl.media('A'))->madeOffline);
    CHECK(offline->findMedia(tl.media('A'))->locator.offline);
    const auto json = io::toJson(*offline);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findMedia(tl.media('A'))->madeOffline);
    const std::array<MediaId, 1> missing{MediaId{999999}};
    CHECK_FALSE(edit::makeOffline(tl.state, missing));
}

// ---- Make Subsequence -----------------------------------------------------------------------------------------------

TEST_CASE("Make Subsequence copies the selection into a new sequence on the same tracks", "[model][subsequence]") {
    Timeline tl(R"(
        V2     |.....Cccc...|
        V1 tgt |..AaaaBbbbb.|
        A1 tgt |..AaaaBbbbb.|
    )");
    tl.linkMatching();
    const std::array<ClipId, 2> clips{tl.clip("V1@6"), tl.clip("V2@5")};
    const auto sub = edit::makeSubsequence(tl.state, tl.seq, clips);
    REQUIRE(sub);
    requireValid(sub->state);
    const Sequence& s = *sub->state.findSequence(sub->id);
    CHECK(s.name == "Fixture Sub_01");
    CHECK(s.frameRate == kRate);
    CHECK_TIMELINE(sub->state, sub->id, R"(
        V2 |Cccc..|
        V1 |.Bbbbb|
        A1 |.Bbbbb|
    )");
    // The B pair stays linked; the parent is unchanged.
    CHECK(onTrack(sub->state, sub->id, "V1").linkGroup.isValid());
    CHECK(onTrack(sub->state, sub->id, "V1").linkGroup == onTrack(sub->state, sub->id, "A1").linkGroup);
    CHECK(onTrack(sub->state, sub->id, "V1").id != tl.clip("V1@6"));
    CHECK(*sub->state.findSequence(tl.seq) == tl.sequence());
    // A second one is Sub_02.
    const auto again = edit::makeSubsequence(sub->state, tl.seq, clips);
    REQUIRE(again);
    CHECK(again->state.findSequence(again->id)->name == "Fixture Sub_02");
}

TEST_CASE("Make Subsequence from In to Out trims to the marks on the targeted tracks", "[model][subsequence]") {
    Timeline tl(R"(
        V2     |CcccccC6cccc|
        V1 tgt |AaaaaaBbbbbb|
    )");
    ProjectState state = ok(edit::markIn(tl.state, tl.seq, f(3))).state;
    state = ok(edit::markOut(state, tl.seq, f(8))).state;
    const auto sub = edit::makeSubsequence(state, tl.seq, {}, "Middle");
    REQUIRE(sub);
    requireValid(sub->state);
    CHECK(sub->state.findSequence(sub->id)->name == "Middle");
    CHECK_TIMELINE(sub->state, sub->id, R"(
        V2 |......|
        V1 |A3aBbb|
    )");
    // Without marks or a selection there is nothing to copy.
    CHECK_FALSE(edit::makeSubsequence(tl.state, tl.seq, {}));
}

// ---- Join Through Edits ---------------------------------------------------------------------------------------------

TEST_CASE("Join Through Edits rejoins the pieces of a razor cut", "[model][throughEdit]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa..A20aaa|
        A1 tgt |Aaaaaaaaaa..A20aaa|
    )");
    tl.linkMatching();
    const std::array<TrackId, 2> tracks{tl.track("V1"), tl.track("A1")};
    const ProjectState cut = ok(edit::addEdit(tl.state, tl.seq, f(4), tracks)).state;
    CHECK(edit::isThroughEdit(onTrack(cut, tl.seq, "V1", 0), onTrack(cut, tl.seq, "V1", 1)));
    CHECK_FALSE(edit::isThroughEdit(onTrack(cut, tl.seq, "V1", 1), onTrack(cut, tl.seq, "V1", 2))); // a gap and a jump

    const std::array<ClipId, 1> right{onTrack(cut, tl.seq, "V1", 1).id};
    const auto joined = ok(edit::joinThroughEdits(cut, tl.seq, right));
    CHECK_TIMELINE(joined.state, tl.seq, R"(
        V1 |Aaaaaaaaaa..A20aaa|
        A1 |Aaaaaaaaaa..A20aaa|
    )");
    CHECK(onTrack(joined.state, tl.seq, "V1").linkGroup == onTrack(joined.state, tl.seq, "A1").linkGroup);
    // Everything at once; and nothing to join is an error.
    CHECK(ok(edit::joinThroughEdits(cut, tl.seq, {})).removed.size() == 2);
    CHECK_FALSE(edit::joinThroughEdits(tl.state, tl.seq, {}));

    // Different effects on the two pieces: not a through edit.
    const ClipId left = onTrack(cut, tl.seq, "V1", 0).id;
    const ProjectState blurred = ok(edit::addEffect(cut, tl.seq, std::span(&left, 1), "wabcut.blur.gaussian")).state;
    CHECK_FALSE(edit::isThroughEdit(onTrack(blurred, tl.seq, "V1", 0), onTrack(blurred, tl.seq, "V1", 1)));
}

// ---- Gaps -----------------------------------------------------------------------------------------------------------

TEST_CASE("Go to Gap finds gaps in the sequence or in tracks", "[model][gaps]") {
    Timeline tl(R"(
        V2     |......Ccc.......|
        V1 tgt |..Aaa....Bbb..Dd|
    )");
    const Sequence& s = tl.sequence();
    // In the sequence: 0-2 and 5-6 and 12-14 (no track has a clip there).
    CHECK(edit::nextGap(s, f(0)) == f(5));
    CHECK(edit::nextGap(s, f(5)) == f(12));
    CHECK_FALSE(edit::nextGap(s, f(12)));
    CHECK(edit::previousGap(s, f(12)) == f(5));
    CHECK(edit::previousGap(s, f(5)) == f(0));
    // In V1: 0-2, 5-9, 12-14.
    const std::array<TrackId, 1> v1{tl.track("V1")};
    CHECK(edit::nextGap(s, f(1), v1) == f(5));
    CHECK(edit::nextGap(s, f(5), v1) == f(12));
}

TEST_CASE("Close Gap removes empty time where every involved track is empty", "[model][gaps]") {
    Timeline tl(R"(
        V1 tgt |..Aaa..Bbb...Ccc|
        A1 tgt |..Aaa..Bbb...Ccc|
    )");
    tl.linkMatching();
    const auto all = ok(edit::closeGaps(tl.state, tl.seq, {}));
    CHECK_TIMELINE(all.state, tl.seq, R"(
        V1 |AaaBbbCcc.......|
        A1 |AaaBbbCcc.......|
    )");
    // Between the selected clips only.
    const std::array<ClipId, 2> two{tl.clip("V1@7"), tl.clip("V1@13")};
    const auto some = ok(edit::closeGaps(tl.state, tl.seq, two));
    CHECK_TIMELINE(some.state, tl.seq, R"(
        V1 |..Aaa..BbbCcc...|
        A1 |..Aaa..BbbCcc...|
    )");
    // A gap with material on another sync-locked track stays.
    Timeline blocked(R"(
        V2     |..Mmmmm.|
        V1 tgt |Aa....Bb|
    )");
    const std::array<ClipId, 2> v1{blocked.clip("V1@0"), blocked.clip("V1@6")};
    CHECK_FALSE(edit::closeGaps(blocked.state, blocked.seq, v1));
}

// ---- Frame Hold Options ---------------------------------------------------------------------------------------------

TEST_CASE("Frame Hold Options holds the chosen frame over the whole clip", "[model][frameHold]") {
    Timeline tl(R"(
        V1 tgt |..A10aaaaa..|
    )");
    const ClipId clip = tl.clip("V1@2");
    const auto hold = [&](edit::HoldOn on, Ticks time, bool filters = false) {
        edit::FrameHoldOptions o;
        o.holdOn = on;
        o.time = time;
        o.holdFilters = filters;
        return ok(edit::setFrameHoldOptions(tl.state, tl.seq, clip, o));
    };
    const auto heldIn = hold(edit::HoldOn::InPoint, {});
    const Clip& in = onTrack(heldIn.state, tl.seq, "V1");
    CHECK(in.isFreeze());
    CHECK(in.sourceIn == f(10));
    CHECK(in.timelineRange == tl.sequence().videoTracks[0]->clips[0].timelineRange);
    CHECK(onTrack(hold(edit::HoldOn::OutPoint, {}).state, tl.seq, "V1").sourceTimeAt(f(3)) ==
          tl.sequence().videoTracks[0]->clips[0].sourceTimeAt(f(10) - Ticks{1}));
    CHECK(onTrack(hold(edit::HoldOn::Playhead, f(5)).state, tl.seq, "V1").sourceIn == f(13));
    CHECK(onTrack(hold(edit::HoldOn::SequenceTimecode, f(4)).state, tl.seq, "V1").sourceIn == f(12));
    CHECK(onTrack(hold(edit::HoldOn::SourceTimecode, f(300)).state, tl.seq, "V1").sourceIn == f(300));

    // Hold Filters: effects hold at the held frame; without, they animate on.
    const auto heldFiltered = hold(edit::HoldOn::InPoint, {}, true);
    const Clip& filtered = onTrack(heldFiltered.state, tl.seq, "V1");
    CHECK(filtered.content().holdFilters);
    CHECK(filtered.curveTimeAt(f(8)) == filtered.curveTimeAt(f(2)));
    CHECK(in.curveTimeAt(f(8)) != in.curveTimeAt(f(2)));

    // Back to normal playback from the held frame.
    edit::FrameHoldOptions off;
    off.enabled = false;
    const auto played = ok(edit::setFrameHoldOptions(hold(edit::HoldOn::Playhead, f(5)).state, tl.seq, clip, off));
    CHECK(onTrack(played.state, tl.seq, "V1").speed == Speed{1, 1});
    CHECK(onTrack(played.state, tl.seq, "V1").sourceIn == f(13));

    // Outside the clip, or an audio clip: refused.
    edit::FrameHoldOptions outside;
    outside.holdOn = edit::HoldOn::Playhead;
    outside.time = f(11);
    CHECK_FALSE(edit::setFrameHoldOptions(tl.state, tl.seq, clip, outside));
}

// ---- Merge Clips ----------------------------------------------------------------------------------------------------

TEST_CASE("Merge Clips syncs a camera clip with recorded sound and edits like a clip", "[model][mergeClips]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
        A2     |..........|
        A3     |..........|
    )");
    // The clap: camera In at 40, recorder In at 10 (In points sync).
    ProjectState state = withItem(tl.state, tl.media('A'), [](MediaItem& m) { m.markIn = f(40); });
    state = withItem(state, tl.media('R'), [](MediaItem& m) { m.markIn = f(10); });
    edit::MergeSpec spec;
    spec.video = tl.media('A');
    spec.audio = {tl.media('R')};
    spec.sync = MulticamSync::InPoints;
    spec.removeCameraAudio = true;
    const auto merged = edit::mergeClips(state, spec);
    REQUIRE(merged);
    requireValid(merged->state);
    const Sequence& m = *merged->state.findSequence(merged->id);
    CHECK(m.name == "A - Merged");
    REQUIRE(m.multicam);
    CHECK(m.multicam->merged);
    REQUIRE(m.multicam->angles.size() == 2);
    CHECK(m.multicam->angles[0].audio.empty()); // the camera's sound left out
    CHECK(m.multicam->angles[1].offset - m.multicam->angles[0].offset == f(30));

    // Edited in: the camera's picture and the recorder's sound, linked.
    edit::SourceClip source;
    source.sequence = merged->id;
    source.sourceRange = {f(40), f(5)};
    source.patches = edit::defaultSourcePatches(*merged->state.findSequence(tl.seq), m);
    const auto placed = ok(edit::overwriteEdit(merged->state, tl.seq, source, f(0)));
    const Clip& v = onTrack(placed.state, tl.seq, "V1");
    const Clip& a = onTrack(placed.state, tl.seq, "A1");
    CHECK(v.sequence == merged->id);
    CHECK(v.angles == AngleSet::of(0));
    CHECK(a.angles.contains(1));
    CHECK(v.linkGroup == a.linkGroup);

    // An audio item without sound, or no audio at all: refused.
    edit::MergeSpec none = spec;
    none.audio.clear();
    CHECK_FALSE(edit::mergeClips(state, none));
}

// ---- Interpret Footage, Modify > Timecode ---------------------------------------------------------------------------

TEST_CASE("Interpret Footage conforms a high frame rate capture to slow motion", "[model][interpret]") {
    // The fixture's media are 25 fps; interpreting A as 12.5 fps plays every frame at half speed.
    Timeline tl(R"(
        V1 tgt |Aaaa....Bbbb|
        A1 tgt |Aaaa........|
    )");
    tl.linkMatching();
    MediaInterpretation slow;
    slow.frameRate = Rate{25, 2};
    const std::array<MediaId, 1> a{tl.media('A')};
    const auto r = ok(edit::interpretFootage(tl.state, a, slow));
    CHECK(edit::conformSpeed(*r.state.findMedia(tl.media('A'))) == Speed{1, 2});
    // Placed clips keep their source In and play the same frames at half speed: twice as long.
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaaaaaaBbbb|
        A1 |Aaaaaaaa....|
    )");
    CHECK(onTrack(r.state, tl.seq, "V1").speed == Speed{1, 2});
    CHECK(onTrack(r.state, tl.seq, "V1").sourceTimeAt(f(6)) == f(3));
    // New clips of it run at the conform speed too.
    edit::SourceClip source;
    source.media = tl.media('A');
    source.sourceRange = {f(100), f(4)};
    source.patches = {{{StreamKind::Video, 0}, tl.track("V1")}};
    const auto placed = ok(edit::overwriteEdit(r.state, tl.seq, source, f(12)));
    const Clip& added = onTrack(placed.state, tl.seq, "V1", 2);
    CHECK(added.speed == Speed{1, 2});
    CHECK(added.duration() == f(8));
    // Back to the file's rate: back to normal speed.
    const auto back = ok(edit::interpretFootage(r.state, a, MediaInterpretation{}));
    CHECK_FALSE(back.state.findMedia(tl.media('A'))->interpretation);
    CHECK(onTrack(back.state, tl.seq, "V1").speed == Speed{1, 1});
    CHECK(onTrack(back.state, tl.seq, "V1").duration() == f(4));

    // Saved and read back with the rest of the interpretation.
    MediaInterpretation full = slow;
    full.pixelAspect = Rate{4, 3};
    full.fieldOrder = FieldOrder::UpperFirst;
    full.ignoreAlpha = true;
    full.premultiplication = AlphaMode::Premultiplied;
    const auto saved = ok(edit::interpretFootage(tl.state, a, full));
    const auto json = io::toJson(saved.state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    REQUIRE(loaded->findMedia(tl.media('A'))->interpretation);
    CHECK(*loaded->findMedia(tl.media('A'))->interpretation == *saved.state.findMedia(tl.media('A'))->interpretation);

    MediaInterpretation bad;
    bad.pixelAspect = Rate{1, 100};
    CHECK_FALSE(edit::interpretFootage(tl.state, a, bad));
}

TEST_CASE("Modify Timecode sets an item's start timecode and reel name", "[model][timecode]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::array<MediaId, 1> a{tl.media('A')};
    CHECK_FALSE(edit::mediaStartTimecode(*tl.state.findMedia(tl.media('A'))));
    const auto set = edit::setMediaTimecode(tl.state, a, Ticks::fromSeconds(3600), "A001");
    REQUIRE(set);
    requireValid(*set);
    const MediaItem& item = *set->findMedia(tl.media('A'));
    CHECK(edit::mediaStartTimecode(item) == Ticks::fromSeconds(3600));
    CHECK(item.timecode->reelName == "A001");
    const auto json = io::toJson(*set);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findMedia(tl.media('A'))->timecode == item.timecode);
    const auto cleared = edit::setMediaTimecode(*set, a, std::nullopt, "");
    REQUIRE(cleared);
    CHECK_FALSE(cleared->findMedia(tl.media('A'))->timecode);
    CHECK_FALSE(edit::setMediaTimecode(tl.state, a, Ticks::fromSeconds(25 * 3600), ""));
    CHECK_FALSE(edit::setMediaTimecode(tl.state, a, std::nullopt, std::string(200, 'r')));
}

TEST_CASE("A still's default duration sets the length of its clips", "[model][stillDuration]") {
    Timeline tl(R"(
        V1 tgt |....|
    )");
    MediaItem still;
    still.name = "card.png";
    still.locator.path = "D:/stills/card.png";
    still.info.isStillImage = true;
    VideoStreamInfo v;
    v.width = 1920;
    v.height = 1080;
    still.info.video.push_back(v);
    still.stillDuration = Ticks::fromSeconds(2);
    auto added = edit::addMedia(tl.state, still);
    REQUIRE(added);
    const std::vector<edit::SourcePatch> patches{{{StreamKind::Video, 0}, tl.track("V1")}};
    const auto edit = edit::resolveThreePointEdit(added->state, tl.seq, added->id, patches, {}, Ticks{});
    REQUIRE(edit);
    CHECK(edit->sourceRange.duration == Ticks::fromSeconds(2));
}
