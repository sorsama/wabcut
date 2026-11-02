// 0.6 (P1 round, task D1) #19: Toggle Proxies and Playback Resolution are commands, not only monitor buttons.
//   * the five keymap commands (command registry, no default key in either key set, ready in the palette);
//   * View > Playback Resolution (Auto / Full / 1/2 / 1/4, the current one checked) and View > Use Proxies above the
//     rulers, and the same rows in the Source Monitor's settings menu - all driving MediaReadiness, the setting both
//     monitors play from;
//   * a screenshot of the View menu with its Playback Resolution submenu.

#include "ShellFixture.h"

#include "CommandPalette.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>

#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

void runCommand(const Shell& shell, const char* id) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
}

bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

void saveCrop(const Shell& shell, const QRectF& area, const QString& file) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
}

/// The editor at 1600x960, motion off, the menu bar and the singletons the test drives.
struct MonitorShell {
    Shell shell;
    QQuickWindow* w = nullptr;
    QObject* bar = nullptr;
    QObject* readiness = nullptr;
    CommandPalette* palette = nullptr;

    MonitorShell() {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        w = shell.window.get();
        bar = shell.object("appMenuBar");
        readiness = shell.engine.singletonInstance<QObject*>("Wabcut", "MediaReadiness");
        palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        REQUIRE(bar != nullptr);
        REQUIRE(readiness != nullptr);
        REQUIRE(palette != nullptr);
        spin(50);
    }

    QString quality() const { return readiness->property("previewQuality").toString(); }
    void setQuality(const QString& value) { readiness->setProperty("previewQuality", value); }
    bool proxies() const { return readiness->property("useProxies").toBool(); }
    void setProxies(bool on) { readiness->setProperty("useProxies", on); }

    QObject* openView() const {
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 6), Q_ARG(bool, false)));
        QObject* view = shell.object("viewMenu");
        REQUIRE(view != nullptr);
        REQUIRE(waitFor([&] { return opened(view); }));
        return view;
    }

    /// The row `name` of the open menu `menu` or of a submenu in it.
    QObject* row(QObject* menu, const char* name) const {
        QObject* found = menu->findChild<QObject*>(QLatin1String(name));
        INFO(name);
        REQUIRE(found != nullptr);
        return found;
    }

    /// Clicks a row of the open menu and waits for the menus to close.
    void choose(QObject* menu, const char* name) const {
        QObject* item = row(menu, name);
        CHECK(item->property("enabled").toBool());
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
        REQUIRE(waitFor([&] { return !shown(menu); }));
        spin(30);
    }

    void dismiss(QObject* menu) const {
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        REQUIRE(waitFor([&] { return !shown(menu); }));
    }
};

} // namespace

