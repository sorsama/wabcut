// Colour-chart matching in the shell (0.7, docs/color.md "Match to a colour chart"): the Color panel's Color Chart section
// finds a synthetic ColorChecker in the clip's picture, the program monitor's overlay offers four corners and the outlines of
// the patches, the swatches and the error line follow the corners, Rotate / Mirror / the print / the colour switch refit,
// Match writes the grade in one undo step (also onto the other selected clips), and another frame or clip closes the chart.
// Screenshots in the dark and the light theme. Offscreen; the pictures come from a fake renderer (no media, no GPU needed).

#include "ChartScene.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "ColorBridge.h"
#include "ProjectController.h"

#include <wabcut/color/ColorCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQuickItem>

#include <algorithm>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace chart = wabcut::test::chart;

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

/// The picture of every clip at every time: the synthetic chart.
class ChartPictures final : public color::ColorServices {
public:
    explicit ChartPictures(std::shared_ptr<const color::ColorImage> image) : m_image(std::move(image)) {}
    std::expected<color::ColorImage, Error> frame(const ProjectState&, SequenceId, const color::FrameRequest&) override { return *m_image; }

private:
    std::shared_ptr<const color::ColorImage> m_image;
};

struct ChartShell {
    Shell shell;
    ProjectController* controller = nullptr;
    ColorBridge* color = nullptr;
    std::shared_ptr<const color::ColorImage> picture;
    color::ChartCorners corners;

    explicit ChartShell(const char* timeline, int width = 1600, int height = 900) : tl(timeline) {
        REQUIRE(shell.window);
        shell.resize(width, height);
        shell.theme->setProperty("reduceMotion", true);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        color = shell.engine.singletonInstance<ColorBridge*>("Wabcut", "Color");
        REQUIRE(controller);
        REQUIRE(color);
        controller->resetProject(tl.state, tl.seq);
        chart::Scene scene;
        scene.cast = chart::diagonalCast;
        scene.noise = 0.003;
        scene.corners = chart::placed(scene, 0.55, 8.0);
        corners = scene.corners;
        picture = std::make_shared<const color::ColorImage>(chart::renderScene(scene));
        color->setPictureServices(std::make_shared<ChartPictures>(picture));
    }

    void select(std::string_view ref) {
        const ClipId id = tl.clip(ref);
        controller->selectClipById(id, ProjectController::Replace);
        controller->setPlayhead(Ticks{});
        REQUIRE(waitFor([&] { return color->targetClip() == id; }));
    }

    QQuickItem* openColorPanel() {
        shell.window->setProperty("workspace", 3); // the Color workspace
        QQuickItem* panel = nullptr;
        REQUIRE(waitFor([&] {
            panel = findNamed(shell.window->contentItem(), QStringLiteral("colorPanel"));
            return panel != nullptr;
        }));
        return panel;
    }

    Timeline tl;
};

QVariantList cornerList(const color::ChartCorners& c) {
    QVariantList out;
    for (const color::ChartPoint& p : c.p)
        out.append(QVariant(QVariantList{p.x, p.y}));
    return out;
}

} // namespace

