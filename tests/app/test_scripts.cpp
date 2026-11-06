// 0.7 (card 050, batch C8): Python scripts in the app (File > Run Python Script…, docs/python.md), offscreen: the script header,
// the permissions remembered per script content, finding Python at run time, a real script run against the open project over
// the private local socket - ONE undo step when it ends cleanly, nothing when it fails or is stopped, only what the user allowed -
// and the dialog's three steps as screenshots (dark and light). The scripts run in a real Python (found like the app does;
// the tests are skipped with a message when there is none) and import the package from the source tree.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "ProjectController.h"
#include "ScriptRun.h"
#include "ScriptSupport.h"
#include "ScriptsBridge.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;
namespace sc = wabcut::app::scripts;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
)";

QString writeScript(const QTemporaryDir& dir, const QString& name, const QByteArray& text) {
    const QString path = dir.filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(text);
    return path;
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

QQuickItem* inDialog(QObject* dialog, const QString& name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    return findNamed(content->parentItem() ? content->parentItem() : content, name);
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

std::unique_ptr<QQuickItem> makeDialogHost(Shell& shell) {
    QQmlComponent component(&shell.engine);
    component.setData("import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: d\n    ScriptsDialog { id: d }\n}\n",
                      QUrl(QStringLiteral("qrc:/c8-scripts/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setSize(shell.window->contentItem()->size());
    return std::unique_ptr<QQuickItem>(item);
}

QString outputText(ScriptsBridge* scripts) {
    auto* model = qobject_cast<ScriptOutputModel*>(scripts->output());
    REQUIRE(model);
    QStringList lines;
    for (int row = 0; row < model->rowCount(); ++row)
        lines << model->data(model->index(row), ScriptOutputModel::TextRole).toString();
    return lines.join(QLatin1Char('\n'));
}

/// The open project, the Scripts bridge pointed at a scratch folder and the package in the source tree, and a Python.
struct ScriptsRig {
    QTemporaryDir root;
    Shell shell;
    Timeline tl{kPicture};
    ProjectController* controller = nullptr;
    ScriptsBridge* scripts = nullptr;
    sc::PythonRuntime python;
    fs::path project;

    explicit ScriptsRig(bool grantFile = true) {
        REQUIRE(root.isValid());
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        scripts = shell.engine.singletonInstance<ScriptsBridge*>("Wabcut", "Scripts");
        REQUIRE(controller);
        REQUIRE(scripts);
        const fs::path base(root.path().toStdU16String());
        project = base / "film" / "film.wabcut";
        fs::create_directories(project.parent_path());
        controller->resetProject(tl.state, tl.seq);
        controller->setProjectFile(project);
        scripts->setPaths(grantFile ? root.filePath(QStringLiteral("script-permissions.json")) : QString(),
                          QStringLiteral(WABCUT_PYTHON_FOLDER), QString());
        python = sc::locatePython(QString());
        if (python.ok())
            scripts->setPythonOverride(python.program);
        spin(100);
    }
    bool havePython() const { return python.ok(); }
    ProjectSnapshot state() const { return controller->snapshot(); }
    std::size_t markers() const { return state()->sequences.front()->markers.size(); }
    std::size_t undoSteps() const { return controller->session().undoCount(); }

    /// Chooses the script, allows everything it asks for, runs it and waits for the end.
    void run(const QString& path, bool waitForEnd = true) {
        REQUIRE(scripts->choose(QUrl::fromLocalFile(path)).isEmpty());
        REQUIRE(scripts->state() == QLatin1String("ready"));
        REQUIRE(scripts->run().isEmpty());
        REQUIRE(scripts->state() == QLatin1String("running"));
        if (waitForEnd)
            REQUIRE(waitFor([&] { return scripts->state() == QLatin1String("finished"); }, 90000));
    }
};

#define REQUIRE_PYTHON(rig) \
    if (!(rig).havePython())  \
    SKIP("no Python 3.11 or newer was found (put python3 on the PATH or set WABCUT_PYTHON): the script runs did not happen")

constexpr const char* kEditScript = R"(# wabcut-name: Test chapters
# wabcut-description: Two markers and a new name.
# wabcut-permissions: Read, EditSequence
import wabcut

print("hello from the script")
with wabcut.connect() as session:
    session.marker.add(time="00:00:01:00", name="One")
    session.marker.add(time="00:00:02:00", name="Two")
    session.clip.rename(clips=[{"track": "V1", "time": 0}], name="Renamed")
print("done")
)";

} // namespace

TEST_CASE("Script headers: the name, the description and the permissions a script asks for", "[app][scripts]") {
    const sc::ScriptHeader h = sc::parseScriptHeader(
        "#!/usr/bin/env python\n# -*- coding: utf-8 -*-\n# wabcut-name:  Add chapters \n# wabcut-description: One per row.\n"
        "# Wabcut-Permissions: editproject, Read ,EditSequence; Files, Network, nonsense\nimport wabcut\n");
    CHECK(h.name == QStringLiteral("Add chapters"));
    CHECK(h.description == QStringLiteral("One per row."));
    CHECK(h.requested == QStringList{QStringLiteral("Read"), QStringLiteral("EditSequence"), QStringLiteral("EditProject"),
                                     QStringLiteral("Files")}); // the dialog's order, whatever the header's
    CHECK(h.unknown == QStringList{QStringLiteral("Network"), QStringLiteral("nonsense")}); // never granted

    CHECK(sc::parseScriptHeader("import wabcut\n").requested.isEmpty());
    // Only the first 60 lines count, and only comment lines.
    QString late;
    for (int i = 0; i < 70; ++i)
        late += QStringLiteral("x = 1\n");
    late += QStringLiteral("# wabcut-permissions: EditProject\n");
    CHECK(sc::parseScriptHeader(late).requested.isEmpty());
    CHECK(sc::parseScriptHeader(QStringLiteral("wabcut-permissions: EditProject\n")).requested.isEmpty());
    // A name cannot carry control characters or text direction overrides (it is shown in the dialog and in the undo step).
    const sc::ScriptHeader odd = sc::parseScriptHeader(QStringLiteral("# wabcut-name: Safe\u202Etxt.exe\u0001\n"));
    CHECK(odd.name == QStringLiteral("Safetxt.exe"));
    CHECK(sc::parseScriptHeader(QStringLiteral("# wabcut-name: ") + QString(500, QLatin1Char('x'))).name.size() == 80);
}

TEST_CASE("Script permissions are remembered for the exact content, written atomically, and a damaged file asks again",
          "[app][scripts]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("sub/script-permissions.json"));
    const QString hash = sc::contentHash("print(1)\n");
    CHECK(hash.size() == 64);
    CHECK(hash != sc::contentHash("print(2)\n"));
    {
        sc::ScriptGrantStore store(file);
        CHECK(store.grantsFor(hash).isEmpty());
        store.remember(hash, QStringLiteral("C:/scripts/a.py"), {QStringLiteral("EditSequence"), QStringLiteral("Read"), QStringLiteral("Bogus")});
        store.addRecent(QStringLiteral("a.py"));
        store.addRecent(QStringLiteral("b.py"));
        store.addRecent(QStringLiteral("a.py"));
        store.setPythonPath(QStringLiteral("C:/Python312/python.exe"));
        REQUIRE(store.save());
    }
    sc::ScriptGrantStore again(file);
    CHECK(again.grantsFor(hash) == QStringList{QStringLiteral("Read"), QStringLiteral("EditSequence")}); // known names only
    CHECK(again.grantsFor(sc::contentHash("print(2)\n")).isEmpty()); // an edited script asks again
    CHECK(again.recent() == QStringList{QStringLiteral("a.py"), QStringLiteral("b.py")});
    CHECK(again.pythonPath() == QStringLiteral("C:/Python312/python.exe"));
    CHECK_FALSE(QFile::exists(file + QStringLiteral(".new"))); // the temporary file was renamed over it
    again.forget(hash);
    CHECK(again.grantsFor(hash).isEmpty());

    QFile damaged(file);
    REQUIRE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
    damaged.write("{not json");
    damaged.close();
    sc::ScriptGrantStore broken(file);
    CHECK(broken.grantsFor(hash).isEmpty());
    CHECK(broken.recent().isEmpty());

    sc::ScriptGrantStore many(dir.filePath(QStringLiteral("many.json")));
    for (int i = 0; i < 30; ++i)
        many.addRecent(QStringLiteral("s%1.py").arg(i));
    CHECK(many.recent().size() == 12);
    CHECK(many.recent().first() == QStringLiteral("s29.py"));
}

TEST_CASE("Python 3.11 or newer is looked for at run time, and a wrong choice says what is wrong", "[app][scripts]") {
    CHECK(sc::versionAtLeast(QStringLiteral("3.11.0"), 3, 11));
    CHECK(sc::versionAtLeast(QStringLiteral("3.14.6"), 3, 11));
    CHECK(sc::versionAtLeast(QStringLiteral("4.0.0"), 3, 11));
    CHECK_FALSE(sc::versionAtLeast(QStringLiteral("3.10.9"), 3, 11));
    CHECK_FALSE(sc::versionAtLeast(QStringLiteral("2.7.18"), 3, 11));
    CHECK_FALSE(sc::versionAtLeast(QStringLiteral("garbage"), 3, 11));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const sc::PythonRuntime missing = sc::locatePython(dir.filePath(QStringLiteral("no-such-python.exe")));
    CHECK_FALSE(missing.ok());
    CHECK(missing.error.contains(QStringLiteral("no-such-python"))); // it names what it tried
    const QString notPython = writeScript(dir, QStringLiteral("not-python.txt"), "this is a text file");
    CHECK_FALSE(sc::locatePython(notPython).ok());

    const sc::PythonRuntime found = sc::locatePython(QString());
    if (found.ok()) {
        CHECK(sc::versionAtLeast(found.version, 3, 11));
        CHECK(found.display().startsWith(QStringLiteral("Python ")));
    } else {
        INFO(found.error.toStdString());
        CHECK(found.error.contains(QStringLiteral("python.org"))); // says what to do
    }
}

TEST_CASE("Scripts: a script that edits the project is applied as ONE undo step, and only what it asks for", "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QString path = writeScript(r.root, QStringLiteral("chapters.py"), kEditScript);

    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(path)).isEmpty());
    CHECK(r.scripts->scriptName() == QStringLiteral("Test chapters"));
    CHECK(r.scripts->scriptDescription() == QStringLiteral("Two markers and a new name."));
    CHECK(r.scripts->canRun());
    const QVariantList permissions = r.scripts->permissions();
    REQUIRE(permissions.size() == 5);
    CHECK(permissions[0].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("Read"));
    CHECK(permissions[0].toMap().value(QStringLiteral("chosen")).toBool());
    CHECK(permissions[0].toMap().value(QStringLiteral("fixed")).toBool());
    CHECK(permissions[1].toMap().value(QStringLiteral("requested")).toBool());  // EditSequence: asked for ...
    CHECK(permissions[1].toMap().value(QStringLiteral("chosen")).toBool());     // ... and on by default
    CHECK_FALSE(permissions[2].toMap().value(QStringLiteral("requested")).toBool()); // EditProject: not asked for
    CHECK_FALSE(permissions[2].toMap().value(QStringLiteral("chosen")).toBool());
    r.scripts->setChosen(QStringLiteral("EditProject"), true); // not asked for: cannot be switched on
    CHECK_FALSE(r.scripts->permissions()[2].toMap().value(QStringLiteral("chosen")).toBool());

    const std::size_t steps = r.undoSteps();
    REQUIRE(r.scripts->run().isEmpty());
    CHECK(r.scripts->running());
    REQUIRE(waitFor([&] { return r.scripts->state() == QLatin1String("finished"); }, 90000));
    INFO(outputText(r.scripts).toStdString());
    CHECK(r.scripts->succeeded());
    CHECK(r.scripts->editCount() == 3);
    CHECK(r.scripts->commandCount() >= 3);
    CHECK(outputText(r.scripts).contains(QStringLiteral("hello from the script")));
    CHECK(outputText(r.scripts).contains(QStringLiteral("done")));
    CHECK(r.scripts->summary().contains(QStringLiteral("3 edit"))); // "3 edits" (or "3 edit(s)" without the English plurals)

    // ONE undo step with the script's name; everything the script did is in it.
    CHECK(r.undoSteps() == steps + 1);
    CHECK(r.controller->session().undoLabel() == std::string("Script: Test chapters"));
    CHECK(r.markers() == 2);
    CHECK(r.state()->sequences.front()->videoTracks.front()->clips.front().name() == "Renamed");
    REQUIRE(r.controller->undo());
    CHECK(r.markers() == 0);
    CHECK(r.state()->sequences.front()->videoTracks.front()->clips.front().name() == "A");
    CHECK(r.undoSteps() == steps);

    // The audit trail: a line when the run starts and ends, one per command, never what the command was given.
    QFile audit(r.root.filePath(QStringLiteral("script-audit.jsonl")));
    REQUIRE(audit.open(QIODevice::ReadOnly));
    const QString auditText = QString::fromUtf8(audit.readAll());
    const QStringList auditLines = auditText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(auditLines.size() >= 5); // start, three commands, end
    CHECK(auditLines.first().contains(QStringLiteral("\"event\":\"start\"")));
    CHECK(auditLines.last().contains(QStringLiteral("\"event\":\"end\"")));
    CHECK(auditText.contains(QStringLiteral("\"tool\":\"marker_add\"")));
    CHECK(auditText.contains(QStringLiteral("\"tool\":\"clip_rename\"")));
    CHECK(auditText.contains(QStringLiteral("\"script\":\"Test chapters\"")));
    CHECK_FALSE(auditText.contains(QStringLiteral("Renamed"))); // parameters are never recorded
    CHECK_FALSE(auditText.contains(QStringLiteral("00:00:01:00")));

    // Remembered for this exact script; the recent list has it.
    CHECK(r.scripts->recent().size() == 1);
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(path)).isEmpty());
    CHECK(r.scripts->permissions()[1].toMap().value(QStringLiteral("note")).toString() == QStringLiteral("Allowed before"));
}

