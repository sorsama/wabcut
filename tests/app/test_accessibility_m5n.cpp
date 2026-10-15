// M5n accessibility and onboarding refresh, offscreen:
//   * A11yAudit.h, the reusable checker: it names the unnamed controls and the ones outside the accessibility tree;
//   * the panels added since M5j name every control and Tab reaches their primary ones: review notes and version
//     history (rename and the second version of a comparison by keyboard), the caption workshop (presets by keyboard),
//     Essential Sound, the plugin browser and settings, voice-over (Settings > Audio, the overlay, record arm), the
//     Library's list view (Sort by in its tools menu), masks (a mask is chosen with the keyboard), the multi-camera
//     view and the M5l dialogs, Explain this frame;
//   * the Keyboard Trainer's new rounds (clipboard, markers, trim, multicam) on Premiere's keys;
//   * What's new for this build; the welcome's AI page (where each feature runs, consent, the no-cloud lock).
// Screenshots: whats-new.png, keyboard-trainer-categories.png, tour-ai-privacy.png.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandJob.h"
#include "HistoryBridge.h"
#include "KeyboardTrainerBridge.h"
#include "LibraryBridge.h"
#include "MaskBridge.h"
#include "ProjectController.h"
#include "ReviewBridge.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/keymap/KeymapManager.h>

#include <QDir>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;
namespace km = wabcut::keymap;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
)";

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

QQuickItem* inWindow(const Shell& shell, const QString& name) { return findNamed(shell.window->contentItem(), name); }

std::unique_ptr<QObject> makeQml(Shell& shell, const QByteArray& qml) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5n-a11y/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    return object;
}

/// A component of the Wabcut module over the window at `at` (Main.qml places the panels in the app).
std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QRectF at) {
    std::unique_ptr<QObject> object = makeQml(shell, qml);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(at.topLeft());
    item->setSize(at.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

/// The visual item of an open dialog or popover (what a screen reader reads as the dialog).
QQuickItem* popupItem(QObject* popup) {
    auto* content = popup ? popup->property("contentItem").value<QQuickItem*>() : nullptr;
    return content ? content->parentItem() : nullptr;
}

/// A dialog of the Wabcut module, made inside a host item (`dialog` is its alias) and opened.
QObject* openHostedDialog(Shell& shell, std::unique_ptr<QQuickItem>& host, const QByteArray& qml) {
    host = makeItem(shell, qml, QRectF(0, 0, kShotWidth, kShotHeight));
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
    return dialog;
}

QVariant call(QObject* object, const char* method, const QVariant& arg = {}) {
    QVariant result;
    if (arg.isValid())
        QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, arg));
    else
        QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result));
    return result;
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name, double margin = 16) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = sceneRect(item).adjusted(-margin, -margin, margin, margin)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

std::filesystem::path pathOf(const QTemporaryDir& dir, const char* name) {
    return std::filesystem::path(QDir(dir.path()).filePath(QString::fromLatin1(name)).toStdU16String());
}

/// Every issue the audit finds under `item`, as one message (empty: none).
#define CHECK_ACCESSIBLE(item)                                                                                         \
    do {                                                                                                               \
        REQUIRE(item);                                                                                                 \
        const QStringList issues_ = auditAccessibility(item);                                                          \
        INFO(join(issues_));                                                                                           \
        CHECK(issues_.isEmpty());                                                                                      \
    } while (false)

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("A11y audit: unnamed controls and focusable items outside the tree are named; Tab order inside a panel",
          "[app][a11y][qml]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    const auto panel = makeItem(shell,
                                "import QtQuick\nimport Wabcut\nColumn {\n"
                                "    PillButton { objectName: \"named\"; text: \"Save\" }\n"
                                "    IconButton { objectName: \"unnamedIcon\"; iconName: \"plus\" }\n"
                                "    FieldInput { objectName: \"unnamedField\"; width: 120 }\n"
                                "    FieldInput { objectName: \"namedField\"; width: 120; Accessible.name: \"Title\" }\n"
                                "    Rectangle { objectName: \"bareFocusable\"; width: 20; height: 20; activeFocusOnTab: true }\n"
                                "    Rectangle { objectName: \"namedFocusable\"; width: 20; height: 20; activeFocusOnTab: true;\n"
                                "                Accessible.role: Accessible.Button; Accessible.name: \"Spot\" }\n"
                                "    PillButton { objectName: \"hiddenButton\"; visible: false }\n"
                                "}\n",
                                QRectF(40, 40, 400, 400));
    spin(50);
    const QStringList issues = auditAccessibility(panel.get());
    INFO(join(issues));
    const auto mentions = [&](const char* name) {
        return std::ranges::any_of(issues, [&](const QString& issue) { return issue.contains(QLatin1String(name)); });
    };
    REQUIRE(mentions("unnamedIcon"));
    CHECK(mentions("unnamedField"));
    CHECK(mentions("bareFocusable"));
    CHECK_FALSE(mentions("(named)"));
    CHECK_FALSE(mentions("(namedField)"));
    CHECK_FALSE(mentions("namedFocusable"));
    CHECK_FALSE(mentions("hiddenButton"));
    CHECK(issues.filter(QStringLiteral("unnamedIcon")).first().endsWith(QStringLiteral("no accessible name")));
    // A skipped subtree is not audited.
    CHECK(auditAccessibility(panel.get(), {QStringLiteral("unnamedIcon"), QStringLiteral("unnamedField"),
                                           QStringLiteral("bareFocusable")})
              .isEmpty());

    // Tab order: the panel's controls in order, hidden ones left out; Space presses the focused button.
    INFO(describeTabChain(panel.get()).toStdString());
    const QList<QQuickItem*> chain = tabChain(panel.get());
    QStringList names;
    for (const QQuickItem* item : chain)
        names << item->objectName();
    CHECK(names.indexOf(QStringLiteral("named")) < names.indexOf(QStringLiteral("namedField")));
    CHECK(names.contains(QStringLiteral("namedFocusable")));
    CHECK_FALSE(names.contains(QStringLiteral("hiddenButton")));
    CHECK(tabReaches(panel.get(), QStringLiteral("unnamedIcon")));
    checkNoMessages();
}

