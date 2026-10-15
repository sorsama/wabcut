// M5n: the extensions (C.12) in the app, offscreen: the Extensions manager (rows, statuses, actions, the log), the
// permission dialog and the trust dialog of a project extension, extension commands in the command palette and on
// shortcuts (an unavailable one keeps its key), ui.prompt forms and ui.notify / ui.toast, and the crash badge. The
// extensions live in a temporary folder and run in the real wabcut-scripthost next to the test (the crash test in the
// misbehaving wabcut_fake_scripthost of the script suite). No window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "CommandPalette.h"
#include "ExtensionsBridge.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "ShortcutsBridge.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/script/Host.h>
#include <wabcut/script/Manager.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
)";

// A test extension: a notification, a toast and a form (every field type).
constexpr const char* kNotesScript = R"JS(
wabcut.ui.registerCommand('hello', 'Say Hello', () => {
    console.log('saying hello');
    wabcut.ui.notify('Hello from the tests', {level: 'info'});
    return {ok: true};
});
wabcut.ui.registerCommand('ask', 'Ask for Markers', () => wabcut.ui.prompt({
    title: 'Markers',
    message: 'How many markers, and where?',
    submitLabel: 'Add markers',
    fields: [
        {id: 'count', type: 'number', label: 'Count', value: 2, min: 1, max: 10, step: 1},
        {id: 'name', type: 'text', label: 'Name', value: 'Beat', maxLength: 40, placeholder: 'Prefix'},
        {id: 'all', type: 'checkbox', label: 'Every sequence', value: false},
        {id: 'every', type: 'choice', label: 'Mark', value: 'beat',
         options: [{value: 'beat', label: 'Every beat'}, {value: 'bar', label: 'Every bar'}]},
        {id: 'out', type: 'text', label: 'Chapters', value: '0:00 Intro\n0:42 Interview', multiline: true, readOnly: true},
    ],
}).then((a) => {
    wabcut.ui.toast(a ? `Got ${a.count} ${a.name}` : 'Cancelled');
    return a;
}));
)JS";

void writeFile(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

void writeExtension(const fs::path& folder, const std::string& id, const std::string& name,
                    const std::string& capabilities, const std::string& commands, const std::string& script) {
    writeFile(folder / "wabcut-extension.json",
              "{\"manifestVersion\": 1, \"id\": \"" + id + "\", \"name\": \"" + name +
                  "\", \"version\": \"1.2.0\", \"author\": \"Wabcut tests\", \"licence\": \"MIT\", "
                  "\"description\": \"A test extension.\", \"minWabcutVersion\": \"0.1.0\", \"capabilities\": [" +
                  capabilities + "], \"main\": \"main.js\", \"commands\": [" + commands + "]}");
    writeFile(folder / "main.js", script);
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

/// Inside a dialog (a Popup): its content item's tree.
QQuickItem* inDialog(QObject* dialog, const QString& name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    return findNamed(content->parentItem() ? content->parentItem() : content, name);
}

std::unique_ptr<QObject> makeQml(Shell& shell, const QByteArray& qml) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5n-extensions/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    return object;
}

std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml) {
    std::unique_ptr<QObject> object = makeQml(shell, qml);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setSize(shell.window->contentItem()->size());
    return std::unique_ptr<QQuickItem>(item);
}

void click(QObject* button) {
    REQUIRE(button);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
}

/// Crops a dialog's card (with a margin) out of the window and saves it.
void shoot(Shell& shell, QObject* dialog, const char* file) {
    spin(300);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* card = content->parentItem() ? content->parentItem() : content;
    const double dpr = image.devicePixelRatio();
    const double margin = 16;
    const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                            .adjusted(-margin, -margin, margin, margin)
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QString::fromLatin1(file))));
}

/// A switch flipped the way a click does (the repo's tests: toggle, then toggled).
void flip(QQuickItem* toggle) {
    REQUIRE(toggle);
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
}

QVariantMap rowOf(ExtensionsBridge* bridge, const QString& id) {
    for (const QVariant& v : bridge->extensions()) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("id")).toString() == id)
            return row;
    }
    return {};
}

QString statusOf(ExtensionsBridge* bridge, const QString& id) {
    return rowOf(bridge, id).value(QStringLiteral("status")).toString();
}

struct Notice {
    QString source, level, text;
};

struct ExtensionsRig {
    QTemporaryDir root;
    std::vector<Notice> notices; // before the shell: the bridge may still notify while it goes
    Shell shell;
    Timeline tl{kPicture};
    ProjectController* controller = nullptr;
    ExtensionsBridge* extensions = nullptr;
    KeymapDispatcher* keys = nullptr;
    script::ExtensionPaths paths;
    fs::path project;

