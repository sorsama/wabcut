// Design language v3 "Echo": the echo mark (EchoMark) draws both brand inks at icon and logo sizes, its loaders
// (EchoLoader) loop only without reduce motion and otherwise show the still mark, and the launch intro (LaunchIntro)
// never shows in a window that is not shown and finishes at once under reduce motion. Offscreen; no window is shown.

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QImage>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace wabcut::test;

namespace {

/// `qml` (a component of the Wabcut module) created in the shell's engine, over the window's content at `at`.
std::unique_ptr<QQuickItem> makeHost(Shell& shell, const QByteArray& qml, const QRectF& at) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/echo-motion/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(at.topLeft());
    item->setSize(at.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

bool closeTo(const QColor& c, int r, int g, int b, int tolerance) {
    return std::abs(c.red() - r) <= tolerance && std::abs(c.green() - g) <= tolerance && std::abs(c.blue() - b) <= tolerance;
}

struct InkCount {
    int colours = 0;
    bool violet = false;
    bool lime = false;
};

/// The distinct colours under `item` in the grab, and whether the brand violet (#9B6CFF) and lime (#D6F54C) show.
InkCount inks(const QImage& image, const QQuickItem* item) {
    const double dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
    const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    const int x0 = static_cast<int>(std::lround(scene.left() * dpr));
    const int y0 = static_cast<int>(std::lround(scene.top() * dpr));
    const int x1 = static_cast<int>(std::lround(scene.right() * dpr));
    const int y1 = static_cast<int>(std::lround(scene.bottom() * dpr));
    QSet<QRgb> seen;
    InkCount out;
    for (int y = std::max(0, y0); y < std::min(image.height(), y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(image.width(), x1); ++x) {
            const QColor c = image.pixelColor(x, y);
            seen.insert(c.rgb());
            out.violet = out.violet || closeTo(c, 0x9B, 0x6C, 0xFF, 36);
            out.lime = out.lime || closeTo(c, 0xD6, 0xF5, 0x4C, 36);
        }
    }
    out.colours = static_cast<int>(seen.size());
    return out;
}

} // namespace

TEST_CASE("The echo mark draws both brand inks at 16 and 64 px", "[app][design][echo]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);

    const auto host = makeHost(shell,
                               "import QtQuick\nimport Wabcut\n"
                               "Rectangle {\n"
                               "    color: Theme.canvas\n"
                               "    EchoMark { objectName: \"mark16\"; x: 8; y: 8; size: 16 }\n"
                               "    EchoMark { objectName: \"mark64\"; x: 40; y: 8; size: 64 }\n"
                               "}\n",
                               QRectF(0, 0, 120, 80));
    auto* mark16 = host->findChild<QQuickItem*>(QStringLiteral("mark16"));
    auto* mark64 = host->findChild<QQuickItem*>(QStringLiteral("mark64"));
    REQUIRE(mark16);
    REQUIRE(mark64);
    CHECK(mark16->property("small").toBool());   // the three-clip mark below 28 px
    CHECK_FALSE(mark64->property("small").toBool());

    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    image.save(outputDir() + QStringLiteral("/echo-mark.png"));
    for (const QQuickItem* mark : {static_cast<const QQuickItem*>(mark16), static_cast<const QQuickItem*>(mark64)}) {
        INFO(mark->objectName().toStdString());
        const InkCount found = inks(image, mark);
        CHECK(found.colours > 2);
        CHECK(found.violet);
        CHECK(found.lime);
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Echo loaders loop only without reduce motion and otherwise show the still mark", "[app][design][echo]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);

    for (const char* variant : {"equalizer", "chase", "scan", "build"}) {
        for (const int size : {20, 110}) {
            INFO(variant << " at " << size << " px");
            shell.theme->setProperty("reduceMotion", false);
            const QByteArray qml = QByteArray("import QtQuick\nimport Wabcut\nItem {\n    EchoLoader { objectName: \"loader\"; size: ")
                                   + QByteArray::number(size) + "; variant: \"" + variant + "\" }\n}\n";
            const auto host = makeHost(shell, qml, QRectF(0, 0, 120, 120));
            QObject* loader = host->findChild<QObject*>(QStringLiteral("loader"));
            REQUIRE(loader);
            QObject* motion = loader->findChild<QObject*>(QStringLiteral("echoLoaderMotion"));
            auto* still = loader->findChild<QQuickItem*>(QStringLiteral("echoLoaderMark"));
            REQUIRE(motion);
            REQUIRE(still);

            // Running: the variant's motion is loaded, the still mark is hidden.
            CHECK(loader->property("animating").toBool());
            CHECK(motion->property("active").toBool());
            CHECK(motion->property("item").value<QObject*>() != nullptr);
            CHECK_FALSE(still->isVisible());

            // Reduce motion: nothing loops; the mark stands still.
            shell.theme->setProperty("reduceMotion", true);
            CHECK_FALSE(loader->property("animating").toBool());
            CHECK_FALSE(motion->property("active").toBool());
            CHECK(still->isVisible());

            // Not running (a paused job): the same.
            shell.theme->setProperty("reduceMotion", false);
            CHECK(loader->property("animating").toBool());
            loader->setProperty("running", false);
            CHECK_FALSE(loader->property("animating").toBool());
            CHECK(still->isVisible());
        }
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The launch intro stays hidden in a window that was not shown", "[app][design][echo]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    spin(100);

    // Main mounts it and plays it only when its window first shows: the test window never does.
    QQuickItem* intro = shell.item("launchIntro");
    REQUIRE(intro != nullptr);
    CHECK_FALSE(intro->property("playing").toBool());
    CHECK_FALSE(intro->isVisible());
    CHECK_FALSE(intro->isEnabled());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The launch intro finishes at once under reduce motion and a skip ends it", "[app][design][echo]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);

    const auto host = makeHost(shell,
                               "import QtQuick\nimport Wabcut\n"
                               "Item {\n"
                               "    id: host\n"
                               "    property int finishedCount: 0\n"
                               "    LaunchIntro { objectName: \"intro\"; onFinished: host.finishedCount += 1 }\n"
                               "}\n",
                               QRectF(0, 0, kShotWidth, kShotHeight));
    auto* intro = host->findChild<QQuickItem*>(QStringLiteral("intro"));
    REQUIRE(intro);
    CHECK_FALSE(intro->isVisible());

    // Reduce motion: play() finishes straight away and nothing is ever shown.
    shell.theme->setProperty("reduceMotion", true);
    REQUIRE(QMetaObject::invokeMethod(intro, "play"));
    CHECK(host->property("finishedCount").toInt() == 1);
    CHECK_FALSE(intro->property("playing").toBool());
    CHECK_FALSE(intro->isVisible());
    spin(300);
    CHECK_FALSE(intro->isVisible());
    CHECK(host->property("finishedCount").toInt() == 1);

    // With motion it covers the window and takes input until skipped (a click or Esc).
    shell.theme->setProperty("reduceMotion", false);
    REQUIRE(QMetaObject::invokeMethod(intro, "play"));
    CHECK(intro->property("playing").toBool());
    CHECK(intro->isVisible());
    CHECK(intro->isEnabled());
    CHECK(host->property("finishedCount").toInt() == 1);
    REQUIRE(QMetaObject::invokeMethod(intro, "skip"));
    CHECK(host->property("finishedCount").toInt() == 2);
    CHECK_FALSE(intro->property("playing").toBool());
    CHECK_FALSE(intro->isVisible());
    CHECK_FALSE(intro->isEnabled());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Segmented tabs give every label its room and elide only when the control is too narrow",
          "[app][design][echo]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);

    // The timeline display popover's Video thumbnails choice, whose labels differ a lot in length.
    const auto host = makeHost(shell,
                               "import QtQuick\nimport Wabcut\n"
                               "Rectangle {\n"
                               "    color: Theme.panel\n"
                               "    PillTabs {\n"
                               "        objectName: \"tabs\"; x: 8; y: 8; width: 320; compact: true; currentIndex: 3\n"
                               "        model: [\"Off\", \"Head\", \"Head and tail\", \"Continuous\"]\n"
                               "    }\n"
                               "}\n",
                               QRectF(0, 0, 400, 60));
    auto* tabs = host->findChild<QQuickItem*>(QStringLiteral("tabs"));
    REQUIRE(tabs);

    const auto widths = [&] {
        std::vector<double> out;
        for (const QVariant& w : tabs->property("segmentWidths").toList())
            out.push_back(w.toDouble());
        return out;
    };
    const auto natural = [&] {
        std::vector<double> out;
        for (const QVariant& w : tabs->property("naturalWidths").toList())
            out.push_back(w.toDouble());
        return out;
    };
    const auto sum = [](const std::vector<double>& v) {
        double total = 0;
        for (const double w : v)
            total += w;
        return total;
    };
    const double room = [&] { return tabs->property("availableWidth").toDouble(); }();

    // Wide enough for the labels but not for four equal segments: each segment holds its label, together they fill
    // the track, and the long ones are wider than the short ones.
    std::vector<double> got = widths();
    std::vector<double> want = natural();
    REQUIRE(got.size() == 4);
    REQUIRE(want.size() == 4);
    CHECK(std::abs(sum(got) - room) < 0.5);
    for (std::size_t i = 0; i < got.size(); ++i) {
        INFO("segment " << i);
        CHECK(got[i] + 0.5 >= want[i]);
    }
    CHECK(got[2] > got[0]);

    // Roomy: equal segments, as before.
    tabs->setWidth(640);
    got = widths();
    for (const double w : got)
        CHECK(std::abs(w - got[0]) < 0.5);

    // Too narrow for the labels: the segments shrink in proportion and still fill the track exactly.
    tabs->setWidth(200);
    got = widths();
    CHECK(std::abs(sum(got) - tabs->property("availableWidth").toDouble()) < 0.5);
    for (const double w : got)
        CHECK(w > 0);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("A current tab's flared foot draws only the panel colour, over the strip or a hovered neighbour",
          "[app][design][echo]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.setAppearance(true, "ultraviolet");
    shell.theme->setProperty("reduceMotion", true);

    // A hovered neighbour (its hover fill) ends where the current tab starts; the tab's left flare lies over it.
    const auto host = makeHost(shell,
                               "import QtQuick\nimport Wabcut\n"
                               "Rectangle {\n"
                               "    color: Theme.sunken\n"
                               "    Rectangle { x: 0; y: 10; width: 60; height: 30; color: Theme.hover }\n"
                               "    PanelTab { objectName: \"tab\"; x: 60; y: 10; width: 100; height: 30; text: \"Timeline\" }\n"
                               "}\n",
                               QRectF(0, 0, 200, 40));
    REQUIRE(host->findChild<QQuickItem*>(QStringLiteral("tab")));
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
    const auto at = [&](double x, double y) {
        return image.pixelColor(static_cast<int>(std::lround(x * dpr)), static_cast<int>(std::lround(y * dpr)));
    };
    const QColor hover = shell.theme->property("hover").value<QColor>();
    const QColor panel = shell.theme->property("panel").value<QColor>();
    const double r = shell.theme->property("radiusTab").toDouble();
    REQUIRE(r >= 6);

    // Inside the curve (just beside the tab's side, up the flare): the neighbour shows, not a patch of the strip.
    const QColor inCurve = at(60 - r + 1.5, 40 - r + 1.5);
    INFO("in the curve " << inCurve.name().toStdString());
    CHECK(closeTo(inCurve, hover.red(), hover.green(), hover.blue(), 6));
    // In the corner the curve leaves: the panel's colour.
    const QColor inFoot = at(60 - 1.5, 40 - 1.5);
    INFO("in the foot " << inFoot.name().toStdString());
    CHECK(closeTo(inFoot, panel.red(), panel.green(), panel.blue(), 6));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
