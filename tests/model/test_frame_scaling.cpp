// P.2 (E5): Scale to Frame Size, Set to Frame Size, Set to Fill Frame and the default media scaling (FrameScaling.h).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// A picture of `width` x `height` (pixel aspect `par`, display rotation `rotation`) with one audio stream.
MediaId addPicture(ProjectState& state, std::string name, int width, int height, Rate par = {1, 1}, int rotation = 0,
                   MediaScaling scaling = MediaScaling::None) {
    MediaItem item;
    item.name = std::move(name);
    item.locator.path = "D:/footage/" + item.name + ".mov";
    item.info.duration = f(500);
    VideoStreamInfo v;
    v.index = 0;
    v.width = width;
    v.height = height;
    v.sampleAspect = par;
    v.rotationDegrees = rotation;
    v.frameRate = kRate;
    v.duration = f(500);
    item.info.video.push_back(v);
    AudioStreamInfo a;
    a.index = 1;
    a.channels = 2;
    a.duration = f(500);
    item.info.audio.push_back(a);
    item.scaling = scaling;
    auto added = edit::addMedia(state, std::move(item));
    REQUIRE(added);
    state = std::move(added->state);
    return added->id;
}

edit::SourceClip sourceOf(const Timeline& tl, const ProjectState& state, MediaId media) {
    edit::SourceClip s;
    s.media = media;
    s.sourceRange = {f(0), f(10)};
    s.patches = edit::defaultSourcePatches(*state.findSequence(tl.seq), *state.findMedia(media));
    return s;
}

const Clip& clipOn(const ProjectState& state, SequenceId seq, TrackKind kind, std::size_t track, std::size_t index = 0) {
    const Sequence& s = *state.findSequence(seq);
    return s.tracks(kind)[track]->clips[index];
}

} // namespace

TEST_CASE("Fit and fill factors use the displayed picture size", "[model][scaling]") {
    // A UHD picture fits a 1080p frame at 50 %; a vertical 1080x1920 one at 56.25 % (fill: 177.8 %).
    CHECK(frameFitScale({3840, 2160}, 1920, 1080, false) == Approx(0.5));
    CHECK(frameFitScale({3840, 2160}, 1920, 1080, true) == Approx(0.5));
    CHECK(frameFitScale({1080, 1920}, 1920, 1080, false) == Approx(0.5625));
    CHECK(frameFitScale({1080, 1920}, 1920, 1080, true) == Approx(1920.0 / 1080.0));
    CHECK(frameFitScale({0, 0}, 1920, 1080, false) == 1.0);

    // Anamorphic HDV (1440x1080, pixel aspect 4:3) shows 1920 wide; a phone video turned 90 degrees is portrait.
    MediaItem hdv;
    VideoStreamInfo v;
    v.width = 1440;
    v.height = 1080;
    v.sampleAspect = {4, 3};
    hdv.info.video.push_back(v);
    const auto shown = mediaPictureSize(hdv, 0);
    REQUIRE(shown);
    CHECK(shown->width == Approx(1920.0));
    CHECK(shown->height == Approx(1080.0));
    hdv.info.video[0] = VideoStreamInfo{};
    hdv.info.video[0].width = 1920;
    hdv.info.video[0].height = 1080;
    hdv.info.video[0].rotationDegrees = 90;
    const auto turned = mediaPictureSize(hdv, 0);
    REQUIRE(turned);
    CHECK(turned->width == Approx(1080.0));
    CHECK(turned->height == Approx(1920.0));
    CHECK_FALSE(mediaPictureSize(hdv, 1));
}

