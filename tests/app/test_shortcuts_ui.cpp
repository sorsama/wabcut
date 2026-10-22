// M5f: the Keyboard Shortcuts editor — the keyboard's colours, assigning a typed chord (conflicts reported first, the
// dispatcher resolves the new key at once), removing and resetting, key sets (Premiere Pro default, Wabcut, a saved
// user set), a Premiere .kys import with its unmapped commands, export, and the dialog in the shell (Ctrl+Alt+K, + and
// a typed key with a conflict, Reassign). Offscreen; no window is shown.

#include "ShellFixture.h"

#include "KeymapDispatcher.h"
#include "ShortcutsBridge.h"

#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/Scope.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>

#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace km = wabcut::keymap;

namespace {

QString kysFixture(const char* name) {
    return QDir(QStringLiteral(WABCUT_KEYMAP_TEST_DATA_DIR)).filePath(QString::fromLatin1(name));
}

std::string resolved(const KeymapDispatcher& keys, km::Scope scope, const char* chord) {
    const auto parsed = km::parseKeyChord(chord, km::Platform::Windows);
    REQUIRE(parsed);
    return keys.keymap().resolve(scope, *parsed).value_or(std::string());
}

QVariantMap row(ShortcutsBridge& s, const QString& id) {
    const int index = s.commands()->indexOf(id);
    return index >= 0 ? s.commands()->get(index) : QVariantMap();
}

/// The editor over a dispatcher whose overrides live in a temporary folder (Windows keys).
struct Fixture {
    QTemporaryDir config;
    KeymapDispatcher keys;
    ShortcutsBridge shortcuts;

    Fixture() {
        resetSettings();
        keys.setOverridesPath(config.filePath(QStringLiteral("keymap/overrides.json")));
        keys.setPlatform(QStringLiteral("windows"));
        shortcuts.setDispatcher(&keys);
    }
};

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

void click(QQuickWindow* window, QQuickItem* item) {
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::NoButton, {});
    QCoreApplication::sendEvent(window, &release);
}

void key(QQuickWindow* window, int code, Qt::KeyboardModifiers modifiers, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
    QCoreApplication::sendEvent(window, &release);
}

} // namespace

TEST_CASE("Shortcuts: the keyboard shows what each key does, the list filters by name, shortcut and panel", "[app][shortcuts]") {
    Fixture fx;
    ShortcutsBridge& s = fx.shortcuts;
    CHECK_FALSE(s.macModifiers());
    CHECK(s.keyboard().size() > 100);
    CHECK(s.modifiers().size() == 3);

    // No modifier: C is the Razor (application), Backspace only does things in panels.
    const QVariantMap plain = s.keyAssignments(0);
    const QVariantMap c = plain.value(QString::number(Qt::Key_C)).toMap();
    CHECK(c.value("kind").toString() == QStringLiteral("application"));
    CHECK(c.value("names").toStringList() == QStringList{s.commandName(QStringLiteral("tool.razor"))});
    CHECK(plain.value(QString::number(Qt::Key_Backspace)).toMap().value("kind").toString() == QStringLiteral("panel"));
    // Ctrl: Ctrl+K is Add Edit.
    const QVariantMap ctrl = s.keyAssignments(Qt::ControlModifier);
    CHECK(ctrl.value(QString::number(Qt::Key_K)).toMap().value("names").toStringList().contains(
        s.commandName(QStringLiteral("sequence.addEdit"))));
    // What a key does in every panel.
    const QVariantList backspace = s.commandsForKey(Qt::Key_Backspace, 0);
    CHECK(backspace.size() >= 3);
    bool timeline = false;
    for (const QVariant& v : backspace)
        timeline = timeline || v.toMap().value("commandId").toString() == QStringLiteral("timeline.clearSelection");
    CHECK(timeline);

    // Chords from key events.
    CHECK(s.chordText(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier) == QStringLiteral("Ctrl+Shift+K"));
    CHECK(s.chordText(Qt::Key_Control, Qt::ControlModifier).isEmpty());
    CHECK(s.chordText(Qt::Key_5, Qt::KeypadModifier).contains(QStringLiteral("5")));
    CHECK(s.displayChord(QStringLiteral("Ctrl+Shift+K")) == QStringLiteral("Ctrl+Shift+K"));

    // The list: every command, then filtered.
    const int all = s.commands()->rowCount();
    CHECK(all >= 200);
    s.setSearch(QStringLiteral("razor"));
    CHECK(s.commands()->indexOf(QStringLiteral("tool.razor")) >= 0);
    CHECK(s.commands()->rowCount() < all);
    s.setSearch(QStringLiteral("Ctrl+K")); // a shortcut finds its command
    CHECK(s.commands()->indexOf(QStringLiteral("sequence.addEdit")) >= 0);
    s.setSearch({});
    s.setScopeFilter(QStringLiteral("timeline"));
    REQUIRE(s.commands()->rowCount() > 0);
    for (int i = 0; i < s.commands()->rowCount(); ++i)
        CHECK(s.commands()->get(i).value("scope").toString() == QStringLiteral("timeline"));
    s.setScopeFilter(QStringLiteral("panels"));
    for (int i = 0; i < s.commands()->rowCount(); ++i)
        CHECK_FALSE(s.commands()->get(i).value("application").toBool());
    s.setScopeFilter({});
    CHECK(s.commands()->rowCount() == all);
    CHECK(row(s, QStringLiteral("tool.razor")).value("available").toBool());
    CHECK(row(s, QStringLiteral("edit.keyboardShortcuts")).value("available").toBool());
}