    explicit ExtensionsRig(bool fakeHelper = false) {
        REQUIRE(root.isValid());
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        extensions = shell.engine.singletonInstance<ExtensionsBridge*>("Wabcut", "Extensions");
        keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
        REQUIRE(controller);
        REQUIRE(extensions);
        REQUIRE(keys);
        const fs::path base(root.path().toStdU16String());
        project = base / "film" / "film.wabcut";
        fs::create_directories(project.parent_path());
        controller->resetProject(tl.state, tl.seq);
        controller->setProjectFile(project);
        paths = script::extensionPathsUnder(base / "appdata");
        if (fakeHelper)
            paths.helper = fs::path((QCoreApplication::applicationDirPath() + QStringLiteral("/wabcut_fake_scripthost")
#ifdef _WIN32
                                     + QStringLiteral(".exe")
#endif
                                         )
                                        .toStdU16String());
        QObject::connect(extensions, &ExtensionsBridge::notified,
                         [this](const QString& s, const QString& l, const QString& t) { notices.push_back({s, l, t}); });
        spin(100);
    }
    void use() { extensions->setPaths(paths); }
    fs::path user(const std::string& id) const { return paths.userExtensions / id; }
    bool waitStatus(const QString& id, const QString& status, int ms = 20000) {
        if (waitFor([&] { return statusOf(extensions, id) == status; }, ms))
            return true;
        const QVariantMap row = rowOf(extensions, id);
        QStringList log;
        for (const QVariant& line : extensions->log(id))
            log << line.toMap().value(QStringLiteral("message")).toString();
        WARN(id.toStdString() << ": status " << row.value(QStringLiteral("status")).toString().toStdString() << ", reason "
                              << row.value(QStringLiteral("reason")).toString().toStdString() << ", log "
                              << log.join(QStringLiteral(" | ")).toStdString() << ", message "
                              << extensions->message().toStdString());
        return false;
    }
    bool waitNotice(const QString& text, int ms = 20000) {
        return waitFor([&] {
            return std::ranges::any_of(notices, [&](const Notice& n) { return n.text == text; });
        }, ms);
    }
};

constexpr const char* kNotesCommands =
    R"({"id": "hello", "title": "Say Hello"}, {"id": "ask", "title": "Ask for Markers"})";

} // namespace