TEST_CASE("Scripts: a script without the permission is refused by Wabcut, and a switch left off is not granted",
          "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QByteArray script = R"(# wabcut-permissions: Read, EditSequence
import wabcut
with wabcut.connect() as session:
    print("may edit:", session.access.edit)
    try:
        session.marker.add(time="00:00:01:00", name="Nope")
    except wabcut.PermissionDeniedError as error:
        print("refused:", error.code)
    try:
        session.track.setVolume(track="A1", db=-3)
    except wabcut.CommandError as error:
        print("also refused:", error.code)
)";
    const QString path = writeScript(r.root, QStringLiteral("denied.py"), script);
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(path)).isEmpty());
    r.scripts->setChosen(QStringLiteral("EditSequence"), false); // the user leaves it off
    const std::size_t steps = r.undoSteps();
    REQUIRE(r.scripts->run().isEmpty());
    REQUIRE(waitFor([&] { return r.scripts->state() == QLatin1String("finished"); }, 90000));
    const QString text = outputText(r.scripts);
    INFO(text.toStdString());
    CHECK(text.contains(QStringLiteral("may edit: False")));
    CHECK(text.contains(QStringLiteral("refused: PermissionDenied")));
    CHECK(text.contains(QStringLiteral("also refused: PermissionDenied")));
    CHECK(r.scripts->succeeded()); // the script handled it and ended cleanly
    CHECK(r.scripts->summary().contains(QStringLiteral("no changes")));
    CHECK(r.undoSteps() == steps);
    CHECK(r.markers() == 0);
}

