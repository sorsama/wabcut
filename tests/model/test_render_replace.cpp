// Stream A, batch A8 (0.7), card 069: Render and Replace for nested sequences and Restore Unrendered (RenderReplace.h).

#include "NestedTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/model/ClipLock.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RenderReplace.h>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// A nest of everything in `tl` (a picture clip and its sound), trimmed to frames [from, to) of the nest.
struct NestedClips {
    ProjectState state;
    SequenceId nested;
    ClipId video;
    ClipId audio;
};

NestedClips nestTrimmed(Timeline& tl, std::int64_t from, std::int64_t to) {
    tl.linkMatching();
    const edit::NestResult nest = nestEverything(tl.state, tl.seq);
    REQUIRE_FALSE(nest.clips.empty());
    ProjectState s = nest.edit.state;
    if (from != 0) {
        s = ok(edit::trimEdge(s, tl.seq, nest.clips[0], edit::Edge::Head, f(from))).state;
    }
    const Clip& v = s.findSequence(tl.seq)->findClip(nest.clips[0])->clip();
    if (f(to) != v.sourceOut()) {
        s = ok(edit::trimEdge(s, tl.seq, nest.clips[0], edit::Edge::Tail, f(to) - v.sourceOut())).state;
    }
    requireValid(s);
    return {s, nest.nested, nest.clips[0], nest.clips.size() > 1 ? nest.clips[1] : ClipId{}};
}

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) {
    return s.findSequence(seq)->findClip(id)->clip();
}

/// A rendered file of `frames` frames with a picture and a stereo sound, as a media item.
MediaItem renderedFile(const ProjectState& s, MediaId like, std::int64_t frames, bool withSound = true) {
    MediaItem item = *s.findMedia(like);
    item.name = "Rendered";
    item.info.duration = f(frames);
    for (VideoStreamInfo& stream : item.info.video) {
        stream.duration = f(frames);
        stream.frameCount = frames;
    }
    for (AudioStreamInfo& stream : item.info.audio) {
        stream.duration = f(frames);
    }
    if (!withSound) {
        item.info.audio.clear();
    }
    return item;
}

} // namespace

TEST_CASE("Render and Replace plans one file per stretch of a nest with handles kept inside it",
          "[model][nesting][render-replace]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCcccDddd|
        A1 tgt |AaaaBbbbCcccDddd|
    )");
    const NestedClips n = nestTrimmed(tl, 4, 12);
    const ClipId ids[] = {n.video};

    const auto plan = edit::planRenderAndReplace(n.state, tl.seq, ids, f(2));
    REQUIRE(plan);
    REQUIRE(plan->size() == 1);
    const edit::NestRenderUnit& unit = plan->front();
    CHECK(unit.nested == n.nested);
    CHECK(unit.video);
    CHECK(unit.audio);
    // The picture clip and its linked sound share the file; the picture comes first.
    REQUIRE(unit.clips.size() == 2);
    CHECK(unit.clips[0] == n.video);
    CHECK(unit.clips[1] == n.audio);
    CHECK(unit.used.start == f(4));
    CHECK(unit.used.end() == f(12));
    CHECK(unit.range.start == f(2));
    CHECK(unit.range.end() == f(14));

    // Handles never reach past the nested sequence.
    const auto wide = edit::planRenderAndReplace(n.state, tl.seq, ids, f(100));
    REQUIRE(wide);
    CHECK(wide->front().range.start == f(0));
    CHECK(wide->front().range.end() == f(16));
    // No handles: exactly what the clips play.
    const auto bare = edit::planRenderAndReplace(n.state, tl.seq, ids, Ticks{});
    REQUIRE(bare);
    CHECK(bare->front().range.start == f(4));
    CHECK(bare->front().range.end() == f(12));
}