TEST_CASE("Extensions manager: rows and statuses, the permission dialog, enable, disable, revoke, the log; screenshots",
          "[app][extensions][qml][screenshot]") {
    resetSettings();
    clearMessages();
    ExtensionsRig r;
    writeExtension(r.user("com.test.notes"), "com.test.notes", "Note Taker", R"("Read", "EditSequence")",
                   kNotesCommands, kNotesScript);
    writeFile(r.user("com.test.broken") / "wabcut-extension.json", R"({"manifestVersion": 1, "id": "com.test.broken"})");
    writeExtension(script::projectExtensionsRoot(r.project) / "com.test.shared", "com.test.shared", "Shared Chapters",
                   R"("Read")", R"({"id": "chapters", "title": "Chapters"})", "wabcut.ui.registerCommand('chapters', 'Chapters', () => 1);");
    r.use();

    // The rows: user extensions first, then the project's.
    REQUIRE(r.extensions->count() == 3);
    const QVariantMap notes = rowOf(r.extensions, QStringLiteral("com.test.notes"));
    CHECK(notes.value(QStringLiteral("name")).toString() == QStringLiteral("Note Taker"));
    CHECK(notes.value(QStringLiteral("version")).toString() == QStringLiteral("1.2.0"));
    CHECK(notes.value(QStringLiteral("author")).toString() == QStringLiteral("Wabcut tests"));
    CHECK(notes.value(QStringLiteral("licence")).toString() == QStringLiteral("MIT"));
    CHECK(notes.value(QStringLiteral("status")).toString() == QStringLiteral("needsReview"));
    CHECK(notes.value(QStringLiteral("badge")).toString() == QStringLiteral("Review needed"));
    CHECK(notes.value(QStringLiteral("commands")).toList().size() == 2);
    CHECK_FALSE(notes.value(QStringLiteral("project")).toBool());
    const QVariantMap broken = rowOf(r.extensions, QStringLiteral("invalid:com.test.broken"));
    CHECK(broken.value(QStringLiteral("name")).toString() == QStringLiteral("com.test.broken"));
    CHECK(broken.value(QStringLiteral("badge")).toString() == QStringLiteral("Broken"));
    CHECK_FALSE(broken.value(QStringLiteral("reason")).toString().isEmpty());
    const QVariantMap shared = rowOf(r.extensions, QStringLiteral("com.test.shared"));
    CHECK(shared.value(QStringLiteral("project")).toBool());
    CHECK(shared.value(QStringLiteral("status")).toString() == QStringLiteral("needsReview"));
    CHECK(shared.value(QStringLiteral("needsTrust")).toBool());
    CHECK(shared.value(QStringLiteral("badge")).toString() == QStringLiteral("Not trusted"));
    CHECK(r.extensions->extensions().last().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("com.test.shared"));
    CHECK(r.extensions->paletteCommands().isEmpty()); // nothing runs before it is reviewed

    // The manager.
    const auto host = makeItem(r.shell, "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: m\n"
                                        "    ExtensionsManager { id: m }\n}\n");
    auto* manager = host->property("dialog").value<QObject*>();
    REQUIRE(manager);
    REQUIRE(QMetaObject::invokeMethod(manager, "open"));
    REQUIRE(waitFor([&] { return manager->property("opened").toBool(); }));
    // The rows are rebuilt when the list changes: find them again before each use.
    const auto inRow = [&](const char* id, const char* name) {
        QQuickItem* row = inDialog(manager, QStringLiteral("extensionRow_") + QLatin1String(id));
        INFO(id << " " << name);
        REQUIRE(row);
        QQuickItem* item = findNamed(row, QString::fromLatin1(name));
        REQUIRE(item);
        return item;
    };
    CHECK(inDialog(manager, QStringLiteral("extensionRow_com.test.shared")));
    CHECK(inRow("com.test.notes", "extensionReview")->isVisible());
    CHECK_FALSE(inRow("com.test.notes", "extensionDisable")->isVisible());

    // Review…: the permission dialog, one switch per capability (on by default); EditSequence is left off.
    click(inRow("com.test.notes", "extensionReview"));
    auto* permissions = manager->property("permissionDialog").value<QObject*>();
    REQUIRE(permissions);
    REQUIRE(waitFor([&] { return permissions->property("opened").toBool(); }));
    CHECK(inDialog(permissions, QStringLiteral("extensionPermissionsLead"))->property("text").toString()
              == QStringLiteral("Allow <b>Note Taker</b> to:"));
    QQuickItem* read = inDialog(permissions, QStringLiteral("extensionCapability_Read"));
    QQuickItem* edit = inDialog(permissions, QStringLiteral("extensionCapabilitySwitch_EditSequence"));
    REQUIRE(read);
    REQUIRE(edit);
    CHECK(edit->property("checked").toBool());
    CHECK(inDialog(permissions, QStringLiteral("extensionPermissionsAuthor"))->property("text").toString()
              == QStringLiteral("By Wabcut tests · Licence: MIT · Version 1.2.0"));
    CHECK_FALSE(inDialog(permissions, QStringLiteral("extensionTrustWarning"))->isVisible());
    flip(edit);
    CHECK_FALSE(edit->property("checked").toBool());
    shoot(r.shell, permissions, "extension-permissions.png");

    // Allow: it runs with Read only, and is marked as asking for more.
    click(inDialog(permissions, QStringLiteral("extensionPermissionsAllow")));
    REQUIRE(waitFor([&] { return !permissions->property("visible").toBool(); }));
    REQUIRE(r.waitStatus(QStringLiteral("com.test.notes"), QStringLiteral("running")));
    QVariantMap on = rowOf(r.extensions, QStringLiteral("com.test.notes"));
    CHECK(on.value(QStringLiteral("badge")).toString() == QStringLiteral("On"));
    REQUIRE(on.value(QStringLiteral("granted")).toList().size() == 1);
    CHECK(on.value(QStringLiteral("granted")).toList().front().toMap().value(QStringLiteral("capability")).toString()
          == QStringLiteral("Read"));
    CHECK(on.value(QStringLiteral("needsReview")).toBool());
    REQUIRE(r.extensions->paletteCommands().size() == 2);

    // The project's extension: Trust… shows where the code comes from; Trust and enable runs it.
    click(inRow("com.test.shared", "extensionTrust"));
    REQUIRE(waitFor([&] { return permissions->property("opened").toBool(); }));
    CHECK(inDialog(permissions, QStringLiteral("extensionTrustWarning"))->isVisible());
    CHECK(inDialog(permissions, QStringLiteral("extensionPermissionsAllow"))->property("text").toString()
          == QStringLiteral("Trust and enable"));
    click(inDialog(permissions, QStringLiteral("extensionPermissionsAllow")));
    REQUIRE(r.waitStatus(QStringLiteral("com.test.shared"), QStringLiteral("running")));
    CHECK(r.extensions->distrust(QStringLiteral("com.test.shared")).isEmpty());
    CHECK(statusOf(r.extensions, QStringLiteral("com.test.shared")) == QStringLiteral("needsTrust"));
    REQUIRE(r.extensions->trustAndEnable(QStringLiteral("com.test.shared"), {QStringLiteral("Read")}).isEmpty());
    REQUIRE(r.waitStatus(QStringLiteral("com.test.shared"), QStringLiteral("running")));

    // The log.
    REQUIRE(r.extensions->run(QStringLiteral("ext:com.test.notes/hello")).isEmpty());
    REQUIRE(r.waitNotice(QStringLiteral("Hello from the tests")));
    REQUIRE(waitFor([&] { return !r.extensions->log(QStringLiteral("com.test.notes")).isEmpty(); }));
    click(inRow("com.test.notes", "extensionShowLog"));
    CHECK(inRow("com.test.notes", "extensionLog")->isVisible());
    shoot(r.shell, manager, "extensions-manager.png");
    CHECK(inRow("com.test.notes", "extensionLog")->isVisible()); // still open after the list changed

    // Disable, then Revoke (every grant back, off).
    click(inRow("com.test.notes", "extensionDisable"));
    REQUIRE(r.waitStatus(QStringLiteral("com.test.notes"), QStringLiteral("disabled")));
    CHECK(rowOf(r.extensions, QStringLiteral("com.test.notes")).value(QStringLiteral("badge")).toString() == QStringLiteral("Off"));
    CHECK(r.extensions->paletteCommands().size() == 1); // the shared extension's
    REQUIRE(r.extensions->enable(QStringLiteral("com.test.notes"), {QStringLiteral("Read"), QStringLiteral("EditSequence")}).isEmpty());
    REQUIRE(r.waitStatus(QStringLiteral("com.test.notes"), QStringLiteral("running")));
    CHECK_FALSE(rowOf(r.extensions, QStringLiteral("com.test.notes")).value(QStringLiteral("needsReview")).toBool());
    CHECK(r.extensions->revoke(QStringLiteral("com.test.notes")).isEmpty());
    const QVariantMap revoked = rowOf(r.extensions, QStringLiteral("com.test.notes"));
    CHECK(revoked.value(QStringLiteral("granted")).toList().isEmpty());
    CHECK(revoked.value(QStringLiteral("status")).toString() != QStringLiteral("running"));

    // Uninstall asks first, then removes the folder.
    click(inRow("com.test.notes", "extensionUninstall"));
    CHECK(inRow("com.test.notes", "extensionConfirmUninstall")->isVisible());
    click(inRow("com.test.notes", "extensionConfirmUninstall"));
    REQUIRE(waitFor([&] { return rowOf(r.extensions, QStringLiteral("com.test.notes")).isEmpty(); }));
    CHECK_FALSE(fs::exists(r.user("com.test.notes")));

    REQUIRE(QMetaObject::invokeMethod(manager, "close"));
    REQUIRE(waitFor([&] { return !manager->property("visible").toBool(); }));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Extensions: install from a folder after the permission dialog", "[app][extensions][qml]") {
    resetSettings();
    clearMessages();
    ExtensionsRig r;
    r.use();
    const fs::path source = fs::path(r.root.path().toStdU16String()) / "downloads" / "beat-markers";
    writeExtension(source, "com.test.beats", "Beat Helper", R"("Read")", R"({"id": "go", "title": "Go"})",
                   "wabcut.ui.registerCommand('go', 'Go', () => 1);");
    const auto host = makeItem(r.shell, "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: m\n"
                                        "    ExtensionsManager { id: m }\n}\n");
    auto* manager = host->property("dialog").value<QObject*>();
    REQUIRE(manager);
    REQUIRE(QMetaObject::invokeMethod(manager, "open"));
    REQUIRE(waitFor([&] { return manager->property("opened").toBool(); }));
    CHECK(inDialog(manager, QStringLiteral("extensionsEmpty"))->isVisible());
    const QUrl folder = QUrl::fromLocalFile(QString::fromStdU16String(source.u16string()));
    REQUIRE(QMetaObject::invokeMethod(manager, "installFrom", Q_ARG(QVariant, folder)));
    auto* permissions = manager->property("permissionDialog").value<QObject*>();
    REQUIRE(permissions);
    REQUIRE(waitFor([&] { return permissions->property("opened").toBool(); }));
    CHECK(permissions->property("title").toString() == QStringLiteral("Install extension"));
    CHECK(inDialog(permissions, QStringLiteral("extensionPermissionsAllow"))->property("text").toString()
          == QStringLiteral("Install and allow"));
    click(inDialog(permissions, QStringLiteral("extensionPermissionsAllow")));
    REQUIRE(r.waitStatus(QStringLiteral("com.test.beats"), QStringLiteral("running")));
    CHECK(fs::exists(r.user("com.test.beats") / "wabcut-extension.json"));
    CHECK(fs::exists(r.user("com.test.beats") / "main.js"));
    CHECK_FALSE(inDialog(manager, QStringLiteral("extensionsEmpty"))->isVisible());
    // A folder that is not an extension: the dialog says why and Allow is off.
    REQUIRE(QMetaObject::invokeMethod(manager, "installFrom",
                                      Q_ARG(QVariant, QUrl::fromLocalFile(QString::fromStdU16String(
                                                          (fs::path(r.root.path().toStdU16String()) / "downloads").u16string())))));
    REQUIRE(waitFor([&] { return permissions->property("opened").toBool(); }));
    CHECK_FALSE(inDialog(permissions, QStringLiteral("extensionPermissionsAllow"))->isEnabled());
    CHECK_FALSE(inDialog(permissions, QStringLiteral("extensionPermissionsError"))->property("text").toString().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(permissions, "close"));
    REQUIRE(QMetaObject::invokeMethod(manager, "close"));
    spin(200);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Extension commands: the palette's Extensions group, a shortcut, an unavailable command keeps its key",
          "[app][extensions][palette]") {
    resetSettings();
    clearMessages();
    ExtensionsRig r;
    writeExtension(r.user("com.test.notes"), "com.test.notes", "Note Taker", R"("Read")", kNotesCommands, kNotesScript);
    r.use();
    REQUIRE(r.extensions->enable(QStringLiteral("com.test.notes"), {QStringLiteral("Read")}).isEmpty());
    REQUIRE(r.waitStatus(QStringLiteral("com.test.notes"), QStringLiteral("running")));
    const QString hello = QStringLiteral("ext:com.test.notes/hello");

    // The palette: an "Extensions" group with the command and Manage Extensions….
    auto* palette = r.shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(palette);
    palette->setPersistRecent(false);
    palette->setQuery(QStringLiteral("say hello"));
    QVariantMap row;
    for (const QVariant& v : palette->results()) {
        if (v.toMap().value(QStringLiteral("key")).toString() == QStringLiteral("extension:") + hello)
            row = v.toMap();
    }
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value(QStringLiteral("title")).toString() == QStringLiteral("Note Taker: Say Hello"));
    CHECK(row.value(QStringLiteral("group")).toString() == QStringLiteral("Extensions"));
    CHECK(palette->activate(QStringLiteral("extension:") + hello).isEmpty());
    REQUIRE(r.waitNotice(QStringLiteral("Hello from the tests")));
    CHECK(r.notices.back().source == QStringLiteral("Note Taker"));
    CHECK(r.notices.back().level == QStringLiteral("info"));
    bool managerAsked = false;
    QObject guard; // after managerAsked: the connection goes first
    QObject::connect(r.extensions, &ExtensionsBridge::managerRequested, &guard, [&] { managerAsked = true; });
    CHECK(palette->activate(QStringLiteral("extension:manage")).isEmpty());
    CHECK(managerAsked);

    // A shortcut: assignable in the editor's list (Extensions group, available), the key runs it.
    auto* shortcuts = r.shell.engine.singletonInstance<ShortcutsBridge*>("Wabcut", "Shortcuts");
    REQUIRE(shortcuts);
    const int index = shortcuts->commands()->indexOf(hello);
    REQUIRE(index >= 0);
    const QVariantMap entry = shortcuts->commands()->get(index);
    CHECK(entry.value(QStringLiteral("group")).toString() == QStringLiteral("Extensions"));
    CHECK(entry.value(QStringLiteral("name")).toString() == QStringLiteral("Note Taker: Say Hello"));
    CHECK(entry.value(QStringLiteral("available")).toBool());
    CHECK(entry.value(QStringLiteral("shortcuts")).toStringList().isEmpty()); // no default key
    const QVariantMap assigned = r.keys->assignShortcut(hello, QStringLiteral("Ctrl+Alt+Shift+H"));
    INFO(assigned.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(assigned.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(r.keys->shortcutText(hello).isEmpty());
    const std::size_t before = r.notices.size();
    CHECK(r.keys->trigger(hello));
    REQUIRE(waitFor([&] { return r.notices.size() > before; }, 20000));

    // Turned off: the command is listed as unavailable and keeps its key; on again, it is back.
    CHECK(r.extensions->disable(QStringLiteral("com.test.notes")).isEmpty());
    CHECK_FALSE(r.keys->isExtraCommandAvailable(hello));
    CHECK_FALSE(r.keys->shortcutText(hello).isEmpty());
    const int gone = shortcuts->commands()->indexOf(hello);
    REQUIRE(gone >= 0);
    CHECK_FALSE(shortcuts->commands()->get(gone).value(QStringLiteral("available")).toBool());
    CHECK(shortcuts->commands()->get(gone).value(QStringLiteral("name")).toString().contains(QStringLiteral("unavailable")));
    const std::size_t off = r.notices.size();
    CHECK(r.keys->trigger(hello)); // its key still reaches it: it says why nothing runs
    REQUIRE(waitFor([&] { return r.notices.size() > off; }));
    CHECK(r.notices.back().level == QStringLiteral("error"));
    REQUIRE(r.extensions->enable(QStringLiteral("com.test.notes"), {QStringLiteral("Read")}).isEmpty());
    CHECK(r.keys->isExtraCommandAvailable(hello));
    CHECK_FALSE(r.keys->shortcutText(hello).isEmpty());

    // The key is saved with the user's shortcuts; a restart without the extension keeps it.
    CHECK(r.keys->resetShortcut(hello));
    CHECK(r.keys->shortcutText(hello).isEmpty());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Extension forms: ui.prompt answered and cancelled, values checked against the form; ui.toast; screenshot",
          "[app][extensions][qml][screenshot]") {
    resetSettings();
    clearMessages();
    ExtensionsRig r;
    writeExtension(r.user("com.test.notes"), "com.test.notes", "Note Taker", R"("Read")", kNotesCommands, kNotesScript);
    r.use();
    REQUIRE(r.extensions->enable(QStringLiteral("com.test.notes"), {QStringLiteral("Read")}).isEmpty());
    REQUIRE(r.waitStatus(QStringLiteral("com.test.notes"), QStringLiteral("running")));
    const auto host = makeItem(r.shell, "import QtQuick\nimport Wabcut\nExtensionPromptHost {}\n");

    REQUIRE(r.extensions->run(QStringLiteral("ext:com.test.notes/ask")).isEmpty());
    REQUIRE(waitFor([&] { return host->property("count").toInt() == 1; }, 20000));
    QVariant value;
    REQUIRE(QMetaObject::invokeMethod(host.get(), "dialogAt", Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, 0)));
    QObject* form = value.value<QObject*>();
    REQUIRE(form);
    REQUIRE(waitFor([&] { return form->property("opened").toBool(); }));
    CHECK(form->property("title").toString() == QStringLiteral("Note Taker: Markers"));
    CHECK_FALSE(form->property("modal").toBool());
    CHECK(inDialog(form, QStringLiteral("extensionPromptMessage"))->property("text").toString()
          == QStringLiteral("How many markers, and where?"));
    QQuickItem* count = inDialog(form, QStringLiteral("extensionNumber_count"));
    QQuickItem* name = inDialog(form, QStringLiteral("extensionInput_name"));
    QQuickItem* every = inDialog(form, QStringLiteral("extensionChoice_every"));
    QQuickItem* all = inDialog(form, QStringLiteral("extensionCheck_all"));
    QQuickItem* out = inDialog(form, QStringLiteral("extensionArea_out"));
    REQUIRE(count);
    REQUIRE(name);
    REQUIRE(every);
    REQUIRE(all);
    REQUIRE(out);
    CHECK(count->isVisible());
    CHECK(count->property("value").toDouble() == 2.0);
    CHECK(name->property("text").toString() == QStringLiteral("Beat"));
    CHECK(every->property("text").toString() == QStringLiteral("Every beat"));
    CHECK_FALSE(all->property("checked").toBool());
    CHECK(out->property("readOnly").toBool());
    CHECK(inDialog(form, QStringLiteral("extensionCopy_out"))->isVisible());
    CHECK(inDialog(form, QStringLiteral("extensionPromptSubmit"))->property("text").toString() == QStringLiteral("Add markers"));
    shoot(r.shell, form, "extension-prompt.png");

    // A count above the form's maximum is refused: the form stays open and says so.
    REQUIRE(QMetaObject::invokeMethod(form, "setValue", Q_ARG(QVariant, QStringLiteral("count")), Q_ARG(QVariant, 99)));
    click(inDialog(form, QStringLiteral("extensionPromptSubmit")));
    CHECK(form->property("opened").toBool());
    CHECK(inDialog(form, QStringLiteral("extensionPromptError"))->isVisible());
    CHECK(r.extensions->prompts().size() == 1);

    // Good answers: the extension gets them (and toasts them).
    REQUIRE(QMetaObject::invokeMethod(form, "setValue", Q_ARG(QVariant, QStringLiteral("count")), Q_ARG(QVariant, 4)));
    name->setProperty("text", QStringLiteral("Downbeat"));
    flip(all);
    click(inDialog(form, QStringLiteral("extensionPromptSubmit")));
    REQUIRE(r.waitNotice(QStringLiteral("Got 4 Downbeat")));
    CHECK(r.notices.back().level == QStringLiteral("toast"));
    REQUIRE(waitFor([&] { return host->property("count").toInt() == 0; }));
    CHECK(r.extensions->prompts().isEmpty());

    // Cancel answers null.
    REQUIRE(r.extensions->run(QStringLiteral("ext:com.test.notes/ask")).isEmpty());
    REQUIRE(waitFor([&] { return host->property("count").toInt() == 1; }, 20000));
    REQUIRE(QMetaObject::invokeMethod(host.get(), "dialogAt", Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, 0)));
    form = value.value<QObject*>();
    REQUIRE(form);
    REQUIRE(waitFor([&] { return form->property("opened").toBool(); }));
    click(inDialog(form, QStringLiteral("extensionPromptCancel")));
    REQUIRE(r.waitNotice(QStringLiteral("Cancelled")));
    REQUIRE(waitFor([&] { return host->property("count").toInt() == 0; }));

    // An extension that stops closes its open forms.
    REQUIRE(r.extensions->run(QStringLiteral("ext:com.test.notes/ask")).isEmpty());
    REQUIRE(waitFor([&] { return host->property("count").toInt() == 1; }, 20000));
    CHECK(r.extensions->disable(QStringLiteral("com.test.notes")).isEmpty());
    REQUIRE(waitFor([&] { return host->property("count").toInt() == 0; }));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Extension crashes: the badge, the toast, and three in a row turn it off", "[app][extensions]") {
    resetSettings();
    clearMessages();
    ExtensionsRig r(true); // wabcut_fake_scripthost: the "crash" mode aborts on the first command
    if (!fs::exists(r.paths.helper))
        SKIP("wabcut_fake_scripthost is not built (the script suite builds it)");
    writeExtension(r.user("com.test.crash"), "com.test.crash", "Crashy", R"("Read")", R"({"id": "go", "title": "Go"})", "// fake");
    r.use();
    struct Fault {
        QString id, name, reason;
        bool disabled = false;
    };
    std::vector<Fault> faults;
    QObject guard; // after faults: the connection goes first
    QObject::connect(r.extensions, &ExtensionsBridge::faulted, &guard,
                     [&](const QString& id, const QString& name, const QString& reason, bool disabled) {
                         faults.push_back({id, name, reason, disabled});
                     });
    REQUIRE(r.extensions->enable(QStringLiteral("com.test.crash"), {QStringLiteral("Read")}).isEmpty());
    REQUIRE(r.waitStatus(QStringLiteral("com.test.crash"), QStringLiteral("running")));
    for (int crash = 1; crash <= 3; ++crash) {
        INFO("crash " << crash);
        REQUIRE(r.extensions->run(QStringLiteral("ext:com.test.crash/go")).isEmpty());
        REQUIRE(waitFor([&] { return static_cast<int>(faults.size()) >= crash; }, 20000));
        CHECK(faults.back().name == QStringLiteral("Crashy"));
        if (crash < 3) {
            CHECK_FALSE(faults.back().disabled);
            const QVariantMap row = rowOf(r.extensions, QStringLiteral("com.test.crash"));
            CHECK(row.value(QStringLiteral("crashed")).toBool());
            CHECK(row.value(QStringLiteral("crashes")).toInt() == crash);
            REQUIRE(waitFor([&] { return statusOf(r.extensions, QStringLiteral("com.test.crash")) == QStringLiteral("running")
                                         || statusOf(r.extensions, QStringLiteral("com.test.crash")) == QStringLiteral("idle"); }));
        }
    }
    CHECK(faults.back().disabled);
    REQUIRE(r.waitStatus(QStringLiteral("com.test.crash"), QStringLiteral("disabled")));
    const QVariantMap off = rowOf(r.extensions, QStringLiteral("com.test.crash"));
    CHECK(off.value(QStringLiteral("badge")).toString() == QStringLiteral("Off"));
    CHECK_FALSE(off.value(QStringLiteral("reason")).toString().isEmpty());
    // Enabling again resets the count.
    REQUIRE(r.extensions->enable(QStringLiteral("com.test.crash"), {QStringLiteral("Read")}).isEmpty());
    CHECK(rowOf(r.extensions, QStringLiteral("com.test.crash")).value(QStringLiteral("crashes")).toInt() == 0);
    takeMessages();
}