TEST_CASE("Scripts: a script that fails leaves the project exactly as it was", "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QByteArray script = R"(# wabcut-permissions: Read, EditSequence
import sys, wabcut
session = wabcut.connect()
session.marker.add(time="00:00:01:00", name="Half done")
session.marker.add(time="00:00:02:00", name="Half done too")
print("about to fail", flush=True)
sys.exit(3)
)";
    const std::size_t steps = r.undoSteps();
    const ProjectSnapshot before = r.state();
    r.run(writeScript(r.root, QStringLiteral("fails.py"), script));
    INFO(outputText(r.scripts).toStdString());
    CHECK_FALSE(r.scripts->succeeded());
    CHECK(r.scripts->summary().contains(QStringLiteral("exit code 3")));
    CHECK(r.scripts->summary().contains(QStringLiteral("Nothing was changed")));
    CHECK(outputText(r.scripts).contains(QStringLiteral("about to fail")));
    CHECK(r.undoSteps() == steps);
    CHECK(r.state().get() == before.get()); // not even a new snapshot
    CHECK(r.markers() == 0);
}

TEST_CASE("Scripts: an error in the script shows its traceback and changes nothing", "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QByteArray script = R"(# wabcut-permissions: Read, EditSequence
import wabcut
with wabcut.connect() as session:
    session.marker.add(time="00:00:01:00", name="Before the error")
    session.marker.add(time="not a time", name="Bad")
)";
    r.run(writeScript(r.root, QStringLiteral("error.py"), script));
    const QString text = outputText(r.scripts);
    INFO(text.toStdString());
    CHECK_FALSE(r.scripts->succeeded());
    CHECK(text.contains(QStringLiteral("Traceback")));
    CHECK(text.contains(QStringLiteral("InvalidParamsError")));
    CHECK(r.markers() == 0);
}

