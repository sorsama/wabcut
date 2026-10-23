// M5j onboarding, offscreen:
//   * the Keyboard Trainer: every prompt is a known command with Premiere keys; scoring, streaks and hints against a
//     custom keymap (keys resolve from the active keymap, so edited or imported keys train themselves);
//   * the first-run welcome: after the launch Home screen, skippable (Esc) and not shown again; completed (a .kys
//     import, the workspace and the accent), then the three-step tour (Library → Timeline → Export);
//   * Help and the command palette run it all again; so do the rows of Home's Learn page (Home closes first);
//   * the trainer in the window: keys answer the prompt and never reach the editor.
// Screenshots: the welcome, the tour and the trainer.

#include "ShellFixture.h"

#include "CommandPalette.h"
#include "HomeBridge.h"
#include "KeyboardTrainerBridge.h"
#include "KeymapDispatcher.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/keymap/KeymapManager.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <functional>
#include <map>
#include <optional>
#include <set>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace km = wabcut::keymap;

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

QQuickItem* named(const Shell& shell, const char* name) {
    return findNamed(shell.window->contentItem(), QLatin1String(name));
}

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

void tapChord(QQuickWindow* window, km::KeyChord chord) {
    tap(window, chord.key, Qt::KeyboardModifiers::fromInt(static_cast<int>(chord.mods.bits())));
}

bool waitUntil(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms)
        spin(20);
    return done();
}

bool isVisible(QObject* object) {
    return object && object->property("visible").toBool();
}

void click(QObject* button) {
    REQUIRE(button);
    QMetaObject::invokeMethod(button, "clicked");
}

QVariant call(QObject* object, const char* method, const QVariant& arg = {}) {
    QVariant result;
    if (arg.isValid())
        QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, arg));
    else
        QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result));
    return result;
}

QString focusName(const Shell& shell) {
    QQuickItem* f = shell.window->activeFocusItem();
    return f ? f->objectName() : QString();
}

/// Focus sits on something the user can see (M5j: focus is never lost).
bool focusVisible(const Shell& shell) {
    QQuickItem* f = shell.window->activeFocusItem();
    return f && f != shell.window->contentItem() && f->isVisible();
}

QString displayName(std::string_view id) {
    const km::CommandInfo* info = km::CommandRegistry::builtin().find(id);
    return info ? QString::fromUtf8(info->displayName.data(), static_cast<qsizetype>(info->displayName.size()))
                : QString();
}

QRectF sceneRect(QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void save(const QImage& image, const QString& name) {
    image.save(QDir(outputDir()).filePath(name));
}

QString kysFixture() {
    return QStringLiteral(WABCUT_KEYMAP_TEST_DATA_DIR) +
           (km::usesMacModifiers(km::currentPlatform()) ? QStringLiteral("/macos-custom.kys")
                                                        : QStringLiteral("/windows-custom.kys"));
}

} // namespace

TEST_CASE("Keyboard Trainer: every prompt is a known command with a Premiere key", "[app][onboarding]") {
    std::set<std::string> seen;
    std::map<std::string, int> perCategory;
    for (const auto& prompt : KeyboardTrainerBridge::allPrompts()) {
        INFO(prompt.command);
        CHECK(seen.insert(prompt.command).second); // listed once
        REQUIRE(km::CommandRegistry::builtin().find(prompt.command) != nullptr);
        ++perCategory[prompt.category];
    }
    CHECK(perCategory.size() == 8); // M5n: clipboard, trim and multicam joined editing, marks, playback, tools, timeline
    for (const km::Platform platform : {km::Platform::Windows, km::Platform::MacOS}) {
        auto keys = km::KeymapManager::withDefaults(platform);
        REQUIRE(keys.has_value());
        QStringList unbound;
        for (const auto& prompt : KeyboardTrainerBridge::allPrompts()) {
            if (keys->chordsFor(prompt.command).empty())
                unbound << QString::fromStdString(prompt.command);
        }
        INFO("platform " << static_cast<int>(platform) << ", without a key: " << join(unbound));
        CHECK(unbound.isEmpty());
    }
    for (const auto& [category, count] : perCategory) {
        INFO(category);
        CHECK(count >= 8);
    }
}

