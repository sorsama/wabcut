// 0.5 (P0 round, stream B) in the shell: the Audio Meters panel and audio scrubbing.
//   * Audio Meters: right of the timeline in the Edit preset (new and reset workspaces; a saved layout keeps its own
//     panels), the mix's meter from a synthetic feed (two bars, the dB scale where the meter draws those levels, the
//     peak readout, the clip light a click clears), screenshots in the dark, light and high-contrast themes; the meter
//     viewer count it shares with the Audio Track Mixer; Window > Audio Meters (window.audioMeters) and the docking:
//     closed, opened again beside the timeline, a tab of the timeline's group, a window of its own.
//   * Audio scrubbing: with Playback.audioScrubbing on (the default), a drag in the ruler or on the Program Monitor's
//     time bar and frame steps (the arrow keys) ask for a window at the new playhead while stopped, other playhead moves
//     do not; Shift+S, Sequence > Audio Scrubbing and Settings > Timeline & editing turn it off and on, remembered.

#include "ShellFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "MeterBar.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QScreen>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

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

QQuickItem* scene(const Shell& shell, const char* name) {
    QQuickItem* item = findNamed(shell.window->contentItem(), QString::fromLatin1(name));
    INFO(name);
    REQUIRE(item != nullptr);
    return item;
}

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

QString groupsText(const Shell& shell) {
    return evaluate(shell, QStringLiteral("Workspaces.groupsOf(docking.tree).map(g => g.panels.join('+')).join(' ')")).toString();
}