TEST_CASE("Scripts: Stop ends a running script and discards what it did", "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QByteArray script = R"(# wabcut-permissions: Read, EditSequence
import time, wabcut
with wabcut.connect() as session:
    session.marker.add(time="00:00:01:00", name="Lost")
    print("started", flush=True)
    time.sleep(120)
)";
    const std::size_t steps = r.undoSteps();
    r.run(writeScript(r.root, QStringLiteral("long.py"), script), false);
    REQUIRE(waitFor([&] { return outputText(r.scripts).contains(QStringLiteral("started")); }, 90000));
    CHECK(r.scripts->running());
    r.scripts->cancel();
    REQUIRE(waitFor([&] { return r.scripts->state() == QLatin1String("finished"); }, 30000));
    CHECK_FALSE(r.scripts->succeeded());
    CHECK(r.scripts->summary().contains(QStringLiteral("Stopped")));
    CHECK(r.markers() == 0);
    CHECK(r.undoSteps() == steps);
}

TEST_CASE("Scripts: edits made some other way while a script runs are never overwritten", "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QByteArray script = R"(# wabcut-permissions: Read, EditSequence
import time, wabcut
with wabcut.connect() as session:
    session.marker.add(time="00:00:01:00", name="From the script")
    print("waiting", flush=True)
    time.sleep(2)
)";
    r.run(writeScript(r.root, QStringLiteral("slow.py"), script), false);
    REQUIRE(waitFor([&] { return outputText(r.scripts).contains(QStringLiteral("waiting")); }, 90000));
    command::CommandEngine engine(r.controller->session());
    command::CommandRequest edit;
    edit.command = "marker.add";
    edit.params = command::Json{{"time", "00:00:09:00"}, {"name", "From the user"}};
    REQUIRE(engine.execute(edit).ok());
    REQUIRE(waitFor([&] { return r.scripts->state() == QLatin1String("finished"); }, 60000));
    CHECK_FALSE(r.scripts->succeeded());
    CHECK(r.scripts->summary().contains(QStringLiteral("changed while the script ran")));
    CHECK(r.markers() == 1); // the user's marker only
    CHECK(r.state()->sequences.front()->markers.front().name == "From the user");
}