TEST_CASE("A11y M5n: review notes and versions name every control; rename and compare by keyboard",
          "[app][a11y][review][history][qml]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* review = shell.engine.singletonInstance<ReviewBridge*>("Wabcut", "Review");
    auto* history = shell.engine.singletonInstance<HistoryBridge*>("Wabcut", "History");
    REQUIRE(controller);
    REQUIRE(review);
    REQUIRE(history);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setProjectFile(pathOf(dir, "Film.wabcut"));
    history->reopen();
    REQUIRE(history->available());
    REQUIRE_FALSE(shell.grab().isNull());

    // Two named versions with an edit between them, and a note with a mark and a reply.
    history->onSaved();
    REQUIRE(history->saveVersion(QStringLiteral("Cut v1")).isEmpty());
    controller->setSelection({tl.clip("V1@10")});
    controller->deleteSelection();
    REQUIRE(history->saveVersion(QStringLiteral("Cut v2")).isEmpty());
    controller->setPlayhead(f(4));
    review->setPendingMark(0.4, 0.3);
    REQUIRE(review->add(QStringLiteral("Logo clips the safe area")).isEmpty());
    const int note = review->notes().value(0).toMap().value(QStringLiteral("id")).toInt();
    REQUIRE(note > 0);
    REQUIRE(review->reply(note, QStringLiteral("Fixed in the next cut")).isEmpty());

    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(library);
    const auto panel = makeItem(shell, "import Wabcut\nReviewPanel {}\n", sceneRect(library));
    spin(200);

    // Notes: every control named; Tab reaches the tabs, the note's time and its reply field.
    CHECK_ACCESSIBLE(panel.get());
    INFO(describeTabChain(panel.get()).toStdString());
    CHECK(tabReaches(panel.get(), QStringLiteral("reviewTabs")));
    CHECK(tabReaches(panel.get(), QStringLiteral("reviewNoteTime_%1").arg(note)));

    // Versions.
    REQUIRE(QMetaObject::invokeMethod(panel.get(), "showPage", Q_ARG(QVariant, 1)));
    spin(200);
    CHECK_ACCESSIBLE(panel.get());
    const QVariantList versions = history->versions();
    REQUIRE(versions.size() >= 2);
    const int newest = versions.value(0).toMap().value(QStringLiteral("id")).toInt();
    const int older = versions.value(1).toMap().value(QStringLiteral("id")).toInt();
    CHECK(tabReaches(panel.get(), QStringLiteral("historyCompareNow_%1").arg(newest)));
    QQuickItem* more = findNamed(panel.get(), QStringLiteral("historyActions_%1").arg(newest));
    REQUIRE(more);
    CHECK(tabReaches(panel.get(), more->objectName()));

    // More (Space) > Rename…: the name field opens in the row with the focus; Esc closes it.
    REQUIRE(pressWithKey(w, more, Qt::Key_Space, QStringLiteral(" ")));
    QObject* actions = panel->findChild<QObject*>(QStringLiteral("historyActionsPopover"));
    REQUIRE(actions);
    REQUIRE(waitFor([&] { return actions->property("opened").toBool(); }, 3000));
    CHECK_ACCESSIBLE(popupItem(actions));
    QObject* rename = panel->findChild<QObject*>(QStringLiteral("historyRenameAction"));
    REQUIRE(rename);
    REQUIRE(QMetaObject::invokeMethod(rename, "clicked"));
    QQuickItem* field = findNamed(panel.get(), QStringLiteral("historyRename_%1").arg(newest));
    REQUIRE(field);
    REQUIRE(waitFor([&] { return field->isVisible() && field->hasActiveFocus(); }, 3000));
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &esc);
    REQUIRE(waitFor([&] { return !field->isVisible(); }, 3000));

    // Compare with another version…: the other rows offer "Compare … with this one", pressed with Space.
    REQUIRE(pressWithKey(w, more, Qt::Key_Space, QStringLiteral(" ")));
    REQUIRE(waitFor([&] { return actions->property("opened").toBool(); }, 3000));
    QObject* compareWith = panel->findChild<QObject*>(QStringLiteral("historyCompareWith"));
    REQUIRE(compareWith);
    REQUIRE(QMetaObject::invokeMethod(compareWith, "clicked"));
    // The list makes rows as they scroll into view; a window that is never shown lays its layouts out only when a
    // frame is made, so render one before looking for the older version's row.
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* versionsList = findNamed(panel.get(), QStringLiteral("historyVersionsList"));
    REQUIRE(versionsList);
    REQUIRE(QMetaObject::invokeMethod(versionsList, "positionViewAtIndex", Q_ARG(int, 1), Q_ARG(int, 4 /* ListView.Contain */)));
    spin(50);
    QQuickItem* pick = findNamed(panel.get(), QStringLiteral("historyComparePick_%1").arg(older));
    {
        QStringList names;
        std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            if (item->objectName().startsWith(QStringLiteral("historyCompare")))
                names << item->objectName() + (item->isVisible() ? QString() : QStringLiteral(" (hidden)"));
            for (QQuickItem* child : item->childItems())
                walk(child);
        };
        walk(panel.get());
        INFO("older " << older << ", newest " << newest << ": " << names.join(QStringLiteral(", ")).toStdString()
                      << "; list count " << versionsList->property("count").toInt() << ", height "
                      << versionsList->height() << ", contentHeight " << versionsList->property("contentHeight").toDouble()
                      << ", versions " << history->versions().size());
        REQUIRE(pick);
    }
    REQUIRE(waitFor([&] { return pick->isVisible(); }, 3000));
    CHECK_FALSE(findNamed(panel.get(), QStringLiteral("historyComparePick_%1").arg(newest))->isVisible());
    CHECK_ACCESSIBLE(panel.get());
    REQUIRE(pressWithKey(w, pick, Qt::Key_Space, QStringLiteral(" ")));
    REQUIRE(waitFor([&] { return history->comparison().contains(QStringLiteral("from")); }, 3000));
    CHECK_FALSE(pick->isVisible());
    checkNoMessages();
}

