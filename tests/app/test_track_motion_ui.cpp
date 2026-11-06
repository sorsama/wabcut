// 0.7 (card #83): Track Motion - pin a graphic, title or censor bar to something that moves in the shot. The TrackMotion
// bridge over the engine's track.transform job (synthetic moving pictures, synchronous): the clips default from the shown
// clip, a box (or a mask) marks the subject, Track keys the follower's Motion (Position; with "full" Scale and Rotation) or
// the footage itself (Stabilize) in ONE undo step, the box is turned into frame coordinates through the footage clip's
// Motion, and what cannot run says why. Then the section in Effect Controls and the subject box on the program monitor in
// the real shell (a real mouse drag draws, moves and resizes the box; screenshots in the dark and light themes).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "ConfidenceBuckets.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "SyntheticMotion.h"
#include "TrackMotionBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/PreparedComposite.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QStyleHints>

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// Pictures of the fixture's media (1920x1080, 25 fps) at most 320 wide: the content moves (+0.8, -0.3) working pixels per
/// frame (+4.8, -1.8 source pixels).
class FakePictures final : public masks::TrackingServices {
public:
    std::function<void()> onProgress;
    std::function<void(const masks::TrackProgress&)> onEach; ///< every progress report (a Stop at frame N)
    std::function<void()> onFrame;                           ///< every picture asked for
    void progress(const masks::TrackProgress& p) override {
        if (onEach) {
            onEach(p);
        }
        if (onProgress) {
            auto callback = std::move(onProgress);
            callback();
        }
    }
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        if (onFrame) {
            onFrame();
        }
        const VideoStreamInfo& v = state.findMedia(media)->info.video.at(static_cast<std::size_t>(stream));
        if (time.value < 0 || time >= v.duration) {
            return std::unexpected(Error{ErrorCode::NotFound, "past the end"});
        }
        const std::int64_t k = ticksToFrames(time, v.frameRate);
        const int w = std::min(maxWidth, 320);
        masks::TrackingFrame f;
        f.luma = masks::test::render(m_texture, w, w * v.height / v.width,
                                     masks::Motion{1, 0, -0.8 * static_cast<double>(k), 0, 1, 0.3 * static_cast<double>(k)});
        f.start = framesToTicks(k, v.frameRate);
        f.duration = framesToTicks(1, v.frameRate);
        return f;
    }

private:
    masks::test::Texture m_texture{23};
};

/// Footage on V1 and a clip over it on V2 (a video clip, or a still: a picture to pin).
struct Fixture {
    Timeline tl;
    ProjectController controller;
    TrackMotionBridge bridge;
    ClipId footage;
    ClipId over;

    explicit Fixture(bool stillOnTop = false, bool mirroredFootage = false)
        : tl("V2 |Bbbbbbbbbbbbbbbbbbbbbbbb|\n"
             "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\n"
             "A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|") {
        if (mirroredFootage) { // card 154: a front-camera clip, stored mirrored
            for (MediaPtr& m : tl.state.media) {
                if (m && m->id == tl.media('A')) {
                    auto copy = std::make_shared<MediaItem>(*m);
                    copy->info.video.front().mirrored = true;
                    m = std::move(copy);
                }
            }
        }
        if (stillOnTop) {
            for (MediaPtr& m : tl.state.media) {
                if (m && m->id == tl.media('B')) {
                    auto still = std::make_shared<MediaItem>(*m);
                    still->info.isStillImage = true;
                    m = std::move(still);
                }
            }
        }
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        footage = clipAt(*controller.snapshot(), tl.seq, "V1@0");
        over = clipAt(*controller.snapshot(), tl.seq, "V2@0");
        controller.setPlayhead(Ticks{});
        bridge.setController(&controller);
        bridge.setSynchronous(true);
        bridge.setTrackingServices(std::make_shared<FakePictures>());
    }
    const Clip& clip(ClipId id) const { return controller.currentSequence()->findClip(id)->clip(); }
    double value(ClipId id, ParamId param, std::int64_t frame) const {
        return clip(id).properties().value(param, clip(id).curveTimeAt(clip(id).start() + framesToTicks(frame, kRate25)));
    }
    std::size_t undoCount() { return controller.session().undoCount(); }
};

} // namespace

TEST_CASE("Track Motion: the clips default from the shown clip and the lists name what can be picked", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    // Footage selected: it is the subject; the clip over it follows.
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    CHECK(t.available());
    CHECK(t.subject() == clipKey(f.footage));
    CHECK(t.follower() == clipKey(f.over));
    CHECK(t.subjectName() == QStringLiteral("A"));
    CHECK(t.followerName() == QStringLiteral("B"));
    CHECK(t.subjects().size() == 2); // both are footage here
    CHECK(t.followers().size() == 1);
    CHECK(t.followers()[0].toMap().value(QStringLiteral("track")).toString() == QStringLiteral("V2"));
    CHECK_FALSE(t.stabilize());
    CHECK(t.match() == QStringLiteral("position"));
    // Nothing marked yet: Track says what to do.
    CHECK(t.blocker().contains(QStringLiteral("box")));
    CHECK_FALSE(t.track(QStringLiteral("forward"), 3));
    CHECK(t.trackStatus() == t.blocker());
    CHECK(f.undoCount() == 0);

    // The other video clip selected: now it is the subject (also footage), the first clip follows.
    f.controller.selectClipById(f.over, ProjectController::Replace, false);
    CHECK(t.subject() == clipKey(f.over));
    CHECK(t.follower() == clipKey(f.footage));
    // A user pick wins until another clip is shown.
    t.setSubject(clipKey(f.footage));
    t.setFollower(clipKey(f.over));
    CHECK(t.subject() == clipKey(f.footage));
    CHECK(t.follower() == clipKey(f.over));
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    f.controller.selectClipById(f.over, ProjectController::Replace, false);
    CHECK(t.subject() == clipKey(f.over)); // the picks started over with the shown clip
}