TEST_CASE("Render and Replace refuses what is not a nest it can bake", "[model][nesting][render-replace]") {
    Timeline plain(R"(
        V1 tgt |AaaaBbbb|
    )");
    // A plain clip is not a nest.
    const ClipId a = plain.clip("V1@0");
    const auto none = edit::planRenderAndReplace(plain.state, plain.seq, std::span(&a, 1), f(1));
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("nested") != std::string::npos);

    // A multi-camera clip shows one angle of its source.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaa|
    )");
    const PlacedMulticam mc =
        placeMulticam(tl.state, tl.seq, {tl.media('A'), tl.media('B')}, {Ticks{}, Ticks{}}, TimeRange{Ticks{}, f(8)}, Ticks{});
    const auto multicam = edit::planRenderAndReplace(mc.state, tl.seq, std::span(&mc.video, 1), f(1));
    REQUIRE_FALSE(multicam);
    CHECK(multicam.error().message.find("multi-camera") != std::string::npos);

    // A locked clip cannot change.
    Timeline lockedTl(R"(
        V1 tgt |AaaaBbbb|
    )");
    const NestedClips n = nestTrimmed(lockedTl, 0, 8);
    const ClipId lockIds[] = {n.video};
    const ProjectState locked = ok(edit::setClipsLocked(n.state, lockedTl.seq, lockIds, true)).state;
    CHECK_FALSE(edit::planRenderAndReplace(locked, lockedTl.seq, lockIds, f(1)));
}

TEST_CASE("Render and Replace swaps the nest for the rendered file and Restore Unrendered brings the nest back exactly",
          "[model][nesting][render-replace]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCcccDddd|
        A1 tgt |AaaaBbbbCcccDddd|
    )");
    NestedClips n = nestTrimmed(tl, 4, 12);
    // Two keyframes of the nested picture clip, at frames 4 and 8 of the nest (curve time is the nest's time).
    n.state = ok(edit::addKeyframe(n.state, tl.seq, n.video, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(4), 1.0})).state;
    n.state = ok(edit::addKeyframe(n.state, tl.seq, n.video, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(8), 0.0})).state;
    requireValid(n.state);
    const Clip videoBefore = clipOf(n.state, tl.seq, n.video);
    const Clip audioBefore = clipOf(n.state, tl.seq, n.audio);
    REQUIRE(videoBefore.isNested());

    const ClipId ids[] = {n.video};
    const auto plan = edit::planRenderAndReplace(n.state, tl.seq, ids, f(2));
    REQUIRE(plan);
    const Ticks origin = plan->front().range.start; // frame 2 of the nest
    REQUIRE(origin == f(2));
    const MediaItem file = renderedFile(n.state, tl.media('A'), 12);

    const edit::EditResult done = ok(edit::renderAndReplace(n.state, tl.seq, plan->front().clips, file, origin));
    requireValid(done.state);
    CHECK(done.createdMedia.isValid());
    const MediaItem* made = done.state.findMedia(done.createdMedia);
    REQUIRE(made);
    CHECK(made->name == "Rendered");
    CHECK(made->bin == done.state.findSequence(n.nested)->bin); // beside the nest

    for (const ClipId id : {n.video, n.audio}) {
        const Clip& c = clipOf(done.state, tl.seq, id);
        INFO(id.value);
        CHECK_FALSE(c.isNested());
        CHECK(c.media == done.createdMedia);
        REQUIRE(c.renderedNest() != nullptr);
        CHECK(c.renderedNest()->sequence == n.nested);
        CHECK(c.renderedNest()->origin == origin);
        // Place, length, links and the stream kind are what they were; the In point moved with the file.
        const Clip& was = id == n.video ? videoBefore : audioBefore;
        CHECK(c.timelineRange == was.timelineRange);
        CHECK(c.linkGroup == was.linkGroup);
        CHECK(c.stream == was.stream);
        CHECK(c.sourceIn == was.sourceIn - origin);
        CHECK(c.speed == was.speed);
    }
    // The keyframes moved with the file's time: the clip fades exactly as it did over the nest.
    const Clip& baked = clipOf(done.state, tl.seq, n.video);
    for (const std::int64_t frame : {0, 1, 3, 5, 7}) {
        const Ticks at = videoBefore.start() + f(frame);
        INFO(frame);
        CHECK(baked.parameterAt(ParamId::Opacity, at) == videoBefore.parameterAt(ParamId::Opacity, at));
    }
    CHECK(videoBefore.parameterAt(ParamId::Opacity, videoBefore.start() + f(2)) == 0.5); // halfway down: nest frame 6

    // Restore Unrendered: the clips are exactly what they were (In point, keyframes, nest), the file stays in the project.
    const edit::EditResult back = ok(edit::restoreUnrendered(done.state, tl.seq, ids));
    requireValid(back.state);
    CHECK(clipOf(back.state, tl.seq, n.video) == videoBefore);
    CHECK(clipOf(back.state, tl.seq, n.audio) == audioBefore);
    CHECK(back.state.findMedia(done.createdMedia) != nullptr);
}