TEST_CASE("A11y M5n: the caption workshop names every control; a preset is applied with the keyboard",
          "[app][a11y][captions][qml]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    REQUIRE(controller);
    REQUIRE(captions);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(captions->addTrack(QStringLiteral("English"), QStringLiteral("Podcast")).isEmpty());
    for (const auto& [from, to, text] : {std::tuple{0, 30, "Welcome back to the show."},
                                         std::tuple{32, 70, "Today we cut a whole episode."}}) {
        REQUIRE(CommandJob::runNow(controller, nullptr, "captions.addCue",
                                   Json{{"start", f(from).value}, {"end", f(to).value}, {"text", text}})
                    .ok());
    }
    REQUIRE(captions->setCueSpeaker(0, captions->addSpeaker(QStringLiteral("Host"))).isEmpty());

    w->setProperty("workspace", 4); // Captions
    REQUIRE(QMetaObject::invokeMethod(w, "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("captions")))));
    REQUIRE(waitFor([&] { return inWindow(shell, QStringLiteral("transcriptTabs")) != nullptr; }, 5000));
    inWindow(shell, QStringLiteral("transcriptTabs"))->setProperty("currentIndex", 2); // Captions
    REQUIRE(waitFor([&] {
        QQuickItem* style = inWindow(shell, QStringLiteral("captionStylePanel"));
        QQuickItem* page = inWindow(shell, QStringLiteral("captionsPage"));
        return style && style->isVisible() && page && page->isVisible();
    }, 5000));
    spin(200);
    QQuickItem* style = inWindow(shell, QStringLiteral("captionStylePanel"));
    QQuickItem* page = inWindow(shell, QStringLiteral("captionsPage"));
    CHECK_ACCESSIBLE(style);
    CHECK_ACCESSIBLE(page);

    // The presets are Tab stops; Space applies one (the chip says it is the current look).
    INFO(describeTabChain(style).toStdString());
    const QString preset = QStringLiteral("Bold Box");
    QQuickItem* chip = findNamed(style, QStringLiteral("captionPreset_") + preset);
    REQUIRE(chip);
    CHECK(tabReaches(style, chip->objectName()));
    CHECK(tabReaches(style, QStringLiteral("captionPresetName")));
    REQUIRE(pressWithKey(w, chip, Qt::Key_Space, QStringLiteral(" ")));
    const auto current = [&] {
        for (const QVariant& p : captions->presets()) {
            if (p.toMap().value(QStringLiteral("name")).toString() == preset)
                return p.toMap().value(QStringLiteral("current")).toBool();
        }
        return false;
    };
    REQUIRE(waitFor(current, 3000));
    // Applying it marks it current: the chips are made again, and the new chip keeps the keyboard focus.
    chip = findNamed(style, QStringLiteral("captionPreset_") + preset);
    REQUIRE(chip);
    REQUIRE(waitFor([&] { return chip->hasActiveFocus(); }, 3000));
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(chip);
    REQUIRE(iface);
    CHECK(iface->role() == QAccessible::RadioButton);
    CHECK(iface->state().checked);
    w->setProperty("workspace", 0);
    spin(100);
    checkNoMessages();
}

TEST_CASE("A11y M5n: Essential Sound, the plugin browser, Settings Audio and the voice-over overlay name every control",
          "[app][a11y][essential][plugins][voiceover][qml]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setSelection({tl.clip("A1@0")});
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(library);

    // Essential Sound on an audio clip: the type buttons are named Tab stops.
    {
        const auto panel = makeItem(shell, "import Wabcut\nEssentialSoundPanel {}\n", sceneRect(library));
        spin(200);
        CHECK_ACCESSIBLE(panel.get());
        INFO(describeTabChain(panel.get()).toStdString());
        CHECK(tabReaches(panel.get(), QStringLiteral("essentialType_dialogue")));
    }
    // The Effects panel's audio plugins (none scanned here: the notices and Rescan).
    {
        const auto browser = makeItem(shell, "import Wabcut\nPluginBrowser {}\n", sceneRect(library));
        spin(100);
        CHECK_ACCESSIBLE(browser.get());
    }
    // Settings > Audio plugins.
    {
        std::unique_ptr<QQuickItem> host;
        QObject* dialog = openHostedDialog(shell, host,
                                           "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: settings\n"
                                           "    AudioPluginsSettings { id: settings }\n}\n");
        CHECK_ACCESSIBLE(popupItem(dialog));
        // Rescan is off while a scan runs (the plugin browser above starts one).
        QObject* plugins = shell.engine.singletonInstance<QObject*>("Wabcut", "Plugins");
        REQUIRE(plugins);
        REQUIRE(waitFor([&] { return !plugins->property("scanning").toBool(); }, 60000));
        INFO("chain: " << describeTabChain(popupItem(dialog)).toStdString());
        // Rescan and the folders show only where plugins are hosted (no plugin system is set up here); Close always.
        if (plugins->property("available").toBool())
            CHECK(tabReaches(popupItem(dialog), QStringLiteral("rescanPlugins")));
        CHECK(tabReaches(popupItem(dialog), QStringLiteral("closeAudioPluginsSettings")));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    }
    // Settings > Audio (the devices are given: enumerating would start the system's audio).
    {
        std::unique_ptr<QQuickItem> host;
        QObject* dialog = openHostedDialog(shell, host,
                                           "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: settings\n"
                                           "    AudioSettings {\n        id: settings\n        refreshOnOpen: false\n"
                                           "        devices: [{ id: \"mic-usb\", name: \"USB microphone\", isDefault: true }]\n"
                                           "    }\n}\n");
        CHECK_ACCESSIBLE(popupItem(dialog));
        INFO(describeTabChain(popupItem(dialog)).toStdString());
        CHECK(tabReaches(popupItem(dialog), QStringLiteral("inputDeviceButton")));
        CHECK(tabReaches(popupItem(dialog), QStringLiteral("countInField")));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    }
    // The voice-over overlay while a take records (the badge and the meter speak; nothing to press).
    {
        QQuickItem* well = inWindow(shell, QStringLiteral("programMonitorWell"));
        REQUIRE(well);
        const auto overlay = makeItem(shell, "import Wabcut\nVoiceOverOverlay {}\n", sceneRect(well));
        overlay->setProperty("trackLabel", QStringLiteral("A1"));
        overlay->setProperty("passState", QStringLiteral("recording"));
        overlay->setProperty("seconds", 3.2);
        spin(100);
        CHECK(overlay->isVisible());
        CHECK_ACCESSIBLE(overlay.get());
    }
    // The track header's record arm.
    QQuickItem* arm = inWindow(shell, QStringLiteral("recordArm_A1"));
    REQUIRE(arm);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(arm);
    REQUIRE(iface);
    CHECK(iface->text(QAccessible::Name) == QStringLiteral("Record-arm A1"));
    CHECK(iface->role() == QAccessible::CheckBox);
    checkNoMessages();
}

TEST_CASE("A11y M5n: the Library list, masks, the multi-camera view and dialogs, Explain this frame",
          "[app][a11y][library][masks][multicam][qml]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    auto* masks = shell.engine.singletonInstance<MaskBridge*>("Wabcut", "Masks");
    REQUIRE(controller);
    REQUIRE(library);
    REQUIRE(masks);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    REQUIRE_FALSE(shell.grab().isNull());

    // The Library's list view: a Tab stop; its headers and rows speak; Sort by in the tools menu.
    library->setView(QStringLiteral("list"));
    REQUIRE(waitFor([&] {
        QQuickItem* list = inWindow(shell, QStringLiteral("libraryList"));
        return list && list->isVisible();
    }, 5000));
    spin(100);
    QQuickItem* panel = shell.item("libraryPanel");
    REQUIRE(panel);
    CHECK_ACCESSIBLE(panel);
    INFO(describeTabChain(panel).toStdString());
    CHECK(tabReaches(panel, QStringLiteral("libraryList")));
    QQuickItem* header = findNamed(panel, QStringLiteral("libraryColumn_name"));
    REQUIRE(header);
    QAccessibleInterface* headerIface = QAccessible::queryAccessibleInterface(header);
    REQUIRE(headerIface);
    CHECK(headerIface->role() == QAccessible::ColumnHeader);
    QObject* sortMenu = w->findChild<QObject*>(QStringLiteral("librarySortMenu"));
    REQUIRE(sortMenu);
    const QVariantList columns = library->columns();
    REQUIRE(columns.size() >= 2);
    REQUIRE(waitFor([&] { return sortMenu->property("count").toInt() == columns.size(); }, 3000));
    QQuickItem* second = nullptr;
    REQUIRE(QMetaObject::invokeMethod(sortMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, second), Q_ARG(int, 1)));
    REQUIRE(second);
    const QString secondColumn = columns.value(1).toMap().value(QStringLiteral("id")).toString();
    CHECK(second->objectName() == QStringLiteral("librarySortBy_") + secondColumn);
    REQUIRE(QMetaObject::invokeMethod(second, "triggered"));
    CHECK(library->sortColumn() == secondColumn);
    library->setView(QStringLiteral("grid"));

    // Masks in Effect Controls: two masks; the first is chosen with Tab and Space.
    const ClipId clip = tl.clip("V1@0");
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    REQUIRE(waitFor([&] { return masks->clip() == clipKey(clip); }, 5000));
    REQUIRE(masks->addMask(QStringLiteral("rectangle")));
    REQUIRE(masks->addMask(QStringLiteral("ellipse")));
    QQuickItem* section = nullptr;
    REQUIRE(waitFor([&] {
        section = inWindow(shell, QStringLiteral("maskSection"));
        return section && section->isVisible();
    }, 5000));
    spin(100);
    CHECK_ACCESSIBLE(section);
    const QVariantList list = masks->property("masks").toList();
    REQUIRE(list.size() == 2);
    const QString first = list.value(0).toMap().value(QStringLiteral("key")).toString();
    const QString last = list.value(1).toMap().value(QStringLiteral("key")).toString();
    masks->select(last);
    QQuickItem* row = findNamed(section, QStringLiteral("maskRow_") + first);
    REQUIRE(row);
    INFO(describeTabChain(section).toStdString());
    CHECK(tabReaches(section, row->objectName()));
    REQUIRE(pressWithKey(w, row, Qt::Key_Space, QStringLiteral(" ")));
    CHECK(masks->selected() == first);
    QAccessibleInterface* rowIface = QAccessible::queryAccessibleInterface(row);
    REQUIRE(rowIface);
    CHECK(rowIface->state().selected);

    // The multi-camera view (no multi-camera clip here: the empty state and the switch target).
    {
        QQuickItem* well = inWindow(shell, QStringLiteral("programMonitorWell"));
        REQUIRE(well);
        const auto view = makeItem(shell, "import Wabcut\nMulticamView {}\n", sceneRect(well));
        spin(100);
        CHECK_ACCESSIBLE(view.get());
        QAccessibleInterface* viewIface = QAccessible::queryAccessibleInterface(view.get());
        REQUIRE(viewIface);
        CHECK(viewIface->text(QAccessible::Name) == QStringLiteral("Multi-camera angles"));
    }
    // M5l's dialogs: Nest (a name), Enhance Speech (a strength).
    controller->setSelection({tl.clip("V1@0"), tl.clip("A1@0")});
    {
        std::unique_ptr<QQuickItem> host = makeItem(shell,
                                                    "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: nest\n"
                                                    "    NestDialog { id: nest }\n}\n",
                                                    QRectF(0, 0, kShotWidth, kShotHeight));
        auto* dialog = host->property("dialog").value<QObject*>();
        REQUIRE(dialog);
        call(dialog, "openForSelection");
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        CHECK_ACCESSIBLE(popupItem(dialog));
        CHECK(tabReaches(popupItem(dialog), QStringLiteral("nestName")));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    }
    {
        std::unique_ptr<QQuickItem> host = makeItem(shell,
                                                    "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: enhance\n"
                                                    "    EnhanceSpeechDialog { id: enhance }\n}\n",
                                                    QRectF(0, 0, kShotWidth, kShotHeight));
        auto* dialog = host->property("dialog").value<QObject*>();
        REQUIRE(dialog);
        call(dialog, "openFor", QVariant(QStringList{clipKey(tl.clip("A1@0"))}));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        CHECK_ACCESSIBLE(popupItem(dialog));
        CHECK(tabReaches(popupItem(dialog), QStringLiteral("enhanceStrength")));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    }
    // Explain this frame.
    {
        QQuickItem* side = shell.item("inspectorPanel");
        const QRectF at = side ? sceneRect(side) : QRectF(1100, 60, 480, 600);
        const auto inspector = makeItem(shell, "import Wabcut\nFrameInspectorPanel {}\n", at);
        spin(100);
        CHECK_ACCESSIBLE(inspector.get());
    }
    checkNoMessages();
}

TEST_CASE("Keyboard Trainer M5n: clipboard, markers, trim and multicam rounds on Premiere's keys",
          "[app][onboarding][a11y]") {
    const std::map<std::string, std::set<std::string>> expected{
        {"clipboard", {"edit.cut", "edit.copy", "edit.paste", "edit.pasteInsert", "edit.pasteAttributes", "edit.duplicate",
                       "edit.selectAll", "edit.deselectAll"}},
        {"trim", {"sequence.trimEdit", "sequence.toggleTrimType", "sequence.trimBackward", "sequence.trimForward",
                  "sequence.trimBackwardMany", "sequence.trimForwardMany", "sequence.rippleTrimPreviousToPlayhead",
                  "sequence.rippleTrimNextToPlayhead"}},
    };
    for (const km::Platform platform : {km::Platform::Windows, km::Platform::MacOS}) {
        INFO("platform " << static_cast<int>(platform));
        auto keys = km::KeymapManager::withDefaults(platform);
        REQUIRE(keys.has_value());
        KeyboardTrainerBridge trainer;
        trainer.setKeymap(&*keys);
        trainer.setSeed(11);
        std::map<QString, int> counts;
        QStringList order;
        for (const QVariant& c : trainer.categories()) {
            counts[c.toMap().value(QStringLiteral("key")).toString()] = c.toMap().value(QStringLiteral("count")).toInt();
            order << c.toMap().value(QStringLiteral("key")).toString();
        }
        CHECK(order == QStringList{QStringLiteral("all"), QStringLiteral("editing"), QStringLiteral("clipboard"),
                                   QStringLiteral("marks"), QStringLiteral("trim"), QStringLiteral("playback"),
                                   QStringLiteral("tools"), QStringLiteral("timeline"), QStringLiteral("multicam")});
        // Every prompt of the new rounds has a key the trainer can take (Cut to Camera 9 has none: not asked).
        CHECK(counts[QStringLiteral("clipboard")] == 8);
        CHECK(counts[QStringLiteral("marks")] == 14);
        CHECK(counts[QStringLiteral("trim")] == 8);
        CHECK(counts[QStringLiteral("multicam")] == 18);
        CHECK(counts[QStringLiteral("all")] >= 80);

        for (const auto& [category, commands] : expected) {
            INFO(category);
            REQUIRE(trainer.start(QString::fromStdString(category)));
            CHECK(trainer.roundLength() == static_cast<int>(commands.size()));
            std::set<std::string> asked;
            for (int i = 0; i < trainer.roundLength(); ++i) {
                asked.insert(trainer.promptCommand().toStdString());
                trainer.next();
            }
            CHECK(asked == commands);
        }
    }
    // A multicam round answered with Premiere's Windows keys, Paste Attributes too.
    auto keys = km::KeymapManager::withDefaults(km::Platform::Windows);
    REQUIRE(keys.has_value());
    KeyboardTrainerBridge trainer;
    trainer.setKeymap(&*keys);
    trainer.setSeed(5);
    for (const char* category : {"multicam", "clipboard"}) {
        REQUIRE(trainer.start(QString::fromLatin1(category)));
        CHECK(trainer.roundLength() == (std::string_view(category) == "multicam" ? 10 : 8));
        for (int i = 0; i < trainer.roundLength(); ++i) {
            const std::string command = trainer.promptCommand().toStdString();
            INFO(command);
            const auto chords = keys->chordsFor(command);
            REQUIRE_FALSE(chords.empty());
            CHECK(trainer.press(chords.front().key, static_cast<int>(chords.front().mods.bits())) == QStringLiteral("correct"));
            trainer.next();
        }
        CHECK(trainer.state() == QStringLiteral("finished"));
        CHECK(trainer.correct() == trainer.roundLength());
    }
}

TEST_CASE("Keyboard Trainer M5n in the window: the nine rounds side by side; screenshot",
          "[app][onboarding][a11y][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    shell.preferences->setProperty("welcomeDone", true);
    auto* trainer = shell.engine.singletonInstance<KeyboardTrainerBridge*>("Wabcut", "Trainer");
    REQUIRE(trainer);
    trainer->setSeed(2);
    QObject* onboarding = shell.object("onboarding");
    QObject* dialog = shell.object("keyboardTrainer");
    REQUIRE(onboarding);
    REQUIRE(dialog);
    REQUIRE(call(onboarding, "run", QStringLiteral("trainer")).toBool());
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
    QQuickItem* tabs = findNamed(popupItem(dialog), QStringLiteral("trainerCategories"));
    REQUIRE(tabs);
    CHECK(tabs->property("count").toInt() == 9);
    // Each tab fits its label (nine side by side).
    const double segment = tabs->property("segmentWidth").toDouble();
    CHECK(segment >= 72);
    // The multicam round: the tab (as an arrow key picks it), then Start.
    REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 8)));
    CHECK(dialog->property("category").toString() == QStringLiteral("multicam"));
    QQuickItem* start = findNamed(popupItem(dialog), QStringLiteral("trainerStartButton"));
    REQUIRE(start);
    REQUIRE(QMetaObject::invokeMethod(start, "clicked"));
    REQUIRE(trainer->state() == QStringLiteral("asking"));
    CHECK(trainer->promptCommand().startsWith(QStringLiteral("multicam.")));
    CHECK_ACCESSIBLE(popupItem(dialog));
    spin(150);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, popupItem(dialog), QStringLiteral("keyboard-trainer-categories.png"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    checkNoMessages();
}