TEST_CASE("Track Motion: a graphic (a still) follows the footage under it", "[app][track][motion]") {
    Fixture f(true);
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.over, ProjectController::Replace, false);
    CHECK(t.available());
    CHECK(t.subject() == clipKey(f.footage)); // a still is not footage: the footage under it is tracked
    CHECK(t.follower() == clipKey(f.over));
    CHECK(t.subjects().size() == 1);
    // Nothing under the playhead but the graphic: the section has nothing to track.
    f.controller.selectClipById(f.over, ProjectController::Replace, false);
    f.controller.setPlayhead(framesToTicks(40, kRate25)); // past both clips
    CHECK_FALSE(t.available());
    CHECK(t.subject().isEmpty());
}

TEST_CASE("Track Motion: a box tracked forward keys the follower's Position in one undo step", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    // The follower sits over the subject at frame 0; a 1920 x 1080 sequence on 1920 x 1080 footage: canvas = frame.
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    CHECK(t.box().size() == 4);
    CHECK(t.blocker().isEmpty());
    const double startX = f.value(f.over, ParamId::PositionX, 0);
    const double startY = f.value(f.over, ParamId::PositionY, 0);
    const std::size_t before = f.undoCount();
    REQUIRE(t.track(QStringLiteral("forward"), 8));
    CHECK_FALSE(t.tracking());
    CHECK(t.confidence().size() == 8);
    CHECK(t.trackStatus() == QStringLiteral("Tracked 8 frames"));
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Track Motion"));
    // The follower's Position moves with the content: +4.8 and -1.8 source (= canvas) pixels a frame.
    for (const std::int64_t frame : {0, 3, 8}) {
        INFO("frame " << frame);
        CHECK(f.value(f.over, ParamId::PositionX, frame) == Approx(startX + 4.8 * static_cast<double>(frame)).margin(1.0));
        CHECK(f.value(f.over, ParamId::PositionY, frame) == Approx(startY - 1.8 * static_cast<double>(frame)).margin(1.0));
    }
    CHECK(f.clip(f.over).properties()[ParamId::PositionX].isCurve());
    // Position only: no Scale or Rotation keys.
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::Scale].isCurve());
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::Rotation].isCurve());
    // The footage itself is untouched; the box was used up; the playhead is on the last frame tracked.
    CHECK_FALSE(f.clip(f.footage).properties()[ParamId::PositionX].isCurve());
    CHECK(t.box().isEmpty());
    CHECK(f.controller.playhead() == framesToTicks(8, kRate25));
    // Undo takes every key back at once.
    REQUIRE(f.controller.undo());
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::PositionX].isCurve());
    CHECK(f.value(f.over, ParamId::PositionX, 4) == Approx(startX));
}

TEST_CASE("Track Motion: backward, scale and rotation, and stabilizing the footage itself", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);

    // Backward from frame 10: the follower's keys run back to frame 4 (6 frames).
    f.controller.setPlayhead(framesToTicks(10, kRate25));
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    const double atTen = f.value(f.over, ParamId::PositionX, 10);
    REQUIRE(t.track(QStringLiteral("backward"), 6));
    CHECK(t.trackStatus() == QStringLiteral("Tracked 6 frames"));
    CHECK(f.value(f.over, ParamId::PositionX, 10) == Approx(atTen).margin(0.5)); // where it started stays
    CHECK(f.value(f.over, ParamId::PositionX, 6) == Approx(atTen - 4.8 * 4.0).margin(1.0));
    CHECK(f.controller.playhead() == framesToTicks(4, kRate25));
    REQUIRE(f.controller.undo());

    // "Position, scale, rotation" keys Scale and Rotation too (this content neither grows nor turns).
    f.controller.setPlayhead(Ticks{});
    t.setMatch(QStringLiteral("full"));
    CHECK(t.match() == QStringLiteral("full"));
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    REQUIRE(t.track(QStringLiteral("forward"), 6));
    CHECK(f.clip(f.over).properties()[ParamId::Scale].isCurve());
    CHECK(f.clip(f.over).properties()[ParamId::Rotation].isCurve());
    CHECK(f.value(f.over, ParamId::Scale, 5) == Approx(f.value(f.over, ParamId::Scale, 0)).margin(0.01));
    CHECK(f.value(f.over, ParamId::Rotation, 5) == Approx(f.value(f.over, ParamId::Rotation, 0)).margin(0.3));
    REQUIRE(f.controller.undo());
    t.setMatch(QStringLiteral("nonsense")); // anything else is a plain move
    CHECK(t.match() == QStringLiteral("position"));

    // Stabilize: the footage clip itself is keyed the other way, so the subject stays where it was.
    f.controller.setPlayhead(Ticks{});
    t.setStabilize(true);
    CHECK(t.stabilize());
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    CHECK(t.blocker().isEmpty()); // no follower needed
    REQUIRE(t.track(QStringLiteral("forward"), 6));
    CHECK(f.clip(f.footage).properties()[ParamId::PositionX].isCurve());
    CHECK(f.value(f.footage, ParamId::PositionX, 5) == Approx(-4.8 * 5.0).margin(1.5));
    CHECK(f.value(f.footage, ParamId::PositionY, 5) == Approx(1.8 * 5.0).margin(1.5));
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::PositionX].isCurve());
    CHECK(f.controller.undoText() == QStringLiteral("Undo Track Motion"));
}