TEST_CASE("Render and Replace keeps trims and speed and effects of the clip working on the rendered file",
          "[model][nesting][render-replace]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCcccDddd|
        A1 tgt |AaaaBbbbCcccDddd|
    )");
    NestedClips n = nestTrimmed(tl, 4, 12);
    edit::SpeedChange twice;
    twice.speed = Speed{2, 1};
    n.state = ok(edit::setSpeed(n.state, tl.seq, std::vector<ClipId>{n.video}, twice)).state;
    n.state = ok(edit::addEffect(n.state, tl.seq, std::span<const ClipId>(&n.video, 1), "wabcut.blur.gaussian")).state;
    const Clip before = clipOf(n.state, tl.seq, n.video);
    const ClipId ids[] = {n.video};
    const auto plan = edit::planRenderAndReplace(n.state, tl.seq, ids, f(1));
    REQUIRE(plan);
    // The clip plays 8 frames of the nest at double speed: it is shorter than it was.
    CHECK(plan->front().used.duration == before.sourceDuration());
    const MediaItem file = renderedFile(n.state, tl.media('A'), 16);
    const edit::EditResult done = ok(edit::renderAndReplace(n.state, tl.seq, plan->front().clips, file, plan->front().range.start));
    requireValid(done.state);
    const Clip& baked = clipOf(done.state, tl.seq, n.video);
    CHECK(baked.speed == Speed{2, 1});
    CHECK(baked.effects().size() == before.effects().size()); // the effects stay on the clip
    CHECK(baked.timelineRange == before.timelineRange);
    // The restored clip is the original one again.
    const edit::EditResult back = ok(edit::restoreUnrendered(done.state, tl.seq, ids));
    CHECK(clipOf(back.state, tl.seq, n.video) == before);
}

TEST_CASE("Render and Replace refuses a file that does not hold the nest", "[model][nesting][render-replace]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCcccDddd|
        A1 tgt |AaaaBbbbCcccDddd|
    )");
    const NestedClips n = nestTrimmed(tl, 4, 12);
    const ClipId ids[] = {n.video};
    const std::vector<ClipId> both{n.video, n.audio};
    // Too short: the clips play up to frame 12 of the nest, the file ends at nest frame 2 + 5.
    const auto shortFile = edit::renderAndReplace(n.state, tl.seq, ids, renderedFile(n.state, tl.media('A'), 5), f(2));
    REQUIRE_FALSE(shortFile);
    CHECK(shortFile.error().message.find("past the end") != std::string::npos);
    // Starting after the first frame the clip plays.
    const auto late = edit::renderAndReplace(n.state, tl.seq, ids, renderedFile(n.state, tl.media('A'), 20), f(6));
    REQUIRE_FALSE(late);
    CHECK(late.error().message.find("before") != std::string::npos);
    // No sound in the file for the sound clip.
    const auto silent = edit::renderAndReplace(n.state, tl.seq, both, renderedFile(n.state, tl.media('A'), 20, false), f(2));
    REQUIRE_FALSE(silent);
    CHECK(silent.error().message.find("sound") != std::string::npos);
    // Nothing nested among the clips.
    Timeline plain(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId a = plain.clip("V1@0");
    CHECK_FALSE(edit::renderAndReplace(plain.state, plain.seq, std::span(&a, 1), renderedFile(plain.state, plain.media('A'), 8), Ticks{}));
    // Restoring clips that were never rendered.
    CHECK_FALSE(edit::restoreUnrendered(n.state, tl.seq, ids));
}

TEST_CASE("Restore Unrendered says so when the nest is gone", "[model][nesting][render-replace]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    const NestedClips n = nestTrimmed(tl, 0, 8);
    const ClipId ids[] = {n.video};
    const auto plan = edit::planRenderAndReplace(n.state, tl.seq, ids, Ticks{});
    REQUIRE(plan);
    const edit::EditResult done =
        ok(edit::renderAndReplace(n.state, tl.seq, plan->front().clips, renderedFile(n.state, tl.media('A'), 8), plan->front().range.start));
    // Nothing plays the nest now, so the project lets it go.
    const auto removed = edit::removeSequence(done.state, n.nested);
    REQUIRE(removed);
    const auto restored = edit::restoreUnrendered(*removed, tl.seq, ids);
    REQUIRE_FALSE(restored);
    CHECK(restored.error().message.find("not in the project") != std::string::npos);
}