TEST_CASE("Color Chart: the section finds the chart, the corners and swatches follow and Match is one undo step (0.7)",
          "[app][color][chart]") {
    resetSettings();
    clearMessages();
    ChartShell s("V1 tgt |Aaaaaaaaaa|");
    s.select("V1@0");
    QQuickItem* panel = s.openColorPanel();
    CHECK(findNamed(panel, QStringLiteral("colorChartSection")) != nullptr);
    CHECK(findNamed(panel, QStringLiteral("chartFindButton")) != nullptr);
    CHECK_FALSE(s.color->chart().value(QStringLiteral("active")).toBool());
    CHECK_FALSE(findNamed(s.shell.window->contentItem(), QStringLiteral("chartCorner0"))); // no handles without a chart

    // Find: the picture is read, the chart is found where it was drawn.
    REQUIRE(s.color->showChart(s.picture));
    QVariantMap chartState = s.color->chart();
    REQUIRE(chartState.value(QStringLiteral("active")).toBool());
    CHECK(chartState.value(QStringLiteral("found")).toBool());
    CHECK(chartState.value(QStringLiteral("patches")).toList().size() == 24);
    CHECK(chartState.value(QStringLiteral("quads")).toList().size() == 24);
    CHECK(chartState.value(QStringLiteral("improved")).toBool());
    CHECK(chartState.value(QStringLiteral("canApply")).toBool());
    CHECK(chartState.value(QStringLiteral("meanBefore")).toDouble() > 8.0);
    CHECK(chartState.value(QStringLiteral("meanAfter")).toDouble() < 1.5);
    CHECK(chartState.value(QStringLiteral("edition")).toString() == QStringLiteral("after-2014"));
    const QVariantList found = chartState.value(QStringLiteral("corners")).toList();
    REQUIRE(found.size() == 4);
    for (int i = 0; i < 4; ++i) {
        const QVariantList p = found.at(i).toList();
        CHECK(std::hypot(p.at(0).toDouble() - s.corners.p[static_cast<std::size_t>(i)].x, p.at(1).toDouble() - s.corners.p[static_cast<std::size_t>(i)].y) < 0.015);
    }

    // The monitor shows the four handles; the panel shows 24 swatches, the legend and the error line.
    QQuickItem* overlay = nullptr;
    REQUIRE(waitFor([&] {
        overlay = findNamed(s.shell.window->contentItem(), QStringLiteral("colorChartOverlay"));
        return overlay && overlay->isVisible() && findNamed(overlay, QStringLiteral("chartCorner3")) != nullptr;
    }));
    for (const char* name : {"chartCorner0", "chartCorner1", "chartCorner2", "chartCorner3"}) {
        CHECK(findNamed(overlay, QString::fromLatin1(name)) != nullptr);
    }
    QQuickItem* swatches = findNamed(panel, QStringLiteral("chartSwatches"));
    REQUIRE(swatches);
    REQUIRE(waitFor([&] { return swatches->isVisible() && swatches->childItems().size() >= 24; }));
    QQuickItem* error = findNamed(panel, QStringLiteral("chartError"));
    REQUIRE(error);
    CHECK(error->property("text").toString().contains(QStringLiteral("Mean")));
    CHECK(findNamed(panel, QStringLiteral("chartMatchButton"))->isVisible());
    CHECK(findNamed(panel, QStringLiteral("chartMatchButton"))->isEnabled());

    // Dragging a corner moves the outlines at once and the numbers after the release.
    color::ChartCorners dragged = s.corners;
    dragged.p[2].x += 0.01;
    const double meanBefore = chartState.value(QStringLiteral("meanAfter")).toDouble();
    s.color->setChartCorners(cornerList(dragged), false);
    chartState = s.color->chart();
    CHECK(chartState.value(QStringLiteral("stale")).toBool());
    CHECK_FALSE(chartState.value(QStringLiteral("canApply")).toBool());
    CHECK(chartState.value(QStringLiteral("meanAfter")).toDouble() == meanBefore); // the numbers wait for the release
    s.color->setChartCorners(cornerList(dragged), true);
    chartState = s.color->chart();
    CHECK_FALSE(chartState.value(QStringLiteral("stale")).toBool());
    CHECK(chartState.value(QStringLiteral("canApply")).toBool());
    // A shape that is not a four-sided one is ignored.
    s.color->setChartCorners(cornerList({{color::ChartPoint{0.2, 0.2}, color::ChartPoint{0.8, 0.8}, color::ChartPoint{0.8, 0.2}, color::ChartPoint{0.2, 0.8}}}), true);
    CHECK(s.color->chart().value(QStringLiteral("corners")) == chartState.value(QStringLiteral("corners")));
    s.color->setChartCorners(cornerList(s.corners), true);

    // Turned or mirrored: the grey row no longer runs white to black, so there is nothing to apply, with the reason.
    s.color->rotateChart();
    chartState = s.color->chart();
    CHECK_FALSE(chartState.value(QStringLiteral("canApply")).toBool());
    CHECK_FALSE(chartState.value(QStringLiteral("error")).toString().isEmpty());
    for (int i = 0; i < 3; ++i)
        s.color->rotateChart();
    CHECK(s.color->chart().value(QStringLiteral("canApply")).toBool());
    s.color->mirrorChart();
    CHECK_FALSE(s.color->chart().value(QStringLiteral("canApply")).toBool());
    s.color->mirrorChart();
    CHECK(s.color->chart().value(QStringLiteral("canApply")).toBool());

    // The print and the colour switch refit.
    const double newer = s.color->chart().value(QStringLiteral("meanBefore")).toDouble();
    s.color->setChartEdition(QStringLiteral("before-2014"));
    CHECK(s.color->chart().value(QStringLiteral("edition")).toString() == QStringLiteral("before-2014"));
    CHECK(s.color->chart().value(QStringLiteral("meanBefore")).toDouble() != newer);
    s.color->setChartEdition(QStringLiteral("after-2014"));
    s.color->setChartColours(false);
    CHECK_FALSE(s.color->chart().value(QStringLiteral("colours")).toBool());
    CHECK(s.color->chart().value(QStringLiteral("canApply")).toBool());
    s.color->setChartColours(true);

    // Match: one undo step, a grade appears, the chart closes.
    const std::size_t undoBefore = s.controller->session().undoCount();
    REQUIRE(s.color->applyChart(false));
    CHECK(s.controller->session().undoCount() == undoBefore + 1);
    CHECK_FALSE(s.color->chart().value(QStringLiteral("active")).toBool());
    CHECK(s.color->status().startsWith(QStringLiteral("Matched to the chart")));
    CHECK(s.color->hasGrade());
    REQUIRE(waitFor([&] { return s.color->params().value(QStringLiteral("basic.temperature")).toDouble() != 0.0; }));
    REQUIRE(waitFor([&] { return !findNamed(s.shell.window->contentItem(), QStringLiteral("colorChartOverlay"))->isVisible(); }));
    REQUIRE(s.controller->session().undo());
    CHECK_FALSE(s.color->hasGrade());
}

