// Colour-chart matching (0.7): the colour science (delta E 2000 against the published test pairs, the chart's sRGB values),
// the chart geometry, measuring patches robustly, finding a synthetic ColorChecker (upright, turned, mirrored, tilted,
// among flat distractors, under a cast), the fit (per-channel casts and channel mixing come out below a small delta E),
// and color.chartMatch through the CommandEngine (detect or manual corners, one undo step, save and reload, "also").

#include "ChartScene.h"
#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/color/ChartMatch.h>
#include <wabcut/color/ColorCommands.h>
#include <wabcut/color/Looks.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::color;
using namespace wabcut::test;
using namespace wabcut::test::chart;
using Catch::Approx;
using command::Json;


namespace {

/// The numbers of a fit for the reader of the test output (<build>/test-output/color/<file>).
void writeReport(const char* file, const char* title, const ChartMatchResult& r) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "color";
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / file, std::ios::trunc);
    out << "# " << title << "\n\n" << r.summary << "\n\n| patch | before | after |\n|---|---|---|\n";
    for (const ChartPatchReport& p : r.patches) {
        out << "| " << p.name << " | " << p.deltaBefore << " | " << p.deltaAfter << " |\n";
    }
}

} // namespace

TEST_CASE("deltaE2000 matches the published test pairs", "[color][chart]") {
    // Sharma, Wu and Dalal (2005), supplementary test data.
    CHECK(deltaE2000({50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}) == Approx(2.0425).margin(1e-4));
    CHECK(deltaE2000({50.0, 3.1571, -77.2803}, {50.0, 0.0, -82.7485}) == Approx(2.8615).margin(1e-4));
    CHECK(deltaE2000({50.0, 2.8361, -74.0200}, {50.0, 0.0, -82.7485}) == Approx(3.4412).margin(1e-4));
    CHECK(deltaE2000({50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}) == Approx(2.3669).margin(1e-4));
    CHECK(deltaE2000({50.0, 2.49, -0.001}, {50.0, -2.49, 0.0009}) == Approx(7.1792).margin(1e-4));
    CHECK(deltaE2000({50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}) == Approx(27.1492).margin(1e-4));
    CHECK(deltaE2000({50.0, 2.5, 0.0}, {61.0, -5.0, 29.0}) == Approx(22.8977).margin(1e-4));
    CHECK(deltaE2000({60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}) == Approx(1.2644).margin(1e-4));
    CHECK(deltaE2000({22.7233, 20.0904, -46.694}, {23.0331, 14.973, -42.5619}) == Approx(2.0373).margin(1e-4));
    // Symmetric and zero for equal colours.
    CHECK(deltaE2000({40.0, 10.0, -20.0}, {43.0, 14.0, -18.0}) == Approx(deltaE2000({43.0, 14.0, -18.0}, {40.0, 10.0, -20.0})).margin(1e-9));
    CHECK(deltaE2000({40.0, 10.0, -20.0}, {40.0, 10.0, -20.0}) == 0.0);
}