TEST_CASE("A rendered-and-replaced clip is saved with its way back and loads as it was", "[model][nesting][render-replace][io]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCcccDddd|
        A1 tgt |AaaaBbbbCcccDddd|
    )");
    const NestedClips n = nestTrimmed(tl, 4, 12);
    const ClipId ids[] = {n.video};
    const auto plan = edit::planRenderAndReplace(n.state, tl.seq, ids, f(2));
    REQUIRE(plan);
    const edit::EditResult done = ok(edit::renderAndReplace(n.state, tl.seq, plan->front().clips,
                                                             renderedFile(n.state, tl.media('A'), 12), plan->front().range.start));
    const auto text = io::toJson(done.state);
    REQUIRE(text);
    CHECK(text->find("\"renderedFrom\"") != std::string::npos);
    // A project without a rendered clip writes no such key (builds before it open every file).
    const auto plainText = io::toJson(n.state);
    REQUIRE(plainText);
    CHECK(plainText->find("renderedFrom") == std::string::npos);

    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    const Clip& c = clipOf(*loaded, tl.seq, n.video);
    REQUIRE(c.renderedNest() != nullptr);
    CHECK(c.renderedNest()->sequence == n.nested);
    CHECK(c.renderedNest()->origin == f(2));
    CHECK(c.media.isValid());
    // And the way back still works after the round trip.
    const edit::EditResult back = ok(edit::restoreUnrendered(*loaded, tl.seq, ids));
    CHECK(clipOf(back.state, tl.seq, n.video).isNested());
}

TEST_CASE("A nest may show transparency where its lowest track has a gap or a graphic", "[model][nesting][render-replace]") {
    Timeline solid(R"(
        V1 tgt |AaaaBbbb|
    )");
    const NestedClips whole = nestTrimmed(solid, 0, 8);
    CHECK_FALSE(edit::nestMayBeTransparent(whole.state, whole.nested, TimeRange{Ticks{}, f(8)}));

    Timeline gappy(R"(
        V1 tgt |Aaaa....|
    )");
    gappy.linkMatching();
    const NestedClips partial = nestTrimmed(gappy, 0, 4);
    CHECK_FALSE(edit::nestMayBeTransparent(partial.state, partial.nested, TimeRange{Ticks{}, f(4)}));
    CHECK(edit::nestMayBeTransparent(partial.state, partial.nested, TimeRange{Ticks{}, f(8)})); // the gap shows nothing
}

TEST_CASE("A pasted copy of a rendered clip plays the file but has no way back (card 069)", "[model][nesting][render-replace]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb........|
        A1 tgt |AaaaBbbb........|
    )");
    const NestedClips n = nestTrimmed(tl, 0, 8);
    const ClipId ids[] = {n.video};
    const auto plan = edit::planRenderAndReplace(n.state, tl.seq, ids, Ticks{});
    REQUIRE(plan);
    const edit::EditResult done =
        ok(edit::renderAndReplace(n.state, tl.seq, plan->front().clips, renderedFile(n.state, tl.media('A'), 8), plan->front().range.start));
    REQUIRE(clipOf(done.state, tl.seq, n.video).renderedNest() != nullptr);

    // A razor cut keeps it on both halves: each can be restored.
    const TrackId v1 = tl.track("V1");
    const edit::EditResult cut = ok(edit::addEdit(done.state, tl.seq, f(4), std::span(&v1, 1)));
    int rendered = 0;
    for (const TrackPtr& track : cut.state.findSequence(tl.seq)->videoTracks) {
        for (const Clip& c : track->clips) {
            rendered += c.renderedNest() != nullptr ? 1 : 0;
        }
    }
    CHECK(rendered == 2);

    // Copy and paste: the copy plays the file, the nest it names is not carried (another project's sequence of that id
    // must never stand in for it), so the copy cannot be restored.
    const auto copied = copyClips(done.state, tl.seq, ids);
    REQUIRE(copied);
    const auto pasted = pasteClips(done.state, tl.seq, *copied, f(10));
    REQUIRE(pasted);
    int plain = 0;
    int withRecord = 0;
    for (const TrackPtr& track : pasted->edit.state.findSequence(tl.seq)->videoTracks) {
        for (const Clip& c : track->clips) {
            if (c.start() >= f(10)) {
                ++plain;
                withRecord += c.renderedNest() != nullptr ? 1 : 0;
                CHECK_FALSE(c.isNested());
                CHECK(c.media.isValid());
            }
        }
    }
    CHECK(plain == 1);
    CHECK(withRecord == 0);
}
