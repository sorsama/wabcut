// Card 38 (0.7): VR / 360 video in the interface - Interpret Footage and Sequence Settings set the format, the monitor's
// wrench menu turns the VR viewer on for a VR sequence, and dragging, scrolling, double-clicking and the keys look around
// in it. Offscreen; screenshots (dark and light) in test-output/ui/.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "EditingBridge.h"
#include "MonitorBridge.h"
#include "ProjectController.h"

#include <wabcut/model/EditingOps.h>
#include <wabcut/model/Vr.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QWheelEvent>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

QQuickItem* named(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = named(child, name)) {
            return found;
        }
    }
    return nullptr;
}

struct VrShell {
    Shell shell;
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaa|
        A1     |Aaaaaaaaaa|
    )"};
    ProjectController* controller = nullptr;
    EditingBridge* editing = nullptr;
    MonitorBridge* monitors = nullptr;

    VrShell() {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
        monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
        REQUIRE(controller);
        REQUIRE(editing);
        REQUIRE(monitors);
        controller->resetProject(tl.state, tl.seq);
    }

    QQuickItem* find(const char* name) { return named(shell.window->contentItem(), QString::fromLatin1(name)); }

    QObject* opened(const char* name) {
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = shell.window->findChild<QObject*>(QString::fromLatin1(name));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        return dialog;
    }

    /// The window (or the item `crop` is) in both themes, saved as <name>-dark.png and <name>-light.png.
    void shots(const QString& name, QObject* crop = nullptr) {
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
            spin(60);
            QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            if (auto* item = qobject_cast<QQuickItem*>(crop)) {
                const double dpr = image.devicePixelRatio();
                const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                image = image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect());
            }
            REQUIRE(image.save(QDir(outputDir()).filePath(name + (dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png")))));
        }
        shell.setAppearance(true, "ultraviolet");
    }

};

VrFormat sequenceVr(const VrShell& s) { return s.controller->snapshot()->findSequence(s.tl.seq)->vr; }

void send(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButtons buttons) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, scene, scene, window->mapToGlobal(scene), button, buttons, {});
    QCoreApplication::sendEvent(window, &event);
}

} // namespace