TEST_CASE("The ColorChecker reference matches its published sRGB values and the neutrals are neutral", "[color][chart]") {
    // BabelColor's sRGB values of the chart (8 bit, D65): dark skin 115 82 68, white 243 243 242, black 52 52 52 (rounded,
    // from the averaged measurements). Computed here from the published L*a*b* with Bradford adaptation, checked against an
    // independent calculation (red: 180 49 57): within a few codes of the list for the greys and the skin.
    const auto code8 = [](int index, ChartEdition e) {
        const Rgb c = chartReferenceCode(index, e);
        return std::array<double, 3>{c[0] * 255.0, c[1] * 255.0, c[2] * 255.0};
    };
    const auto darkSkin = code8(0, ChartEdition::BeforeNov2014);
    CHECK(darkSkin[0] == Approx(115.0).margin(3.0));
    CHECK(darkSkin[1] == Approx(82.0).margin(3.0));
    CHECK(darkSkin[2] == Approx(68.0).margin(3.0));
    const auto red = code8(14, ChartEdition::BeforeNov2014);
    CHECK(red[0] == Approx(180.0).margin(1.0));
    CHECK(red[1] == Approx(49.0).margin(1.0));
    CHECK(red[2] == Approx(57.0).margin(1.0));
    const auto white = code8(18, ChartEdition::BeforeNov2014);
    CHECK(white[0] == Approx(243.0).margin(3.0));
    CHECK(white[2] == Approx(242.0).margin(3.0));
    const auto black = code8(23, ChartEdition::BeforeNov2014);
    CHECK(black[1] == Approx(52.0).margin(4.0));
    for (const ChartEdition e : {ChartEdition::AfterNov2014, ChartEdition::BeforeNov2014}) {
        double previous = 2.0;
        for (int i = 18; i < kChartPatches; ++i) {
            const Rgb c = chartReferenceCode(i, e);
            CHECK(std::abs(c[0] - c[1]) < 0.03);
            CHECK(std::abs(c[2] - c[1]) < 0.03);
            CHECK(c[1] < previous); // white to black
            previous = c[1];
            // The target L*a*b* is the one of the code itself.
            CHECK(deltaE2000(chartTargetLab(i, e), chartReferenceLab(i, e)) < 1.0);
        }
    }
    // The two prints differ by about a delta E or two on some patches, not more.
    for (int i = 0; i < kChartPatches; ++i) {
        CHECK(deltaE2000(chartReferenceLab(i, ChartEdition::AfterNov2014), chartReferenceLab(i, ChartEdition::BeforeNov2014)) < 3.0);
    }
    CHECK(chartPatchName(0) == "Dark skin");
    CHECK(chartPatchName(23) == "Black");
    CHECK(chartEditionFromId("after-2014") == ChartEdition::AfterNov2014);
    CHECK_FALSE(chartEditionFromId("2014"));
}

TEST_CASE("Chart geometry: corners rotate and mirror, patches map into the picture", "[color][chart]") {
    const ChartCorners c{{ChartPoint{0.2, 0.2}, ChartPoint{0.8, 0.2}, ChartPoint{0.8, 0.6}, ChartPoint{0.2, 0.6}}};
    CHECK(chartCornersPlausible(c));
    CHECK(rotatedCorners(c, 4) == c);
    CHECK(rotatedCorners(rotatedCorners(c, 1), 3) == c);
    CHECK(mirroredCorners(mirroredCorners(c)) == c);
    CHECK(mirroredCorners(c).p[0] == c.p[1]);
    // The chart's corners and the first patch's middle.
    const ChartPoint tl = chartToPicture(c, 0, 0);
    CHECK(tl.x == Approx(0.2).margin(1e-9));
    CHECK(tl.y == Approx(0.2).margin(1e-9));
    const ChartPoint br = chartToPicture(c, kChartWidth, kChartHeight);
    CHECK(br.x == Approx(0.8).margin(1e-9));
    CHECK(br.y == Approx(0.6).margin(1e-9));
    const auto quad = chartPatchQuad(c, 0, 1.0);
    CHECK(quad[0].x == Approx(0.2).margin(1e-9)); // the first patch touches the chart's corner
    CHECK(quad[1].x - quad[0].x == Approx(0.6 * kChartPatchSide / kChartWidth).margin(1e-9));
    // The patches do not overlap: patch 1's right edge is left of patch 2's left edge.
    CHECK(chartPatchQuad(c, 0)[1].x < chartPatchQuad(c, 1)[0].x);
    // A bow tie, a sliver and a far-away shape are not plausible.
    CHECK_FALSE(chartCornersPlausible({{ChartPoint{0.2, 0.2}, ChartPoint{0.8, 0.6}, ChartPoint{0.8, 0.2}, ChartPoint{0.2, 0.6}}}));
    CHECK_FALSE(chartCornersPlausible({{ChartPoint{0.2, 0.2}, ChartPoint{0.8, 0.2}, ChartPoint{0.8, 0.2001}, ChartPoint{0.2, 0.2001}}}));
    CHECK_FALSE(chartCornersPlausible({{ChartPoint{5, 5}, ChartPoint{6, 5}, ChartPoint{6, 6}, ChartPoint{5, 6}}}));
    // The default shape is plausible and has the chart's proportions on screen.
    const ChartCorners d = defaultChartCorners(16.0 / 9.0);
    CHECK(chartCornersPlausible(d));
    CHECK((d.p[1].x - d.p[0].x) * 16.0 / ((d.p[3].y - d.p[0].y) * 9.0) == Approx(kChartWidth / kChartHeight).margin(1e-6));
}

