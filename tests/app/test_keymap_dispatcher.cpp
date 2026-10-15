// KeymapDispatcher bridge tests: a hidden QQuickWindow (tests/app/KeymapScene.qml) receives synthetic key
// presses and releases; the tests check the commands and transport intents the bridge emits. The window is
// never shown. QGuiApplication comes from the suite's listener in test_ui_shell.cpp.

#include "bridge/KeymapDispatcher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

using wabcut::app::KeymapDispatcher;
using Kind = KeymapDispatcher::TransportKind;

namespace {

struct Intent {
    Kind kind = Kind::Stop;
    int num = 0;
    int den = 1;
    int frames = 0;
    bool operator==(const Intent&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Intent& i) {
    return os << "{kind " << static_cast<int>(i.kind) << ", " << i.num << "/" << i.den << ", frames "
              << i.frames << "}";
}

using Intents = std::vector<Intent>;

Intent shuttle(int speed) {
    return {Kind::ShuttleStep, speed, 1, 0};
}
Intent slow(int dir) {
    return {Kind::SetSpeed, dir, 4, 0};
}
Intent step(int frames) {
    return {Kind::StepFrames, 0, 1, frames};
}
const Intent kStop{Kind::Stop, 0, 1, 0};

void spin(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

/// KeymapScene.qml in a hidden window, with a KeymapDispatcher attached and its outputs recorded.
struct Scene {
    QTemporaryDir configDir;
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;
    QStringList errors;
    QStringList commands;
    Intents intents;
    std::unique_ptr<KeymapDispatcher> dispatcher; // declared last: detaches from the window first

    explicit Scene(const QString& platform = QStringLiteral("windows")) {
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(WABCUT_APP_TEST_DIR "/KeymapScene.qml")));
        QObject* object = component.create();
        for (const QQmlError& e : component.errors()) {
            errors << e.toString();
        }
        window.reset(qobject_cast<QQuickWindow*>(object));
        if (!window) {
            delete object;
            return;
        }
        window->contentItem()->setSize(QSizeF(800, 600));

        dispatcher = std::make_unique<KeymapDispatcher>();
        dispatcher->setOverridesPath(configDir.filePath(QStringLiteral("keymap/overrides.json")));
        dispatcher->setPlatform(platform);
        dispatcher->setWindow(window.get());
        QObject::connect(dispatcher.get(), &KeymapDispatcher::commandTriggered,
                         [this](const QString& id) { commands << id; });
        QObject::connect(
            dispatcher.get(), &KeymapDispatcher::transportIntent,
            [this](Kind kind, int num, int den, int frames) { intents.push_back({kind, num, den, frames}); });
    }

    QQuickItem* item(const char* name) const { return window->findChild<QQuickItem*>(QLatin1String(name)); }

    void focus(const char* name) {
        QQuickItem* target = item(name);
        REQUIRE(target != nullptr);
        target->forceActiveFocus();
        REQUIRE(window->activeFocusItem() == target);
    }

    /// Sends a key press to the window; returns whether the event ended up accepted.
    bool press(int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {},
               bool autoRepeat = false) {
        QKeyEvent event(QEvent::KeyPress, key, mods, text, autoRepeat);
        QCoreApplication::sendEvent(window.get(), &event);
        return event.isAccepted();
    }

    bool release(int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {},
                 bool autoRepeat = false) {
        QKeyEvent event(QEvent::KeyRelease, key, mods, text, autoRepeat);
        QCoreApplication::sendEvent(window.get(), &event);
        return event.isAccepted();
    }

    /// Press then release.
    void tap(int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
        press(key, mods, text);
        release(key, mods, text);
    }

    QStringList takeCommands() {
        QStringList out;
        out.swap(commands);
        return out;
    }

    Intents takeIntents() {
        Intents out;
        out.swap(intents);
        return out;
    }
};

std::string str(const QStringList& list) {
    return list.join(QStringLiteral(", ")).toStdString();
}

} // namespace