TEST_CASE("Keyboard Trainer: custom navigation bindings never become impossible prompts", "[app][onboarding]") {
    km::KeymapManager keys(km::Platform::Windows);
    REQUIRE(keys.setChords("sequence.addEdit", {km::KeyChord(km::Key_Escape)}));
    REQUIRE(keys.setChords("edit.copy", {km::KeyChord(km::Key_Tab)}));
    REQUIRE(keys.setChords("edit.paste", {km::KeyChord(km::Key_Backtab, km::Modifier::Shift)}));
    KeyboardTrainerBridge trainer;
    trainer.setKeymap(&keys);
    CHECK_FALSE(trainer.start(QStringLiteral("editing")));
    REQUIRE(keys.setChords("sequence.addEdit", {km::KeyChord(km::Key_Escape),
                                               km::KeyChord(km::Key_K, km::Modifier::Control)}));
    REQUIRE(trainer.start(QStringLiteral("editing")));
    CHECK(trainer.roundLength() == 1);
    trainer.showHint();
    CHECK(trainer.hint() == QStringLiteral("Ctrl+K"));
    CHECK(trainer.press(Qt::Key_K, Qt::ControlModifier) == QStringLiteral("correct"));
}

TEST_CASE("Keyboard Trainer: scoring, streaks and hints follow a custom keymap", "[app][onboarding]") {
    // A key set with three keys, two of them not Premiere's: Ripple Delete on Ctrl+X, the Razor on B.
    km::KeymapManager keys(km::Platform::Windows);
    REQUIRE(keys.setChords("edit.rippleDelete", {km::KeyChord(km::Key_X, km::Modifier::Control)}).has_value());
    REQUIRE(keys.setChords("tool.razor", {km::KeyChord(km::Key_B)}).has_value());
    REQUIRE(keys.setChords("marker.markIn", {km::KeyChord(km::Key_I)}).has_value());
    const std::map<QString, km::KeyChord> answer{
        {QStringLiteral("edit.rippleDelete"), km::KeyChord(km::Key_X, km::Modifier::Control)},
        {QStringLiteral("tool.razor"), km::KeyChord(km::Key_B)},
        {QStringLiteral("marker.markIn"), km::KeyChord(km::Key_I)},
    };

    KeyboardTrainerBridge trainer;
    trainer.setKeymap(&keys);
    trainer.setSeed(7);
    int sparks = 0;
    QObject::connect(&trainer, &KeyboardTrainerBridge::answeredCorrectly, [&sparks] { ++sparks; });

    // Categories count only prompts with a key.
    std::map<QString, int> counts;
    for (const QVariant& c : trainer.categories())
        counts[c.toMap().value(QStringLiteral("key")).toString()] = c.toMap().value(QStringLiteral("count")).toInt();
    CHECK(counts[QStringLiteral("all")] == 3);
    CHECK(counts[QStringLiteral("editing")] == 1);
    CHECK(counts[QStringLiteral("tools")] == 1);
    CHECK(counts[QStringLiteral("marks")] == 1);
    CHECK(counts[QStringLiteral("playback")] == 0);
    CHECK_FALSE(trainer.start(QStringLiteral("playback")));
    CHECK(trainer.state() == QStringLiteral("idle"));

    REQUIRE(trainer.start(QStringLiteral("all")));
    CHECK(trainer.state() == QStringLiteral("asking"));
    CHECK(trainer.round() == 1);
    CHECK(trainer.roundLength() == 3);
    CHECK(trainer.hint().isEmpty());
    CHECK(trainer.press(Qt::Key_Shift, Qt::ShiftModifier) == QStringLiteral("ignored")); // a bare modifier

    // Prompt 1: an unbound key, then another prompt's key (named), then the right one.
    const QString first = trainer.promptCommand();
    CHECK(trainer.prompt() == displayName(first.toStdString()));
    CHECK(trainer.press(Qt::Key_F7, 0) == QStringLiteral("wrong"));
    CHECK(trainer.state() == QStringLiteral("wrong"));
    CHECK(trainer.lastPressed() == QStringLiteral("F7"));
    CHECK(trainer.lastPressedAction().isEmpty());
    const QString other = first == QStringLiteral("tool.razor") ? QStringLiteral("marker.markIn")
                                                                : QStringLiteral("tool.razor");
    const km::KeyChord otherKey = answer.at(other);
    CHECK(trainer.press(otherKey.key, static_cast<int>(otherKey.mods.bits())) == QStringLiteral("wrong"));
    CHECK(trainer.lastPressedAction() == displayName(other.toStdString()));
    if (first == QStringLiteral("edit.rippleDelete")) // Premiere's own key is not this key set's
        CHECK(trainer.press(Qt::Key_Delete, Qt::ShiftModifier) == QStringLiteral("wrong"));
    const km::KeyChord firstKey = answer.at(first);
    CHECK(trainer.press(firstKey.key, static_cast<int>(firstKey.mods.bits())) == QStringLiteral("correct"));
    CHECK(trainer.state() == QStringLiteral("correct"));
    CHECK(sparks == 1);
    CHECK(trainer.correct() == 0); // not on the first try
    CHECK(trainer.streak() == 1);
    CHECK(trainer.lastPoints() >= 100);
    CHECK(trainer.lastPoints() <= 150);
    CHECK(trainer.score() == trainer.lastPoints());
    CHECK(trainer.press(firstKey.key, static_cast<int>(firstKey.mods.bits())) == QStringLiteral("ignored"));

    // Prompt 2: right first time; the streak adds ten.
    trainer.next();
    CHECK(trainer.round() == 2);
    CHECK(trainer.state() == QStringLiteral("asking"));
    const QString second = trainer.promptCommand();
    CHECK(second != first);
    const km::KeyChord secondKey = answer.at(second);
    CHECK(trainer.press(secondKey.key, static_cast<int>(secondKey.mods.bits())) == QStringLiteral("correct"));
    CHECK(trainer.correct() == 1);
    CHECK(trainer.streak() == 2);
    CHECK(trainer.lastPoints() >= 110);
    CHECK(trainer.lastPoints() <= 160);

    // Prompt 3: with the hint (the key set's key, as the menus show it), half points and no first-try credit.
    trainer.next();
    const QString third = trainer.promptCommand();
    const km::KeyChord thirdKey = answer.at(third);
    trainer.showHint();
    CHECK(trainer.hintShown());
    CHECK(trainer.hint() == QString::fromStdString(km::formatKeyChord(thirdKey, km::Platform::Windows,
                                                                      km::ChordFormat::Native)));
    CHECK(trainer.press(thirdKey.key, static_cast<int>(thirdKey.mods.bits())) == QStringLiteral("correct"));
    CHECK(trainer.lastPoints() <= 85);
    CHECK(trainer.correct() == 1);
    CHECK(trainer.streak() == 3);
    CHECK(trainer.bestStreak() == 3);

    trainer.next();
    CHECK(trainer.state() == QStringLiteral("finished"));
    CHECK(trainer.averageMs() >= 0);
    CHECK(sparks == 3);

    // Skipping breaks the streak; a saved best is kept.
    REQUIRE(trainer.start(QStringLiteral("tools")));
    CHECK(trainer.roundLength() == 1);
    trainer.next();
    CHECK(trainer.state() == QStringLiteral("finished"));
    CHECK(trainer.streak() == 0);
    trainer.setSavedBest(12);
    CHECK(trainer.bestStreak() == 12);
    trainer.stop();
    CHECK(trainer.state() == QStringLiteral("idle"));
}