TEST_CASE("Shortcuts: assigning asks about conflicts first, then the dispatcher resolves the new key; remove, reset", "[app][shortcuts]") {
    Fixture fx;
    ShortcutsBridge& s = fx.shortcuts;
    REQUIRE(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip");

    // X belongs to Mark Clip (Application): giving it to the Razor is a conflict.
    const QVariantMap check = s.checkAssign(QStringLiteral("tool.razor"), QStringLiteral("X"));
    REQUIRE(check.value("ok").toBool());
    CHECK(check.value("chord").toString() == QStringLiteral("X"));
    const QVariantList conflicts = check.value("conflicts").toList();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].toMap().value("commandId").toString() == QStringLiteral("marker.markClip"));
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip"); // nothing changed yet

    const QVariantMap assigned = s.assign(QStringLiteral("tool.razor"), QStringLiteral("X"));
    REQUIRE(assigned.value("ok").toBool());
    CHECK(assigned.value("lost").toStringList().size() == 1);
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "tool.razor");
    CHECK(resolved(fx.keys, km::Scope::Timeline, "C") == "tool.razor"); // added, C kept
    CHECK(row(s, QStringLiteral("tool.razor")).value("overridden").toBool());
    CHECK(row(s, QStringLiteral("tool.razor")).value("shortcuts").toStringList() == QStringList{"C", "X"});
    CHECK(s.modified());
    CHECK(QFile::exists(fx.keys.overridesPath())); // saved at once

    // Already there: no change to make.
    CHECK(s.checkAssign(QStringLiteral("tool.razor"), QStringLiteral("X")).value("already").toBool());

    // An application command on a chord panels use: it says which one wins where.
    const QVariantMap shadow = s.checkAssign(QStringLiteral("marker.add"), QStringLiteral("Backspace"));
    REQUIRE(shadow.value("ok").toBool());
    CHECK(shadow.value("conflicts").toList().isEmpty());
    CHECK(shadow.value("shadows").toList().size() >= 3);
    CHECK(shadow.value("shadows").toList()[0].toMap().value("message").toString().contains(QStringLiteral("has focus")));

    // Remove one key, then reset the command.
    REQUIRE(s.unassign(QStringLiteral("tool.razor"), QStringLiteral("C")).isEmpty());
    CHECK(resolved(fx.keys, km::Scope::Timeline, "C").empty());
    REQUIRE(s.resetCommand(QStringLiteral("tool.razor")));
    CHECK(resolved(fx.keys, km::Scope::Timeline, "C") == "tool.razor");
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X").empty()); // Mark Clip lost it; only a reset gives it back
    s.resetAll();
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip");
    CHECK_FALSE(s.modified());
    CHECK_FALSE(QFile::exists(fx.keys.overridesPath()));

    // Bad input
    CHECK_FALSE(s.checkAssign(QStringLiteral("tool.razor"), QStringLiteral("Cmd+X")).value("ok").toBool());
    CHECK_FALSE(s.assign(QStringLiteral("no.such"), QStringLiteral("X")).value("ok").toBool());
}

