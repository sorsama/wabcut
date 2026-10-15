// C.15 (gaming): sequence presets exact in ticks at high frame rates, the variable-frame-rate conform policy, and the
// facecam / vertical-shorts layouts as plain clip edits.

#include "TimelineFixture.h"

#include <wabcut/model/Layouts.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/Vfr.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) { return state.findSequence(seq)->findClip(id)->clip(); }

/// The timeline's sequence turned into a 1080x1920 vertical one (clips stay where they are).
void makeVertical(Timeline& tl) {
    auto seq = std::make_shared<Sequence>(*tl.state.findSequence(tl.seq));
    seq->width = 1080;
    seq->height = 1920;
    for (SequencePtr& s : tl.state.sequences) {
        if (s->id == tl.seq) {
            s = seq;
        }
    }
}

/// Where a source point (display px) lands on the canvas under the clip's Motion (render::layerToCanvas, no rotation).
std::pair<double, double> onCanvas(const Clip& c, double sourceW, double sourceH, double canvasW, double canvasH, double x, double y) {
    const ClipProperties& p = c.properties();
    const double s = p.value(ParamId::Scale);
    return {canvasW / 2.0 + p.value(ParamId::PositionX) + s * (x - sourceW / 2.0 - p.value(ParamId::AnchorX)),
            canvasH / 2.0 + p.value(ParamId::PositionY) + s * (y - sourceH / 2.0 - p.value(ParamId::AnchorY))};
}

std::vector<std::string> layoutEffects(const Clip& c) {
    std::vector<std::string> out;
    for (const EffectInstance& e : c.effects()) {
        if (e.findData(kLayoutDataKey)) {
            out.push_back(e.effect);
        }
    }
    return out;
}

double param(const Clip& c, std::string_view effect, std::string_view key) {
    for (const EffectInstance& e : c.effects()) {
        if (e.effect == effect) {
            return effectParameterAt(e, key, Ticks{}, -1.0);
        }
    }
    return -1.0;
}

} // namespace

// ---- Presets and frame rates ----------------------------------------------------------------------------------------

TEST_CASE("Sequence presets are exact in ticks at 60 to 240 fps (C.15)", "[model][gaming][presets]") {
    std::set<std::string_view> ids;
    for (const SequencePreset& p : sequencePresets()) {
        INFO(p.id);
        CHECK(ids.insert(p.id).second);
        CHECK(p.frameRate.isExact());
        // A whole number of seconds' worth of frames lands exactly on it: no drift (an hour; 1001 s for NTSC rates).
        const std::int64_t seconds = p.frameRate.den == 1 ? 3600 : 1001;
        const std::int64_t frames = seconds * p.frameRate.num / p.frameRate.den;
        CHECK(frames * p.frameRate.den == seconds * p.frameRate.num);
        CHECK(framesToTicks(frames, p.frameRate) == Ticks::fromSeconds(seconds));
        CHECK(ticksToFrames(Ticks::fromSeconds(seconds), p.frameRate) == frames);
        const edit::SequenceSettings s = sequenceSettingsFor(p);
        CHECK(s.width == p.width);
        CHECK(s.frameRate == p.frameRate);
        CHECK(s.name == p.name);
    }
    for (const char* id : {"1080p60", "1080p120", "1080p144", "1080p240", "1440p60", "1440p144", "2160p60", "2160p240"}) {
        CHECK(findSequencePreset(id) != nullptr);
    }
    const SequencePreset* shorts = findSequencePreset("shorts");
    REQUIRE(shorts);
    CHECK(shorts->width == 1080);
    CHECK(shorts->height == 1920);
    CHECK(shorts->frameRate == Rate{60, 1});
    CHECK(findSequencePreset("1080p165") == nullptr);
    // A sequence made from a preset is valid and saves.
    const auto made = edit::addSequence(ProjectState{}, sequenceSettingsFor(*findSequencePreset("1440p144"), "Ranked"));
    REQUIRE(made.has_value());
    requireValid(made->state);
    CHECK(made->state.findSequence(made->id)->frameRate == Rate{144, 1});
}