TEST_CASE("Onboarding: the first-run welcome follows Home, skips with Esc and does not come back",
          "[app][onboarding][qml]") {
    resetSettings();
    clearMessages();
    {
        // Launched like the app: on the Home screen.
        struct LaunchHome {
            LaunchHome() { HomeBridge::setShowAtLaunch(true); }
            ~LaunchHome() { HomeBridge::setShowAtLaunch(false); }
        };
        std::optional<LaunchHome> launch(std::in_place);
        Shell shell;
        launch.reset();
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight); // with motion: the welcome's key run lights up
        QObject* home = shell.object("homeScreen");
        QObject* welcome = shell.object("welcomeDialog");
        REQUIRE(home);
        REQUIRE(welcome);
        CHECK(home->property("opened").toBool());
        CHECK_FALSE(shell.preferences->property("welcomeDone").toBool());
        spin(100);
        CHECK_FALSE(isVisible(welcome)); // never over Home

        // Closing Home (a new project, an opened one) brings the welcome.
        QMetaObject::invokeMethod(home, "close");
        REQUIRE(waitUntil([&] { return isVisible(welcome); }));
        CHECK(welcome->property("page").toInt() == 0);
        REQUIRE(waitUntil([&] { return focusName(shell) == QStringLiteral("welcomeNextButton"); }));
        QQuickItem* hero = named(shell, "welcomeHero");
        REQUIRE(hero);
        CHECK(hero->isVisible());
        spin(300); // the key run lights up a few keys
        save(shell.grab(), QStringLiteral("welcome.png"));

        // Esc skips; it is remembered.
        tap(shell.window.get(), Qt::Key_Escape);
        REQUIRE(waitUntil([&] { return !isVisible(welcome); }));
        CHECK(welcome->property("outcome").toString() == QStringLiteral("skipped"));
        CHECK(shell.preferences->property("welcomeDone").toBool());
        CHECK(waitUntil([&] { return focusVisible(shell); }));
        QObject* onboarding = shell.object("onboarding");
        REQUIRE(onboarding);
        CHECK_FALSE(call(onboarding, "firstRun").toBool());
        CHECK(waitUntil([] { return QSettings().value(QStringLiteral("onboarding/welcomeDone")).toBool(); }));
    }
    // A later launch does not show it.
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        CHECK(shell.preferences->property("welcomeDone").toBool());
        QObject* onboarding = shell.object("onboarding");
        REQUIRE(onboarding);
        CHECK_FALSE(call(onboarding, "firstRun").toBool());
        CHECK_FALSE(isVisible(shell.object("welcomeDialog")));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}