TEST_CASE("Scripts: a transaction inside the script rolls back its own part; file commands need the Files permission",
          "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    const QByteArray script = R"(# wabcut-permissions: Read, EditSequence
import wabcut
with wabcut.connect() as session:
    session.marker.add(time="00:00:01:00", name="Kept")
    try:
        with session.transaction("Part"):
            session.marker.add(time="00:00:02:00", name="Rolled back")
            raise ValueError("no")
    except ValueError:
        pass
    print("markers:", [m["name"] for m in session.marker.list()["markers"]])
    try:
        session.interchange.export(path="x.xml")
    except wabcut.PermissionDeniedError as error:
        print("files:", error.client_side)
)";
    r.run(writeScript(r.root, QStringLiteral("parts.py"), script));
    const QString text = outputText(r.scripts);
    INFO(text.toStdString());
    CHECK(r.scripts->succeeded());
    CHECK(text.contains(QStringLiteral("markers: ['Kept']")));
    CHECK(text.contains(QStringLiteral("files: True")));
    CHECK(r.markers() == 1);
}

TEST_CASE("Scripts: the script's environment has its own connection, never Wabcut's keys; a script that does not use Wabcut changes nothing",
          "[app][scripts][qml]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    qputenv("WABCUT_TEST_PROVIDER_API_KEY", "super-secret-value");
    qputenv("WABCUT_TEST_HARMLESS", "kept");
    const QByteArray script = R"(import os, sys
print("key:", os.environ.get("WABCUT_TEST_PROVIDER_API_KEY"))
print("harmless:", os.environ.get("WABCUT_TEST_HARMLESS"))
print("token length:", len(os.environ.get("WABCUT_TOKEN", "")))
print("session set:", bool(os.environ.get("WABCUT_SESSION")))
print("script:", os.path.basename(os.environ.get("WABCUT_SCRIPT", "")))
print("package:", any(p.replace("\\", "/").endswith("/python") for p in sys.path))
print("cwd:", os.path.basename(os.getcwd()))
)";
    const std::size_t steps = r.undoSteps();
    r.run(writeScript(r.root, QStringLiteral("environment.py"), script));
    qunsetenv("WABCUT_TEST_PROVIDER_API_KEY");
    qunsetenv("WABCUT_TEST_HARMLESS");
    const QString text = outputText(r.scripts);
    INFO(text.toStdString());
    CHECK(text.contains(QStringLiteral("key: None"))); // a credential-named WABCUT_ variable never reaches a script
    CHECK(text.contains(QStringLiteral("harmless: kept")));
    CHECK(text.contains(QStringLiteral("token length: 32")));
    CHECK(text.contains(QStringLiteral("session set: True")));
    CHECK(text.contains(QStringLiteral("script: environment.py")));
    CHECK(text.contains(QStringLiteral("package: True")));
    CHECK(r.scripts->succeeded());
    CHECK(r.scripts->summary().contains(QStringLiteral("no changes")));
    CHECK(r.undoSteps() == steps);
}