TEST_CASE("Shortcuts: key sets (Wabcut, Premiere Pro default, a saved user set); switching changes the keys", "[app][shortcuts]") {
    Fixture fx;
    ShortcutsBridge& s = fx.shortcuts;
    CHECK(s.preset() == QStringLiteral("premiere"));
    CHECK(s.presetName() == QStringLiteral("Premiere Pro (default)"));
    REQUIRE(resolved(fx.keys, km::Scope::Timeline, "Backspace") == "timeline.clearSelection");
    REQUIRE(resolved(fx.keys, km::Scope::Application, "Ctrl+Y").empty());

    REQUIRE(s.applyPreset(QStringLiteral("wabcut")).isEmpty());
    CHECK(s.preset() == QStringLiteral("wabcut"));
    CHECK_FALSE(s.modified());
    CHECK(resolved(fx.keys, km::Scope::Timeline, "Backspace") == "timeline.rippleDelete");
    CHECK(resolved(fx.keys, km::Scope::Timeline, "Alt+Backspace") == "timeline.rippleDelete");
    CHECK(resolved(fx.keys, km::Scope::Application, "Ctrl+Y") == "edit.redo");
    CHECK(resolved(fx.keys, km::Scope::Application, "Ctrl+Shift+Z") == "edit.redo");
    CHECK(QSettings().value(QStringLiteral("shortcuts/preset")).toString() == QStringLiteral("wabcut"));

    // A change on top makes it "modified"; saving makes a user set.
    REQUIRE(s.assign(QStringLiteral("tool.razor"), QStringLiteral("X")).value("ok").toBool());
    CHECK(s.modified());
    REQUIRE(s.savePreset(QStringLiteral("Mine")).isEmpty());
    CHECK(s.preset() == QStringLiteral("user:Mine"));
    CHECK_FALSE(s.modified());
    bool listed = false;
    for (const QVariant& p : s.presets())
        listed = listed || p.toMap().value("id").toString() == QStringLiteral("user:Mine");
    CHECK(listed);
    CHECK_FALSE(s.savePreset(QStringLiteral("a/b")).isEmpty());
    CHECK_FALSE(s.savePreset(QStringLiteral("Wabcut")).isEmpty());

    // Back to Premiere's defaults, then to the user set again.
    REQUIRE(s.applyPreset(QStringLiteral("premiere")).isEmpty());
    CHECK(resolved(fx.keys, km::Scope::Timeline, "Backspace") == "timeline.clearSelection");
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip");
    CHECK_FALSE(fx.keys.keymap().hasOverrides());
    REQUIRE(s.applyPreset(QStringLiteral("user:Mine")).isEmpty());
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "tool.razor");
    CHECK(resolved(fx.keys, km::Scope::Timeline, "Backspace") == "timeline.rippleDelete");

    REQUIRE(s.deletePreset(QStringLiteral("user:Mine")).isEmpty());
    CHECK(s.preset() == QStringLiteral("premiere"));
    CHECK(s.modified()); // the keys stayed; they now differ from Premiere's
    CHECK_FALSE(s.deletePreset(QStringLiteral("wabcut")).isEmpty());
    CHECK_FALSE(s.applyPreset(QStringLiteral("nope")).isEmpty());
}

TEST_CASE("Shortcuts: a Premiere .kys imports with its unmapped commands listed; overrides export and import as .json", "[app][shortcuts]") {
    Fixture fx;
    ShortcutsBridge& s = fx.shortcuts;
    const QUrl kys = QUrl::fromLocalFile(kysFixture("windows-custom.kys"));

    const QVariantMap preview = s.previewKeySet(kys);
    REQUIRE(preview.value("ok").toBool());
    CHECK(preview.value("kind").toString() == QStringLiteral("kys"));
    CHECK(preview.value("changed").toInt() > 0);
    CHECK_FALSE(preview.value("unmapped").toStringList().isEmpty());
    CHECK(preview.value("unmapped").toStringList().join(' ').contains(QStringLiteral("cmd.multicam.transmit.gridview")));
    CHECK_FALSE(preview.value("warnings").toStringList().isEmpty());
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip"); // a preview changes nothing

    const QVariantMap imported = s.importKeySet(kys);
    REQUIRE(imported.value("ok").toBool());
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "tool.razor"); // the .kys rebinds the Razor to X
    CHECK(s.modified());

    // Export, reset, import the .json: the same keys again.
    const QString json = fx.config.filePath(QStringLiteral("mine.json"));
    REQUIRE(s.exportOverrides(QUrl::fromLocalFile(json)).isEmpty());
    QFile exported(json);
    REQUIRE(exported.open(QIODevice::ReadOnly));
    CHECK(exported.readAll().contains("tool.razor"));
    exported.close();
    s.resetAll();
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip");
    const QVariantMap back = s.importKeySet(QUrl::fromLocalFile(json));
    REQUIRE(back.value("ok").toBool());
    CHECK(back.value("kind").toString() == QStringLiteral("json"));
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "tool.razor");

    // P.2: Export… to a .kys saves the whole key set as Premiere's; importing it gives the same keys.
    const QString saved = fx.config.filePath(QStringLiteral("mine.kys"));
    REQUIRE(s.exportOverrides(QUrl::fromLocalFile(saved)).isEmpty());
    QFile set(saved);
    REQUIRE(set.open(QIODevice::ReadOnly));
    CHECK(set.readAll().contains("<PremiereData Version=\"3\">"));
    set.close();
    s.resetAll();
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "marker.markClip");
    const QVariantMap fromKys = s.importKeySet(QUrl::fromLocalFile(saved));
    REQUIRE(fromKys.value("ok").toBool());
    CHECK(fromKys.value("kind").toString() == QStringLiteral("kys"));
    CHECK(resolved(fx.keys, km::Scope::Timeline, "X") == "tool.razor");

    // A macOS key set does not fit Windows keys; a missing file is an error.
    CHECK_FALSE(s.importKeySet(QUrl::fromLocalFile(kysFixture("macos-custom.kys"))).value("ok").toBool());
    CHECK_FALSE(s.previewKeySet(QUrl::fromLocalFile(fx.config.filePath(QStringLiteral("none.kys")))).value("ok").toBool());
}