TEST_CASE("Onboarding: Home's Learn page runs the tour and the Keyboard Trainer and What's new after Home closes",
          "[app][onboarding][home][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.preferences->setProperty("welcomeDone", true);
    QObject* home = shell.object("homeScreen");
    REQUIRE(home);
    const auto fromLearn = [&](const char* row) {
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
        REQUIRE(waitUntil([&] { return home->property("opened").toBool(); }));
        REQUIRE(QMetaObject::invokeMethod(home, "showPage", Q_ARG(QString, QStringLiteral("learn"))));
        spin(100);
        QQuickItem* item = named(shell, row);
        REQUIRE(item);
        REQUIRE(item->isVisible());
        click(item);
        // Home closes first: the tour and the dialogs run over the editor.
        REQUIRE(waitUntil([&] { return !home->property("opened").toBool(); }));
    };

    // Take the tour: the coach marks over the editor; Esc ends it.
    QObject* tour = shell.object("coachMarks");
    REQUIRE(tour);
    fromLearn("homeLearnTour");
    REQUIRE(waitUntil([&] { return tour->property("active").toBool(); }));
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !tour->property("active").toBool(); }));

    // Keyboard Trainer.
    QObject* trainer = shell.object("keyboardTrainer");
    REQUIRE(trainer);
    fromLearn("homeLearnTrainer");
    REQUIRE(waitUntil([&] { return isVisible(trainer); }));
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !isVisible(trainer); }));

    // What's new.
    QObject* whatsNew = shell.object("whatsNewDialog");
    REQUIRE(whatsNew);
    fromLearn("homeLearnWhatsNew");
    REQUIRE(waitUntil([&] { return isVisible(whatsNew); }));
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !isVisible(whatsNew); }));
    CHECK(waitUntil([&] { return focusVisible(shell); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}

TEST_CASE("Onboarding: the welcome sets keys, workspace and accent, then tours Library, Timeline and Export",
          "[app][onboarding][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* dispatcher = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(dispatcher);
    // The imported keys are saved here, not in the test app's config (a later run starts from Premiere's keys).
    QTemporaryDir config;
    dispatcher->setOverridesPath(config.filePath(QStringLiteral("keymap/overrides.json")));
    QObject* onboarding = shell.object("onboarding");
    QObject* welcome = shell.object("welcomeDialog");
    QObject* tour = shell.object("coachMarks");
    REQUIRE(onboarding);
    REQUIRE(welcome);
    REQUIRE(tour);

    REQUIRE(call(onboarding, "firstRun").toBool());
    REQUIRE(waitUntil([&] { return isVisible(welcome); }));
    REQUIRE(waitUntil([&] { return focusName(shell) == QStringLiteral("welcomeNextButton"); }));

    // Page 1, the keys (Return on Next): import a Premiere key set. Its Razor is X.
    tap(shell.window.get(), Qt::Key_Return);
    REQUIRE(welcome->property("page").toInt() == 1);
    CHECK(welcome->property("keysChoice").toString() == QStringLiteral("premiere"));
    CHECK(call(welcome, "importKeySet", QUrl::fromLocalFile(kysFixture())).toBool());
    CHECK(welcome->property("keysChoice").toString() == QStringLiteral("kys"));
    CHECK(welcome->property("importMessage").toString().contains(QStringLiteral(".kys")));
    const auto razor = dispatcher->keymap().chordsFor("tool.razor");
    REQUIRE_FALSE(razor.empty());
    CHECK(razor.front().key == km::Key_X);
    // A file that is not a key set says why.
    CHECK_FALSE(call(welcome, "importKeySet", QUrl::fromLocalFile(QStringLiteral(WABCUT_APP_QML_DIR "/Main.qml")))
                    .toBool());
    CHECK(welcome->property("importFailed").toBool());

    // Page 2: the workspace (applies at once, remembered for the next launch) and the accent.
    call(welcome, "goTo", 2);
    REQUIRE(welcome->property("page").toInt() == 2);
    click(named(shell, "welcomeWorkspace_color"));
    CHECK(shell.window->property("workspacePreset").toInt() == 3); // a workspace made from the Color preset
    CHECK(shell.preferences->property("startWorkspace").toInt() == 3);
    click(named(shell, "welcomeAccent_phosphor"));
    CHECK(shell.theme->property("accentName").toString() == QStringLiteral("phosphor"));

    // Page 3 (M5n): AI on your terms; page 4: take the tour.
    call(welcome, "goTo", 3);
    REQUIRE(welcome->property("page").toInt() == 3);
    CHECK(named(shell, "welcomeAiPage")->isVisible());
    call(welcome, "goTo", 4);
    click(named(shell, "welcomeTourButton"));
    REQUIRE(waitUntil([&] { return !isVisible(welcome); }));
    CHECK(welcome->property("outcome").toString() == QStringLiteral("tour"));
    CHECK(shell.preferences->property("welcomeDone").toBool());

    // Step 1, the Library: it is opened (the Color preset has none) and highlighted; the card has focus.
    REQUIRE(waitUntil([&] { return tour->property("active").toBool(); }));
    CHECK(tour->property("step").toInt() == 0);
    REQUIRE(waitUntil([&] { return focusName(shell) == QStringLiteral("coachMarkCard"); }));
    CHECK(shell.window->property("workspacePreset").toInt() == 3);
    CHECK(shell.item("libraryPanel")->isVisible());
    auto* library = onboarding->property("libraryTarget").value<QQuickItem*>();
    REQUIRE(library);
    REQUIRE(waitUntil([&] {
        const QRectF hole = tour->property("hole").toRectF();
        return hole.contains(sceneRect(library).adjusted(1, 1, -1, -1));
    }));
    QQuickItem* title = named(shell, "coachMarkTitle");
    REQUIRE(title);
    CHECK(title->property("text").toString() == QStringLiteral("The Library"));

    // Step 2, the Timeline (Right). The card sits beside or around the highlight, inside the window.
    tap(shell.window.get(), Qt::Key_Right);
    CHECK(tour->property("step").toInt() == 1);
    auto* timeline = onboarding->property("timelineTarget").value<QQuickItem*>();
    REQUIRE(timeline);
    REQUIRE(waitUntil([&] { return tour->property("hole").toRectF().contains(sceneRect(timeline).adjusted(1, 1, -1, -1)); }));
    QQuickItem* card = named(shell, "coachMarkCard");
    REQUIRE(card);
    const QRectF cardRect = sceneRect(card);
    CHECK(QRectF(0, 0, kShotWidth, kShotHeight).contains(cardRect));
    CHECK_FALSE(cardRect.intersects(tour->property("hole").toRectF().adjusted(2, 2, -2, -2)));
    CHECK(title->property("text").toString() == QStringLiteral("The Timeline"));
    QQuickItem* text = named(shell, "coachMarkText");
    REQUIRE(text);
    // The step names the active key set's keys.
    CHECK(text->property("text").toString().contains(dispatcher->shortcutText(QStringLiteral("sequence.addEdit"))));
    save(shell.grab(), QStringLiteral("tour-timeline.png"));

    // Left goes back, Right again; step 3, Export: opening it completes the tour.
    tap(shell.window.get(), Qt::Key_Left);
    CHECK(tour->property("step").toInt() == 0);
    tap(shell.window.get(), Qt::Key_Right);
    tap(shell.window.get(), Qt::Key_Right);
    CHECK(tour->property("step").toInt() == 2);
    auto* exportTab = onboarding->property("exportTarget").value<QQuickItem*>();
    REQUIRE(exportTab);
    // No workspace made from the Export preset yet: the Export button.
    CHECK(exportTab->objectName() == QStringLiteral("topBarExportButton"));
    REQUIRE(waitUntil([&] { return tour->property("hole").toRectF().contains(sceneRect(exportTab).adjusted(1, 1, -1, -1)); }));
    click(named(shell, "coachMarkActionButton"));
    CHECK(shell.window->property("workspacePreset").toInt() == 2);
    REQUIRE(waitUntil([&] { return !tour->property("active").toBool(); }));
    CHECK(waitUntil([&] { return focusVisible(shell); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}

TEST_CASE("Onboarding: Help and the command palette run the welcome, the tour, the trainer and What's new again",
          "[app][onboarding][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.preferences->setProperty("welcomeDone", true);
    QObject* onboarding = shell.object("onboarding");
    REQUIRE(onboarding);
    CHECK_FALSE(call(onboarding, "firstRun").toBool());

    // UX rework: the Help menu of the menu bar (it replaced the left rail's Help popover).
    QObject* help = shell.object("helpMenu");
    REQUIRE(help);
    const auto fromHelp = [&](const char* item, QObject* dialog) {
        click(named(shell, "menuBar_help"));
        REQUIRE(waitUntil([&] { return isVisible(help); }));
        click(shell.object(item));
        REQUIRE(waitUntil([&] { return isVisible(dialog); }));
        CHECK_FALSE(isVisible(help));
        // The dialog keeps the focus (the menu had closed and given it back before the dialog opened).
        auto* content = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        QQuickItem* popupItem = content->parentItem();
        CHECK(waitUntil([&] {
            for (QQuickItem* f = shell.window->activeFocusItem(); f; f = f->parentItem()) {
                if (f == popupItem)
                    return true;
            }
            return false;
        }));
    };

    // Help > Welcome: even though it was done.
    QObject* welcome = shell.object("welcomeDialog");
    REQUIRE(welcome);
    fromHelp("helpItem_welcome", welcome);
    CHECK(welcome->property("page").toInt() == 0);
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !isVisible(welcome); }));
    CHECK(shell.preferences->property("welcomeDone").toBool());

    // Help > Keyboard Trainer; Esc closes it.
    QObject* trainer = shell.object("keyboardTrainer");
    REQUIRE(trainer);
    fromHelp("helpItem_trainer", trainer);
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !isVisible(trainer); }));
    CHECK(waitUntil([&] { return focusVisible(shell); }));

    // Help > What's new.
    QObject* whatsNew = shell.object("whatsNewDialog");
    REQUIRE(whatsNew);
    fromHelp("helpItem_whatsNew", whatsNew);
    QQuickItem* list = named(shell, "whatsNewList");
    REQUIRE(list);
    CHECK(list->property("count").toInt() >= 6);
    click(named(shell, "whatsNewCloseButton"));
    REQUIRE(waitUntil([&] { return !isVisible(whatsNew); }));

    // The palette: its Help rows.
    auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(palette);
    palette->setQuery(QStringLiteral("keyboard trainer"));
    bool listed = false;
    for (const QVariant& row : palette->results())
        listed = listed || row.toMap().value(QStringLiteral("key")).toString() == QStringLiteral("action:help.keyboardTrainer");
    CHECK(listed);
    CHECK(palette->activate(QStringLiteral("action:help.whatsNew")).isEmpty());
    REQUIRE(waitUntil([&] { return isVisible(whatsNew); }));
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !isVisible(whatsNew); }));

    // Palette > Take the Tour; Esc ends it and focus stays in the window.
    QObject* tour = shell.object("coachMarks");
    REQUIRE(tour);
    CHECK(palette->activate(QStringLiteral("action:help.tour")).isEmpty());
    REQUIRE(waitUntil([&] { return tour->property("active").toBool(); }));
    REQUIRE(waitUntil([&] { return focusName(shell) == QStringLiteral("coachMarkCard"); }));
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !tour->property("active").toBool(); }));
    CHECK(waitUntil([&] { return focusVisible(shell); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}

TEST_CASE("Keyboard Trainer in the window: the active key set answers, nothing reaches the editor",
          "[app][onboarding][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.preferences->setProperty("welcomeDone", true);
    auto* dispatcher = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    auto* trainer = shell.engine.singletonInstance<KeyboardTrainerBridge*>("Wabcut", "Trainer");
    REQUIRE(dispatcher);
    REQUIRE(trainer);
    trainer->setSeed(3);
    QStringList editorCommands;
    QObject::connect(dispatcher, &KeymapDispatcher::commandTriggered,
                     [&editorCommands](const QString& id) { editorCommands << id; });

    QObject* onboarding = shell.object("onboarding");
    QObject* dialog = shell.object("keyboardTrainer");
    REQUIRE(onboarding);
    REQUIRE(dialog);
    REQUIRE(call(onboarding, "run", QStringLiteral("trainer")).toBool());
    REQUIRE(waitUntil([&] { return isVisible(dialog); }));
    REQUIRE(waitUntil([&] { return focusName(shell) == QStringLiteral("trainerStartButton"); }));

    // Return starts a round of everything; the stage takes the keys.
    tap(shell.window.get(), Qt::Key_Return);
    REQUIRE(trainer->state() == QStringLiteral("asking"));
    CHECK(trainer->roundLength() == 10);
    REQUIRE(waitUntil([&] { return focusName(shell) == QStringLiteral("trainerStage"); }));

    // A key that does nothing in the editor is wrong, and says so.
    tap(shell.window.get(), Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    CHECK(trainer->state() == QStringLiteral("wrong"));
    QQuickItem* status = named(shell, "trainerStatus");
    REQUIRE(status);
    CHECK(status->property("text").toString().contains(QStringLiteral("does nothing")));

    // Three right answers in a row, with keys from the active keymap.
    for (int i = 0; i < 3; ++i) {
        const auto chords = dispatcher->keymap().chordsFor(trainer->promptCommand().toStdString());
        REQUIRE_FALSE(chords.empty());
        INFO(trainer->promptCommand().toStdString());
        tapChord(shell.window.get(), chords.front());
        REQUIRE(trainer->state() == QStringLiteral("correct"));
        if (i < 2) // the spark, then the next prompt by itself
            REQUIRE(waitUntil([&] { return trainer->state() == QStringLiteral("asking"); }));
    }
    CHECK(trainer->streak() == 3);
    CHECK(trainer->correct() == 2);
    CHECK(shell.preferences->property("trainerBest").toInt() == 3);
    INFO(join(editorCommands));
    CHECK(editorCommands.isEmpty()); // the trainer keeps every key
    spin(120); // the spark is lit
    save(shell.grab(), QStringLiteral("keyboard-trainer.png"));

    // Esc closes the trainer (and ends the round).
    tap(shell.window.get(), Qt::Key_Escape);
    REQUIRE(waitUntil([&] { return !isVisible(dialog); }));
    CHECK(trainer->state() == QStringLiteral("idle"));
    CHECK(editorCommands.isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}