TEST_CASE("A sequence of non-square pixels fits pictures by its displayed width (145 156)", "[model][scaling][par]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )");
    ProjectState state = tl.state;
    // An HDV sequence: 1440x1080 canvas pixels of 4:3 (seen 1920x1080).
    {
        auto copy = std::make_shared<Sequence>(*state.findSequence(tl.seq));
        copy->width = 1440;
        copy->height = 1080;
        copy->pixelAspect = Rate{4, 3};
        for (SequencePtr& s : state.sequences) {
            if (s->id == tl.seq) {
                s = copy;
            }
        }
    }
    const Sequence& hdvSequence = *state.findSequence(tl.seq);
    CHECK(sequencePixelAspect(hdvSequence) == Approx(4.0 / 3.0));
    const MediaId hdvClip = addPicture(state, "HDV", 1440, 1080, {4, 3});
    const MediaId square = addPicture(state, "FullHD", 1920, 1080);
    const MediaId uhd = addPicture(state, "UHD", 3840, 2160);

    const ProjectState base = state;
    // The HDV clip is 1920x1080 as seen, the sequence is seen 1920x1080: a factor of 1, not 0.75 as in a square canvas.
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, hdvClip), f(0))).state;
    const Clip& placed = clipOn(state, tl.seq, TrackKind::Video, 0);
    const ClipId video = placed.id;
    const auto on = ok(edit::setScaleToFrameSize(state, tl.seq, std::vector<ClipId>{video}, true));
    const Clip& fitted = clipOn(on.state, tl.seq, TrackKind::Video, 0);
    CHECK(scaleToFrameFactor(on.state, fitted, 1440, 1080, sequencePixelAspect(*on.state.findSequence(tl.seq))) == Approx(1.0));
    CHECK(scaleToFrameFactor(on.state, fitted, 1440, 1080) == Approx(0.75)); // a square canvas: the old answer
    // A UHD picture fits the seen frame at 50 %, a Full HD one at 100 % (1440 canvas pixels wide once seen as 1920).
    const auto uhdFit = frameFitScale(*mediaPictureSize(*on.state.findMedia(uhd), 0), 1440 * (4.0 / 3.0), 1080, false);
    CHECK(uhdFit == Approx(0.5));
    const auto hdFit = frameFitScale(*mediaPictureSize(*on.state.findMedia(square), 0), 1440 * (4.0 / 3.0), 1080, false);
    CHECK(hdFit == Approx(1.0));
    // Set to Frame Size on a UHD clip: 50 % of the seen frame (a square 1440-wide canvas would say 37.5 %).
    const ProjectState placedUhd = ok(edit::overwriteEdit(base, tl.seq, sourceOf(tl, base, uhd), f(0))).state;
    const ClipId big = clipOn(placedUhd, tl.seq, TrackKind::Video, 0).id;
    const auto sized = ok(edit::setToFrameSize(placedUhd, tl.seq, std::vector<ClipId>{big}, edit::FrameFit::Frame));
    CHECK(clipOn(sized.state, tl.seq, TrackKind::Video, 0).properties()[ParamId::Scale].constant() == Approx(0.5));
}
TEST_CASE("Scale to Frame Size fits the picture before Motion and keeps Scale at 100 percent", "[model][scaling]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )");
    ProjectState state = tl.state;
    const MediaId uhd = addPicture(state, "UHD", 3840, 2160);
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, uhd), f(0))).state;
    const Clip& placed = clipOn(state, tl.seq, TrackKind::Video, 0);
    CHECK_FALSE(placed.content().scaleToFrame);
    CHECK(scaleToFrameFactor(state, placed, 1920, 1080) == 1.0); // off: pixel size (Premiere's "None")

    const ClipId video = placed.id;
    const ClipId audio = clipOn(state, tl.seq, TrackKind::Audio, 0).id;
    const std::vector<ClipId> both{video, audio};
    const auto on = ok(edit::setScaleToFrameSize(state, tl.seq, both, true));
    const Clip& fitted = clipOn(on.state, tl.seq, TrackKind::Video, 0);
    CHECK(fitted.content().scaleToFrame);
    CHECK(scaleToFrameFactor(on.state, fitted, 1920, 1080) == Approx(0.5));
    CHECK(fitted.properties()[ParamId::Scale].constant() == 1.0); // Motion > Scale stays 100 %
    CHECK_FALSE(clipOn(on.state, tl.seq, TrackKind::Audio, 0).content().scaleToFrame); // audio is skipped
    REQUIRE(on.changes.sequences.size() == 1);

    // Again: nothing changes. Off: the picture is at its pixel size again.
    const auto again = ok(edit::setScaleToFrameSize(on.state, tl.seq, both, true));
    CHECK(again.changes.sequences.empty());
    const auto off = ok(edit::setScaleToFrameSize(on.state, tl.seq, std::vector<ClipId>{video}, false));
    CHECK(scaleToFrameFactor(off.state, clipOn(off.state, tl.seq, TrackKind::Video, 0), 1920, 1080) == 1.0);

    // Only audio: refused.
    CHECK_FALSE(edit::setScaleToFrameSize(state, tl.seq, std::vector<ClipId>{audio}, true));
}