TEST_CASE("Color Chart: Match can give the other selected clips the same correction, and another frame closes the chart (0.7)",
          "[app][color][chart]") {
    resetSettings();
    clearMessages();
    ChartShell s("V1 tgt |AaaaaBbbbbCcccc|");
    const ClipId a = s.tl.clip("V1@0");
    const ClipId b = s.tl.clip("V1@5");
    s.controller->selectClipById(a, ProjectController::Replace);
    s.controller->selectClipById(b, ProjectController::Add);
    s.controller->setPlayhead(Ticks{});
    REQUIRE(waitFor([&] { return s.color->targetClip().isValid(); }));
    const ClipId target = s.color->targetClip();
    s.openColorPanel();
    REQUIRE(s.color->showChart(s.picture));
    CHECK(s.color->chart().value(QStringLiteral("others")).toInt() == 1);
    const std::size_t undoBefore = s.controller->session().undoCount();
    REQUIRE(s.color->applyChart(true));
    CHECK(s.controller->session().undoCount() == undoBefore + 1);
    const auto gradeOf = [&](ClipId id) -> const EffectInstance* {
        for (const EffectInstance& e : s.controller->currentSequence()->findClip(id)->clip().effects()) {
            if (e.effect == color::kGradeEffect)
                return &e;
        }
        return nullptr;
    };
    CHECK(gradeOf(a) != nullptr);
    CHECK(gradeOf(b) != nullptr);
    CHECK(gradeOf(s.tl.clip("V1@10")) == nullptr);
    (void)target;

    // The chart belongs to one frame: moving the playhead closes it.
    s.controller->selectClipById(a, ProjectController::Replace);
    REQUIRE(waitFor([&] { return s.color->targetClip() == a; }));
    s.controller->setPlayhead(Ticks{});
    REQUIRE(s.color->showChart(s.picture));
    CHECK(s.color->chart().value(QStringLiteral("active")).toBool());
    s.controller->setPlayhead(framesToTicks(2, Rate{25, 1}));
    REQUIRE(waitFor([&] { return !s.color->chart().value(QStringLiteral("active")).toBool(); }));
    // Another clip too.
    s.controller->setPlayhead(Ticks{});
    REQUIRE(s.color->showChart(s.picture));
    s.controller->selectClipById(s.tl.clip("V1@10"), ProjectController::Replace);
    s.controller->setPlayhead(framesToTicks(10, Rate{25, 1}));
    REQUIRE(waitFor([&] { return !s.color->chart().value(QStringLiteral("active")).toBool(); }));
    // Without a clip at the playhead, Find says what to do.
    s.controller->selectClipById(a, ProjectController::Replace);
    s.controller->setPlayhead(framesToTicks(10, Rate{25, 1}));
    REQUIRE(waitFor([&] { return s.color->targetClip().isValid(); }));
    s.color->clearChart();
    CHECK_FALSE(s.color->chart().value(QStringLiteral("active")).toBool());
}