TEST_CASE("Scripts: without Python the dialog says what to do and nothing runs", "[app][scripts][qml]") {
    ScriptsRig r;
    const QString path = writeScript(r.root, QStringLiteral("any.py"), "print('hi')\n");
    r.scripts->setPythonOverride(r.root.filePath(QStringLiteral("no-python-here")));
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(path)).isEmpty());
    CHECK_FALSE(r.scripts->pythonOk());
    CHECK(r.scripts->pythonChecked());
    CHECK_FALSE(r.scripts->canRun());
    CHECK(r.scripts->pythonText().contains(QStringLiteral("no-python-here")));
    const QString why = r.scripts->run();
    CHECK_FALSE(why.isEmpty());
    CHECK(r.scripts->state() == QLatin1String("ready"));
}

TEST_CASE("Scripts: only a readable .py file can be chosen", "[app][scripts]") {
    ScriptsRig r;
    CHECK_FALSE(r.scripts->choose(QUrl::fromLocalFile(r.root.filePath(QStringLiteral("missing.py")))).isEmpty());
    CHECK(r.scripts->state() == QLatin1String("idle"));
    const QString text = writeScript(r.root, QStringLiteral("notes.txt"), "hello");
    CHECK(r.scripts->choose(QUrl::fromLocalFile(text)).contains(QStringLiteral(".py")));
    const QString big = writeScript(r.root, QStringLiteral("big.py"), QByteArray(3 * 1024 * 1024, '#'));
    CHECK(r.scripts->choose(QUrl::fromLocalFile(big)).contains(QStringLiteral("2 MB")));
    CHECK(r.scripts->state() == QLatin1String("idle"));
    CHECK(r.scripts->problem().contains(QStringLiteral("2 MB")));
}