void runCommand(const Shell& shell, const char* id) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF p, Qt::MouseButtons buttons) {
    QMouseEvent event(type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QCoreApplication::sendEvent(window, &event);
}

/// A press at `from`, a move to `to` and a release there (the left button).
void drag(QQuickWindow* window, QPointF from, QPointF to) {
    mouse(window, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(window, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
    mouse(window, QEvent::MouseMove, to, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, to, Qt::NoButton);
}

void key(QQuickWindow* window, int code, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

/// A place beyond every screen: a window there puts nothing on the desktop.
QPoint offScreen() {
    QRect desktop;
    for (QScreen* screen : QGuiApplication::screens())
        desktop |= screen->geometry();
    return {desktop.right() + 400, desktop.top() + 40};
}

void saveCrop(const Shell& shell, const QRectF& area, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(name)));
}

/// WCAG contrast ratio of two opaque colours.
double contrast(const QColor& a, const QColor& b) {
    const auto lum = [](const QColor& c) {
        const auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
    };
    const double la = lum(a);
    const double lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// Every item named `name` under `root`.
void collectNamed(QQuickItem* root, const QString& name, QList<QQuickItem*>& out) {
    if (!root)
        return;
    if (root->objectName() == name)
        out << root;
    for (QQuickItem* child : root->childItems())
        collectNamed(child, name, out);
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Audio Meters: right of the timeline in the Edit preset with the mix's meter; screenshots",
          "[app][audio][meters][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer != nullptr);
    // The mix at -6 dBFS peaks, no clip yet.
    AudioMixerBridge::MeterReading master{2, {0.5f, 0.45f}, {0.2f, 0.17f}, false};
    mixer->setMeterSource([&master](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        if (index == -1)
            return master;
        return std::nullopt;
    });
    spin(200);
    REQUIRE_FALSE(shell.grab().isNull()); // a hidden window lays its layouts out when it renders

    // The Edit preset: the timeline with the Audio Meters at its right, slim, as tall as the timeline.
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline audioMeters"));
    QQuickItem* panel = shell.item("audioMetersPanel");
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(panel != nullptr);
    REQUIRE(timeline != nullptr);
    CHECK(panel->isVisible());
    const QRectF meters = sceneRect(panel);
    const QRectF lanes = sceneRect(timeline);
    INFO("meters " << meters.x() << "," << meters.y() << " " << meters.width() << "x" << meters.height() << "; timeline "
                   << lanes.x() << "," << lanes.y() << " " << lanes.width() << "x" << lanes.height());
    CHECK(meters.left() > lanes.right());
    CHECK(meters.left() - lanes.right() < 20); // the gutter
    CHECK(std::abs(meters.top() - lanes.top()) < 1.0);
    CHECK(std::abs(meters.bottom() - lanes.bottom()) < 1.0);
    CHECK(meters.width() >= 119);
    CHECK(meters.width() < 200);
    CHECK(lanes.width() > 1200);

    // It shows meters: the mixer's poll runs for it alone.
    CHECK(mixer->metersActive());
    CHECK(mixer->meterViewers() == 1);
    CHECK(mixer->isMeterViewer(panel));
    auto* meter = qobject_cast<MeterBar*>(scene(shell, "audioMetersMeter"));
    REQUIRE(meter != nullptr);
    REQUIRE(waitFor([&] { return meter->channels() == 2; }));
    CHECK(meter->peakText() == QStringLiteral("−6.0"));
    CHECK(scene(shell, "audioMetersPeak")->property("text").toString() == QStringLiteral("−6.0 dB"));
    CHECK(meter->height() > 250);

    // The scale: 0 dB at the meter's top, -60 at its bottom, -6 a tenth of the way down.
    const QRectF bar = sceneRect(meter);
    const QRectF top = sceneRect(scene(shell, "audioMetersMark_0"));
    const QRectF six = sceneRect(scene(shell, "audioMetersMark_6"));
    const QRectF floor = sceneRect(scene(shell, "audioMetersMark_60"));
    CHECK(std::abs(top.top() - bar.top()) < 1.0);
    CHECK(std::abs(floor.bottom() - bar.bottom()) < 1.0);
    CHECK(std::abs(six.center().y() - (bar.top() + bar.height() * 0.1)) < 1.5);
    CHECK(top.right() <= bar.left());

    // The clip light: red once the mix reached 0 dBFS, until it is clicked.
    QQuickItem* light = scene(shell, "audioMetersClipLight");
    CHECK_FALSE(sameColor(light->property("color").value<QColor>(), shell.themeColor("danger")));
    master.peak = {1.02f, 0.9f};
    master.clipped = true;
    REQUIRE(waitFor([&] { return meter->clipped(); }));
    CHECK(sameColor(light->property("color").value<QColor>(), shell.themeColor("danger")));
    spin(100);
    REQUIRE_FALSE(shell.grab().isNull()); // a frame draws the bars
    CHECK(meter->drawnRects() >= 4);
    const QRectF shot = meters.united(QRectF(lanes.right() - 420, lanes.top(), 420, lanes.height())).adjusted(-8, -8, 8, 8);
    saveCrop(shell, shot, QStringLiteral("audio-meters.png"));
    REQUIRE(shell.grab().save(QDir(outputDir()).filePath(QStringLiteral("audio-meters-edit.png"))));

    master.peak = {0.5f, 0.45f};
    master.clipped = false;
    mouse(shell.window.get(), QEvent::MouseButtonPress, sceneRect(light).center(), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, sceneRect(light).center(), Qt::NoButton);
    REQUIRE(waitFor([&] { return !meter->clipped(); }));
    CHECK_FALSE(sameColor(light->property("color").value<QColor>(), shell.themeColor("danger")));

    // The light theme and high contrast.
    master.peak = {0.98f, 0.7f};
    master.rms = {0.4f, 0.3f};
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(100);
    saveCrop(shell, shot, QStringLiteral("audio-meters-light.png"));
    // The scale's small numerals read at 4.5:1 or better on the light panel (fix round: they were tertiary, 3.3:1).
    QList<QQuickItem*> labels;
    collectNamed(panel, QStringLiteral("audioMetersMarkLabel"), labels);
    REQUIRE(labels.size() == 8);
    for (QQuickItem* label : labels) {
        const QColor ink = label->property("color").value<QColor>();
        CHECK(sameColor(ink, shell.themeColor("textSecondary")));
        CHECK(contrast(ink, shell.themeColor("panel")) >= 4.5);
    }
    shell.setAppearance(true, "ultraviolet");
    shell.theme->setProperty("highContrast", true);
    REQUIRE(shell.settle(true, shell.themeColor("accentFill")));
    spin(100);
    saveCrop(shell, shot, QStringLiteral("audio-meters-contrast.png"));
    shell.theme->setProperty("highContrast", false);
    mixer->setMeterSource({});

    checkNoMessages();
}

TEST_CASE("Audio Meters: Window > Audio Meters with tabs and a window of its own; the meters poll while one shows",
          "[app][audio][meters][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    spin(100);
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer != nullptr);
    QQuickItem* panel = shell.item("audioMetersPanel");
    REQUIRE(panel != nullptr);
    REQUIRE(mixer->meterViewers() == 1);

    // Closed: no meter on screen, nothing polls; the timeline takes the row.
    evaluate(shell, QStringLiteral("docking.closePanel('audioMeters')"));
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline"));
    CHECK_FALSE(panel->isVisible());
    CHECK(mixer->meterViewers() == 0);
    CHECK_FALSE(mixer->metersActive());

    // Window > Audio Meters (window.audioMeters): right of the timeline again, slim, and focused.
    runCommand(shell, "window.audioMeters");
    spin(200);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline audioMeters"));
    CHECK(panel->isVisible());
    CHECK(panel->width() < 200);
    CHECK(sceneRect(panel).left() > sceneRect(shell.item("timelinePanel")).right());
    CHECK(mixer->meterViewers() == 1);
    CHECK(mixer->metersActive());
    CHECK(evaluate(shell, QStringLiteral("docking.activeGroup === docking.groupOfPanel('audioMeters').group")).toBool());

    // The timeline closed and opened again (Shift+3) comes back left of the meters, not under them.
    evaluate(shell, QStringLiteral("docking.closePanel('timeline')"));
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls audioMeters"));
    runCommand(shell, "window.timeline");
    spin(200);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline audioMeters"));
    CHECK(sceneRect(panel).left() > sceneRect(shell.item("timelinePanel")).right());
    CHECK(panel->width() < 200);

    // The Audio Track Mixer as well (Shift+6): two viewers; closing one leaves the other polling.
    runCommand(shell, "window.audioTrackMixer");
    spin(300);
    QQuickItem* mixerPanel = shell.item("audioMixerPanel");
    REQUIRE(mixerPanel != nullptr);
    CHECK(mixerPanel->isVisible());
    CHECK(mixer->meterViewers() == 2);
    CHECK(mixer->isMeterViewer(mixerPanel));
    evaluate(shell, QStringLiteral("docking.closePanel('audioMeters')"));
    spin(100);
    CHECK(mixer->meterViewers() == 1);
    CHECK(mixer->metersActive());
    CHECK_FALSE(mixer->isMeterViewer(panel));
    evaluate(shell, QStringLiteral("docking.closePanel('mixer')"));
    spin(100);
    CHECK(mixer->meterViewers() == 0);
    CHECK_FALSE(mixer->metersActive());

    // A tab of the timeline's group: it polls while its tab shows.
    runCommand(shell, "window.audioMeters");
    spin(100);
    evaluate(shell, QStringLiteral("docking.joinPanel('audioMeters', docking.groupOfPanel('timeline').group)"));
    spin(200);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline+audioMeters"));
    CHECK(panel->isVisible());
    CHECK(mixer->meterViewers() == 1);
    // A tabbed panel has its tab strip whatever its width (the group's tabs).
    CHECK(panel->property("tabbed").toBool());
    evaluate(shell, QStringLiteral("docking.activate('timeline')"));
    spin(200);
    CHECK_FALSE(panel->isVisible());
    CHECK(mixer->meterViewers() == 0);
    evaluate(shell, QStringLiteral("docking.activate('audioMeters')"));
    spin(200);
    CHECK(mixer->meterViewers() == 1);

    // A window of its own: it keeps metering there, and comes back when the window closes.
    evaluate(shell, QStringLiteral("docking.closePanel('audioMeters')"));
    runCommand(shell, "window.audioMeters");
    spin(100);
    const QPoint at = offScreen();
    evaluate(shell, QStringLiteral("docking.popOutAt(docking.slotOfPanel('audioMeters'), Qt.rect(%1, %2, 300, 500))")
                        .arg(at.x())
                        .arg(at.y()));
    spin(100);
    auto* floating = qobject_cast<QQuickWindow*>(
        evaluate(shell, QStringLiteral("docking.slotOfPanel('audioMeters').floatWindow")).value<QObject*>());
    REQUIRE(floating != nullptr);
    CHECK(panel->window() == floating);
    CHECK(panel->isVisible());
    CHECK(mixer->meterViewers() == 1);
    QCloseEvent close;
    QCoreApplication::sendEvent(floating, &close);
    spin(100);
    CHECK(panel->window() == shell.window.get());
    CHECK(mixer->meterViewers() == 1);

    checkNoMessages();
}

TEST_CASE("Audio Meters: a workspace saved before keeps its layout; Reset to Saved Layout docks the meters",
          "[app][audio][meters][workspaces][qml]") {
    resetSettings();
    clearMessages();
    {
        // A 0.4 Edit workspace: the timeline alone along the bottom.
        QSettings settings;
        settings.setValue(QStringLiteral("workspaces/layout"),
                          QStringLiteral(R"({"version":2,"workspaces":[{"id":100,"preset":"edit","name":"Edit","layout":)"
                                         R"({"split":"column","weights":[0.5,0.5],"children":[{"split":"row",)"
                                         R"("weights":[0.22,0.21,0.21,0.36],"children":[{"group":"g1","panels":["library"],)"
                                         R"("current":"library"},{"group":"g2","panels":["source"],"current":"source"},)"
                                         R"({"group":"g3","panels":["program"],"current":"program"},{"group":"g4",)"
                                         R"("panels":["effectControls"],"current":"effectControls"}]},{"group":"g5",)"
                                         R"("panels":["timeline"],"current":"timeline"}]},"floating":{},"volumeLanes":false}]})"));
    }
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline"));
    runCommand(shell, "window.resetToSavedLayout");
    spin(200);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline audioMeters"));
    // A new workspace from the Edit preset has them too.
    evaluate(shell, QStringLiteral("window.workspace = Workspaces.createFromPreset('edit', 'Edit 2')"));
    spin(200);
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline audioMeters"));
    checkNoMessages();
}

TEST_CASE("Audio scrubbing: ruler and time bar drags and frame steps ask for sound while stopped; Shift+S the menu "
          "and Settings turn it off",
          "[app][audio][scrub][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    spin(100);
    QQuickWindow* w = shell.window.get();
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(playback != nullptr);
    REQUIRE(controller != nullptr);
    CHECK(playback->audioScrubbing()); // on by default, as in Premiere
    std::vector<Ticks> heard;
    std::vector<int> directions;
    playback->setAudioScrubSink([&](editor::PlaybackSession* session, Ticks at, int direction) {
        CHECK(session == playback->session()); // the program's
        heard.push_back(at);
        directions.push_back(direction);
        return true;
    });
    const auto frame = [&](std::int64_t n) { return Ticks{controller->frameTicks().value * n}; };

    // A drag in the ruler: a window at each new frame, where the playhead is.
    QQuickItem* ruler = scene(shell, "rulerScrubArea");
    REQUIRE(ruler->isVisible());
    const auto rulerAt = [&](double f) { return ruler->mapToScene(QPointF(ruler->width() * f, ruler->height() / 2)); };
    controller->setPlayhead(Ticks{}); // not a scrub: silent (the demo starts further on)
    CHECK(heard.empty());
    drag(w, rulerAt(0.2), rulerAt(0.3));
    REQUIRE(heard.size() >= 2);
    CHECK(heard.back() == controller->playhead());
    CHECK(playback->audioScrubRequests() == static_cast<int>(heard.size()));
    CHECK(std::ranges::all_of(directions, [](int d) { return d == 1; }));
    // Dragged back: played backwards (Premiere's backward scrub).
    std::size_t backFrom = heard.size();
    drag(w, rulerAt(0.3), rulerAt(0.15));
    REQUIRE(heard.size() >= backFrom + 2);
    // (the press lands where the last drag ended; the two moves go back)
    CHECK(std::count(directions.begin() + static_cast<std::ptrdiff_t>(backFrom), directions.end(), -1) >= 2);
    CHECK(playback->lastAudioScrubDirection() == -1);
    CHECK(heard.back() == controller->playhead());
    // A click (a press and a release, no drag) left of the playhead plays the frame it lands on forward: a click is not
    // a backward drag (fix round).
    {
        const std::size_t clickFrom = heard.size();
        mouse(w, QEvent::MouseButtonPress, rulerAt(0.05), Qt::LeftButton);
        mouse(w, QEvent::MouseButtonRelease, rulerAt(0.05), Qt::NoButton);
        REQUIRE(heard.size() == clickFrom + 1);
        CHECK(directions.back() == 1);
        CHECK(heard.back() == controller->playhead());
    }
    // Other playhead moves are not scrubs: nothing plays.
    std::size_t before = heard.size();
    controller->goToStart();
    controller->setPlayhead(frame(40));
    CHECK(heard.size() == before);

    // Frame steps (Right, Shift+Left, from the timeline): the frame stepped to is heard.
    QQuickItem* lanes = shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(lanes->hasActiveFocus());
    key(w, Qt::Key_Right);
    REQUIRE(heard.size() == before + 1);
    CHECK(heard.back() == frame(41));
    key(w, Qt::Key_Left, Qt::ShiftModifier);
    REQUIRE(heard.size() == before + 2);
    CHECK(heard.back() == frame(36));
    CHECK(directions.back() == 1); // a step back plays the frame stepped to forward

    // A drag on the Program Monitor's time bar.
    QQuickItem* timeBar = scene(shell, "programTimeBarArea");
    REQUIRE(timeBar->isVisible());
    before = heard.size();
    const auto barAt = [&](double f) { return timeBar->mapToScene(QPointF(timeBar->width() * f, timeBar->height() / 2)); };
    drag(w, barAt(0.5), barAt(0.6));
    CHECK(heard.size() >= before + 2);
    CHECK(heard.back() == controller->playhead());
    // A click on it left of the playhead: forward too; a drag back: backwards.
    mouse(w, QEvent::MouseButtonPress, barAt(0.2), Qt::LeftButton);
    mouse(w, QEvent::MouseButtonRelease, barAt(0.2), Qt::NoButton);
    CHECK(directions.back() == 1);
    drag(w, barAt(0.2), barAt(0.1));
    CHECK(directions.back() == -1);

    // Shift+S: off (a toast says so, remembered); drags and steps are silent now.
    QObject* menuRow = shell.object("sequenceMenuAudioScrubbing");
    REQUIRE(menuRow != nullptr);
    CHECK(menuRow->property("checked").toBool());
    key(w, Qt::Key_S, Qt::ShiftModifier, QStringLiteral("S"));
    CHECK_FALSE(playback->audioScrubbing());
    CHECK_FALSE(menuRow->property("checked").toBool());
    CHECK(shell.object("toast")->property("text").toString().contains(QStringLiteral("off")));
    CHECK_FALSE(QSettings().value(QStringLiteral("timeline/audioScrubbing"), true).toBool());
    before = heard.size();
    drag(w, rulerAt(0.4), rulerAt(0.5));
    key(w, Qt::Key_Right);
    CHECK(heard.size() == before);

    // Sequence > Audio Scrubbing turns it back on (the row is a keymap command with Shift+S on it).
    CHECK(menuRow->property("shortcut").toString().contains(QStringLiteral("S")));
    REQUIRE(QMetaObject::invokeMethod(shell.object("appMenuBar"), "open", Q_ARG(int, 3), Q_ARG(bool, false)));
    QObject* sequenceMenu = shell.object("sequenceMenuBar");
    REQUIRE(sequenceMenu != nullptr);
    REQUIRE(waitFor([&] { return sequenceMenu->property("opened").toBool(); }));
    CHECK(menuRow->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(menuRow, "clicked"));
    REQUIRE(waitFor([&] { return !sequenceMenu->property("visible").toBool(); }));
    REQUIRE(waitFor([&] { return playback->audioScrubbing(); }));
    CHECK(menuRow->property("checked").toBool());

    // Settings > Timeline & editing: the row shows the switch and turns it off and on.
    REQUIRE(QMetaObject::invokeMethod(w, "openSettings", Q_ARG(QString, QStringLiteral("timeline")),
                                      Q_ARG(QString, QStringLiteral("audioScrubbing"))));
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader != nullptr);
    QObject* settingsWindow = loader->property("item").value<QObject*>();
    REQUIRE(settingsWindow != nullptr);
    REQUIRE(waitFor([&] { return settingsWindow->property("opened").toBool(); }));
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] {
        REQUIRE(QMetaObject::invokeMethod(settingsWindow, "page", Q_RETURN_ARG(QQuickItem*, page),
                                          Q_ARG(QString, QStringLiteral("timeline"))));
        return page != nullptr && page->isVisible();
    }));
    QQuickItem* toggle = findNamed(page, QStringLiteral("settingsAudioScrubbing"));
    REQUIRE(toggle != nullptr);
    CHECK(toggle->property("checked").toBool());
    toggle->setProperty("checked", false);
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
    CHECK_FALSE(playback->audioScrubbing());
    // Shift+S again while the page shows: the row follows.
    playback->setAudioScrubbing(true);
    CHECK(toggle->property("checked").toBool());
    spin(600); // the reveal's scroll and highlight
    REQUIRE_FALSE(shell.grab().isNull());
    spin(300);
    {
        const QRectF pageRect = sceneRect(page);
        const QRectF rows(pageRect.left(), sceneRect(toggle).center().y() - 150, pageRect.width(), 220);
        saveCrop(shell, rows.intersected(pageRect), QStringLiteral("settings-audio-scrubbing.png"));
        // The light theme too (fix round).
        shell.setAppearance(false, "ultraviolet");
        REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        spin(100);
        saveCrop(shell, rows.intersected(pageRect), QStringLiteral("settings-audio-scrubbing-light.png"));
        shell.setAppearance(true, "ultraviolet");
    }
    playback->setAudioScrubSink({});

    checkNoMessages();
}

