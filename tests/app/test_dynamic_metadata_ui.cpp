// Card "Dolby Vision 8.4 and HDR10+ dynamic metadata" in the Export drawer: the Dynamic metadata row appears for an HDR sequence on
// HEVC, offers HDR10+ (picking it sets the PQ master it describes), lists Dolby Vision greyed out with the reason this build has no
// licensed writer, and drops the choice again when the master changes to SDR. Screenshots: export-drawer-dynamic-metadata.png / -light.png.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "ExportController.h"
#include "ProjectController.h"

#include <wabcut/model/ColorManagementOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQmlComponent>
#include <QQuickItem>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

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

} // namespace

TEST_CASE("Export drawer: the Dynamic metadata row offers HDR10+ on a PQ HEVC master and says why Dolby Vision is not there",
          "[app][export][hdr][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl("V1 |Aaaaaaaaaa|\nA1 |Aaaaaaaaaa|");
    ColorManagement management;
    management.mode = ColorManagementMode::HdrPq;
    auto hdr = edit::setColorManagement(tl.state, management);
    REQUIRE(hdr);
    controller->resetProject(*hdr, tl.seq);
    shell.window->setProperty("workspace", 2); // Export
    spin(200);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* drawer = nullptr;
    REQUIRE(waitFor([&] {
        drawer = findNamed(content, QStringLiteral("exportSettingsDrawer"));
        return drawer != nullptr && drawer->isVisible();
    }));
    drawer->setProperty("open", true);
    spin(150);
    const auto summary = [&] { return drawer->property("summary").toMap(); };
    const auto overrides = [&] { return drawer->property("overrides").toMap(); };

    // H.264 cannot carry an HDR master: no row at all.
    QQuickItem* row = findNamed(drawer, QStringLiteral("exportDynamicMetadata"));
    REQUIRE(row);
    CHECK_FALSE(row->isVisible());

    // HEVC: the row is there, on None, with the note about Dolby Vision (this build has no licensed writer).
    QQuickItem* codecs = findNamed(drawer, QStringLiteral("exportCodecTabs"));
    REQUIRE(codecs);
    REQUIRE(QMetaObject::invokeMethod(codecs, "activated", Q_ARG(int, 1)));
    REQUIRE(waitFor([&] { return row->isVisible(); }));
    QQuickItem* tabs = findNamed(row, QStringLiteral("exportDynamicMetadataTabs"));
    QQuickItem* note = findNamed(row, QStringLiteral("exportDynamicMetadataNote"));
    REQUIRE(tabs);
    REQUIRE(note);
    CHECK(tabs->property("currentIndex").toInt() == 0);
    REQUIRE(waitFor([&] { return !summary().value(QStringLiteral("dolbyVisionReason")).toString().isEmpty(); }));
    CHECK(summary().value(QStringLiteral("dolbyVisionReason")).toString().contains(QStringLiteral("Dolby licence")));
    CHECK(summary().value(QStringLiteral("hdr10PlusAvailable")).toBool());
    CHECK(note->isVisible());
    CHECK(note->property("text").toString().contains(QStringLiteral("Dolby Vision is not available in this build")));
    const auto enabled = [&](int index) {
        bool ok = false;
        REQUIRE(QMetaObject::invokeMethod(tabs, "segmentEnabled", Q_RETURN_ARG(bool, ok), Q_ARG(int, index)));
        return ok;
    };
    CHECK(enabled(0));
    CHECK(enabled(1));        // HDR10+
    CHECK_FALSE(enabled(2));  // Dolby Vision 8.1
    CHECK_FALSE(enabled(3));  // Dolby Vision 8.4

    // HDR10+: the override is set, and the master it describes (PQ, 10-bit) follows; the note says what will happen.
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 1)));
    REQUIRE(waitFor([&] { return overrides().value(QStringLiteral("dynamicMetadata")).toString() == QStringLiteral("hdr10plus"); }));
    REQUIRE(waitFor([&] { return summary().value(QStringLiteral("dynamicMetadataSetting")).toString() == QStringLiteral("hdr10plus"); }));
    CHECK(summary().value(QStringLiteral("dynamicRangeSetting")).toString() == QStringLiteral("pq"));
    CHECK(note->property("text").toString().contains(QStringLiteral("measured from the picture in a first pass")));
    CHECK(tabs->property("currentIndex").toInt() == 1);

    // The screenshots (the same technique as the colour space row's: a second drawer, opened without its animation).
    QQmlComponent component(&shell.engine, "Wabcut", "ExportSettingsDrawer");
    INFO(component.errorString().toStdString());
    QObject* made = component.createWithInitialProperties(
        {{QStringLiteral("open"), true}, {QStringLiteral("summary"), drawer->property("summary")},
         {QStringLiteral("overrides"), drawer->property("overrides")}, {QStringLiteral("width"), 600}});
    auto* shot = qobject_cast<QQuickItem*>(made);
    REQUIRE(shot);
    shot->setParentItem(shell.window->contentItem());
    shot->setZ(10'000);
    shot->setHeight(shot->implicitHeight());
    spin(300);
    QQuickItem* shotRow = findNamed(shot, QStringLiteral("exportDynamicMetadata"));
    REQUIRE(shotRow);
    CHECK(shotRow->isVisible());
    (void)shell.grab();
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(300);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = shell.window->effectiveDevicePixelRatio();
        const QRectF rect = QRectF(0, 0, shot->width(), std::min<double>(shot->height(), shell.window->height()));
        REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                    .save(outputDir() + (dark ? QStringLiteral("/export-drawer-dynamic-metadata.png")
                                              : QStringLiteral("/export-drawer-dynamic-metadata-light.png"))));
    }
    shell.setAppearance(true, "ultraviolet");
    delete shot;

    // Choosing SDR for the master takes the metadata away (it describes an HDR master).
    QQuickItem* ranges = findNamed(drawer, QStringLiteral("exportDynamicRangeTabs"));
    REQUIRE(ranges);
    REQUIRE(QMetaObject::invokeMethod(ranges, "activated", Q_ARG(int, 1)));
    REQUIRE(waitFor([&] { return !overrides().contains(QStringLiteral("dynamicMetadata")); }));
    REQUIRE(waitFor([&] { return summary().value(QStringLiteral("dynamicMetadataSetting")).toString() == QStringLiteral("none"); }));
    const QStringList messages = takeMessages();
    INFO(messages.join(QLatin1Char('\n')).toStdString());
    CHECK(messages.isEmpty());
    resetSettings();
}