TEST_CASE("measureChart reads every patch, ignoring a speck, and flags a clipped patch", "[color][chart]") {
    Scene scene;
    scene.corners = placed(scene, 0.6, 0.0);
    scene.speck = 7;
    scene.noise = 0.004;
    const ChartMeasurement m = measureChart(renderScene(scene), scene.corners);
    CHECK(m.usable == kChartPatches);
    for (int i = 0; i < kChartPatches; ++i) {
        INFO("patch " << i);
        const Rgb expected = chartReferenceCode(i, ChartEdition::AfterNov2014);
        for (std::size_t c = 0; c < 3; ++c) {
            CHECK(m.patches[static_cast<std::size_t>(i)].display[c] == Approx(expected[c]).margin(0.01));
        }
    }
    // Overexposed: the white patch clips and is left out.
    Scene bright;
    bright.corners = scene.corners;
    bright.cast = [](const Rgb& c) { return c * 1.4; };
    const ChartMeasurement clipped = measureChart(renderScene(bright), bright.corners);
    CHECK_FALSE(clipped.patches[18].usable);
    CHECK(clipped.usable < kChartPatches);
    CHECK(clipped.usable >= 20);
    // Corners that put patches outside the picture leave those unusable.
    const ChartCorners off{{ChartPoint{0.7, 0.3}, ChartPoint{1.5, 0.3}, ChartPoint{1.5, 0.8}, ChartPoint{0.7, 0.8}}};
    CHECK(measureChart(renderScene(scene), off).usable < kChartPatches);
}

TEST_CASE("detectColorChecker finds an upright chart under a colour cast", "[color][chart]") {
    for (const Cast& cast : {Cast(diagonalCast), Cast(mixedCast), Cast([](const Rgb& c) { return c; })}) {
        Scene scene;
        scene.cast = cast;
        scene.noise = 0.006;
        scene.corners = placed(scene, 0.55, 0.0);
        const auto found = detectColorChecker(renderScene(scene));
        REQUIRE(found);
        CHECK(found->patches >= 20);
        CHECK(found->confidence > 0.5);
        CHECK(cornerError(found->corners, scene.corners) < 0.012);
    }
}

TEST_CASE("detectColorChecker finds a turned, mirrored, tilted or smaller chart", "[color][chart]") {
    struct Pose {
        double fraction, degrees, keystone;
        bool mirror;
    };
    for (const Pose& pose : {Pose{0.55, 17.0, 0.0, false}, Pose{0.45, -33.0, 0.0, false}, Pose{0.55, 180.0, 0.0, false},
                             Pose{0.45, 90.0, 0.0, false}, Pose{0.45, -90.0, 0.0, false}, Pose{0.55, 0.0, 0.0, true},
                             Pose{0.55, 0.0, 0.18, false}, Pose{0.3, 8.0, 0.1, false}}) {
        INFO("fraction " << pose.fraction << " turn " << pose.degrees << " keystone " << pose.keystone << " mirror " << pose.mirror);
        Scene scene;
        scene.cast = diagonalCast;
        scene.noise = 0.004;
        scene.corners = placed(scene, pose.fraction, pose.degrees, pose.keystone, pose.mirror);
        const auto found = detectColorChecker(renderScene(scene));
        REQUIRE(found);
        CHECK(cornerError(found->corners, scene.corners) < 0.015);
    }
}