TEST_CASE("Track Motion: the box is read through the footage clip's own Motion", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    // The footage is scaled to 50 % and moved: the same canvas box sits on another part of its picture; tracking still
    // finds the (textured) content there and the follower moves by what the footage content moves ON THE CANVAS (half).
    REQUIRE(f.controller.session()
                .apply("Motion", [&](const ProjectState& s) {
                    auto a = edit::setParameter(s, f.tl.seq, f.footage, edit::ParamRef::of(ParamId::Scale), ParamValue(0.5)); // the model's scale is a factor: 50 %
                    if (!a)
                        return a;
                    return edit::setParameter(a->state, f.tl.seq, f.footage, edit::ParamRef::of(ParamId::PositionX), ParamValue(120.0));
                })
                .has_value());
    // On the footage's picture (960 x 540 at the canvas centre, moved 120 px right) this box covers u 0.3..0.5, v 0.35..0.65 of
    // its frame: the same part of the textured content as the plain box of the first test.
    REQUIRE(t.setBox(0.4625, 0.425, 0.5625, 0.575));
    const double startX = f.value(f.over, ParamId::PositionX, 0);
    REQUIRE(t.track(QStringLiteral("forward"), 6));
    INFO(t.trackStatus().toStdString());
    CHECK(t.trackStatus() == QStringLiteral("Tracked 6 frames"));
    // 4.8 source pixels a frame at 50 % scale: 2.4 canvas pixels a frame.
    CHECK(f.value(f.over, ParamId::PositionX, 5) == Approx(startX + 2.4 * 5.0).margin(1.0));
}

namespace {

/// The region (x, y, width, height; frame fractions) the last track() sent to track.transform.
std::array<double, 4> sentRegion(const TrackMotionBridge& t) {
    const command::Json request = command::Json::parse(t.lastRequestJson());
    const command::Json& r = request.at("region");
    return {r.at("x").get<double>(), r.at("y").get<double>(), r.at("width").get<double>(), r.at("height").get<double>()};
}

/// What playback itself does with the canvas box: the layer the footage clip evaluates to, placed on the canvas the compositor
/// draws (the sequence's pixel aspect ratio included), and the box taken back to the stored frame. The ground truth the
/// tracker's region must equal.
std::array<double, 4> regionFromPlayback(const Fixture& f, std::array<double, 4> box) {
    const ProjectSnapshot snap = f.controller.snapshot();
    const Sequence& seq = *f.controller.currentSequence();
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(*snap, seq, f.controller.playhead(), specs, false);
    const auto spec = std::ranges::find_if(specs, [&](const playback::LayerSpec& s) { return s.clip == f.footage; });
    REQUIRE(spec != specs.end());
    const VideoStreamInfo& v = spec->item->info.video.at(static_cast<std::size_t>(spec->streamIndex));
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    const render::Affine2D toCanvas =
        render::layerToCanvas(spec->transform, v.width, v.height, par, {seq.width, seq.height, sequencePixelAspect(seq)});
    const render::Affine2D toFrame = toCanvas.inverted();
    double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (const auto& corner : {std::pair{box[0], box[1]}, std::pair{box[2], box[1]}, std::pair{box[2], box[3]}, std::pair{box[0], box[3]}}) {
        const render::Vec2 p = toFrame.map({corner.first * seq.width, corner.second * seq.height});
        lo[0] = std::min(lo[0], p.x / v.width);
        lo[1] = std::min(lo[1], p.y / v.height);
        hi[0] = std::max(hi[0], p.x / v.width);
        hi[1] = std::max(hi[1], p.y / v.height);
    }
    for (double& x : lo)
        x = std::clamp(x, 0.0, 1.0);
    for (double& x : hi)
        x = std::clamp(x, 0.0, 1.0);
    return {lo[0], lo[1], hi[0] - lo[0], hi[1] - lo[1]};
}

void setSequencePixelAspect(Fixture& f, int width, int height, int num, int den) {
    command::CommandEngine engine(f.controller.session());
    command::CommandRequest request;
    request.command = "sequence.setSettings";
    request.params = command::Json{{"sequence", f.controller.currentSequenceId().value},
                                   {"width", width},
                                   {"height", height},
                                   {"pixelAspect", command::Json{{"num", num}, {"den", den}}}};
    const auto result = engine.execute(request);
    INFO(command::toJson(result).dump());
    REQUIRE(result.ok());
}

} // namespace

TEST_CASE("Track Motion: the box of a mirrored stream is read on the mirrored side (review: own placement code)",
          "[app][track][motion]") {
    // A front-camera clip is stored mirrored: the picture the user sees has the left of the stored frame on the right. A box
    // drawn on the left of the monitor lies on the RIGHT of the stored frame, where the tracker looks.
    for (const bool mirrored : {false, true}) {
        INFO("mirrored " << mirrored);
        Fixture f(false, mirrored);
        f.controller.selectClipById(f.footage, ProjectController::Replace, false);
        const std::array<double, 4> box{0.10, 0.35, 0.30, 0.65};
        REQUIRE(f.bridge.setBox(box[0], box[1], box[2], box[3]));
        REQUIRE(f.bridge.track(QStringLiteral("forward"), 2));
        const std::array<double, 4> region = sentRegion(f.bridge);
        const std::array<double, 4> truth = regionFromPlayback(f, box);
        for (int i = 0; i < 4; ++i) {
            INFO("component " << i);
            CHECK(region[static_cast<std::size_t>(i)] == Approx(truth[static_cast<std::size_t>(i)]).margin(1e-6));
        }
        CHECK(region[0] == Approx(mirrored ? 0.70 : 0.10).margin(1e-6)); // x
        CHECK(region[1] == Approx(0.35).margin(1e-6));
        CHECK(region[2] == Approx(0.20).margin(1e-6));
        CHECK(region[3] == Approx(0.30).margin(1e-6));
    }
}