TEST_CASE("Color Chart: the section, its swatches and the monitor's corners (screenshots)", "[app][color][chart][shot]") {
    struct Size {
        int width, height;
        const char* suffix;
    };
    // The workspace at the size of the shots and at the smallest window (1100 x 700).
    for (const Size size : {Size{kShotWidth, kShotHeight, ""}, Size{1100, 700, "-narrow"}}) {
        INFO("window " << size.width << " x " << size.height);
        resetSettings();
        clearMessages();
        ChartShell s("V1 tgt |Aaaaaaaaaa|", size.width, size.height);
        s.select("V1@0");
        QQuickItem* panel = s.openColorPanel();
        REQUIRE(s.color->showChart(s.picture));
        QQuickItem* section = nullptr;
        REQUIRE(waitFor([&] {
            section = findNamed(panel, QStringLiteral("colorChartSection"));
            QQuickItem* swatches = findNamed(panel, QStringLiteral("chartSwatches"));
            return section && swatches && swatches->isVisible() && swatches->childItems().size() >= 24;
        }));
        // Scroll the panel to the end. An unexposed window lays out when it renders, so render, scroll, and again: the
        // content is taller once the swatches have their width.
        for (int pass = 0; pass < 3; ++pass) {
            REQUIRE_FALSE(s.shell.grab().isNull());
            for (QQuickItem* item = section->parentItem(); item; item = item->parentItem()) {
                if (item->property("contentY").isValid() && item->property("contentHeight").isValid()) {
                    item->setProperty("contentY", std::max(0.0, item->property("contentHeight").toDouble() - item->height()));
                    break;
                }
            }
            spin(120);
        }
        REQUIRE_FALSE(s.shell.grab().isNull());
        for (const bool dark : {true, false}) {
            s.shell.setAppearance(dark, "ultraviolet");
            REQUIRE(s.shell.settle(dark, s.shell.themeColor("accentFill")));
            const QImage image = s.shell.grab();
            REQUIRE_FALSE(image.isNull());
            const double dpr = image.devicePixelRatio();
            const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
            const auto save = [&](QQuickItem* item, const QString& name) {
                const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                            .save(outputDir() + QStringLiteral("/%1%2-%3.png").arg(name, QString::fromLatin1(size.suffix), theme)));
            };
            save(panel, QStringLiteral("color-chart-panel"));
            QQuickItem* overlay = findNamed(s.shell.window->contentItem(), QStringLiteral("colorChartOverlay"));
            REQUIRE(overlay);
            save(overlay, QStringLiteral("color-chart-monitor"));
            REQUIRE(image.save(outputDir() + QStringLiteral("/color-chart-workspace%1-%2.png").arg(QString::fromLatin1(size.suffix), theme)));
        }
        s.shell.setAppearance(true, "ultraviolet");
        // Nothing runs off the panel's edge at this width.
        QQuickItem* grid = findNamed(panel, QStringLiteral("chartSwatches"));
        REQUIRE(grid);
        const QRectF gridRect = grid->mapRectToItem(panel, QRectF(0, 0, grid->width(), grid->height()));
        CHECK(gridRect.right() <= panel->width() + 0.5);
        for (const char* name : {"chartFindButton", "chartMatchButton", "chartRotateButton", "chartMirrorButton", "chartClearButton"}) {
            QQuickItem* button = findNamed(panel, QString::fromLatin1(name));
            REQUIRE(button);
            const QRectF r = button->mapRectToItem(panel, QRectF(0, 0, button->width(), button->height()));
            INFO(name);
            CHECK(r.right() <= panel->width() + 0.5);
            CHECK(r.left() >= -0.5);
        }
    }
}