TEST_CASE("detectColorChecker is not fooled by flat squares and finds nothing without a chart", "[color][chart]") {
    Scene scene;
    scene.distractors = true;
    scene.cast = mixedCast;
    scene.corners = placed(scene, 0.5, 5.0);
    const auto found = detectColorChecker(renderScene(scene));
    REQUIRE(found);
    CHECK(cornerError(found->corners, scene.corners) < 0.015);

    Scene none;
    none.distractors = true;
    none.chart = false;
    none.noise = 0.004;
    CHECK_FALSE(detectColorChecker(renderScene(none)));
    // Too small a picture, an empty one.
    CHECK_FALSE(detectColorChecker(ColorImage{}));
    ColorImage tiny;
    tiny.width = 16;
    tiny.height = 16;
    tiny.rgba.assign(16 * 16 * 4, 0.5f);
    CHECK_FALSE(detectColorChecker(tiny));
}

TEST_CASE("solveChartMatch removes a per-channel cast", "[color][chart]") {
    Scene scene;
    scene.cast = diagonalCast;
    scene.noise = 0.003;
    scene.corners = placed(scene, 0.55, 0.0);
    const ChartMeasurement m = measureChart(renderScene(scene), scene.corners);
    const ChartMatchResult r = solveChartMatch(m, GradeSpec{}, GradeCurves{});
    REQUIRE(r.ok);
    INFO(r.summary);
    writeReport("chart-match-diagonal.md", "Colour-chart match on a synthetic chart with a per-channel cast", r);
    CHECK(r.improved);
    CHECK(r.meanBefore > 8.0);
    CHECK(r.meanAfter < 1.5);
    CHECK(r.maxAfter < 4.0);
    CHECK(r.holdOutMean >= r.meanAfter - 1e-9); // the estimate between the patches is never better than on them
    CHECK(r.holdOutMean < 3.0);
    CHECK(r.used == kChartPatches);
    CHECK(r.params.temperature != 0.0); // warm light: cooled
    // Every patch moved closer to its colour.
    for (const ChartPatchReport& p : r.patches) {
        CHECK(p.deltaAfter < p.deltaBefore + 0.5);
    }
    // The numbers are the grade's: evaluating the stored result on the measured colours gives the reported errors.
    GradeSpec spec;
    spec.params = r.params;
    spec.curves = bakeCurves(r.curves);
    double sum = 0.0;
    for (int i = 0; i < kChartPatches; ++i) {
        const Rgb out = encode709(gradePixel(spec, decode709(m.patches[static_cast<std::size_t>(i)].display)));
        sum += deltaE2000(srgbToLab(out), chartTargetLab(i, ChartEdition::AfterNov2014));
    }
    CHECK(sum / kChartPatches == Approx(r.meanAfter).margin(0.05));
}

TEST_CASE("solveChartMatch lowers the error of a cast with channel mixing and honours neutralOnly", "[color][chart]") {
    Scene scene;
    scene.cast = mixedCast;
    scene.noise = 0.003;
    scene.corners = placed(scene, 0.55, 0.0);
    const ChartMeasurement m = measureChart(renderScene(scene), scene.corners);
    const ChartMatchResult full = solveChartMatch(m, GradeSpec{}, GradeCurves{});
    REQUIRE(full.ok);
    INFO(full.summary);
    writeReport("chart-match.md", "Colour-chart match on a synthetic chart with channel mixing", full);
    CHECK(full.improved);
    CHECK(full.meanAfter < 0.5 * full.meanBefore);
    CHECK(full.meanAfter < 5.0);
    ChartOptions neutral;
    neutral.colours = false;
    const ChartMatchResult grey = solveChartMatch(m, GradeSpec{}, GradeCurves{}, neutral);
    REQUIRE(grey.ok);
    INFO(grey.summary);
    CHECK(grey.params.saturation == 100.0);
    CHECK(grey.hueCurvePoints == 0);
    CHECK(grey.curves.hueHue.empty());
    CHECK(grey.meanAfter < grey.meanBefore);
    // The grey row ends up neutral.
    for (int i = 18; i < kChartPatches; ++i) {
        const Rgb a = full.patches[static_cast<std::size_t>(i)].after;
        CHECK(std::abs(a[0] - a[2]) < 0.04);
    }
    // The other print of the chart is a different target.
    ChartOptions older;
    older.edition = ChartEdition::BeforeNov2014;
    const ChartMatchResult other = solveChartMatch(m, GradeSpec{}, GradeCurves{}, older);
    REQUIRE(other.ok);
    CHECK(other.meanBefore != full.meanBefore);
}