TEST_CASE("KeymapDispatcher resolves the focused item's nearest KeymapScope", "[app][keymap]") {
    Scene scene;
    INFO(str(scene.errors));
    REQUIRE(scene.window != nullptr);

    CHECK(scene.dispatcher->scopeAt(scene.item("timeline")) == QStringLiteral("timeline"));
    CHECK(scene.dispatcher->scopeAt(scene.item("timelineClip")) == QStringLiteral("timeline")); // inherited
    CHECK(scene.dispatcher->scopeAt(scene.item("programMonitor")) == QStringLiteral("programMonitor"));
    CHECK(scene.dispatcher->scopeAt(scene.item("plain")) == QStringLiteral("application"));
    CHECK(scene.dispatcher->scopeAt(nullptr) == QStringLiteral("application"));

    // Ripple Delete: Alt+Backspace only in the Timeline panel, Shift+Delete everywhere (Edit menu).
    scene.focus("timelineClip");
    CHECK(scene.press(Qt::Key_Backspace, Qt::AltModifier));
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("timeline.rippleDelete")});
    scene.tap(Qt::Key_Delete, Qt::ShiftModifier);
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("edit.rippleDelete")});
    scene.focus("plain");
    CHECK_FALSE(scene.press(Qt::Key_Backspace, Qt::AltModifier)); // not bound outside the Timeline
    CHECK(scene.takeCommands().isEmpty());
    scene.tap(Qt::Key_Delete, Qt::ShiftModifier);
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("edit.rippleDelete")});

    // Ctrl+R: Show Rulers in the Program Monitor, Speed/Duration elsewhere.
    scene.focus("programMonitor");
    scene.tap(Qt::Key_R, Qt::ControlModifier);
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("programMonitor.showRulers")});
    scene.focus("timeline");
    scene.tap(Qt::Key_R, Qt::ControlModifier);
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("clip.speedDuration")});
}