TEST_CASE("Set to Frame Size and Set to Fill Frame set Motion Scale", "[model][scaling]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )");
    ProjectState state = tl.state;
    const MediaId phone = addPicture(state, "Phone", 1080, 1920);
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, phone), f(0))).state;
    const ClipId clip = clipOn(state, tl.seq, TrackKind::Video, 0).id;
    // Scale to Frame Size first: Set to Frame Size turns it off and shows the factor in Motion instead.
    state = ok(edit::setScaleToFrameSize(state, tl.seq, std::vector<ClipId>{clip}, true)).state;

    const auto fit = ok(edit::setToFrameSize(state, tl.seq, std::vector<ClipId>{clip}, edit::FrameFit::Frame));
    const Clip& fitted = clipOn(fit.state, tl.seq, TrackKind::Video, 0);
    CHECK_FALSE(fitted.content().scaleToFrame);
    CHECK(fitted.properties()[ParamId::Scale].constant() == Approx(0.5625));
    CHECK(scaleToFrameFactor(fit.state, fitted, 1920, 1080) == 1.0);

    const auto fill = ok(edit::setToFrameSize(fit.state, tl.seq, std::vector<ClipId>{clip}, edit::FrameFit::Fill));
    CHECK(clipOn(fill.state, tl.seq, TrackKind::Video, 0).properties()[ParamId::Scale].constant() ==
          Approx(1920.0 / 1080.0).epsilon(1e-6));

    // A keyed Scale is replaced by the constant.
    ProjectState keyed = fill.state;
    {
        ClipProperties p = clipOn(keyed, tl.seq, TrackKind::Video, 0).properties();
        Curve curve;
        curve.keys.push_back(Keyframe{f(0), 1.0});
        curve.keys.push_back(Keyframe{f(5), 2.0});
        p[ParamId::Scale] = ParamValue(curve);
        edit::ClipAttributeChange change;
        change.properties = p;
        keyed = ok(edit::setClipAttributes(keyed, tl.seq, std::vector<ClipId>{clip}, change)).state;
    }
    const auto replaced = ok(edit::setToFrameSize(keyed, tl.seq, std::vector<ClipId>{clip}, edit::FrameFit::Frame));
    const ParamValue& scale = clipOn(replaced.state, tl.seq, TrackKind::Video, 0).properties()[ParamId::Scale];
    CHECK_FALSE(scale.isCurve());
    CHECK(scale.constant() == Approx(0.5625));
}

TEST_CASE("Clips on a locked track are not scaled", "[model][scaling]") {
    Timeline tl(R"(
        V1 lock |Aaaa|
    )");
    CHECK_FALSE(edit::setScaleToFrameSize(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, true));
    CHECK_FALSE(edit::setToFrameSize(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, edit::FrameFit::Frame));
}

TEST_CASE("Default media scaling applies to the video clips a source edit places", "[model][scaling]") {
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |....................|
    )");
    ProjectState state = tl.state;
    const MediaId scaled = addPicture(state, "Scaled", 3840, 2160, {1, 1}, 0, MediaScaling::ScaleToFrame);
    const MediaId set = addPicture(state, "Set", 3840, 2160, {1, 1}, 0, MediaScaling::SetToFrame);
    const MediaId fill = addPicture(state, "Fill", 1080, 1920, {1, 1}, 0, MediaScaling::SetToFill);
    const MediaId same = addPicture(state, "Same", 1920, 1080, {1, 1}, 0, MediaScaling::SetToFrame);
    const MediaId none = addPicture(state, "None", 3840, 2160);

    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, scaled), f(0))).state;
    CHECK(clipOn(state, tl.seq, TrackKind::Video, 0, 0).content().scaleToFrame);
    CHECK(clipOn(state, tl.seq, TrackKind::Video, 0, 0).properties()[ParamId::Scale].constant() == 1.0);
    CHECK_FALSE(clipOn(state, tl.seq, TrackKind::Audio, 0, 0).content().scaleToFrame);

    state = ok(edit::insertEdit(state, tl.seq, sourceOf(tl, state, set), f(10))).state;
    CHECK(clipOn(state, tl.seq, TrackKind::Video, 0, 1).properties()[ParamId::Scale].constant() == Approx(0.5));
    CHECK_FALSE(clipOn(state, tl.seq, TrackKind::Video, 0, 1).content().scaleToFrame);

    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, fill), f(20))).state;
    CHECK(clipOn(state, tl.seq, TrackKind::Video, 0, 2).properties()[ParamId::Scale].constant() ==
          Approx(1920.0 / 1080.0).epsilon(1e-6));

    // A picture that already matches the frame, and an item without a default, keep the media's shared content.
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, same), f(30))).state;
    CHECK(clipOn(state, tl.seq, TrackKind::Video, 0, 3).sharesContentWith(clipOn(state, tl.seq, TrackKind::Audio, 0, 3)));
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, none), f(40))).state;
    CHECK(clipOn(state, tl.seq, TrackKind::Video, 0, 4).properties()[ParamId::Scale].constant() == 1.0);

    // Setting an item's default later leaves its placed clips alone.
    const auto changed = edit::setMediaScaling(state, std::vector<MediaId>{none}, MediaScaling::SetToFill);
    REQUIRE(changed);
    CHECK(changed->findMedia(none)->scaling == MediaScaling::SetToFill);
    CHECK(clipOn(*changed, tl.seq, TrackKind::Video, 0, 4).properties()[ParamId::Scale].constant() == 1.0);
    CHECK_FALSE(edit::setMediaScaling(state, std::vector<MediaId>{MediaId{999999}}, MediaScaling::None));
}