TEST_CASE("solveChartMatch refuses a chart that is turned, clipped or mostly unreadable", "[color][chart]") {
    Scene scene;
    scene.cast = diagonalCast;
    scene.corners = placed(scene, 0.55, 0.0);
    const ColorImage picture = renderScene(scene);
    // The same corners read a quarter turn off: the grey row does not run white to black.
    const ChartMeasurement turned = measureChart(picture, rotatedCorners(scene.corners, 1));
    const ChartMatchResult wrong = solveChartMatch(turned, GradeSpec{}, GradeCurves{});
    CHECK_FALSE(wrong.ok);
    CHECK_FALSE(wrong.error.empty());
    // A picture where most patches clip.
    Scene bright;
    bright.corners = scene.corners;
    bright.cast = [](const Rgb& c) { return c * 3.5; };
    const ChartMatchResult clipped = solveChartMatch(measureChart(renderScene(bright), bright.corners), GradeSpec{}, GradeCurves{});
    CHECK_FALSE(clipped.ok);
    // Corners off the picture.
    const ChartCorners off{{ChartPoint{0.9, 0.1}, ChartPoint{1.9, 0.1}, ChartPoint{1.9, 0.6}, ChartPoint{0.9, 0.6}}};
    CHECK_FALSE(solveChartMatch(measureChart(picture, off), GradeSpec{}, GradeCurves{}).ok);
}

TEST_CASE("solveChartMatch keeps the grade's other stages and starts from its own settings", "[color][chart]") {
    Scene scene;
    scene.cast = diagonalCast;
    scene.corners = placed(scene, 0.55, 0.0);
    const ChartMeasurement m = measureChart(renderScene(scene), scene.corners);
    GradeSpec base;
    base.params.vibrance = 20;
    base.params.highlights = -10;
    base.params.temperature = 30; // a setting the match replaces
    GradeCurves oldCurves;
    oldCurves.red = {{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}};
    oldCurves.lumaSat = {{0.0, 0.5}, {1.0, 0.5}};
    const ChartMatchResult r = solveChartMatch(m, base, oldCurves);
    REQUIRE(r.ok);
    CHECK(r.params.vibrance == 20);   // untouched
    CHECK(r.params.highlights == -10);
    CHECK(r.curves.lumaSat == oldCurves.lumaSat); // the match does not own this one
    CHECK(r.curves.red != oldCurves.red);          // it does own the RGB curves
    CHECK(r.improved);
    CHECK(r.meanAfter < 3.0);
}

// ---- The command ----------------------------------------------------------------------------------------------------

namespace {

/// The renderer of the commands: the synthetic chart for every clip, at the size asked for.
class ChartRenderer final : public ColorServices {
public:
    Scene scene;
    int calls = 0;

    std::expected<ColorImage, Error> frame(const ProjectState&, SequenceId, const FrameRequest& request) override {
        ++calls;
        lastWidth = request.maxWidth;
        return renderScene(scene);
    }
    int lastWidth = 0;
};

struct ChartHarness {
    std::shared_ptr<ChartRenderer> renderer = std::make_shared<ChartRenderer>();
    CommandHarness h;