namespace {

/// The thread the fake model command last ran on.
std::atomic<QThread*> g_slowThread{nullptr};

/// "test.slow": a stand-in for an AI-model command (runsModel): `steps` checkpoints with progress, 10 ms each, then a
/// marker named "Slow" at the sequence's start.
command::CommandRegistry slowRegistry() {
    command::CommandRegistry registry;
    command::CommandSpec spec;
    spec.name = "test.slow";
    spec.title = "Slow Markers";
    spec.description = "A slow edit for the tests.";
    spec.capability = command::Capability::EditSequence;
    spec.runsModel = true;
    spec.params = command::Json{{"type", "object"},
                                {"properties", {{"sequence", {{"type", "integer"}}}, {"steps", {{"type", "integer"}}}}}};
    spec.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        g_slowThread = QThread::currentThread();
        const std::int64_t steps = c.integer("steps", 10);
        for (std::int64_t i = 0; i < steps; ++i) {
            c.checkpoint("Thinking (" + std::to_string(i * 100 / steps) + "%)");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Marker marker;
        marker.name = "Slow";
        edit::EditResult edited = c.take(edit::addMarker(c.state(), SequenceId{static_cast<std::uint64_t>(c.integer("sequence"))},
                                                         marker));
        command::CommandOutput out;
        out.state = std::move(edited.state);
        return out;
    };
    REQUIRE(registry.add(std::move(spec)));
    return registry;
}

script::LongCommand slowJob(std::uint64_t sequence, int steps, std::optional<command::Json>& answer) {
    script::LongCommand job;
    job.document = command::Json{{"command", "test.slow"}, {"params", {{"sequence", sequence}, {"steps", steps}}}};
    job.policy.undoLabelPrefix = "Beat Markers: ";
    job.finish = [&answer](command::Json result) { answer = std::move(result); };
    return job;
}

std::size_t markersNamed(ProjectController* controller, const std::string& name) {
    const SequencePtr seq = controller->currentSequence();
    return static_cast<std::size_t>(std::count_if(seq->markers.begin(), seq->markers.end(),
                                                  [&](const Marker& m) { return m.name == name; }));
}

} // namespace