TEST_CASE("Scripts dialog: choose, review the permissions, run; screenshots in the dark theme", "[app][scripts][qml][screenshot]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    r.shell.setAppearance(true, "ultraviolet");
    // A recent script, so the first step has something to show.
    const QString recent = writeScript(r.root, QStringLiteral("recent chapters.py"), kEditScript);
    const QString asks = writeScript(r.root, QStringLiteral("asks.py"), R"(# wabcut-name: Transcribe and tidy
# wabcut-description: Transcribes the sequence and removes the filler words.
# wabcut-permissions: Read, EditSequence, MediaAnalysis, Files, Network
print("x")
)");
    r.run(recent);
    r.scripts->reset();
    const auto host = makeDialogHost(r.shell);
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "openDialog"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));

    // 1. Choose.
    CHECK(inDialog(dialog, QStringLiteral("scriptsChoose"))->isVisible());
    CHECK_FALSE(inDialog(dialog, QStringLiteral("scriptsReview"))->isVisible());
    CHECK(inDialog(dialog, QStringLiteral("scriptsRecent_0"))->isVisible());
    shoot(r.shell, dialog, "scripts-dialog-1-choose-dark.png");

    // 2. Review: only what the script asks for, as switches; Network is ignored with a note.
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(asks)).isEmpty());
    REQUIRE(waitFor([&] { return inDialog(dialog, QStringLiteral("scriptsReview"))->isVisible(); }));
    CHECK(inDialog(dialog, QStringLiteral("scriptPermission_EditSequence"))->isVisible());
    CHECK(inDialog(dialog, QStringLiteral("scriptPermission_MediaAnalysis"))->isVisible());
    CHECK(inDialog(dialog, QStringLiteral("scriptPermission_Files"))->isVisible());
    CHECK_FALSE(inDialog(dialog, QStringLiteral("scriptPermission_EditProject"))->isVisible());
    CHECK(inDialog(dialog, QStringLiteral("scriptsReviewLead"))->property("text").toString().contains(QStringLiteral("Transcribe and tidy")));
    CHECK(inDialog(dialog, QStringLiteral("scriptsPython"))->property("text").toString().startsWith(QStringLiteral("Python ")));
    CHECK(inDialog(dialog, QStringLiteral("scriptsRunButton"))->isEnabled());
    shoot(r.shell, dialog, "scripts-dialog-2-review-dark.png");

    // 3. Run: output streams into the log, then the summary.
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(recent)).isEmpty());
    REQUIRE(r.scripts->run().isEmpty());
    REQUIRE(waitFor([&] { return r.scripts->state() == QLatin1String("finished"); }, 90000));
    CHECK(inDialog(dialog, QStringLiteral("scriptsRun"))->isVisible());
    CHECK(inDialog(dialog, QStringLiteral("scriptsStatus"))->property("text").toString().contains(QStringLiteral("one undo step")));
    CHECK(inDialog(dialog, QStringLiteral("scriptsClose"))->isVisible());
    CHECK_FALSE(inDialog(dialog, QStringLiteral("scriptsStop"))->isVisible());
    shoot(r.shell, dialog, "scripts-dialog-3-finished-dark.png");
}