TEST_CASE("Monitor preview settings: View > Playback Resolution and Use Proxies, the Source Monitor's rows and the "
          "keymap commands",
          "[app][monitor][p1]") {
    resetSettings();
    clearMessages();
    MonitorShell s;
    const QString originalQuality = s.quality();
    const bool originalProxies = s.proxies();

    // The five commands: known, enabled (they always act), and with no default key in either shipped key set (a key
    // would show here; the registry carries no .kys name for them either).
    for (const char* id : {"monitor.toggleProxies", "monitor.playbackResolutionAuto", "monitor.playbackResolutionFull",
                           "monitor.playbackResolutionHalf", "monitor.playbackResolutionQuarter"}) {
        INFO(id);
        const QVariantMap state = s.palette->commandState(QString::fromLatin1(id));
        CHECK(state.value(QStringLiteral("known")).toBool());
        CHECK(state.value(QStringLiteral("enabled")).toBool());
        CHECK(state.value(QStringLiteral("reason")).toString().isEmpty());
        CHECK(evaluate(s.shell, QStringLiteral("KeymapDispatcher.shortcutText('%1')").arg(QLatin1String(id)))
                  .toString()
                  .isEmpty());
    }

    // The commands change the app's preview settings, which both monitors play from.
    s.setQuality("full");
    runCommand(s.shell, "monitor.playbackResolutionHalf");
    CHECK(s.quality() == QLatin1String("half"));
    runCommand(s.shell, "monitor.playbackResolutionQuarter");
    CHECK(s.quality() == QLatin1String("quarter"));
    runCommand(s.shell, "monitor.playbackResolutionAuto");
    CHECK(s.quality() == QLatin1String("auto"));
    runCommand(s.shell, "monitor.playbackResolutionFull");
    CHECK(s.quality() == QLatin1String("full"));
    const bool proxies = s.proxies();
    runCommand(s.shell, "monitor.toggleProxies");
    CHECK(s.proxies() == !proxies);
    runCommand(s.shell, "monitor.toggleProxies");
    CHECK(s.proxies() == proxies);

    // View > Playback Resolution, above the rulers: four checkable rows, the current one checked.
    s.setQuality("half");
    QObject* view = s.openView();
    QObject* submenu = view->findChild<QObject*>(QStringLiteral("viewPlaybackResolutionMenu"));
    REQUIRE(submenu != nullptr);
    CHECK(submenu->property("title").toString() == QStringLiteral("Playback Resolution"));
    QObject* half = s.row(view, "menuCommand_monitor.playbackResolutionHalf");
    QObject* full = s.row(view, "menuCommand_monitor.playbackResolutionFull");
    CHECK(half->property("checkable").toBool());
    CHECK(half->property("checked").toBool());
    CHECK_FALSE(full->property("checked").toBool());
    CHECK(full->property("text").toString() == QStringLiteral("Full"));
    // Choosing another resolution runs the command, and the checks move with the setting.
    REQUIRE(QMetaObject::invokeMethod(submenu, "open"));
    REQUIRE(waitFor([&] { return opened(submenu); }));
    s.choose(view, "menuCommand_monitor.playbackResolutionFull");
    CHECK(s.quality() == QLatin1String("full"));
    CHECK_FALSE(half->property("checked").toBool());
    CHECK(full->property("checked").toBool());
    view = s.openView();
    submenu = view->findChild<QObject*>(QStringLiteral("viewPlaybackResolutionMenu"));
    REQUIRE(QMetaObject::invokeMethod(submenu, "open"));
    REQUIRE(waitFor([&] { return opened(submenu); }));
    s.choose(view, "menuCommand_monitor.playbackResolutionQuarter");
    CHECK(s.quality() == QLatin1String("quarter"));

    // View > Use Proxies: a switch that shows and sets the same setting as the quality pill's film icon.
    view = s.openView();
    QObject* useProxies = s.row(view, "menuCommand_monitor.toggleProxies");
    CHECK(useProxies->property("checkable").toBool());
    CHECK(useProxies->property("checked").toBool() == s.proxies());
    s.choose(view, "menuCommand_monitor.toggleProxies");
    CHECK(s.proxies() == !proxies);
    CHECK(useProxies->property("checked").toBool() == s.proxies());

    // The Source Monitor's settings menu (its wrench) has the same rows; they set the same setting.
    QQmlComponent component(&s.shell.engine, "Wabcut", "SourceSettingsMenu");
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QObject> sourceMenu(component.create());
    REQUIRE(sourceMenu != nullptr);
    CHECK(sourceMenu->property("objectName").toString() == QStringLiteral("sourceMonitorSettingsMenu"));
    QObject* sourceResolution = sourceMenu->findChild<QObject*>(QStringLiteral("sourcePlaybackResolutionMenu"));
    REQUIRE(sourceResolution != nullptr);
    s.setQuality("full");
    bool chose = false;
    for (QObject* row : sourceResolution->findChildren<QObject*>()) {
        if (!row->inherits("QQuickMenuItem") || row->property("text").toString() != QStringLiteral("1/4"))
            continue;
        REQUIRE(QMetaObject::invokeMethod(row, "triggered"));
        chose = true;
    }
    REQUIRE(chose);
    CHECK(s.quality() == QLatin1String("quarter"));
    QObject* sourceProxies = sourceMenu->findChild<QObject*>(QStringLiteral("sourceUseProxiesItem"));
    REQUIRE(sourceProxies != nullptr);
    const bool before = s.proxies();
    REQUIRE(QMetaObject::invokeMethod(sourceProxies, "triggered"));
    CHECK(s.proxies() == !before);

    // The screenshot: the View menu with its Playback Resolution submenu open (dark).
    s.setQuality("half");
    s.setProxies(true);
    view = s.openView();
    submenu = view->findChild<QObject*>(QStringLiteral("viewPlaybackResolutionMenu"));
    REQUIRE(QMetaObject::invokeMethod(submenu, "open"));
    REQUIRE(waitFor([&] { return opened(submenu); }));
    spin(200);
    auto* menuItem = qobject_cast<QQuickItem*>(submenu->property("contentItem").value<QObject*>());
    auto* viewItem = qobject_cast<QQuickItem*>(view->property("contentItem").value<QObject*>());
    REQUIRE(menuItem != nullptr);
    REQUIRE(viewItem != nullptr);
    saveCrop(s.shell, sceneRect(menuItem->parentItem()).united(sceneRect(viewItem->parentItem())).adjusted(-8, -8, 8, 8),
             QStringLiteral("p1d-view-playback-resolution.png"));
    s.dismiss(view);

    // The Program Monitor's quality badge reads its state from the same setting (its film icon shows proxies, and the
    // pill is hidden in the sample project, which has none).
    CHECK(s.quality() == QLatin1String("half"));
    CHECK_FALSE(evaluate(s.shell, QStringLiteral("MediaReadiness.qualityBadge")).toString().isEmpty());

    s.setQuality(originalQuality);
    s.setProxies(originalProxies);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages) {
        // Only what this test's QML could warn about (other streams' windows are not this test's business).
        if (!m.contains(QStringLiteral("Main.qml")) && !m.contains(QStringLiteral("ViewMenu.qml"))
                && !m.contains(QStringLiteral("SourceSettingsMenu.qml")))
            continue;
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
    }
    resetSettings();
}