TEST_CASE("Keyboard Shortcuts dialog in the shell: Ctrl+Alt+K, + and a typed key, a conflict to confirm; screenshot",
          "[app][shortcuts][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    auto* shortcuts = shell.engine.singletonInstance<ShortcutsBridge*>("Wabcut", "Shortcuts");
    REQUIRE(keys != nullptr);
    REQUIRE(shortcuts != nullptr);
    QTemporaryDir config;
    keys->setOverridesPath(config.filePath(QStringLiteral("keymap/overrides.json")));
    keys->setPlatform(QStringLiteral("windows"));
    QObject* dialog = shell.object("keyboardShortcutsDialog");
    REQUIRE(dialog != nullptr);

    // Ctrl+Alt+K (Premiere's chord) opens it.
    QQuickItem* lanes = shell.item("timelineLanesArea");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus();
    key(shell.window.get(), Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
    spin(200);
    REQUIRE(dialog->property("opened").toBool());

    // Find the Razor, press + and type X: X belongs to Mark Clip, so it asks.
    QObject* list = shell.object("shortcutsCommandList");
    REQUIRE(list != nullptr);
    shortcuts->setSearch(QStringLiteral("razor"));
    spin(100);
    QQuickItem* razorRow = findItem(shell.window->contentItem(), [](QQuickItem* i) {
        return i->objectName() == QStringLiteral("shortcutRow_tool.razor") && i->isVisible();
    });
    REQUIRE(razorRow != nullptr);
    QQuickItem* add = findItem(razorRow, [](QQuickItem* i) { return i->objectName() == QStringLiteral("shortcutAdd"); });
    REQUIRE(add != nullptr);
    click(shell.window.get(), add);
    spin(100);
    CHECK(dialog->property("captureFor").toString() == QStringLiteral("tool.razor"));
    QQuickItem* capture = findItem(razorRow, [](QQuickItem* i) { return i->objectName() == QStringLiteral("shortcutCapture"); });
    REQUIRE(capture != nullptr);
    CHECK(capture->hasActiveFocus());
    key(shell.window.get(), Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    spin(100);
    CHECK(dialog->property("captureFor").toString().isEmpty());
    QQuickItem* conflict = shell.item("shortcutsConflict");
    REQUIRE(conflict != nullptr);
    CHECK(conflict->isVisible());
    CHECK(shell.item("shortcutsConflictText")->property("text").toString().contains(QStringLiteral("Mark Clip")));
    CHECK(resolved(*keys, km::Scope::Timeline, "X") == "marker.markClip"); // not yet

    // Show the X key's commands on the keyboard, then take the screenshot with the question open.
    const QString xName = QStringLiteral("key_%1").arg(Qt::Key_X);
    QQuickItem* xKey = findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == xName; });
    REQUIRE(xKey != nullptr);
    click(shell.window.get(), xKey);
    spin(100);
    CHECK(shell.item("shortcutsKeyChord")->property("text").toString() == QStringLiteral("X"));
    {
        // The pointer leaves the keyboard (no tooltip over the keys in the picture).
        const QPointF away(shell.window->width() / 2.0, shell.window->height() - 4.0);
        QMouseEvent move(QEvent::MouseMove, away, away, Qt::NoButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(shell.window.get(), &move);
        spin(50);
    }
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/shortcuts.png")));
    }

    // Reassign: the dispatcher runs the Razor on X now.
    QQuickItem* reassign = shell.item("shortcutsReassign");
    REQUIRE(reassign != nullptr);
    click(shell.window.get(), reassign);
    spin(100);
    CHECK(resolved(*keys, km::Scope::Timeline, "X") == "tool.razor");
    CHECK(shell.item("shortcutsMessageText")->property("text").toString().contains(QStringLiteral("Razor")));

    // The Wabcut key set from the menu's model, then Esc closes the dialog.
    REQUIRE(shortcuts->applyPreset(QStringLiteral("wabcut")).isEmpty());
    spin(50);
    CHECK(shell.item("shortcutsKeySet")->property("text").toString() == QStringLiteral("Wabcut"));
    key(shell.window.get(), Qt::Key_Escape, Qt::NoModifier);
    spin(200);
    CHECK_FALSE(dialog->property("opened").toBool());
    shortcuts->resetAll();

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