TEST_CASE("Scale to Frame Size and the default media scaling are saved", "[model][scaling][io]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )");
    ProjectState state = tl.state;
    const MediaId uhd = addPicture(state, "UHD", 3840, 2160, {1, 1}, 0, MediaScaling::ScaleToFrame);
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, uhd), f(0))).state;
    const auto json = io::toJson(state);
    REQUIRE(json);
    CHECK(json->find("\"scaleToFrame\": true") != std::string::npos);
    CHECK(json->find("\"scaling\": \"scaleToFrameSize\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findMedia(uhd)->scaling == MediaScaling::ScaleToFrame);
    CHECK(clipOn(*loaded, tl.seq, TrackKind::Video, 0).content().scaleToFrame);
    CHECK(*loaded == state);

    // An unknown scaling name is refused (project files are untrusted).
    std::string bad = *json;
    bad.replace(bad.find("scaleToFrameSize"), 16, "stretchToFrame!!");
    CHECK_FALSE(io::fromJson(bad));
}

TEST_CASE("Nearest Neighbor Scaling is a clip flag for video clips and one change and saved only when set", "[model][scaling][io]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )");
    ProjectState state = tl.state;
    const MediaId pixels = addPicture(state, "Pixels", 320, 180);
    state = ok(edit::overwriteEdit(state, tl.seq, sourceOf(tl, state, pixels), f(0))).state;
    const ClipId video = clipOn(state, tl.seq, TrackKind::Video, 0).id;
    const ClipId audio = clipOn(state, tl.seq, TrackKind::Audio, 0).id;
    CHECK_FALSE(clipOn(state, tl.seq, TrackKind::Video, 0).content().nearestNeighbor);

    // On for the video clip of a linked pair; the audio clip is skipped.
    const auto on = ok(edit::setNearestNeighbor(state, tl.seq, std::vector<ClipId>{video, audio}, true));
    CHECK(clipOn(on.state, tl.seq, TrackKind::Video, 0).content().nearestNeighbor);
    CHECK_FALSE(clipOn(on.state, tl.seq, TrackKind::Audio, 0).content().nearestNeighbor);
    REQUIRE(on.changes.sequences.size() == 1);
    CHECK(on.changes.sequences.front().modifiedClips == std::vector<ClipId>{video}); // views and caches hear about the clip
    CHECK(on.changes.sequences.front().roles.has(PropertyRole::Effects)); // and that it changes the picture, not the source

    // It is independent of Scale to Frame Size and Motion > Scale.
    const auto fitted = ok(edit::setScaleToFrameSize(on.state, tl.seq, std::vector<ClipId>{video}, true));
    CHECK(clipOn(fitted.state, tl.seq, TrackKind::Video, 0).content().nearestNeighbor);
    CHECK(clipOn(fitted.state, tl.seq, TrackKind::Video, 0).content().scaleToFrame);

    // Saved only when set, and read back.
    const auto plain = io::toJson(state);
    REQUIRE(plain);
    CHECK(plain->find("nearestNeighbor") == std::string::npos);
    const auto json = io::toJson(on.state);
    REQUIRE(json);
    CHECK(json->find("\"nearestNeighbor\": true") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(clipOn(*loaded, tl.seq, TrackKind::Video, 0).content().nearestNeighbor);
    CHECK(*loaded == on.state);
    CHECK_FALSE(*loaded == state); // the flag is part of the clip's content

    // Again: nothing changes. Off: smooth scaling again, and the file loses the key.
    const auto again = ok(edit::setNearestNeighbor(on.state, tl.seq, std::vector<ClipId>{video}, true));
    CHECK(again.changes.sequences.empty());
    const auto off = ok(edit::setNearestNeighbor(on.state, tl.seq, std::vector<ClipId>{video}, false));
    CHECK_FALSE(clipOn(off.state, tl.seq, TrackKind::Video, 0).content().nearestNeighbor);
    CHECK(off.state == state);

    // Only audio: refused. A clip on a locked track: refused.
    CHECK_FALSE(edit::setNearestNeighbor(state, tl.seq, std::vector<ClipId>{audio}, true));
    Timeline locked(R"(
        V1 lock |Aaaa|
    )");
    CHECK_FALSE(edit::setNearestNeighbor(locked.state, locked.seq, std::vector<ClipId>{locked.clip("V1@0")}, true));
}