TEST_CASE("Track Motion: the box of a non-square-pixel sequence follows the canvas pixel aspect (review: own placement code)",
          "[app][track][motion]") {
    // 1440 x 1080 with pixels 4:3 wide shows the square-pixel 1920 x 1080 footage whole at 100 % (1920 / 1.3333 = 1440 canvas
    // pixels), so a box over the left tenth of the monitor is the left tenth of the stored frame. A placement that ignores the
    // ratio draws the footage 1920 canvas pixels wide on a 1440 canvas (cropped) and reads the box from the wrong part.
    Fixture f;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    setSequencePixelAspect(f, 1440, 1080, 4, 3);
    REQUIRE(f.controller.currentSequence()->width == 1440);
    const std::array<double, 4> box{0.10, 0.35, 0.30, 0.65};
    REQUIRE(f.bridge.setBox(box[0], box[1], box[2], box[3]));
    REQUIRE(f.bridge.track(QStringLiteral("forward"), 2));
    const std::array<double, 4> region = sentRegion(f.bridge);
    const std::array<double, 4> truth = regionFromPlayback(f, box);
    for (int i = 0; i < 4; ++i) {
        INFO("component " << i);
        CHECK(region[static_cast<std::size_t>(i)] == Approx(truth[static_cast<std::size_t>(i)]).margin(1e-6));
    }
    CHECK(region[0] == Approx(0.10).margin(1e-6));
    CHECK(region[2] == Approx(0.20).margin(1e-6));

    // 3:2 pixels on a 1920 x 1080 canvas: the footage is 1920 / 1.5 = 1280 canvas pixels wide, centred, so a box from 30 % to 50 %
    // of the canvas covers 20 % to 50 % of the stored frame (the square-pixel reading would say 30 % to 50 %).
    Fixture g;
    g.controller.selectClipById(g.footage, ProjectController::Replace, false);
    setSequencePixelAspect(g, 1920, 1080, 3, 2);
    const std::array<double, 4> centre{0.30, 0.35, 0.50, 0.65};
    REQUIRE(g.bridge.setBox(centre[0], centre[1], centre[2], centre[3]));
    REQUIRE(g.bridge.track(QStringLiteral("forward"), 2));
    const std::array<double, 4> wide = sentRegion(g.bridge);
    const std::array<double, 4> wideTruth = regionFromPlayback(g, centre);
    for (int i = 0; i < 4; ++i) {
        INFO("component " << i);
        CHECK(wide[static_cast<std::size_t>(i)] == Approx(wideTruth[static_cast<std::size_t>(i)]).margin(1e-6));
    }
    CHECK(wide[0] == Approx(0.20).margin(1e-6));
    CHECK(wide[2] == Approx(0.30).margin(1e-6));
}

TEST_CASE("Track Motion: Stop keeps the frames tracked so far as one undo step (review-2 #3)", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    const double startX = f.value(f.over, ParamId::PositionX, 0);
    auto pictures = std::make_shared<FakePictures>();
    pictures->onEach = [&](const masks::TrackProgress& p) {
        if (p.frame == 4) {
            t.cancelTrack(); // Stop pressed while the fifth picture is being asked for
        }
    };
    t.setTrackingServices(pictures);
    const std::size_t before = f.undoCount();
    REQUIRE(t.track(QStringLiteral("forward"), 20));
    CHECK_FALSE(t.tracking());
    INFO(t.trackStatus().toStdString());
    CHECK(t.trackStatus() == QStringLiteral("Stopped after 4 frames"));
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Track Motion"));
    // The follower's Position was keyed up to frame 4 and no further; the playhead sits on the last frame tracked.
    const Curve* x = f.clip(f.over).properties()[ParamId::PositionX].curve();
    REQUIRE(x != nullptr);
    REQUIRE_FALSE(x->keys.empty());
    CHECK(x->keys.back().time <= f.clip(f.over).curveTimeAt(f.clip(f.over).start() + framesToTicks(4, kRate25)));
    CHECK(f.value(f.over, ParamId::PositionX, 4) == Approx(startX + 4.8 * 4.0).margin(1.0));
    CHECK(f.controller.playhead() == framesToTicks(4, kRate25));
    CHECK(t.confidence().size() == 4);
    REQUIRE(f.controller.undo());
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::PositionX].isCurve());

    // Stopped before any frame was tracked: nothing is changed and it says so.
    int asked = 0;
    pictures->onEach = nullptr;
    pictures->onFrame = [&] {
        if (++asked == 1) {
            t.cancelTrack(); // while the start picture is read
        }
    };
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    const std::size_t undoBefore = f.undoCount();
    REQUIRE(t.track(QStringLiteral("forward"), 20));
    CHECK(t.trackStatus() == QStringLiteral("Tracking stopped before the first frame"));
    CHECK(f.undoCount() == undoBefore);
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::PositionX].isCurve());
}