    explicit ChartHarness(std::string_view picture) : h(picture, policy(), ai::edit::editingCommands()) {
        renderer->scene.cast = diagonalCast;
        renderer->scene.noise = 0.003;
        renderer->scene.corners = placed(renderer->scene, 0.55, 12.0);
    }
    command::EnginePolicy policy() {
        command::EnginePolicy p;
        p.services = renderer;
        return p;
    }
    const EffectInstance* grade(std::string_view ref) {
        return gradeOf(h.state()->findSequence(h.tl.seq)->findClip(h.clip(ref))->clip());
    }
    /// Mean delta E of the chart as the clip's grade shows it.
    double meanError(std::string_view ref) {
        const EffectInstance* g = grade(ref);
        REQUIRE(g);
        const GradeSpec spec = evaluateGrade(*g, Ticks{}, LutLibrary::shared().resolver(*h.state()));
        const ChartMeasurement m = measureChart(renderScene(renderer->scene), renderer->scene.corners);
        double sum = 0.0;
        for (int i = 0; i < kChartPatches; ++i) {
            const Rgb out = encode709(gradePixel(spec, decode709(m.patches[static_cast<std::size_t>(i)].display)));
            sum += deltaE2000(srgbToLab(out), chartTargetLab(i, ChartEdition::AfterNov2014));
        }
        return sum / kChartPatches;
    }
};

} // namespace