TEST_CASE("Variable-frame-rate sources conform to the sequence by timestamp (C.15)", "[model][gaming][vfr]") {
    VideoStreamInfo vfr;
    vfr.width = 1920;
    vfr.height = 1080;
    vfr.frameRate = Rate{5997, 100}; // an OBS capture averaging 59.97
    vfr.variableFrameRate = true;
    VideoStreamInfo cfr = vfr;
    cfr.variableFrameRate = false;
    cfr.frameRate = Rate{30, 1};
    const Rate seq60{60, 1};
    CHECK(vfrConformPolicy() == VfrConform::LatestAtOrBefore);
    CHECK(editingRate(vfr, seq60) == seq60);
    CHECK(editingRate(cfr, seq60) == Rate{30, 1});
    VideoStreamInfo unknown = cfr;
    unknown.frameRate = Rate{0, 1};
    CHECK(editingRate(unknown, seq60) == seq60);
    // Snapping uses that grid.
    const Ticks oneFrame60 = framesToTicks(1, seq60);
    CHECK(snapToEditingGrid(Ticks{oneFrame60.value * 3 + 5}, vfr, seq60) == Ticks{oneFrame60.value * 3});
    CHECK(snapToEditingGrid(framesToTicks(7, Rate{30, 1}) + Ticks{5}, cfr, seq60) == framesToTicks(7, Rate{30, 1}));
    // Nominal rates for display.
    CHECK(nominalRate(Rate{5993, 100}) == Rate{60000, 1001});
    CHECK(nominalRate(Rate{14390, 100}) == Rate{144, 1});
    CHECK(nominalRate(Rate{9050, 100}) == Rate{90, 1});
    CHECK(nominalRate(Rate{7, 1}) == Rate{7, 1});

    // A one-hour VFR clip in a 60 fps sequence: every sequence frame asks for an exact source time, no drift.
    Timeline tl("    V1 |Aaaa|\n", {{'A', 25 * 3600 * 2}});
    Clip clip = tl.sequence().videoTracks[0]->clips[0];
    clip.timelineRange = TimeRange{Ticks{}, Ticks::fromSeconds(3600)};
    clip.sourceIn = Ticks::fromSeconds(10);
    const std::vector<Ticks> first = conformedSourceTimes(clip, seq60, 0, 3);
    REQUIRE(first.size() == 3);
    CHECK(first[0] == Ticks::fromSeconds(10));
    CHECK(first[1] == Ticks::fromSeconds(10) + oneFrame60);
    const std::vector<Ticks> last = conformedSourceTimes(clip, seq60, 216000 - 1, 2); // the last frame, then past the end
    REQUIRE(last.size() == 1);
    CHECK(last[0] == Ticks::fromSeconds(10 + 3600) - oneFrame60);
}

// ---- Layouts -----------------------------------------------------------------------------------------------------------

TEST_CASE("placeRegion centre-crops a region to fill a target exactly (C.15)", "[model][gaming][layout]") {
    // A 1920x1080 picture into the top 60 % of a 1080x1920 frame: the region is trimmed to that box's aspect.
    const NormalizedRect top{0.0, 0.0, 1.0, 0.6};
    const RegionPlacement p = placeRegion(1920, 1080, NormalizedRect{}, top, 1080, 1920);
    CHECK(p.scale == Approx(1152.0 / 1080.0));
    CHECK(p.shown.height == Approx(1.0));
    CHECK(p.shown.width == Approx((1080.0 / p.scale) / 1920.0));
    CHECK(p.shown.x == Approx((1.0 - p.shown.width) / 2.0));
    CHECK(p.positionY == Approx(1152.0 / 2.0 - 960.0));
    CHECK(p.anchorX == Approx(0.0).margin(1e-9));
    // A region off centre: its centre is the anchor.
    const RegionPlacement cam = placeRegion(1920, 1080, NormalizedRect{0.75, 0.0, 0.25, 0.25}, NormalizedRect{0.5, 0.5, 0.25, 0.25}, 1920, 1080);
    CHECK(cam.anchorX == Approx(1920 * 0.875 - 960));
    CHECK(cam.anchorY == Approx(1080 * 0.125 - 540));
    CHECK(cam.scale == Approx(1.0));
}