TEST_CASE("The tracking confidence strip stays bounded and keeps the lost frames (review-2 #3)", "[app][track][motion]") {
    ConfidenceBuckets strip(480);
    // A short track: one value per frame.
    for (int i = 0; i < 8; ++i) {
        strip.add(0.9);
    }
    CHECK(strip.values().size() == 8);
    CHECK(strip.framesPerBucket() == 1);
    // A 10-minute 60 fps track: 36,000 frames, one of them lost; the strip never grows past its capacity.
    strip.clear();
    constexpr std::size_t kFrames = 36000;
    constexpr std::size_t kLost = 27777;
    for (std::size_t i = 0; i < kFrames; ++i) {
        strip.add(i == kLost ? -1.0 : 0.8);
        REQUIRE(strip.values().size() <= 480);
    }
    CHECK(strip.frames() == kFrames);
    CHECK(strip.values().size() >= 240);
    const std::size_t per = strip.framesPerBucket();
    CHECK(per >= 64);
    CHECK((per & (per - 1)) == 0); // a power of two: buckets merge in pairs
    // The buckets cover every frame, and the lost one is marked in the bucket that holds it (the minimum is kept).
    CHECK(strip.values().size() * per >= kFrames);
    CHECK(strip.values().size() * per < kFrames + per);
    CHECK(strip.values()[kLost / per] == Approx(-1.0));
    CHECK(std::ranges::count(strip.values(), -1.0) == 1);
    // The cost of a progress report no longer grows with the track: the published list is at most 480 long.
    TrackMotionBridge bridge;
    CHECK(bridge.confidence().isEmpty());
    CHECK(bridge.confidenceFramesPerValue() == 1);
}

TEST_CASE("Track Motion: a mask of the footage marks the subject, and a box and a mask exclude each other", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    CHECK(t.masks().isEmpty());
    // An ellipse mask on the footage (mask.add).
    {
        command::CommandEngine engine(f.controller.session(), ai::edit::editingCommands());
        command::CommandRequest request;
        request.command = "mask.add";
        request.params = command::Json{{"clip", f.footage.value}, {"shape", "ellipse"},
                                       {"params", command::Json{{"center.x", 0.5}, {"center.y", 0.5}, {"size.x", 0.2}, {"size.y", 0.3}}}};
        REQUIRE(engine.execute(request).ok());
    }
    REQUIRE(t.masks().size() == 1);
    const QString key = t.masks()[0].toMap().value(QStringLiteral("key")).toString();
    CHECK_FALSE(key.isEmpty());
    // Box, then mask: the box goes; mask, then box: the mask goes.
    REQUIRE(t.setBox(0.1, 0.1, 0.2, 0.2));
    t.setMask(key);
    CHECK(t.mask() == key);
    CHECK(t.box().isEmpty());
    CHECK(t.blocker().isEmpty());
    REQUIRE(t.setBox(0.35, 0.35, 0.55, 0.65));
    CHECK(t.mask().isEmpty());
    t.setMask(key);
    // Tracking through the mask: the same content motion.
    const double startX = f.value(f.over, ParamId::PositionX, 0);
    const std::size_t before = f.undoCount();
    REQUIRE(t.track(QStringLiteral("forward"), 6));
    CHECK(t.trackStatus() == QStringLiteral("Tracked 6 frames"));
    CHECK(f.undoCount() == before + 1);
    CHECK(f.value(f.over, ParamId::PositionX, 5) == Approx(startX + 4.8 * 5.0).margin(1.0));
    // A mask that is not there any more is dropped.
    t.setMask(QStringLiteral("e999/9"));
    CHECK(t.mask().isEmpty());
}

TEST_CASE("Track Motion: what cannot run says why, and a track never overwrites a newer edit", "[app][track][motion]") {
    Fixture f;
    TrackMotionBridge& t = f.bridge;
    f.controller.selectClipById(f.footage, ProjectController::Replace, false);
    // No area yet.
    CHECK(t.blocker().contains(QStringLiteral("box")));
    // A box too small, or off the canvas: refused, nothing set.
    CHECK_FALSE(t.setBox(0.5, 0.5, 0.5005, 0.5005));
    CHECK(t.box().isEmpty());
    CHECK_FALSE(t.setBox(std::nan(""), 0.1, 0.5, 0.5));
    // Corners in any order and outside the canvas are clamped.
    REQUIRE(t.setBox(0.9, 0.9, 1.4, 1.4));
    CHECK(t.box()[2].toDouble() == Approx(1.0));
    CHECK(t.box()[0].toDouble() == Approx(0.9));
    t.clearBox();
    CHECK(t.box().isEmpty());
    // placeBox puts a box where the follower sits (the canvas middle for a follower at its default position).
    t.placeBox();
    REQUIRE(t.box().size() == 4);
    CHECK((t.box()[0].toDouble() + t.box()[2].toDouble()) / 2.0 == Approx(0.5).margin(0.01));
    CHECK((t.box()[1].toDouble() + t.box()[3].toDouble()) / 2.0 == Approx(0.5).margin(0.01));
    CHECK(t.blocker().isEmpty());

    // The playhead where the follower is not: tracking starts from a frame where the clip sits over the subject.
    f.controller.setPlayhead(framesToTicks(40, kRate25));
    CHECK_FALSE(t.available());
    CHECK_FALSE(t.track(QStringLiteral("forward"), 3));
    f.controller.setPlayhead(Ticks{});
    CHECK(t.available());
    // A time-remapped footage clip cannot be tracked (the engine's rule), and says so before anything runs.
    REQUIRE(f.controller.session().apply("Remap", [&](const ProjectState& s) { return edit::setTimeRemapEnabled(s, f.tl.seq, f.footage, true); }));
    CHECK(t.blocker().contains(QStringLiteral("Time Remapping")));
    CHECK_FALSE(t.track(QStringLiteral("forward"), 3));
    REQUIRE(f.controller.undo());
    CHECK(t.blocker().isEmpty());

    // An edit made while the track ran wins: the result is dropped and says so.
    auto pictures = std::make_shared<FakePictures>();
    pictures->onProgress = [&] {
        REQUIRE(f.controller.session()
                    .apply("Nudge", [&](const ProjectState& s) { return edit::setParameter(s, f.tl.seq, f.over, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.5)); })
                    .has_value());
    };
    t.setTrackingServices(pictures);
    REQUIRE(t.track(QStringLiteral("forward"), 3));
    CHECK(t.trackStatus().contains(QStringLiteral("changed")));
    CHECK_FALSE(f.clip(f.over).properties()[ParamId::PositionX].isCurve());
    CHECK(f.controller.undoText() != QStringLiteral("Undo Track Motion"));
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