TEST_CASE("Extension AI-model commands run as command jobs: off the GUI thread, progress, Cancel, one undo step",
          "[app][extensions]") {
    resetSettings();
    clearMessages();
    ExtensionsRig r;
    static const command::CommandRegistry registry = slowRegistry(); // outlives every job
    r.extensions->setLongCommandRegistry(&registry);
    const std::uint64_t seq = r.controller->currentSequenceId().value;

    // A run: the GUI thread keeps turning while the worker thinks; the result is one undo step, the extension's.
    double lastProgress = -1;
    bool sawProgress = false;
    QObject::connect(r.extensions, &ExtensionsBridge::longCommandChanged, r.extensions, [&] {
        const QVariantMap state = r.extensions->longCommand();
        if (state.value(QStringLiteral("running")).toBool() && state.value(QStringLiteral("progress")).toDouble() > 0) {
            sawProgress = true;
            lastProgress = state.value(QStringLiteral("progress")).toDouble();
            // The running job's extension (the queue below runs "Chapters" too).
            CHECK_FALSE(state.value(QStringLiteral("extension")).toString().isEmpty());
            CHECK(state.value(QStringLiteral("text")).toString() == QStringLiteral("Thinking"));
        }
    });
    int ticks = 0;
    QTimer ticker;
    ticker.setInterval(5);
    QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
    ticker.start();
    std::optional<command::Json> answer;
    const std::size_t before = r.controller->session().undoCount();
    REQUIRE(r.extensions->startLongCommand(QStringLiteral("Beat Markers"), slowJob(seq, 20, answer)));
    CHECK(r.extensions->longCommand().value(QStringLiteral("running")).toBool());
    CHECK_FALSE(answer.has_value()); // not inside the call: it waits for the worker
    REQUIRE(waitFor([&] { return answer.has_value(); }, 10000));
    INFO(answer->dump());
    CHECK((*answer)["ok"] == true);
    CHECK(g_slowThread.load() != QCoreApplication::instance()->thread());
    CHECK(ticks > 5);
    CHECK(sawProgress);
    CHECK(lastProgress < 1.0);
    CHECK(r.controller->session().undoCount() == before + 1);
    CHECK(markersNamed(r.controller, "Slow") == 1);
    CHECK(r.controller->session().undoLabel().value_or("").starts_with("Beat Markers: "));
    CHECK_FALSE(r.extensions->longCommand().value(QStringLiteral("running")).toBool());

    // Cancel: stopped at its next checkpoint, nothing committed, the extension told "Cancelled".
    std::optional<command::Json> cancelled;
    REQUIRE(r.extensions->startLongCommand(QStringLiteral("Beat Markers"), slowJob(seq, 500, cancelled)));
    REQUIRE(waitFor([&] { return sawProgress && r.extensions->longCommand().value(QStringLiteral("progress")).toDouble() > 0; }, 5000));
    r.extensions->cancelLongCommand();
    REQUIRE(waitFor([&] { return cancelled.has_value(); }, 10000));
    INFO(cancelled->dump());
    CHECK((*cancelled)["ok"] == false);
    CHECK((*cancelled)["error"]["code"] == "Cancelled");
    CHECK(r.controller->session().undoCount() == before + 1);
    CHECK(markersNamed(r.controller, "Slow") == 1);

    // Two at once: the second waits its turn, then runs.
    std::optional<command::Json> first;
    std::optional<command::Json> second;
    REQUIRE(r.extensions->startLongCommand(QStringLiteral("Beat Markers"), slowJob(seq, 10, first)));
    REQUIRE(r.extensions->startLongCommand(QStringLiteral("Chapters"), slowJob(seq, 10, second)));
    CHECK(r.extensions->longCommand().value(QStringLiteral("queued")).toInt() == 1);
    REQUIRE(waitFor([&] { return first.has_value() && second.has_value(); }, 15000));
    CHECK((*first)["ok"] == true);
    CHECK((*second)["ok"] == true);
    CHECK(markersNamed(r.controller, "Slow") == 3);

    // An edit while it runs: the copy is not committed over it; the extension is asked to run it again.
    std::optional<command::Json> stale;
    REQUIRE(r.extensions->startLongCommand(QStringLiteral("Beat Markers"), slowJob(seq, 30, stale)));
    r.controller->setSelection({r.tl.clip("V1@0")});
    r.controller->deleteSelection();
    REQUIRE(waitFor([&] { return stale.has_value(); }, 10000));
    CHECK((*stale)["ok"] == false);
    CHECK((*stale)["error"]["code"] == "StaleRevision");
    CHECK(markersNamed(r.controller, "Slow") == 3);
    r.extensions->setLongCommandRegistry(nullptr);
}