TEST_CASE("What's new M5n: this build's highlights, each one spoken; screenshot", "[app][onboarding][a11y][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    shell.preferences->setProperty("welcomeDone", true);
    QObject* onboarding = shell.object("onboarding");
    QObject* dialog = shell.object("whatsNewDialog");
    REQUIRE(onboarding);
    REQUIRE(dialog);
    const QVariantList highlights = dialog->property("highlights").toList();
    QStringList keys;
    for (const QVariant& h : highlights) {
        keys << h.toMap().value(QStringLiteral("key")).toString();
        CHECK_FALSE(h.toMap().value(QStringLiteral("title")).toString().isEmpty());
        CHECK_FALSE(h.toMap().value(QStringLiteral("text")).toString().isEmpty());
    }
    CHECK(keys == QStringList{QStringLiteral("gamingStart"), QStringLiteral("highlights"), QStringLiteral("layouts"),
                              QStringLiteral("callouts"), QStringLiteral("moments"), QStringLiteral("gamingExport"),
                              QStringLiteral("multicam"), QStringLiteral("library"), QStringLiteral("captions"),
                              QStringLiteral("essentialSound"), QStringLiteral("mixer"), QStringLiteral("voiceOver"),
                              QStringLiteral("channels"), QStringLiteral("history"), QStringLiteral("paste"),
                              QStringLiteral("extensions"), QStringLiteral("ai")});

    REQUIRE(call(onboarding, "run", QStringLiteral("whatsNew")).toBool());
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
    QQuickItem* list = findNamed(popupItem(dialog), QStringLiteral("whatsNewList"));
    REQUIRE(list);
    CHECK(list->property("count").toInt() == highlights.size());
    QQuickItem* first = findNamed(popupItem(dialog), QStringLiteral("whatsNew_gamingStart"));
    REQUIRE(first);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(first);
    REQUIRE(iface);
    CHECK(iface->role() == QAccessible::ListItem);
    CHECK(iface->text(QAccessible::Name).startsWith(QStringLiteral("Made for gaming videos. ")));
    CHECK_ACCESSIBLE(popupItem(dialog));
    spin(150);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, popupItem(dialog), QStringLiteral("whats-new.png"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    checkNoMessages();
}

TEST_CASE("Welcome M5n: AI on your terms says where each AI feature runs, consent and the no-cloud lock; screenshot",
          "[app][onboarding][a11y][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    QObject* welcome = shell.object("welcomeDialog");
    REQUIRE(welcome);
    CHECK(welcome->property("pageCount").toInt() == 5);
    call(welcome, "begin", 0);
    REQUIRE(waitFor([&] { return welcome->property("opened").toBool(); }, 5000));
    call(welcome, "goTo", 3);
    REQUIRE(welcome->property("page").toInt() == 3);
    QQuickItem* popup = popupItem(welcome);
    REQUIRE(popup);
    QQuickItem* page = findNamed(popup, QStringLiteral("welcomeAiPage"));
    REQUIRE(page);
    REQUIRE(waitFor([&] { return page->isVisible(); }, 3000));

    // One row per feature, spoken with where it runs.
    const std::map<QString, QString> where{
        {QStringLiteral("transcript"), QStringLiteral("Local or your key")},
        {QStringLiteral("assistant"), QStringLiteral("Local or your key")},
        {QStringLiteral("enhance"), QStringLiteral("On this computer")},
        {QStringLiteral("captions"), QStringLiteral("Local or your key")},
        {QStringLiteral("search"), QStringLiteral("On this computer")},
        {QStringLiteral("multicam"), QStringLiteral("On this computer")},
    };
    for (const auto& [key, place] : where) {
        INFO(key.toStdString());
        QQuickItem* row = findNamed(page, QStringLiteral("welcomeAi_") + key);
        REQUIRE(row);
        CHECK(row->isVisible());
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(row);
        REQUIRE(iface);
        CHECK(iface->role() == QAccessible::ListItem);
        CHECK(iface->text(QAccessible::Name).contains(QStringLiteral(": ") + place + QStringLiteral(". ")));
    }
    QQuickItem* privacy = findNamed(page, QStringLiteral("welcomeAiPrivacy"));
    REQUIRE(privacy);
    const QString note = privacy->property("text").toString();
    CHECK(note.contains(QStringLiteral("consent")));
    CHECK(note.contains(QStringLiteral("no-cloud lock")));
    CHECK_ACCESSIBLE(popup);
    // Next goes on to the last page (the tour, the trainer).
    QQuickItem* next = findNamed(popup, QStringLiteral("welcomeNextButton"));
    REQUIRE(next);
    CHECK(next->isVisible());
    spin(150);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, popup, QStringLiteral("tour-ai-privacy.png"));
    REQUIRE(QMetaObject::invokeMethod(next, "clicked"));
    CHECK(welcome->property("page").toInt() == 4);
    call(welcome, "finish", QStringLiteral("done"));
    REQUIRE(waitFor([&] { return !welcome->property("visible").toBool(); }, 3000));
    checkNoMessages();
}