TEST_CASE("Picture-in-picture puts the facecam in a corner with rounded corners - a border and a shadow (C.15)", "[model][gaming][layout]") {
    Timeline tl(R"(
        V2 |Ffffffffff|
        V1 |Gggggggggg|
        A1 |Gggggggggg|
    )");
    tl.linkMatching();
    LayoutRequest req;
    req.gameplay = tl.clip("V1@0");
    req.facecam = tl.clip("V2@0");
    req.options.corner = LayoutCorner::BottomLeft;
    req.options.size = 0.25;
    req.options.margin = 0.05;
    const auto laid = edit::applyLayout(tl.state, tl.seq, req);
    REQUIRE(laid.has_value());
    const ProjectState& s = laid->edit.state;
    requireValid(s);
    const Clip& game = clipOf(s, tl.seq, req.gameplay);
    const Clip& cam = clipOf(s, tl.seq, req.facecam);
    CHECK(game.properties().value(ParamId::Scale) == Approx(1.0)); // 1920x1080 already fills 1920x1080
    CHECK(layoutEffects(game).empty());
    // The facecam box: 480 x 270 at 54 px from the left and bottom edges.
    const auto topLeft = onCanvas(cam, 1920, 1080, 1920, 1080, 0, 0);
    const auto bottomRight = onCanvas(cam, 1920, 1080, 1920, 1080, 1920, 1080);
    CHECK(topLeft.first == Approx(54.0));
    CHECK(bottomRight.first == Approx(54.0 + 480.0));
    CHECK(bottomRight.second == Approx(1080.0 - 54.0));
    CHECK(topLeft.second == Approx(1080.0 - 54.0 - 270.0));
    CHECK(layoutEffects(cam) == std::vector<std::string>{"wabcut.crop", "wabcut.mask", std::string(kBorderEffect), "wabcut.dropShadow"});
    CHECK(param(cam, kBorderEffect, "width") == 4.0);
    const EffectInstance& mask = cam.effects()[1];
    REQUIRE(mask.masks.size() == 1);
    CHECK(mask.masks[0].shape == MaskShape::Rectangle);
    CHECK(maskValue(mask.masks[0], "roundness", Ticks{}) == Approx(0.24));
    // Applying again (a circle now) replaces the layout's effects instead of stacking them.
    req.options.circle = true;
    req.options.borderWidth = 0.0;
    req.options.shadow = false;
    const auto again = edit::applyLayout(s, tl.seq, req);
    REQUIRE(again.has_value());
    const Clip& round = clipOf(again->edit.state, tl.seq, req.facecam);
    CHECK(layoutEffects(round) == std::vector<std::string>{"wabcut.crop", "wabcut.mask"});
    CHECK(round.effects()[1].masks[0].shape == MaskShape::Ellipse);
    // The circle shows the centre square of the facecam.
    CHECK(param(round, "wabcut.crop", "left") == Approx((1.0 - 1080.0 / 1920.0) / 2.0));
    requireValid(again->edit.state);
    // Saved and read back the same.
    const auto back = io::fromJson(io::toJson(again->edit.state).value());
    REQUIRE(back.has_value());
    CHECK(clipOf(*back, tl.seq, req.facecam) == round);
}

TEST_CASE("Vertical split puts the gameplay on top and a facecam cut from the capture below (C.15)", "[model][gaming][layout]") {
    Timeline tl(R"(
        V2 |..........|
        V1 |Gggggggggg|
    )");
    makeVertical(tl);
    LayoutRequest req;
    req.preset = LayoutPreset::VerticalSplit;
    req.gameplay = tl.clip("V1@0");
    req.options.facecamRegion = NormalizedRect{0.75, 0.70, 0.25, 0.30}; // the webcam overlay, bottom right of the capture
    const auto laid = edit::applyLayout(tl.state, tl.seq, req);
    REQUIRE(laid.has_value());
    const ProjectState& s = laid->edit.state;
    requireValid(s);
    REQUIRE(laid->facecam.isValid());
    // The facecam copy went on V2 (free) and is linked with the gameplay.
    const Sequence& seq = *s.findSequence(tl.seq);
    REQUIRE(seq.videoTracks[1]->clips.size() == 1);
    const Clip& cam = seq.videoTracks[1]->clips[0];
    CHECK(cam.id == laid->facecam);
    CHECK(cam.media == tl.media('G'));
    const Clip& game = clipOf(s, tl.seq, req.gameplay);
    CHECK(cam.linkGroup.isValid());
    CHECK(cam.linkGroup == game.linkGroup);
    // Gameplay fills the top 60 %, the facecam the bottom 40 %.
    CHECK(onCanvas(game, 1920, 1080, 1080, 1920, 960, 0).second == Approx(0.0).margin(1e-6));
    CHECK(onCanvas(game, 1920, 1080, 1080, 1920, 960, 1080).second == Approx(1152.0));
    const double camTop = 1080.0 * (0.70 + (0.30 - 0.30) / 2.0);
    (void)camTop;
    CHECK(param(cam, "wabcut.crop", "top") >= 0.70 - 1e-9);
    CHECK(param(cam, "wabcut.crop", "left") >= 0.75 - 1e-9);
    const double shownTop = param(cam, "wabcut.crop", "top");
    CHECK(onCanvas(cam, 1920, 1080, 1080, 1920, 1920 * 0.875, 1080 * shownTop).second == Approx(1152.0));
    // A second split on the same clips reuses its copy's track; the request needs a facecam.
    LayoutRequest none;
    none.preset = LayoutPreset::VerticalSplit;
    none.gameplay = tl.clip("V1@0");
    CHECK_FALSE(edit::applyLayout(tl.state, tl.seq, none).has_value());
}