TEST_CASE("Text fields keep their keys; Ctrl shortcuts they do not claim still work", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    QQuickItem* field = scene.item("textField");
    scene.focus("textField");

    // Typing: J, K, L and Space go into the field, not to the transport.
    scene.tap(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    scene.tap(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    scene.tap(Qt::Key_K, Qt::ShiftModifier, QStringLiteral("K"));
    CHECK(field->property("text").toString() == QStringLiteral("j K"));
    CHECK(scene.takeIntents().empty());
    CHECK(scene.takeCommands().isEmpty());

    // Navigation and editing keys stay in the field (NumberField steps with Up/Down), Enter does not render,
    // Alt+Backspace does not ripple delete, and Ctrl+Z is the field's own undo (it claims it).
    scene.tap(Qt::Key_Up);
    scene.tap(Qt::Key_Left);
    scene.tap(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    scene.tap(Qt::Key_Backspace, Qt::AltModifier);
    scene.tap(Qt::Key_Z, Qt::ControlModifier);
    CHECK(scene.takeCommands().isEmpty());

    // Shortcuts the field does not claim still reach the editor.
    scene.tap(Qt::Key_S, Qt::ControlModifier);
    scene.tap(Qt::Key_M, Qt::ControlModifier);
    CHECK(scene.takeCommands() ==
          (QStringList{QStringLiteral("file.save"), QStringLiteral("file.exportMedia")}));
}

TEST_CASE("Items claim keys through ShortcutOverride or KeymapScope.suppressShortcuts", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);

    scene.focus("claimsJ");
    CHECK_FALSE(scene.press(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"))); // the item keeps J
    scene.release(Qt::Key_J);
    CHECK(scene.takeIntents().empty());
    scene.tap(Qt::Key_K, Qt::NoModifier, QStringLiteral("k")); // but not K
    CHECK(scene.takeIntents() == Intents{kStop});

    scene.focus("optedOut");
    scene.tap(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    scene.tap(Qt::Key_S, Qt::ControlModifier);
    CHECK(scene.takeIntents().empty());
    CHECK(scene.takeCommands().isEmpty());
}

TEST_CASE("Popups and dialogs swallow editor shortcuts", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    QObject* popup = scene.window->findChild<QObject*>(QStringLiteral("popup"));
    REQUIRE(popup != nullptr);

    REQUIRE(QMetaObject::invokeMethod(popup, "open"));
    QQuickItem* content = scene.item("popupContent");
    REQUIRE(content != nullptr);
    content->forceActiveFocus();
    REQUIRE(scene.window->activeFocusItem() == content);
    scene.tap(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    scene.tap(Qt::Key_S, Qt::ControlModifier);
    CHECK(scene.takeIntents().empty());
    CHECK(scene.takeCommands().isEmpty());

    REQUIRE(QMetaObject::invokeMethod(popup, "close"));
    scene.focus("plain");
    scene.tap(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    CHECK(scene.takeIntents() == Intents{kStop});
}

TEST_CASE("IME composition swallows editor shortcuts", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    QQuickItem* field = scene.item("textField");
    scene.focus("textField");

    QInputMethodEvent composing(QStringLiteral("か"), {}); // preedit "か"
    QCoreApplication::sendEvent(field, &composing);
    REQUIRE_FALSE(field->property("preeditText").toString().isEmpty());
    scene.tap(Qt::Key_S, Qt::ControlModifier);
    CHECK(scene.takeCommands().isEmpty());

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("か"));
    QCoreApplication::sendEvent(field, &commit);
    REQUIRE(field->property("preeditText").toString().isEmpty());
    scene.tap(Qt::Key_S, Qt::ControlModifier);
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("file.save")});
}

TEST_CASE("J/K/L press and release traces; auto-repeat never climbs the ladder", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    scene.focus("timeline");

    CHECK(scene.press(Qt::Key_L, Qt::NoModifier, QStringLiteral("l")));
    CHECK(scene.press(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"), true)); // OS repeat: consumed, no step
    CHECK(scene.release(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"), true)); // synthetic repeat release
    CHECK(scene.release(Qt::Key_L, Qt::NoModifier, QStringLiteral("l")));
    CHECK(scene.takeIntents() == Intents{shuttle(1)});
    scene.tap(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    CHECK(scene.takeIntents() == Intents{shuttle(2)});

    // Hold K: tap J steps a frame, hold J slow-plays backward, releasing J stops.
    CHECK(scene.press(Qt::Key_K, Qt::NoModifier, QStringLiteral("k")));
    CHECK(scene.dispatcher->isGestureActive());
    scene.press(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    scene.release(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    scene.press(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    scene.press(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"), true);
    scene.press(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"), true);
    scene.release(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    CHECK(scene.release(Qt::Key_K, Qt::NoModifier, QStringLiteral("k")));
    CHECK_FALSE(scene.dispatcher->isGestureActive());
    CHECK(scene.takeIntents() == (Intents{kStop, step(-1), step(-1), slow(-1), kStop}));
    CHECK(scene.takeCommands().isEmpty());
}

TEST_CASE("A K+L hold is recognised without key repeat", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    scene.focus("programMonitor");
    scene.press(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    scene.press(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    CHECK(scene.takeIntents() == (Intents{kStop, step(1)}));
    QElapsedTimer timer;
    timer.start();
    while (scene.intents.empty() && timer.elapsed() < 3000) {
        spin(20);
    }
    CHECK(scene.takeIntents() == Intents{slow(1)});
    scene.release(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    scene.release(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    CHECK(scene.takeIntents() == Intents{kStop});
}

TEST_CASE("Window deactivation resets held gestures", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    scene.focus("timeline");
    scene.press(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    scene.press(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    scene.press(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"), true);
    CHECK(scene.takeIntents() == (Intents{kStop, step(1), slow(1)}));

    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(scene.window.get(), &deactivate);
    CHECK(scene.takeIntents() == Intents{kStop});
    CHECK_FALSE(scene.dispatcher->isGestureActive());
    // The releases arrive after reactivation: nothing is latched and nothing more is emitted.
    scene.release(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    scene.release(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    CHECK(scene.takeIntents().empty());
    scene.tap(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    CHECK(scene.takeIntents() == Intents{shuttle(-1)});
}

TEST_CASE("Keypad Enter/Delete and AltGr are handled explicitly", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    scene.focus("timeline");

    scene.tap(Qt::Key_Enter, Qt::KeypadModifier, QStringLiteral("\r"));
    scene.tap(Qt::Key_Delete, Qt::KeypadModifier);
    CHECK(scene.takeCommands() ==
          (QStringList{QStringLiteral("sequence.renderInToOut"), QStringLiteral("edit.clear")}));
    // Keypad digits and operators are for timecode entry, not the main-row shortcuts.
    scene.tap(Qt::Key_Period, Qt::KeypadModifier, QStringLiteral("."));
    scene.tap(Qt::Key_Plus, Qt::KeypadModifier, QStringLiteral("+"));
    CHECK(scene.takeCommands().isEmpty());

    // AltGr+E types "€" on many layouts and arrives as Ctrl+Alt+E: typing, not New Layer > Ellipse.
    CHECK_FALSE(scene.press(Qt::Key_E, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("€")));
    scene.release(Qt::Key_E, Qt::ControlModifier | Qt::AltModifier);
    CHECK(scene.takeCommands().isEmpty());
    scene.tap(Qt::Key_E, Qt::ControlModifier | Qt::AltModifier); // a real Ctrl+Alt+E carries no text
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("graphics.newEllipseLayer")});
}

TEST_CASE("macOS has no AltGr: Option shortcuts that type characters still fire", "[app][keymap]") {
    Scene scene(QStringLiteral("macos"));
    REQUIRE(scene.window != nullptr);
    scene.focus("timeline");
    // Opt+Cmd+E (Qt: Alt+Control) is New Layer > Ellipse even if the event carries a character.
    scene.tap(Qt::Key_E, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("´"));
    // Opt+I is Clear In.
    scene.tap(Qt::Key_I, Qt::AltModifier, QStringLiteral("ˆ"));
    CHECK(scene.takeCommands() ==
          (QStringList{QStringLiteral("graphics.newEllipseLayer"), QStringLiteral("marker.clearIn")}));
}

TEST_CASE("shortcutText gives native tooltip text per OS", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    KeymapDispatcher& d = *scene.dispatcher;
    CHECK(d.shortcutText(QStringLiteral("edit.undo")) == QStringLiteral("Ctrl+Z"));
    CHECK(d.shortcutText(QStringLiteral("marker.clearIn")) == QStringLiteral("Ctrl+Shift+I"));
    CHECK(d.shortcutText(QStringLiteral("timeline.rippleDelete")) == QStringLiteral("Alt+Backspace"));
    CHECK(d.shortcutText(QStringLiteral("project.listView")) == QStringLiteral("Ctrl+Page Up"));
    CHECK(d.shortcutText(QStringLiteral("sequence.trimEdit")) == QStringLiteral("Shift+T"));
    CHECK(d.shortcutText(QStringLiteral("no.such")).isEmpty());

    const int revision = d.revision();
    d.setPlatform(QStringLiteral("macos"));
    CHECK(d.revision() > revision);
    CHECK(d.shortcutText(QStringLiteral("edit.undo")) == QStringLiteral("⌘Z"));                   // ⌘Z
    CHECK(d.shortcutText(QStringLiteral("marker.clearIn")) == QStringLiteral("⌥I"));              // ⌥I
    CHECK(d.shortcutText(QStringLiteral("timeline.rippleDelete")) == QStringLiteral("⌥⌫"));       // ⌥⌫
    CHECK(d.shortcutText(QStringLiteral("edit.rippleDelete")) == QStringLiteral("⇧⌦"));           // ⇧⌦
    CHECK(d.shortcutText(QStringLiteral("audioMixer.meterInputsOnly")) == QStringLiteral("⌃⇧I")); // ⌃⇧I
    CHECK(d.shortcutText(QStringLiteral("sequence.trimEdit")) == QStringLiteral("⇧T"));           // ⇧T
    CHECK(d.shortcutTexts(QStringLiteral("graphics.newTextLayer")) == QStringList{QStringLiteral("⌘T")});

    // The QML singleton serves the running OS's table to tooltips.
    KeymapDispatcher current;
    CHECK(scene.window->property("undoShortcut").toString() ==
          current.shortcutText(QStringLiteral("edit.undo")));
}

TEST_CASE("Shortcut edits are persisted and reloaded", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    KeymapDispatcher& d = *scene.dispatcher;
    const QString path = d.overridesPath();
    CHECK_FALSE(QFile::exists(path));

    const QVariantMap result = d.assignShortcut(QStringLiteral("tool.razor"), QStringLiteral("X"));
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("lost")).toStringList() ==
          QStringList{QStringLiteral("marker.markClip")});
    CHECK(QFile::exists(path));
    scene.focus("plain");
    scene.tap(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("tool.razor")});

    const QVariantMap bad = d.assignShortcut(QStringLiteral("tool.razor"), QStringLiteral("Cmd+X"));
    CHECK_FALSE(bad.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(bad.value(QStringLiteral("error")).toString().isEmpty());

    {
        KeymapDispatcher reloaded;
        reloaded.setOverridesPath(path);
        reloaded.setPlatform(QStringLiteral("windows"));
        CHECK(reloaded.shortcutTexts(QStringLiteral("tool.razor")) ==
              (QStringList{QStringLiteral("C"), QStringLiteral("X")}));
        CHECK(reloaded.shortcutText(QStringLiteral("marker.markClip")).isEmpty());
    }

    CHECK(d.resetShortcut(QStringLiteral("tool.razor")));
    CHECK(d.resetShortcut(QStringLiteral("marker.markClip")));
    CHECK_FALSE(QFile::exists(path)); // no overrides left: the file is removed
    CHECK(d.shortcutText(QStringLiteral("marker.markClip")) == QStringLiteral("X"));
}

TEST_CASE("Premiere key sets import through the bridge", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    KeymapDispatcher& d = *scene.dispatcher;
    const QString error = d.importPremiereKeySet(
        QUrl::fromLocalFile(QStringLiteral(WABCUT_APP_TEST_DIR "/../keymap/data/windows-custom.kys")));
    CHECK(error.isEmpty());
    CHECK(d.shortcutText(QStringLiteral("tool.razor")) == QStringLiteral("X"));
    CHECK(QFile::exists(d.overridesPath()));
    CHECK_FALSE(d.importPremiereKeySet(QUrl::fromLocalFile(QStringLiteral(
                                           WABCUT_APP_TEST_DIR "/../keymap/data/macos-custom.kys")))
                    .isEmpty()); // a macOS key set does not apply to a Windows keymap
}

TEST_CASE("Menus and buttons trigger commands through the bridge", "[app][keymap]") {
    Scene scene;
    REQUIRE(scene.window != nullptr);
    KeymapDispatcher& d = *scene.dispatcher;
    CHECK(d.trigger(QStringLiteral("edit.undo")));
    CHECK(scene.takeCommands() == QStringList{QStringLiteral("edit.undo")});
    CHECK(d.trigger(QStringLiteral("transport.shuttleForward")));
    d.syncTransportSpeed(2, 1); // e.g. the transport bar changed the speed
    CHECK(d.trigger(QStringLiteral("transport.shuttleForward")));
    CHECK(d.trigger(QStringLiteral("transport.shuttleStop")));
    CHECK(scene.takeIntents() == (Intents{shuttle(1), shuttle(4), kStop}));
    CHECK_FALSE(d.trigger(QStringLiteral("no.such")));
}