TEST_CASE("Audio scrubbing in the Source Monitor: time bar drags and frame steps ask for the clip's sound while stopped",
          "[app][audio][scrub][source][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(playback != nullptr);
    REQUIRE(controller != nullptr);
    REQUIRE(source != nullptr);
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )", {{'S', 250}}};
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(source->load(mediaKey(tl.media('S'))));
    REQUIRE(source->playback() != nullptr);
    struct Ask {
        editor::PlaybackSession* session;
        Ticks at;
        int direction;
    };
    std::vector<Ask> asked;
    playback->setAudioScrubSink([&asked](editor::PlaybackSession* session, Ticks at, int direction) {
        asked.push_back({session, at, direction});
        return true;
    });

    // The time bar (or the waveform): a press, a drag on, then back - the clip's own session, at the frame shown.
    source->seekFraction(0.2);
    REQUIRE(asked.size() == 1);
    CHECK(asked.back().session == source->playback());
    CHECK(asked.back().at == source->positionTicks());
    CHECK(asked.back().direction == 1);
    source->scrubFraction(0.4);
    REQUIRE(asked.size() == 2);
    CHECK(asked.back().at == source->positionTicks());
    CHECK(asked.back().direction == 1);
    source->scrubFraction(0.3);
    REQUIRE(asked.size() == 3);
    CHECK(asked.back().direction == -1); // dragged back: played backwards
    source->scrubFraction(0.3);          // the same frame: nothing new
    CHECK(asked.size() == 3);
    // A click (seekFraction) left of the frame shown: the frame it lands on, forward - a click is not a backward drag.
    source->seekFraction(0.1);
    REQUIRE(asked.size() == 4);
    CHECK(asked.back().direction == 1);
    CHECK(asked.back().at == source->positionTicks());
    // A click on a playing Source Monitor only moves it: no scrub burst (fix round).
    source->play();
    REQUIRE(source->playback()->isPlaying());
    source->seekFraction(0.5);
    CHECK(asked.size() == 4);
    CHECK_FALSE(source->playback()->isPlaying());
    source->seekFraction(0.25); // stopped again: heard
    REQUIRE(asked.size() == 5);

    // Frame steps (the buttons, the arrow keys while it has focus): the frame stepped to, forward.
    const Ticks shown = source->positionTicks();
    source->stepFrames(-1);
    REQUIRE(asked.size() == 6);
    CHECK(asked.back().at < shown);
    CHECK(asked.back().direction == 1);
    REQUIRE(source->runCommand(QStringLiteral("transport.stepForwardFive")));
    REQUIRE(asked.size() == 7);
    CHECK(asked.back().at == source->positionTicks());
    CHECK(std::ranges::all_of(asked, [&](const Ask& a) { return a.session == source->playback(); }));
    CHECK(playback->audioScrubRequests() == 7);

    // The same switch as the program (Shift+S, the menu, Settings): off, the source is silent too.
    playback->setAudioScrubbing(false);
    source->stepFrames(1);
    source->scrubFraction(0.6);
    CHECK(asked.size() == 7);
    playback->setAudioScrubbing(true);
    playback->setAudioScrubSink({});

    checkNoMessages();
}