namespace {

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF p, Qt::MouseButtons buttons) {
    QMouseEvent event(type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QCoreApplication::sendEvent(window, &event);
}

} // namespace

TEST_CASE("Track Motion: Effect Controls' section and the monitor's box in the shell (screenshots)", "[app][track][motion][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* track = shell.engine.singletonInstance<TrackMotionBridge*>("Wabcut", "TrackMotion");
    REQUIRE(controller);
    REQUIRE(track);
    Timeline tl("V2 |Bbbbbbbbbbbbbbbbbbbbbbbb|\n"
                "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\n"
                "A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId footage = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const ClipId over = clipAt(*controller->snapshot(), tl.seq, "V2@0");
    controller->selectClipById(footage, ProjectController::Replace, false);
    controller->setPlayhead(Ticks{});
    track->setSynchronous(true);
    track->setTrackingServices(std::make_shared<FakePictures>());
    REQUIRE(waitFor([&] { return track->available(); }));

    // Closed by default: Effect Controls keeps its room (no section, no box on the monitor).
    QQuickItem* section = nullptr;
    QQuickItem* headerButton = nullptr;
    REQUIRE(waitFor([&] {
        section = findNamed(shell.window->contentItem(), QStringLiteral("trackMotionSection"));
        headerButton = findNamed(shell.window->contentItem(), QStringLiteral("trackMotionToggle"));
        return section != nullptr && headerButton != nullptr && headerButton->isVisible();
    }));
    CHECK_FALSE(section->isVisible());
    CHECK_FALSE(track->panelOpen());
    CHECK(headerButton->isEnabled()); // footage is under the playhead
    const auto grabFrame = [&] {
        spin(60);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        return image;
    };
    const auto saveItem = [&](const QImage& image, const char* name, const QString& file) {
        QQuickItem* item = shell.item(name);
        if (!item)
            item = qobject_cast<QQuickItem*>(shell.window->contentItem()->findChild<QObject*>(QLatin1String(name)));
        REQUIRE(item != nullptr);
        const double dpr = image.devicePixelRatio();
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect()).save(outputDir() + QLatin1Char('/') + file));
    };
    const auto click = [&](QQuickItem* item) {
        REQUIRE_FALSE(shell.grab().isNull()); // a hidden window places its layouts when it renders
        // The section scrolls inside Effect Controls: bring the item into its view first.
        if (QQuickItem* flick = findNamed(section, QStringLiteral("trackMotionFlick"))) {
            if (auto* content = flick->property("contentItem").value<QQuickItem*>(); content && content->isAncestorOf(item)) {
                const QPointF at = item->mapToItem(content, QPointF(0, 0));
                flick->setProperty("contentY", std::max(0.0, at.y() - 8.0));
                REQUIRE_FALSE(shell.grab().isNull());
            }
        }
        spin(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 30);
        const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        mouse(shell.window.get(), QEvent::MouseButtonPress, p, Qt::LeftButton);
        mouse(shell.window.get(), QEvent::MouseButtonRelease, p, Qt::NoButton);
        spin(50);
    };

    // The panel header's button opens it (Clip > Track Motion… and the palette do the same); the summary says what it will do.
    {
        const QImage image = grabFrame();
        saveItem(image, "effectControls", QStringLiteral("track-motion-closed.png"));
    }
    click(headerButton);
    REQUIRE(waitFor([&] { return section->isVisible(); }));
    CHECK(track->panelOpen());
    CHECK(headerButton->property("checked").toBool());
    CHECK(findNamed(section, QStringLiteral("trackSummary"))->property("text").toString() == QStringLiteral("B follows the subject in A."));
    REQUIRE(waitFor([&] { return findNamed(section, QStringLiteral("trackForward")) && findNamed(section, QStringLiteral("trackForward"))->isVisible(); }));

    // Draw box: the monitor's overlay takes a real drag; the box lands where it was dragged (fractions of the canvas).
    QQuickItem* overlay = findNamed(shell.window->contentItem(), QStringLiteral("programTrackOverlay"));
    REQUIRE(overlay);
    CHECK_FALSE(overlay->isVisible()); // no box, no tool: it passes everything through
    QQuickItem* draw = findNamed(section, QStringLiteral("trackDrawBox"));
    REQUIRE(draw);
    click(draw);
    CHECK(track->boxTool());
    REQUIRE(waitFor([&] { return overlay->isVisible(); }));
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* drawArea = findNamed(overlay, QStringLiteral("trackDrawArea"));
    REQUIRE(drawArea);
    REQUIRE(drawArea->isVisible());
    const QPointF top = drawArea->mapToScene(QPointF(drawArea->width() * 0.35, drawArea->height() * 0.3));
    const QPointF end = drawArea->mapToScene(QPointF(drawArea->width() * 0.55, drawArea->height() * 0.7));
    mouse(shell.window.get(), QEvent::MouseButtonPress, top, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseMove, (top + end) / 2, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseMove, end, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, end, Qt::NoButton);
    spin(50);
    REQUIRE(track->box().size() == 4);
    CHECK(track->box()[0].toDouble() == Approx(0.35).margin(0.01));
    CHECK(track->box()[1].toDouble() == Approx(0.3).margin(0.01));
    CHECK(track->box()[2].toDouble() == Approx(0.55).margin(0.01));
    CHECK(track->box()[3].toDouble() == Approx(0.7).margin(0.01));
    CHECK_FALSE(track->boxTool()); // the tool disarms with the box
    QQuickItem* box = findNamed(overlay, QStringLiteral("trackBox"));
    REQUIRE(box);
    REQUIRE(waitFor([&] { return box->isVisible(); }));

    // A drag inside moves the box (it keeps its size); a corner handle resizes it with the opposite corner fixed.
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* body = findNamed(box, QStringLiteral("trackBoxBody"));
    REQUIRE(body);
    const QPointF centre = body->mapToScene(QPointF(body->width() / 2, body->height() / 2));
    const double widthBefore = track->box()[2].toDouble() - track->box()[0].toDouble();
    mouse(shell.window.get(), QEvent::MouseButtonPress, centre, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseMove, centre + QPointF(overlay->width() * 0.1, 0), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseMove, centre + QPointF(overlay->width() * 0.2, overlay->height() * 0.05), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, centre + QPointF(overlay->width() * 0.2, overlay->height() * 0.05), Qt::NoButton);
    spin(50);
    CHECK(track->box()[0].toDouble() == Approx(0.55).margin(0.015));
    CHECK(track->box()[2].toDouble() - track->box()[0].toDouble() == Approx(widthBefore).margin(0.002));
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* corner = findNamed(overlay, QStringLiteral("trackBoxHandle2")); // bottom right
    REQUIRE(corner);
    const QPointF cornerAt = corner->mapToScene(QPointF(corner->width() / 2, corner->height() / 2));
    const double left = track->box()[0].toDouble();
    const double topY = track->box()[1].toDouble();
    mouse(shell.window.get(), QEvent::MouseButtonPress, cornerAt, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseMove, cornerAt + QPointF(overlay->width() * 0.05, overlay->height() * 0.05), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseMove, cornerAt + QPointF(overlay->width() * 0.1, overlay->height() * 0.1), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, cornerAt + QPointF(overlay->width() * 0.1, overlay->height() * 0.1), Qt::NoButton);
    spin(50);
    CHECK(track->box()[0].toDouble() == Approx(left).margin(1e-6)); // the opposite corner stayed
    CHECK(track->box()[1].toDouble() == Approx(topY).margin(1e-6));
    CHECK(track->box()[2].toDouble() > left + widthBefore + 0.05);
    // Put it back over the subject for the shot, and screenshot both themes. The section's header is pinned above its
    // scrolling part: it is in view whatever the scroll (the clicks above scrolled it), and the section opens at its top.
    REQUIRE(track->setBox(0.35, 0.3, 0.55, 0.7));
    QQuickItem* flickable = findNamed(section, QStringLiteral("trackMotionFlick"));
    REQUIRE(flickable);
    const auto headerInView = [&](bool summaryToo = true) {
        QQuickItem* header = findNamed(section, QStringLiteral("trackMotionHeader"));
        QQuickItem* panel = shell.item("effectControls");
        REQUIRE(header);
        REQUIRE(panel);
        REQUIRE(header->isVisible());
        const QRectF headerAt = header->mapRectToScene(QRectF(0, 0, header->width(), header->height()));
        const QRectF sectionAt = section->mapRectToScene(QRectF(0, 0, section->width(), section->height()));
        const QRectF panelAt = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        QQuickItem* list = findNamed(panel, QStringLiteral("effectControlsList"));
        INFO("section " << sectionAt.top() << " .. " << sectionAt.bottom() << " (implicit " << section->implicitHeight() << "), panel "
                        << panelAt.top() << " .. " << panelAt.bottom() << ", effect list height " << (list ? list->height() : -1.0)
                        << ", window " << shell.window->width() << " x " << shell.window->height());
        CHECK(sectionAt.top() >= panelAt.top());
        CHECK(sectionAt.bottom() <= panelAt.bottom() + 1.0);
        CHECK(headerAt.top() >= sectionAt.top());
        CHECK(headerAt.bottom() <= sectionAt.bottom());
        // At the top of the section the summary line right under the header is shown (scrolled to the end it has gone up).
        QQuickItem* summary = findNamed(section, QStringLiteral("trackSummary"));
        REQUIRE(summary);
        const QRectF summaryAt = summary->mapRectToScene(QRectF(0, 0, summary->width(), summary->height()));
        if (summaryToo) {
            CHECK(summaryAt.top() >= headerAt.bottom() - 1.0);
            CHECK(summaryAt.bottom() <= sectionAt.bottom());
        }
    };
    flickable->setProperty("contentY", 120.0);
    track->setPanelOpen(false);
    REQUIRE(waitFor([&] { return !section->isVisible(); }));
    track->setPanelOpen(true);
    REQUIRE(waitFor([&] { return section->isVisible(); }));
    REQUIRE_FALSE(shell.grab().isNull());
    CHECK(flickable->property("contentY").toDouble() == Approx(0.0).margin(0.5)); // opens at its top
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        const QImage image = grabFrame();
        const QString mode = dark ? QStringLiteral("dark") : QStringLiteral("light");
        headerInView();
        saveItem(image, "effectControls", QStringLiteral("track-motion-section-%1.png").arg(mode));
        saveItem(image, "programFrameArea", QStringLiteral("track-motion-monitor-%1.png").arg(mode));
        REQUIRE(image.save(outputDir() + QStringLiteral("/track-motion-%1.png").arg(mode)));
    }
    // The smallest window: the section keeps its header and the line under it, and scrolls for the rest.
    shell.resize(1100, 700);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        const QImage image = grabFrame();
        const QString mode = dark ? QStringLiteral("dark") : QStringLiteral("light");
        headerInView();
        saveItem(image, "effectControls", QStringLiteral("track-motion-small-%1.png").arg(mode));
        REQUIRE(image.save(outputDir() + QStringLiteral("/track-motion-small-window-%1.png").arg(mode)));
    }
    flickable->setProperty("contentY", 1000.0); // scrolled to the end: the header is still there
    REQUIRE_FALSE(shell.grab().isNull());
    headerInView(false);
    CHECK(flickable->property("interactive").toBool() == (flickable->property("contentHeight").toDouble() > flickable->height()));
    flickable->setProperty("contentY", 0.0);
    shell.resize(kShotWidth, kShotHeight);
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(shell.settle(true, shell.themeColor("accentFill")));

    // Track from the section: one undo step, the progress and confidence shown while it ran, the box used up.
    QQuickItem* forward = findNamed(section, QStringLiteral("trackForward"));
    REQUIRE(forward);
    REQUIRE(waitFor([&] { return forward->property("enabled").toBool(); }));
    const std::size_t before = controller->session().undoCount();
    click(forward);
    REQUIRE(waitFor([&] { return !track->tracking(); }));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Track Motion"));
    CHECK(track->trackStatus().startsWith(QStringLiteral("Tracked")));
    CHECK(track->box().isEmpty());
    const Clip& follower = controller->currentSequence()->findClip(over)->clip();
    CHECK(follower.properties()[ParamId::PositionX].isCurve());
    {
        const QImage image = grabFrame();
        saveItem(image, "effectControls", QStringLiteral("track-motion-done.png"));
    }

    // The blocker is the Track buttons' disabled reason; the follower and subject menus list the clips.
    CHECK(findNamed(section, QStringLiteral("trackSubjectButton"))->property("text").toString() == QStringLiteral("A"));
    CHECK(findNamed(section, QStringLiteral("trackFollowerButton"))->property("text").toString() == QStringLiteral("B"));
    CHECK_FALSE(forward->property("enabled").toBool()); // the box is used up: draw another
    CHECK(forward->property("disabledReason").toString().contains(QStringLiteral("box")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("tracking/"), Qt::CaseInsensitive));
}