TEST_CASE("Scripts dialog: the same three steps in the light theme; screenshots", "[app][scripts][qml][screenshot]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    r.shell.setAppearance(false, "ultraviolet");
    const QString script = writeScript(r.root, QStringLiteral("light chapters.py"), kEditScript);
    const QString asks = writeScript(r.root, QStringLiteral("asks.py"), R"(# wabcut-name: Transcribe and tidy
# wabcut-description: Transcribes the sequence and removes the filler words.
# wabcut-permissions: Read, EditSequence, MediaAnalysis, Files
print("x")
)");
    r.run(script);
    r.scripts->reset();
    const auto host = makeDialogHost(r.shell);
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "openDialog"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    shoot(r.shell, dialog, "scripts-dialog-1-choose-light.png");
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(asks)).isEmpty());
    REQUIRE(waitFor([&] { return inDialog(dialog, QStringLiteral("scriptsReview"))->isVisible(); }));
    shoot(r.shell, dialog, "scripts-dialog-2-review-light.png");
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(script)).isEmpty());
    REQUIRE(r.scripts->run().isEmpty());
    REQUIRE(waitFor([&] { return r.scripts->state() == QLatin1String("finished"); }, 90000));
    shoot(r.shell, dialog, "scripts-dialog-3-finished-light.png");
}

TEST_CASE("Scripts dialog: at the smallest window size nothing runs off the window; long lists scroll", "[app][scripts][qml][screenshot]") {
    ScriptsRig r;
    REQUIRE_PYTHON(r);
    r.shell.resize(1100, 700); // the main window's minimum
    r.shell.setAppearance(true, "ultraviolet");
    // Twelve recent scripts, as many as the list keeps (the grant file is read when the paths are set).
    const QString store = r.root.filePath(QStringLiteral("script-permissions.json"));
    {
        sc::ScriptGrantStore seed(store);
        for (int i = 0; i < 12; ++i)
            seed.addRecent(writeScript(r.root, QStringLiteral("recent script %1.py").arg(i), "print('x')\n"));
        REQUIRE(seed.save());
    }
    r.scripts->setPaths(store, QStringLiteral(WABCUT_PYTHON_FOLDER), QString());
    const QString everything = writeScript(r.root, QStringLiteral("everything.py"), R"(# wabcut-name: A script that asks for everything
# wabcut-description: It edits sequences and the project, analyses media and uses files, so the review has every row.
# wabcut-permissions: Read, EditSequence, EditProject, MediaAnalysis, Files, Network
print("x")
)");
    const auto host = makeDialogHost(r.shell);
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "openDialog"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));

    const auto insideWindow = [&] {
        spin(200);
        auto* content = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        QQuickItem* card = content->parentItem() ? content->parentItem() : content;
        const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
        INFO("card " << area.x() << "," << area.y() << " " << area.width() << "x" << area.height() << " in a window of "
                     << r.shell.window->width() << "x" << r.shell.window->height());
        CHECK(area.top() >= 0);
        CHECK(area.bottom() <= r.shell.window->height());
        CHECK(area.left() >= 0);
        CHECK(area.right() <= r.shell.window->width());
    };
    // Choose: twelve recent scripts in a list that scrolls.
    QQuickItem* recent = inDialog(dialog, QStringLiteral("scriptsRecentList"));
    REQUIRE(recent);
    CHECK(recent->isVisible());
    insideWindow();
    shoot(r.shell, dialog, "scripts-dialog-narrow-choose-dark.png");

    // Review: every permission row.
    REQUIRE(r.scripts->choose(QUrl::fromLocalFile(everything)).isEmpty());
    REQUIRE(waitFor([&] { return inDialog(dialog, QStringLiteral("scriptsReview"))->isVisible(); }));
    for (const char* key : {"EditSequence", "EditProject", "MediaAnalysis", "Files"})
        CHECK(inDialog(dialog, QStringLiteral("scriptPermission_") + QLatin1String(key))->isVisible());
    insideWindow();
    shoot(r.shell, dialog, "scripts-dialog-narrow-review-dark.png");
    CHECK(inDialog(dialog, QStringLiteral("scriptsRunButton"))->isVisible());
}