TEST_CASE("color.chartMatch finds the chart, applies it in one undo step and the grade survives save and reload", "[color][chart][commands]") {
    ChartHarness c(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId a = c.h.clip("V1@0");
    const auto result = c.h.ok("color.chartMatch", Json{{"clip", a.value}});
    INFO(result.data["summary"].get<std::string>());
    CHECK(c.h.session.undoCount() == 1);
    CHECK(c.renderer->lastWidth == kChartFrameWidth);
    CHECK(result.data["detected"] == true);
    CHECK(result.data["patchesUsed"] == 24);
    CHECK(result.data["meanDeltaBefore"].get<double>() > 8.0);
    CHECK(result.data["meanDeltaAfter"].get<double>() < 1.5);
    CHECK(result.data["patches"].size() == 24);
    CHECK(result.data["patches"][18]["name"] == "White");
    CHECK(result.data["corners"].size() == 4);
    REQUIRE(c.grade("V1@0"));
    const GradeParams p = gradeParamsAt(*c.grade("V1@0"), Ticks{});
    CHECK(p.temperature == Approx(result.data["params"]["basic.temperature"].get<double>()).margin(1e-6));
    // The fit wrote its curves into the grade (the hue points at the coloured patches' hues).
    const EffectData* hueHue = c.grade("V1@0")->findData("curves.hueHue");
    REQUIRE(hueHue);
    REQUIRE(hueHue->points());
    CHECK(hueHue->points()->size() >= 3);
    CHECK(result.data["curves"].contains("hueHue"));
    // The grade does what the report says (the stored result, not the fit's own numbers).
    CHECK(c.meanError("V1@0") == Approx(result.data["meanDeltaAfter"].get<double>()).margin(0.1));
    // The other clip is untouched.
    CHECK_FALSE(c.grade("V1@4"));

    // Saved and loaded: the same grade.
    const auto json = io::toJson(*c.h.state());
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    const Sequence& seq = *loaded->findSequence(c.h.tl.seq);
    const EffectInstance* reloaded = gradeOf(seq.findClip(a)->clip());
    REQUIRE(reloaded);
    CHECK(gradeParamsAt(*reloaded, Ticks{}) == p);
    CHECK(gradeCurvesOf(*reloaded) == gradeCurvesOf(*c.grade("V1@0")));

    // Undo takes it all back, redo brings it back.
    REQUIRE(c.h.session.undo());
    CHECK_FALSE(c.grade("V1@0"));
    REQUIRE(c.h.session.redo());
    CHECK(gradeParamsAt(*c.grade("V1@0"), Ticks{}) == p);
}

TEST_CASE("color.chartMatch gives other clips the same correction, takes manual corners and a dry run changes nothing", "[color][chart][commands]") {
    ChartHarness c(R"(
        V1 tgt |AaaaBbbbCccc|
    )");
    const ClipId a = c.h.clip("V1@0");
    const ClipId b = c.h.clip("V1@4");
    const ClipId d = c.h.clip("V1@8");
    // A dry run reports and changes nothing.
    const auto revision = c.h.session.revision();
    const auto dry = c.h.run(Json{{"command", "color.chartMatch"}, {"dryRun", true}, {"params", Json{{"clip", a.value}}}});
    REQUIRE(dry.ok());
    CHECK(dry.dryRun);
    CHECK(c.h.session.revision() == revision);
    CHECK_FALSE(c.grade("V1@0"));

    // Manual corners (the true ones), a second clip given with "also": one undo step for both.
    Json corners = Json::array();
    for (const ChartPoint& p : c.renderer->scene.corners.p) {
        corners.push_back(Json::array({p.x, p.y}));
    }
    const auto result = c.h.ok("color.chartMatch", Json{{"clip", a.value}, {"corners", corners}, {"also", Json::array({b.value, a.value})}});
    CHECK(result.data["detected"] == false);
    CHECK(c.h.session.undoCount() == 1);
    CHECK(result.data["appliedTo"].size() == 2); // the source (once) and B
    REQUIRE(c.grade("V1@0"));
    REQUIRE(c.grade("V1@4"));
    CHECK_FALSE(c.grade("V1@8"));
    CHECK(gradeParamsAt(*c.grade("V1@4"), Ticks{}) == gradeParamsAt(*c.grade("V1@0"), Ticks{}));
    CHECK(gradeCurvesOf(*c.grade("V1@4")) == gradeCurvesOf(*c.grade("V1@0")));
    CHECK(c.meanError("V1@4") < 1.5);
    REQUIRE(c.h.session.undo());
    CHECK_FALSE(c.grade("V1@0"));
    CHECK_FALSE(c.grade("V1@4"));

    // A clip's own earlier grade keeps its other stages: set a vignette first.
    c.h.ok("color.grade.set", Json{{"clip", d.value}, {"params", Json{{"vignette.amount", -1.0}, {"basic.vibrance", 15}}}});
    c.renderer->scene.corners = placed(c.renderer->scene, 0.55, 0.0);
    c.h.ok("color.chartMatch", Json{{"clip", d.value}, {"neutralOnly", true}});
    const GradeParams kept = gradeParamsAt(*c.grade("V1@8"), Ticks{});
    CHECK(kept.vignetteAmount == -1.0);
    CHECK(kept.vibrance == 15);
    CHECK(kept.saturation == 100.0);
    const EffectData* hue = c.grade("V1@8")->findData("curves.hueHue");
    CHECK((!hue || !hue->points() || hue->points()->empty()));
}

TEST_CASE("color.chartMatch says why it cannot match", "[color][chart][commands]") {
    ChartHarness c(R"(
        V1 tgt |Aaaa|
    )");
    const ClipId a = c.h.clip("V1@0");
    // No chart in the picture.
    c.renderer->scene.chart = false;
    CHECK(c.h.errorOf("color.chartMatch", Json{{"clip", a.value}}) == "EditRejected");
    // Corners that are not a shape, not four points, or a wrong edition.
    c.renderer->scene.chart = true;
    CHECK(c.h.errorOf("color.chartMatch", Json{{"clip", a.value}, {"corners", Json::array({Json::array({0.1, 0.1})})}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.chartMatch", Json{{"clip", a.value},
                                               {"corners", Json::array({Json::array({0.1, 0.1}), Json::array({0.9, 0.9}), Json::array({0.9, 0.1}), Json::array({0.1, 0.9})})}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.chartMatch", Json{{"clip", a.value}, {"edition", "2020"}}) == "InvalidParams");
    // Corners that put the chart a quarter turn off.
    const ChartCorners turned = rotatedCorners(c.renderer->scene.corners, 1);
    Json corners = Json::array();
    for (const ChartPoint& p : turned.p) {
        corners.push_back(Json::array({p.x, p.y}));
    }
    CHECK(c.h.errorOf("color.chartMatch", Json{{"clip", a.value}, {"corners", corners}}) == "EditRejected");
    // A refused match leaves no grade and no undo step behind.
    CHECK_FALSE(c.grade("V1@0"));
    CHECK(c.h.session.undoCount() == 0);
    // Without a renderer: a clear refusal.
    CommandHarness bare(R"(
        V1 tgt |Aaaa|
    )",
                        {}, ai::edit::editingCommands());
    CHECK(bare.errorOf("color.chartMatch", Json{{"clip", bare.clip("V1@0").value}}) == "InvalidRequest");
}