TEST_CASE("Track Motion: Clip > Track Motion... and the palette open the section; it needs footage under the playhead",
          "[app][track][motion][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* track = shell.engine.singletonInstance<TrackMotionBridge*>("Wabcut", "TrackMotion");
    auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(controller);
    REQUIRE(track);
    REQUIRE(keys);
    Timeline tl("V2 |Bbbbbbbbbbbbbbbbbbbbbbbb|\n"
                "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\n"
                "A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->selectClipById(clipAt(*controller->snapshot(), tl.seq, "V1@0"), ProjectController::Replace, false);
    controller->setPlayhead(Ticks{});
    REQUIRE(waitFor([&] { return track->available(); }));
    CHECK_FALSE(track->panelOpen());
    // The keymap command (what the Clip menu item and the palette run) opens it.
    REQUIRE(keys->trigger(QStringLiteral("clip.trackMotion")));
    CHECK(track->panelOpen());
    QQuickItem* section = nullptr;
    REQUIRE(waitFor([&] {
        section = findNamed(shell.window->contentItem(), QStringLiteral("trackMotionSection"));
        return section && section->isVisible();
    }));
    // Closing: its x, then the command again.
    QQuickItem* close = findNamed(section, QStringLiteral("trackMotionClose"));
    REQUIRE(close != nullptr);
    REQUIRE(QMetaObject::invokeMethod(close, "clicked"));
    CHECK_FALSE(track->panelOpen());
    REQUIRE(waitFor([&] { return !section->isVisible(); }));
    // Closing drops an armed Draw box (nothing left to draw on).
    track->setPanelOpen(true);
    track->setBoxTool(true);
    track->setPanelOpen(false);
    CHECK_FALSE(track->boxTool());
    // Past the clips there is nothing to track: the header button is greyed out with its reason, and the command says so.
    controller->setPlayhead(framesToTicks(40, kRate25));
    REQUIRE(waitFor([&] { return !track->available(); }));
    QQuickItem* headerButton = findNamed(shell.window->contentItem(), QStringLiteral("trackMotionToggle"));
    REQUIRE(headerButton != nullptr);
    CHECK_FALSE(headerButton->isEnabled());
    CHECK(headerButton->property("tooltip").toString().contains(QStringLiteral("footage")));
    REQUIRE(keys->trigger(QStringLiteral("clip.trackMotion")));
    CHECK(track->panelOpen());
    spin(50);
    CHECK_FALSE(findNamed(shell.window->contentItem(), QStringLiteral("trackMotionSection"))->isVisible()); // nothing to show
    CHECK(headerButton->isEnabled()); // open: it can be closed again
    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("tracking/"), Qt::CaseInsensitive));
}