TEST_CASE("Vertical blur fill blurs the gameplay behind a sharp copy fitted to the width (C.15)", "[model][gaming][layout]") {
    Timeline tl(R"(
        V1 |Gggggggggg|
        A1 |Gggggggggg|
    )");
    makeVertical(tl);
    LayoutRequest req;
    req.preset = LayoutPreset::VerticalBlurFill;
    req.gameplay = tl.clip("V1@0");
    req.options.zoom = 1.5;
    req.options.blur = 60.0;
    const auto laid = edit::applyLayout(tl.state, tl.seq, req);
    REQUIRE(laid.has_value());
    const ProjectState& s = laid->edit.state;
    requireValid(s);
    const Sequence& seq = *s.findSequence(tl.seq);
    REQUIRE(seq.videoTracks.size() >= 2); // a new track for the sharp copy
    const Clip& back = clipOf(s, tl.seq, req.gameplay);
    CHECK(layoutEffects(back) == std::vector<std::string>{"wabcut.blur.gaussian"});
    CHECK(param(back, "wabcut.blur.gaussian", "radius") == 60.0);
    CHECK(back.properties().value(ParamId::Scale) == Approx(1920.0 / 1080.0)); // covers 1080x1920
    REQUIRE(laid->foreground.isValid());
    const Clip& front = clipOf(s, tl.seq, laid->foreground);
    CHECK(front.start() == back.start());
    CHECK(front.duration() == back.duration());
    // Zoom 1.5: the middle two thirds of the width fill the frame's width.
    CHECK(param(front, "wabcut.crop", "left") == Approx((1.0 - 1.0 / 1.5) / 2.0));
    CHECK(onCanvas(front, 1920, 1080, 1080, 1920, 1920 * param(front, "wabcut.crop", "left"), 540).first == Approx(0.0).margin(1e-6));
    CHECK(onCanvas(front, 1920, 1080, 1080, 1920, 960, 540).second == Approx(960.0));
    CHECK_FALSE(laid->facecam.isValid());
}

TEST_CASE("Layouts refuse what they cannot lay out (C.15)", "[model][gaming][layout]") {
    Timeline tl(R"(
        V2 |Gggggggggg|
        V1 |Ffffffffff|
        A1 |Ffffffffff|
    )");
    LayoutRequest req;
    req.gameplay = tl.clip("V2@0");
    req.facecam = tl.clip("V1@0"); // below the gameplay
    CHECK(edit::applyLayout(tl.state, tl.seq, req).error().code == ErrorCode::InvalidArgument);
    req.facecam = tl.clip("A1@0"); // not video
    CHECK_FALSE(edit::applyLayout(tl.state, tl.seq, req).has_value());
    req.facecam = {};
    CHECK_FALSE(edit::applyLayout(tl.state, tl.seq, req).has_value()); // pip needs a facecam
    req.options.facecamRegion = NormalizedRect{0.9, 0.9, 0.2, 0.2};     // outside the frame
    CHECK(layoutOptionsProblem(req.options).has_value());
    req.options.facecamRegion.reset();
    req.options.size = 2.0;
    CHECK(layoutOptionsProblem(req.options).has_value());
    req.options = LayoutOptions{};
    req.gameplay = ClipId{999'999};
    CHECK(edit::applyLayout(tl.state, tl.seq, req).error().code == ErrorCode::NotFound);
    CHECK(layoutPresetFromString("verticalBlurFill") == LayoutPreset::VerticalBlurFill);
    CHECK(layoutCornerFromString("topRight") == LayoutCorner::TopRight);
    CHECK_FALSE(layoutPresetFromString("grid").has_value());
}