TEST_CASE("A11y M5n: extensions, Audio Channels, the 5.1 panner, the track format menu and the object mask controls",
          "[app][a11y][extensions][channels][masks][qml]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setSelection({tl.clip("A1@0")});
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(library);

    // Settings > Extensions (none installed in the tests' data folder): Rescan and Install from folder.
    {
        std::unique_ptr<QQuickItem> host;
        QObject* dialog = openHostedDialog(shell, host,
                                           "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: manager\n"
                                           "    ExtensionsManager { id: manager }\n}\n");
        CHECK_ACCESSIBLE(popupItem(dialog));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    }
    // Modify > Audio Channels on the selected audio clip: the format tabs, presets, the grid's checks and mutes.
    {
        const auto host = makeItem(shell,
                                   "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: channels\n"
                                   "    AudioChannelsDialog { id: channels }\n}\n",
                                   QRectF(0, 0, kShotWidth, kShotHeight));
        auto* dialog = host->property("dialog").value<QObject*>();
        REQUIRE(dialog);
        REQUIRE(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QStringLiteral("clips")),
                                          Q_ARG(QVariant, QVariantList{clipKey(tl.clip("A1@0"))})));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        CHECK_ACCESSIBLE(popupItem(dialog));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    }
    // The 5.1 panner: the puck and the LFE send.
    {
        const auto panner = makeItem(shell, "import Wabcut\nSurroundPanner { width: 220; height: 260; label: \"A1\" }\n",
                                     QRectF(0, 0, 220, 260));
        spin(50);
        CHECK_ACCESSIBLE(panner.get());
    }
    // The object mask controls (AI-14) in Effect Controls' masks section.
    {
        const auto controls = makeItem(shell, "import Wabcut\nObjectMaskControls {}\n", sceneRect(library));
        spin(50);
        CHECK_ACCESSIBLE(controls.get());
    }
    checkNoMessages();
}