TEST_CASE("VR UI: Interpret Footage sets the projection and the eye layout and says what the file claims (card 38)",
          "[app][vr][editing][screenshot]") {
    resetSettings();
    clearMessages();
    VrShell s;
    const QString media = mediaKey(s.tl.media('A'));
    s.editing->openInterpretFootage({media});
    QObject* dialog = s.opened("interpretFootageDialog");

    // The file says nothing; the choice starts at "use the file's".
    CHECK(dialog->property("vrProjection").toString().isEmpty());
    CHECK(dialog->property("info").toMap().value(QStringLiteral("fileVrProjection")).toString() == QStringLiteral("none"));
    QQuickItem* projection = s.find("interpretVrProjection");
    QQuickItem* stereo = s.find("interpretVrStereo");
    REQUIRE(projection);
    REQUIRE(stereo);
    REQUIRE(s.shell.grab().isNull() == false);
    CHECK_FALSE(stereo->isVisible()); // a layout only after a projection
    CHECK(s.find("interpretVrFile"));

    dialog->setProperty("vrProjection", QStringLiteral("equirect360"));
    dialog->setProperty("vrStereo", QStringLiteral("topBottom"));
    REQUIRE(s.shell.grab().isNull() == false);
    CHECK(stereo->isVisible());
    s.shots(QStringLiteral("vr-interpret-footage"), dialog);

    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK(dialog->property("errorText").toString().isEmpty());
    const MediaItem* item = s.controller->snapshot()->findMedia(s.tl.media('A'));
    REQUIRE(item);
    CHECK(mediaVr(*item) == VrFormat{VrProjection::Equirectangular360, StereoLayout::TopBottom});

    // Reopened, the dialog shows what is set; "flat" is an answer too, and "use the file's" clears it.
    s.editing->openInterpretFootage({media});
    dialog = s.opened("interpretFootageDialog");
    CHECK(dialog->property("vrProjection").toString() == QStringLiteral("equirect360"));
    CHECK(dialog->property("vrStereo").toString() == QStringLiteral("topBottom"));
    dialog->setProperty("vrProjection", QStringLiteral("none"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK_FALSE(mediaVr(*s.controller->snapshot()->findMedia(s.tl.media('A'))).isVr());
    CHECK(s.controller->snapshot()->findMedia(s.tl.media('A'))->interpretation->vr.has_value());
    s.editing->openInterpretFootage({media});
    dialog = s.opened("interpretFootageDialog");
    dialog->setProperty("vrProjection", QString());
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK_FALSE(s.controller->snapshot()->findMedia(s.tl.media('A'))->interpretation.has_value());
    const QStringList messages = takeMessages();
    INFO(join(messages));
}

TEST_CASE("VR UI: Sequence Settings makes a sequence VR and warns about a frame of the wrong shape (card 38)",
          "[app][vr][editing][screenshot]") {
    resetSettings();
    clearMessages();
    VrShell s;
    REQUIRE(s.editing->runCommand(QStringLiteral("sequence.settings")));
    QObject* dialog = s.opened("sequenceSettingsDialog");
    CHECK(dialog->property("vrProjection").toString() == QStringLiteral("none"));
    QQuickItem* hint = s.find("sequenceSettingsVrHint");
    REQUIRE(hint);
    REQUIRE_FALSE(s.shell.grab().isNull());
    CHECK_FALSE(hint->isVisible());
    CHECK_FALSE(s.find("sequenceSettingsVrStereo")->isVisible());

    dialog->setProperty("vrProjection", QStringLiteral("equirect360"));
    REQUIRE_FALSE(s.shell.grab().isNull());
    CHECK(hint->isVisible());
    // The sequence is 16:9: a 360 picture is 2:1, so the hint asks for the frame size to match.
    CHECK_FALSE(dialog->property("vrShapeFits").toBool());
    CHECK(hint->property("text").toString().contains(QStringLiteral("2:1")));
    s.shots(QStringLiteral("vr-sequence-settings-warning"), dialog);

    dialog->setProperty("frameWidth", 3840);
    dialog->setProperty("frameHeight", 1920);
    dialog->setProperty("vrStereo", QStringLiteral("mono"));
    CHECK(dialog->property("vrShapeFits").toBool());
    dialog->setProperty("vrStereo", QStringLiteral("topBottom")); // two 2:1 eyes one above the other are 1:1
    CHECK_FALSE(dialog->property("vrShapeFits").toBool());
    dialog->setProperty("frameHeight", 3840);
    CHECK(dialog->property("vrShapeFits").toBool());
    s.shots(QStringLiteral("vr-sequence-settings"), dialog);

    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    INFO(dialog->property("errorText").toString().toStdString());
    CHECK(dialog->property("errorText").toString().isEmpty());
    CHECK(sequenceVr(s) == VrFormat{VrProjection::Equirectangular360, StereoLayout::TopBottom});
    CHECK(s.controller->snapshot()->findSequence(s.tl.seq)->height == 3840);
    CHECK(s.controller->sequenceVrProjection() == QStringLiteral("equirect360"));
    CHECK(s.controller->sequenceVrStereo() == QStringLiteral("topBottom"));
    // Back to flat.
    REQUIRE(s.editing->runCommand(QStringLiteral("sequence.settings")));
    dialog = s.opened("sequenceSettingsDialog");
    CHECK(dialog->property("vrProjection").toString() == QStringLiteral("equirect360"));
    dialog->setProperty("vrProjection", QStringLiteral("none"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK_FALSE(sequenceVr(s).isVr());
}

TEST_CASE("VR UI: the Program Monitor looks around in a VR sequence by drag and wheel keys and double-click (card 38)",
          "[app][vr][monitor][screenshot]") {
    resetSettings();
    clearMessages();
    VrShell s;
    QQuickItem* monitor = s.find("programMonitor");
    REQUIRE(monitor);
    REQUIRE_FALSE(s.shell.grab().isNull());

    // A flat sequence: the viewer cannot be turned on (the menu item is off) and nothing shows.
    QObject* menu = s.shell.object("programMonitorSettingsMenu");
    REQUIRE(menu);
    QObject* menuItem = menu->findChild<QObject*>(QStringLiteral("programVrViewItem"));
    REQUIRE(menuItem);
    CHECK_FALSE(menuItem->property("enabled").toBool());
    s.monitors->setProgramVrView(true);
    REQUIRE_FALSE(s.shell.grab().isNull());
    CHECK_FALSE(monitor->property("vrView").toBool());
    QQuickItem* overlay = s.find("vrViewOverlay");
    REQUIRE(overlay);
    CHECK_FALSE(overlay->isVisible());

    // A VR sequence: the item is enabled, the viewer is on, the picture is the shape of the well.
    REQUIRE(s.editing->applySequenceSettings(sequenceKey(s.tl.seq),
                                             {{QStringLiteral("width"), 3840}, {QStringLiteral("height"), 1920},
                                              {QStringLiteral("vr"), QVariantMap{{QStringLiteral("projection"), QStringLiteral("equirect360")},
                                                                                 {QStringLiteral("stereo"), QStringLiteral("mono")}}}})
                .isEmpty());
    REQUIRE_FALSE(s.shell.grab().isNull());
    CHECK(menuItem->property("enabled").toBool());
    CHECK(monitor->property("vrView").toBool());
    CHECK(monitor->property("vrProjection").toString() == QStringLiteral("equirect360"));
    CHECK(overlay->isVisible());
    CHECK(overlay->width() > overlay->height()); // not the 2:1 sequence shape: a view the shape of the well
    CHECK(monitor->property("vrYaw").toDouble() == Approx(0.0));
    CHECK(monitor->property("vrFov").toDouble() == Approx(90.0));

    QQuickWindow* window = s.shell.window.get();
    const QPointF centre = overlay->mapToScene(QPointF(overlay->width() / 2, overlay->height() / 2));
    // Pulling the scene to the right turns the view to the left (negative yaw); pulling it down turns it up.
    send(window, QEvent::MouseButtonPress, centre, Qt::LeftButton);
    send(window, QEvent::MouseMove, centre + QPointF(120, 0), Qt::LeftButton);
    send(window, QEvent::MouseMove, centre + QPointF(120, 60), Qt::LeftButton);
    send(window, QEvent::MouseButtonRelease, centre + QPointF(120, 60), Qt::NoButton);
    CHECK(monitor->property("vrYaw").toDouble() < -1.0);
    CHECK(monitor->property("vrPitch").toDouble() > 1.0);
    // The wheel narrows the field of view; the keys turn it; Home and a double-click go straight ahead.
    const double fov = monitor->property("vrFov").toDouble();
    QWheelEvent wheel(centre, window->mapToGlobal(centre), {}, QPoint(0, 120), Qt::NoButton, {}, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &wheel);
    CHECK(monitor->property("vrFov").toDouble() < fov);
    overlay->forceActiveFocus();
    const double yaw = monitor->property("vrYaw").toDouble();
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, {});
    QCoreApplication::sendEvent(window, &right);
    CHECK(monitor->property("vrYaw").toDouble() > yaw);
    s.shots(QStringLiteral("vr-program-monitor"), nullptr);
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, {});
    QCoreApplication::sendEvent(window, &home);
    CHECK(monitor->property("vrYaw").toDouble() == Approx(0.0));
    CHECK(monitor->property("vrPitch").toDouble() == Approx(0.0));
    CHECK(monitor->property("vrFov").toDouble() == Approx(90.0));
    send(window, QEvent::MouseButtonPress, centre, Qt::LeftButton);
    send(window, QEvent::MouseMove, centre + QPointF(-80, 0), Qt::LeftButton);
    send(window, QEvent::MouseButtonRelease, centre + QPointF(-80, 0), Qt::NoButton);
    CHECK(monitor->property("vrYaw").toDouble() > 1.0);
    // Reset View does the same as Home (a double-click on the picture calls it too: MouseArea.onDoubleClicked).
    QQuickItem* resetButton = named(overlay, QStringLiteral("vrResetView")); // the Source Monitor has one too
    REQUIRE(resetButton);
    REQUIRE(QMetaObject::invokeMethod(resetButton, "clicked"));
    CHECK(monitor->property("vrYaw").toDouble() == Approx(0.0));
    // Pitch stops at the poles and the field of view at its limits.
    QMetaObject::invokeMethod(monitor, "vrLook", Q_ARG(double, 0.0), Q_ARG(double, 500.0));
    CHECK(monitor->property("vrPitch").toDouble() == Approx(90.0));
    for (int i = 0; i < 40; ++i) {
        QMetaObject::invokeMethod(monitor, "vrZoom", Q_ARG(double, 2.0));
    }
    CHECK(monitor->property("vrFov").toDouble() == Approx(10.0));
    QMetaObject::invokeMethod(monitor, "vrReset");

    // Turned off, the overlay goes and the picture is the sequence again.
    s.monitors->setProgramVrView(false);
    REQUIRE_FALSE(s.shell.grab().isNull());
    CHECK_FALSE(monitor->property("vrView").toBool());
    CHECK_FALSE(overlay->isVisible());
    const QStringList messages = takeMessages();
    INFO(join(messages));
}
