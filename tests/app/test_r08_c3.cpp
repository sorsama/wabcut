// 0.7, stream C, batch C3 (app level):
//   * 144 the Library's "Date created" column for sequences, named column layouts, and the column order that is remembered;
//   * 112 the bin nested sequences are filed in (Library bin menu);
//   * 055 New Sequence per Clip / From All Selected Clips for several Library clips;
//   * 143 Reload Footage: a file another program changed is read again in place;
//   * 021 Tab / Shift+Tab / Enter / Shift+Enter between the cells being edited in the Library list (real keys through the
//         window and the keymap);
//   * 003 the Collect Files dialog's Handles field and Sequences checklist, with screenshots (dark and light);
//   * 022 the Media Browser's Ingest Settings with "Copy and Create Proxies", with screenshots.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CollectFilesBridge.h"
#include "EditingBridge.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>
#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* byName(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

void tap(QQuickWindow* window, int code, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent press(QEvent::KeyPress, code, mods);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, mods);
    QCoreApplication::sendEvent(window, &release);
}

QStringList columnIds(const LibraryBridge& library) {
    QStringList out;
    for (const QVariant& c : library.columns())
        out << c.toMap().value(QStringLiteral("id")).toString();
    return out;
}

int columnWidth(const LibraryBridge& library, const QString& id) {
    for (const QVariant& c : library.columns()) {
        if (c.toMap().value(QStringLiteral("id")).toString() == id)
            return c.toMap().value(QStringLiteral("width")).toInt();
    }
    return -1;
}

/// A project from a timeline picture, a Library over it (no settings, a fake analysis scan run in place).
struct Rig {
    Timeline tl;
    ProjectController controller;
    LibraryBridge library;
    EditingBridge editing;

    explicit Rig(std::string_view picture = "    V1 tgt |Aaaa|\n") : tl(picture) {
        controller.resetProject(tl.state, tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
        editing.setController(&controller);
        editing.setLibrary(&library);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    QString cell(const QString& key, LibraryColumn column) const {
        return library.items()->cellText(library.items()->rowOf(key), column);
    }
};

} // namespace

// ---- 144 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Library: the Date created column shows when a sequence was made (144)", "[app][library][v07]") {
    Rig r;
    CHECK_FALSE(columnIds(r.library).contains(QStringLiteral("created"))); // hidden until chosen, like the other extras
    r.library.setColumnVisible(QStringLiteral("created"), true);
    CHECK(columnIds(r.library).contains(QStringLiteral("created")));

    // The sequence the project was opened with has no date (a project saved by an older build): blank, not invented.
    const QString oldKey = sequenceKey(r.tl.seq);
    CHECK(r.cell(oldKey, LibraryColumn::Created).isEmpty());
    CHECK(r.cell(mediaKey(r.tl.media('A')), LibraryColumn::Created).isEmpty()); // media and bins have none

    // A sequence made now is stamped by the session, whichever edit made it.
    SequenceId made;
    REQUIRE(r.controller.session().apply("New Sequence", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        edit::SequenceSettings settings;
        settings.name = "Final cut";
        auto created = edit::addSequence(state, settings);
        if (!created)
            return std::unexpected(created.error());
        made = created->id;
        return created->state;
    }));
    const QString newKey = sequenceKey(made);
    const Sequence* sequence = r.controller.snapshot()->findSequence(made);
    REQUIRE(sequence);
    REQUIRE_FALSE(sequence->createdAt.empty());
    const QDateTime when = QDateTime::fromString(QString::fromStdString(sequence->createdAt), Qt::ISODate);
    REQUIRE(when.isValid());
    CHECK(r.cell(newKey, LibraryColumn::Created) ==
          QDateTime::fromSecsSinceEpoch(when.toSecsSinceEpoch()).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    CHECK(QRegularExpression(QStringLiteral("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}$")).match(r.cell(newKey, LibraryColumn::Created)).hasMatch());
    CHECK(std::abs(when.secsTo(QDateTime::currentDateTimeUtc())) < 300); // made just now

    // Sorting by it puts the newer sequence after the older one, and the other way when descending.
    r.library.sortBy(QStringLiteral("created"));
    CHECK(r.library.items()->rowOf(newKey) > r.library.items()->rowOf(oldKey));
    r.library.sortBy(QStringLiteral("created"));
    CHECK(r.library.items()->rowOf(newKey) < r.library.items()->rowOf(oldKey));
}

TEST_CASE("Library: column layouts are saved, applied and deleted by name (144)", "[app][library][v07]") {
    Rig r;
    for (const char* id : {"scene", "shot", "comment"})
        r.library.setColumnVisible(QString::fromLatin1(id), true);
    r.library.moveColumn(static_cast<int>(columnIds(r.library).indexOf(QStringLiteral("shot"))), 1);
    r.library.setColumnWidth(QStringLiteral("scene"), 333);
    const QStringList logging = columnIds(r.library);
    REQUIRE(logging.size() >= 4);
    CHECK(logging[1] == QStringLiteral("shot"));

    CHECK(r.library.columnPresets().isEmpty());
    CHECK_FALSE(r.library.saveColumnPreset(QStringLiteral("   "))); // a name is needed
    CHECK_FALSE(r.library.message().isEmpty());
    REQUIRE(r.library.saveColumnPreset(QStringLiteral("Logging")));
    CHECK(r.library.columnPresets() == QStringList{QStringLiteral("Logging")});

    // Another arrangement, then the saved one back: the same columns, order and widths.
    r.library.resetColumns();
    CHECK(columnIds(r.library) != logging);
    CHECK(columnWidth(r.library, QStringLiteral("scene")) == -1); // hidden again
    REQUIRE(r.library.applyColumnPreset(QStringLiteral("logging"))); // the name is not case sensitive
    CHECK(columnIds(r.library) == logging);
    CHECK(columnWidth(r.library, QStringLiteral("scene")) == 333);
    CHECK_FALSE(r.library.applyColumnPreset(QStringLiteral("Nothing like it")));

    // The same name saves over it (still one), another name makes a second one.
    r.library.setColumnVisible(QStringLiteral("comment"), false);
    REQUIRE(r.library.saveColumnPreset(QStringLiteral("LOGGING")));
    CHECK(r.library.columnPresets().size() == 1);
    REQUIRE(r.library.saveColumnPreset(QStringLiteral("Delivery")));
    CHECK(r.library.columnPresets() == (QStringList{QStringLiteral("Logging"), QStringLiteral("Delivery")}));
    r.library.resetColumns();
    REQUIRE(r.library.applyColumnPreset(QStringLiteral("Logging")));
    CHECK_FALSE(columnIds(r.library).contains(QStringLiteral("comment"))); // the overwritten layout

    // Delete; and at most kMaxColumnPresets are kept.
    CHECK(r.library.deleteColumnPreset(QStringLiteral("delivery")));
    CHECK_FALSE(r.library.deleteColumnPreset(QStringLiteral("delivery")));
    CHECK(r.library.columnPresets() == QStringList{QStringLiteral("Logging")});
    for (int i = r.library.columnPresets().size(); i < LibraryBridge::kMaxColumnPresets; ++i)
        REQUIRE(r.library.saveColumnPreset(QStringLiteral("Layout %1").arg(i)));
    CHECK_FALSE(r.library.saveColumnPreset(QStringLiteral("One too many")));
    CHECK(r.library.columnPresets().size() == LibraryBridge::kMaxColumnPresets);
    CHECK(r.library.saveColumnPreset(QStringLiteral("Layout 5"))); // replacing one is not adding one
}

TEST_CASE("Library: the column order and the saved layouts come back after a restart (144)", "[app][library][v07]") {
    resetSettings();
    QStringList order;
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, "Wabcut", "LibraryListView"); // loads the module and its singletons
        INFO(component.errorString().toStdString());
        REQUIRE_FALSE(component.isError());
        auto* library = engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        REQUIRE(library);
        for (const char* id : {"scene", "shot", "logNote"})
            library->setColumnVisible(QString::fromLatin1(id), true);
        library->moveColumn(static_cast<int>(columnIds(*library).indexOf(QStringLiteral("logNote"))), 1);
        library->setColumnWidth(QStringLiteral("shot"), 222);
        order = columnIds(*library);
        CHECK(order[1] == QStringLiteral("logNote"));
        REQUIRE(library->saveColumnPreset(QStringLiteral("Logging")));
    }
    {
        QQmlEngine engine; // the next start
        QQmlComponent component(&engine, "Wabcut", "LibraryListView");
        REQUIRE_FALSE(component.isError());
        auto* library = engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        REQUIRE(library);
        CHECK(columnIds(*library) == order); // the chosen set and order are remembered
        CHECK(columnWidth(*library, QStringLiteral("shot")) == 222);
        CHECK(library->columnPresets() == QStringList{QStringLiteral("Logging")});
        library->resetColumns();
        REQUIRE(library->applyColumnPreset(QStringLiteral("Logging")));
        CHECK(columnIds(*library) == order);
    }
    resetSettings();
}

// ---- 112 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Library: a bin can be chosen for nested sequences, one undo step, one bin at most (112)", "[app][library][v07]") {
    Rig r;
    const QString nests = r.library.newBin(QStringLiteral("Nests"));
    const QString other = r.library.newBin(QStringLiteral("Other"));
    REQUIRE_FALSE(nests.isEmpty());
    REQUIRE_FALSE(other.isEmpty());
    CHECK_FALSE(r.library.details(nests).value(QStringLiteral("nestTarget")).toBool());

    const std::size_t before = r.undo();
    REQUIRE(r.library.setNestBin(nests, true));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.session().undoLabel() == "File Nested Sequences in Bin");
    CHECK(r.library.details(nests).value(QStringLiteral("nestTarget")).toBool());
    REQUIRE(r.library.setNestBin(other, true)); // takes it from the first
    CHECK_FALSE(r.library.details(nests).value(QStringLiteral("nestTarget")).toBool());
    CHECK(r.library.details(other).value(QStringLiteral("nestTarget")).toBool());
    REQUIRE(r.library.setNestBin(other, false));
    CHECK_FALSE(r.library.details(other).value(QStringLiteral("nestTarget")).toBool());
    REQUIRE(r.controller.undo()); // and it is all undoable
    CHECK(r.library.details(other).value(QStringLiteral("nestTarget")).toBool());

    // Not a bin key; and a search bin holds nothing.
    CHECK_FALSE(r.library.setNestBin(mediaKey(r.tl.media('A')), true));
    CHECK_FALSE(r.library.message().isEmpty());
    // Nest (the editing command) files the new sequence in the chosen bin.
    command::EnginePolicy policy;
    command::CommandEngine engine(r.controller.session(), command::builtinCommands(), policy);
    command::CommandRequest nest;
    nest.command = "sequence.nest";
    nest.params = command::Json{{"sequence", r.tl.seq.value}, {"clips", command::Json::array({r.tl.clip("V1@0").value})}};
    const command::CommandResult nested = engine.execute(nest);
    REQUIRE(nested.ok());
    const SequenceId nestedId{nested.data["nested"].get<std::uint64_t>()};
    const auto parsed = parseBinKey(other);
    REQUIRE(parsed);
    CHECK(r.controller.snapshot()->findSequence(nestedId)->bin == *parsed);
}

// ---- 055 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Editing: New Sequence per Clip and New Sequence From All Selected Clips for several clips (055)",
          "[app][editing][v07]") {
    Rig r;
    const QStringList keys{mediaKey(r.tl.media('A')), mediaKey(r.tl.media('B')), mediaKey(r.tl.media('C'))};
    const std::size_t sequences = r.controller.snapshot()->sequences.size();
    std::size_t before = r.undo();

    // One sequence per clip, all in one undo step.
    REQUIRE(r.editing.newSequencesFromClips(keys, false).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.snapshot()->sequences.size() == sequences + 3);
    const auto& all = r.controller.snapshot()->sequences;
    CHECK(all[all.size() - 3]->name == "A");
    CHECK(all[all.size() - 2]->name == "B");
    CHECK(all[all.size() - 1]->name == "C");
    REQUIRE(r.controller.undo());
    CHECK(r.controller.snapshot()->sequences.size() == sequences);

    // All in one.
    before = r.undo();
    REQUIRE(r.editing.newSequencesFromClips(keys, true).isEmpty());
    CHECK(r.undo() == before + 1);
    REQUIRE(r.controller.snapshot()->sequences.size() == sequences + 1);
    CHECK(r.controller.snapshot()->sequences.back()->videoTracks[0]->clips.size() == 3);

    // Nothing selected, or bins and sequences only: a message and no change.
    before = r.undo();
    CHECK_FALSE(r.editing.newSequencesFromClips({}, false).isEmpty());
    CHECK_FALSE(r.editing.newSequencesFromClips({sequenceKey(r.tl.seq)}, true).isEmpty());
    CHECK(r.undo() == before);

    // The commands: they need two or more clips selected in the Library, and say so.
    r.library.select(keys[0], LibraryBridge::Replace);
    const QVariantMap one = r.editing.commandAvailability(QStringLiteral("file.newSequencesFromClips"));
    CHECK(one.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(one.value(QStringLiteral("enabled")).toBool());
    CHECK_FALSE(one.value(QStringLiteral("reason")).toString().isEmpty());
    r.library.select(keys[1], LibraryBridge::Toggle);
    CHECK(r.editing.commandAvailability(QStringLiteral("file.newSequencesFromClips")).value(QStringLiteral("enabled")).toBool());
    CHECK(r.editing.commandAvailability(QStringLiteral("file.newSequenceFromClipsTogether")).value(QStringLiteral("enabled")).toBool());
    before = r.undo();
    REQUIRE(r.editing.runCommand(QStringLiteral("file.newSequencesFromClips")));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.snapshot()->sequences.size() == sequences + 3); // A and B: two more, after the earlier "together"
}

// ---- 143 ------------------------------------------------------------------------------------------------------------

namespace {

std::filesystem::path testMediaFile(const char* name) { return std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveReloadMedia() {
    return std::filesystem::is_regular_file(testMediaFile("GX010042.MP4")) &&
           std::filesystem::is_regular_file(testMediaFile("h264_1080p60_aac.mp4"));
}

bool overwriteWith(const char* name, const QString& to) {
    QFile::remove(to);
    return QFile::copy(QString::fromStdU16String(testMediaFile(name).u16string()), to);
}

} // namespace

TEST_CASE("Editing: Reload Footage reads a file another program changed again, in place (143)", "[app][editing][v07]") {
    if (!haveReloadMedia())
        SKIP("test media missing");
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("capture.mp4"));
    REQUIRE(overwriteWith("GX010042.MP4", file));

    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    LibraryBridge library;
    EditingBridge editing;
    editing.setSynchronous(true); // the worker's part runs on this thread; the next test runs it on a real worker
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setController(&controller);
    editing.setController(&controller);
    editing.setLibrary(&library);
    lifecycle.newProject();
    const QVariantMap imported = lifecycle.importFiles({QUrl::fromLocalFile(file)});
    REQUIRE(imported.value(QStringLiteral("added")).toInt() == 1);
    const MediaItem first = *controller.snapshot()->media.front();
    const QString key = mediaKey(first.id);
    const auto before = first.info;
    REQUIRE(first.info.hasVideo());

    // Nothing changed on disk: nothing happens, and it says so.
    const std::size_t steps = controller.session().undoCount();
    REQUIRE(editing.reloadFootage({key}).isEmpty());
    CHECK(editing.message().contains(QStringLiteral("nothing to reload")));
    CHECK(controller.session().undoCount() == steps);

    // Another program writes a different file over it.
    REQUIRE(overwriteWith("h264_1080p60_aac.mp4", file));
    REQUIRE(editing.reloadFootage({key}).isEmpty());
    INFO(editing.message().toStdString());
    CHECK(editing.message().startsWith(QStringLiteral("Reloaded 1 clip")));
    CHECK(controller.session().undoCount() == steps + 1);
    const MediaItem& now = *controller.snapshot()->findMedia(first.id);
    CHECK(now.info.duration != before.duration);
    CHECK(now.info.video.front().width == 1920);
    CHECK(now.info.hasAudio());
    CHECK(now.name == first.name);
    CHECK(now.locator.path == first.locator.path); // in place: the same item, the same file name
    // One undo brings the earlier probe back.
    REQUIRE(controller.undo());
    CHECK(controller.snapshot()->findMedia(first.id)->info == before);

    // The command surface: enabled with clips selected in the Library, refused with a reason otherwise.
    library.clearSelection();
    CHECK_FALSE(editing.commandAvailability(QStringLiteral("clip.reloadFootage")).value(QStringLiteral("enabled")).toBool());
    library.select(key, LibraryBridge::Replace);
    CHECK(editing.commandAvailability(QStringLiteral("clip.reloadFootage")).value(QStringLiteral("enabled")).toBool());
    REQUIRE(editing.runCommand(QStringLiteral("clip.reloadFootage")));
    CHECK(controller.snapshot()->findMedia(first.id)->info.video.front().width == 1920);

    // The file is gone: said, and the item is left as it is.
    QFile::remove(file);
    const std::size_t afterReload = controller.session().undoCount();
    REQUIRE(editing.reloadFootage({key}).isEmpty() == false);
    CHECK(controller.session().undoCount() == afterReload);
    CHECK_FALSE(editing.reloadFootage({}).isEmpty()); // nothing selected
}

TEST_CASE("Editing: Reload Footage reads the files on a worker thread, with progress, and can be cancelled (143)",
          "[app][editing][v07]") {
    if (!haveReloadMedia())
        SKIP("test media missing");
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("capture.mp4"));
    REQUIRE(overwriteWith("GX010042.MP4", file));

    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    LibraryBridge library;
    EditingBridge editing; // not synchronous: the files are read on a worker
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setController(&controller);
    editing.setController(&controller);
    editing.setLibrary(&library);
    lifecycle.newProject();
    REQUIRE(lifecycle.importFiles({QUrl::fromLocalFile(file)}).value(QStringLiteral("added")).toInt() == 1);
    const MediaItem first = *controller.snapshot()->media.front();
    const QString key = mediaKey(first.id);
    const std::size_t steps = controller.session().undoCount();

    // Another program writes a different file over it. The call returns at once with the work running; the project is
    // not touched until the worker is done, and then it is ONE undo step.
    REQUIRE(overwriteWith("h264_1080p60_aac.mp4", file));
    int changes = 0;
    QObject::connect(&editing, &EditingBridge::reloadChanged, [&] { ++changes; });
    CHECK(editing.reloadFootage({key}).isEmpty());
    CHECK(editing.reloading());
    CHECK(controller.session().undoCount() == steps);
    CHECK_FALSE(editing.reloadFootage({key}).isEmpty()); // a second Reload while one runs is refused, with a reason
    REQUIRE(waitFor([&] { return !editing.reloading(); }, 20'000));
    CHECK(changes >= 2); // started, ended
    INFO(editing.message().toStdString());
    CHECK(editing.message().startsWith(QStringLiteral("Reloaded 1 clip")));
    CHECK(controller.session().undoCount() == steps + 1);
    CHECK(controller.snapshot()->findMedia(first.id)->info.video.front().width == 1920);

    // Cancel: the worker stops before reading and nothing changes (the file on disk is another one again).
    REQUIRE(overwriteWith("GX010042.MP4", file));
    const std::size_t afterFirst = controller.session().undoCount();
    CHECK(editing.reloadFootage({key}).isEmpty());
    editing.cancelReload();
    REQUIRE(waitFor([&] { return !editing.reloading(); }, 20'000));
    // The worker may have been past its one checkpoint when the cancel arrived (a single file): then the reload happened
    // as a whole, else not at all - never half of it.
    const std::size_t after = controller.session().undoCount();
    CHECK((after == afterFirst || after == afterFirst + 1));
    if (after == afterFirst) {
        CHECK(editing.message() == QStringLiteral("Cancelled"));
        CHECK(controller.snapshot()->findMedia(first.id)->info.video.front().width == 1920);
    }
}
// ---- 021 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Library list: Tab, Shift+Tab, Enter and Shift+Enter move between the cells being edited (021)",
          "[app][library][keys][qml][v07]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(project);
    REQUIRE(controller);
    REQUIRE(library);
    project->newProject();
    Timeline tl("V1 |Aaaa|");
    controller->resetProject(tl.state, tl.seq);
    spin(50);
    library->setPersistent(false);
    library->setView(QStringLiteral("list"));
    // The editable columns: Name, Scene, Shot, Take (the others hidden), in that order.
    library->resetColumns();
    for (const QVariant& c : library->allColumns()) {
        const QString id = c.toMap().value(QStringLiteral("id")).toString();
        if (id != QStringLiteral("name"))
            library->setColumnVisible(id, id == QStringLiteral("scene") || id == QStringLiteral("shot") || id == QStringLiteral("take"));
    }
    REQUIRE(columnIds(*library) == (QStringList{QStringLiteral("name"), QStringLiteral("scene"), QStringLiteral("shot"),
                                                QStringLiteral("take")}));
    REQUIRE_FALSE(shell.grab().isNull()); // a hidden window lays out when it renders
    spin(100);
    QQuickItem* panel = shell.item("libraryPanel");
    REQUIRE(panel);
    QQuickItem* list = byName(shell.window->contentItem(), QStringLiteral("libraryList"));
    REQUIRE(list);
    QQuickWindow* window = shell.window.get();

    const QString a = mediaKey(tl.media('A'));
    const int rowA = library->items()->rowOf(a);
    REQUIRE(rowA >= 0);
    REQUIRE(rowA + 1 < library->items()->rowCount());
    const QString below = library->items()->keyOf(rowA + 1);
    REQUIRE(below.startsWith(QLatin1Char('m')));
    const auto scene = [&](const QString& key) {
        const auto id = parseMediaKey(key);
        return QString::fromStdString(controller->snapshot()->findMedia(*id)->metadata.scene);
    };
    const auto editingAt = [&] {
        return list->property("editingKey").toString() + QLatin1Char('/') + list->property("editingColumn").toString();
    };
    const auto editor = [&]() -> QQuickItem* { return byName(list, QStringLiteral("libraryCellEditor")); };
    const auto startEdit = [&](const QString& key, const char* column) {
        // The row may lie below the part of the list in view: scroll to it first (the cell is made when it comes in sight).
        REQUIRE(QMetaObject::invokeMethod(list, "showCell", Q_ARG(QString, key), Q_ARG(QString, QString::fromLatin1(column))));
        spin(50);
        (void)shell.grab(); // a hidden window lays out (makes the cells that came in sight) when it renders
        spin(50);
        REQUIRE(QMetaObject::invokeMethod(list, "startEdit", Q_ARG(QVariant, key), Q_ARG(QVariant, QString::fromLatin1(column))));
        const bool focused = waitFor([&] { return editor() != nullptr && editor()->hasActiveFocus(); }, 5000);
        QQuickItem* focusItem = window->activeFocusItem();
        INFO("editing " << list->property("editingKey").toString().toStdString() << "/"
                       << list->property("editingColumn").toString().toStdString() << ", editor "
                       << (editor() != nullptr) << ", list visible " << list->isVisible() << ", list size " << list->width()
                       << "x" << list->height() << ", active focus item "
                       << (focusItem ? focusItem->metaObject()->className() : "none") << " '"
                       << (focusItem ? focusItem->objectName().toStdString() : std::string()) << "'");
        REQUIRE(focused);
    };
    const auto type = [&](const QString& text) {
        REQUIRE(editor());
        editor()->setProperty("text", text);
    };
    const auto expectEditing = [&](const QString& expected) {
        INFO("expected " << expected.toStdString() << ", editing " << editingAt().toStdString());
        REQUIRE(waitFor([&] { return editingAt() == expected && editor() != nullptr && editor()->hasActiveFocus(); }, 5000));
    };

    // Nothing is open: the field keys are not the Library's.
    CHECK_FALSE(library->fieldEditing());
    CHECK_FALSE(library->runCommand(QStringLiteral("project.nextRowField")));
    const QVariantMap idle = library->commandAvailability(QStringLiteral("project.nextColumnField"));
    CHECK(idle.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(idle.value(QStringLiteral("enabled")).toBool());
    CHECK(idle.value(QStringLiteral("reason")).toString() == QStringLiteral("Edit a cell in the Library list first"));

    // Tab along the row: Name -> Scene -> Shot -> Take, each value kept as the cell is left.
    startEdit(a, "name");
    CHECK(library->fieldEditing());
    type(QStringLiteral("Alpha"));
    tap(window, Qt::Key_Tab);
    expectEditing(a + QStringLiteral("/scene"));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(tl.media('A'))->name) == QStringLiteral("Alpha"));
    type(QStringLiteral("S1"));
    tap(window, Qt::Key_Tab);
    expectEditing(a + QStringLiteral("/shot"));
    CHECK(scene(a) == QStringLiteral("S1"));
    type(QStringLiteral("Sh1"));
    tap(window, Qt::Key_Tab);
    expectEditing(a + QStringLiteral("/take"));
    type(QStringLiteral("T1"));
    // Past the last cell of the row: the first cell of the next row.
    tap(window, Qt::Key_Tab);
    expectEditing(below + QStringLiteral("/name"));
    // Shift+Tab goes back, to the last cell of the row above.
    tap(window, Qt::Key_Backtab, Qt::ShiftModifier);
    expectEditing(a + QStringLiteral("/take"));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(tl.media('A'))->metadata.take) == QStringLiteral("T1"));

    // Enter keeps the column and goes one row down; Shift+Enter one row up.
    type(QStringLiteral("T2"));
    tap(window, Qt::Key_Return);
    expectEditing(below + QStringLiteral("/take"));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(tl.media('A'))->metadata.take) == QStringLiteral("T2"));
    tap(window, Qt::Key_Return, Qt::ShiftModifier);
    expectEditing(a + QStringLiteral("/take"));

    // Escape cancels what was typed; nothing further opens.
    type(QStringLiteral("discarded"));
    tap(window, Qt::Key_Escape);
    REQUIRE(waitFor([&] { return list->property("editingKey").toString().isEmpty(); }, 5000));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(tl.media('A'))->metadata.take) == QStringLiteral("T2"));
    CHECK_FALSE(library->fieldEditing());

    // In the Name column Enter only renames: the edit ends, nothing opens below.
    startEdit(a, "name");
    type(QStringLiteral("Alpha 2"));
    tap(window, Qt::Key_Return);
    REQUIRE(waitFor([&] { return list->property("editingKey").toString().isEmpty(); }, 5000));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(tl.media('A'))->name) == QStringLiteral("Alpha 2"));

    // The end of the list: the last media row's Enter keeps the value and stops; Tab past its last cell stops too.
    QString last;
    for (int row = library->items()->rowCount() - 1; row >= 0 && last.isEmpty(); --row) {
        if (library->items()->keyOf(row).startsWith(QLatin1Char('m')))
            last = library->items()->keyOf(row);
    }
    REQUIRE_FALSE(last.isEmpty());
    startEdit(last, "take");
    type(QStringLiteral("Tz"));
    tap(window, Qt::Key_Return);
    REQUIRE(waitFor([&] { return list->property("editingKey").toString().isEmpty(); }, 5000));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(*parseMediaKey(last))->metadata.take) == QStringLiteral("Tz"));
    startEdit(last, "take");
    type(QStringLiteral("Ty"));
    tap(window, Qt::Key_Tab);
    REQUIRE(waitFor([&] { return list->property("editingKey").toString().isEmpty(); }, 5000));
    CHECK(QString::fromStdString(controller->snapshot()->findMedia(*parseMediaKey(last))->metadata.take) == QStringLiteral("Ty"));

    // Rows a column cannot be edited in are skipped: a sequence has no Scene.
    const QString sequence = sequenceKey(tl.seq);
    const int sequenceRow = library->items()->rowOf(sequence);
    REQUIRE(sequenceRow >= 0);
    if (sequenceRow > 0) {
        const QString beforeSequence = library->items()->keyOf(sequenceRow - 1);
        if (beforeSequence.startsWith(QLatin1Char('m'))) {
            startEdit(beforeSequence, "scene");
            tap(window, Qt::Key_Return); // the row below is the sequence; the next media row is the target
            REQUIRE(waitFor([&] {
                const QString now = list->property("editingKey").toString();
                return !now.isEmpty() && now != beforeSequence;
            }, 5000));
            CHECK(list->property("editingKey").toString() != sequence);
            tap(window, Qt::Key_Escape);
        }
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
}

// ---- 003 ------------------------------------------------------------------------------------------------------------

namespace {

/// The shell with the Collect Files dialog open over a project of four sequences.
struct CollectShell {
    Shell shell;
    ProjectController* controller = nullptr;
    QObject* dialog = nullptr;
    QQuickItem* root = nullptr;
    QQuickWindow* window = nullptr;

    explicit CollectShell(bool dark) {
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(project);
        REQUIRE(controller);
        project->newProject();
        Timeline tl("V1 tgt |Aaaa|");
        ProjectState state = tl.state;
        for (const char* name : {"Final cut", "Old version, abandoned", "Rough"}) {
            edit::SequenceSettings settings;
            settings.name = name;
            auto added = edit::addSequence(state, settings);
            REQUIRE(added);
            state = added->state;
        }
        controller->resetProject(state, tl.seq);
        spin(50);
        REQUIRE_FALSE(shell.grab().isNull());
        window = shell.window.get();
        root = window->contentItem();
        QObject* loader = shell.object("collectFilesDialogLoader");
        REQUIRE(loader);
        loader->setProperty("active", true);
        dialog = qvariant_cast<QObject*>(loader->property("item"));
        REQUIRE(dialog);
        REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        spin(150);
    }
    QQuickItem* named(const char* name) const { return byName(root, QString::fromLatin1(name)); }
    QVariantMap options() const {
        QVariant out;
        REQUIRE(QMetaObject::invokeMethod(dialog, "options", Q_RETURN_ARG(QVariant, out)));
        return out.toMap();
    }
    int sequenceCount() const { return dialog->property("sequenceCount").toInt(); }
    int chosenCount() const { return dialog->property("chosenCount").toInt(); }
    /// Ticks or unticks sequence `index` the way a click does (the switch flips, then reports).
    void setSequence(int index, bool on) {
        QQuickItem* toggle = named(QStringLiteral("collectSequence_%1").arg(index).toLatin1().constData());
        REQUIRE(toggle);
        toggle->setProperty("checked", on);
        REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
    }
};

} // namespace

TEST_CASE("Collect Files dialog: Handles are for used ranges and remembered; Sequences are a checklist (003)",
          "[app][collect][qml][v07]") {
    resetSettings();
    clearMessages();
    CollectShell c(true);
    INFO(join(takeMessages()));
    QQuickItem* handles = c.named("collectHandlesField");
    QQuickItem* tabs = c.named("collectTrimTabs");
    REQUIRE(handles);
    REQUIRE(tabs);
    // Whole files: handles do not apply, the field is greyed.
    CHECK_FALSE(handles->isEnabled());
    CHECK_FALSE(c.options().value(QStringLiteral("trim")).toBool());
    // The tab is named for the handles, not for one second.
    const QVariantList model = tabs->property("model").toList();
    REQUIRE(model.size() == 2);
    CHECK(model[1].toString() == QStringLiteral("Used ranges + handles"));
    REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 1)));
    CHECK(handles->isEnabled());
    CHECK(c.options().value(QStringLiteral("trim")).toBool());
    CHECK(c.options().value(QStringLiteral("handles")).toDouble() == 1.0); // the default stays one second

    // 0 to 10 s in steps of half a second; the choice goes into options() and is remembered for the next time.
    CHECK(handles->property("from").toDouble() == 0.0);
    CHECK(handles->property("to").toDouble() == 10.0);
    CHECK(handles->property("stepSize").toDouble() == 0.5);
    REQUIRE(QMetaObject::invokeMethod(handles, "valueModified", Q_ARG(double, 3.5)));
    CHECK(c.options().value(QStringLiteral("handles")).toDouble() == 3.5);
    CHECK(c.dialog->property("handlesSeconds").toDouble() == 3.5);
    // The settings element writes a moment after the change.
    CHECK(waitFor([&] { return QSettings().value(QStringLiteral("collect/handlesSeconds")).toDouble() == 3.5; }, 5000));

    // The sequences: all ticked, one row each, a count; no list in the options while all are ticked.
    REQUIRE(c.sequenceCount() == 4);
    CHECK(c.chosenCount() == 4);
    CHECK_FALSE(c.options().contains(QStringLiteral("sequences")));
    QQuickItem* count = c.named("collectSequencesCount");
    REQUIRE(count);
    CHECK(count->property("text").toString() == QStringLiteral("4 of 4 chosen"));
    for (int i = 0; i < 4; ++i)
        CHECK(c.named(QStringLiteral("collectSequence_%1").arg(i).toLatin1().constData()) != nullptr);

    // None, then one: the options name just that one (its index in the project's sequence order).
    REQUIRE(QMetaObject::invokeMethod(c.named("collectSequencesNone"), "clicked"));
    CHECK(c.chosenCount() == 0);
    CHECK(count->property("text").toString() == QStringLiteral("0 of 4 chosen"));
    QQuickItem* start = c.named("collectStartButton");
    REQUIRE(start);
    CHECK_FALSE(start->isEnabled()); // nothing to collect
    c.setSequence(1, true);
    CHECK(c.chosenCount() == 1);
    CHECK(start->isEnabled());
    const QVariantList picked = c.options().value(QStringLiteral("sequences")).toList();
    REQUIRE(picked.size() == 1);
    CHECK(picked.front().toInt() == 1);
    REQUIRE(QMetaObject::invokeMethod(c.named("collectSequencesAll"), "clicked"));
    CHECK(c.chosenCount() == 4);
    CHECK_FALSE(c.options().contains(QStringLiteral("sequences")));

    // The bridge refuses a list that names no sequence (never "everything"), and clamps the handles.
    auto* collect = c.shell.engine.singletonInstance<CollectFilesBridge*>("Wabcut", "Collect");
    REQUIRE(collect);
    const QUrl folder = QUrl::fromLocalFile(QDir::tempPath());
    CHECK(collect->estimate(folder, QVariantMap{{QStringLiteral("sequences"), QVariantList{}}}).contains(QStringLiteral("at least one sequence")));
    CHECK(collect->estimate(folder, QVariantMap{{QStringLiteral("sequences"), QVariantList{99}}}).contains(QStringLiteral("at least one sequence")));
    CHECK_FALSE(collect->estimate(folder, QVariantMap{{QStringLiteral("sequences"), QVariantList{0, 2}}, {QStringLiteral("handles"), 99.0}})
                    .contains(QStringLiteral("at least one sequence")));
    resetSettings();
}

TEST_CASE("Collect Files dialog: screenshots of the Handles field and the Sequences checklist (003)",
          "[app][collect][qml][screenshot][v07]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        CollectShell c(dark);
        QQuickItem* tabs = c.named("collectTrimTabs");
        REQUIRE(tabs);
        REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 1)));
        // The final cut and the rough cut only.
        REQUIRE(QMetaObject::invokeMethod(c.named("collectSequencesNone"), "clicked"));
        c.setSequence(1, true);
        c.setSequence(3, true);
        REQUIRE(c.chosenCount() == 2);
        spin(200);
        const QImage image = c.shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("collect-handles-sequences-dark.png")
                                                           : QStringLiteral("collect-handles-sequences-light.png"))));
        // The smallest window (1100 x 700): the body scrolls, the dialog stays inside the window, the buttons stay in view.
        c.shell.resize(1100, 700);
        spin(300);
        (void)c.shell.grab();
        const double dialogTop = c.dialog->property("y").toDouble();
        const double dialogHeight = c.dialog->property("height").toDouble();
        INFO("dialog y " << dialogTop << " height " << dialogHeight);
        CHECK(dialogTop >= 0.0);
        CHECK(dialogTop + dialogHeight <= 700.5);
        QQuickItem* start = c.named("collectStartButton");
        REQUIRE(start);
        const QRectF startRect = start->mapRectToScene(QRectF(0, 0, start->width(), start->height()));
        CHECK(QRectF(0, 0, 1100, 700).contains(startRect));
        if (dark) {
            const QImage narrow = c.shell.grab();
            REQUIRE_FALSE(narrow.isNull());
            REQUIRE(narrow.save(QDir(outputDir()).filePath(QStringLiteral("collect-handles-sequences-narrow.png"))));
        }
        c.shell.resize(kShotWidth, kShotHeight);
        spin(200);
        // Nothing runs off the edge of the window.
        QQuickItem* handles = c.named("collectHandlesField");
        REQUIRE(handles);
        const QRectF field = handles->mapRectToScene(QRectF(0, 0, handles->width(), handles->height()));
        CHECK(QRectF(0, 0, kShotWidth, kShotHeight).contains(field));
        QQuickItem* list = c.named("collectSequenceList");
        REQUIRE(list);
        CHECK(QRectF(0, 0, kShotWidth, kShotHeight).contains(list->mapRectToScene(QRectF(0, 0, list->width(), list->height()))));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// ---- 022 and 144: the Media Browser's Ingest Settings and the Library's column chooser, in pictures --------------------

TEST_CASE("Media Browser Ingest Settings offers Copy and Create Proxies; Library column chooser keeps layouts; screenshots",
          "[app][library][media-browser][qml][screenshot][v07]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(library);
        project->newProject();
        Timeline tl("V1 tgt |Aaaa|");
        controller->resetProject(tl.state, tl.seq);
        spin(50);
        library->setPersistent(false);
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* root = shell.window->contentItem();

        // Ingest Settings: four actions, the new one chosen; it asks for a folder like Copy.
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("window.mediaBrowser"))));
        REQUIRE(waitFor([&] { return shell.window->findChild<QObject*>(QStringLiteral("mediaBrowserPanel")) != nullptr; }, 5000));
        QObject* browser = shell.window->findChild<QObject*>(QStringLiteral("mediaBrowserPanel"));
        REQUIRE(browser);
        library->setIngestAction(QStringLiteral("copyProxies"));
        library->setIngest(true);
        QObject* popover = browser->findChild<QObject*>(QStringLiteral("mediaBrowserIngestPopover"));
        REQUIRE(popover);
        REQUIRE(QMetaObject::invokeMethod(popover, "open"));
        REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 5000));
        spin(200);
        QQuickItem* actions = byName(root, QStringLiteral("mediaBrowserIngestAction"));
        REQUIRE(actions);
        const QVariantList model = actions->property("model").toList();
        REQUIRE(model.size() == 4);
        CHECK(model[3].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("Copy and Create Proxies"));
        CHECK(actions->property("currentIndex").toInt() == 3);
        QQuickItem* folderRow = byName(root, QStringLiteral("mediaBrowserIngestFolder"));
        REQUIRE(folderRow);
        CHECK(folderRow->isVisible()); // a folder is needed
        library->setIngestAction(QStringLiteral("proxies"));
        spin(100);
        CHECK_FALSE(folderRow->isVisible()); // Create Proxies alone needs none
        library->setIngestAction(QStringLiteral("copyProxies"));
        spin(100);
        const QImage ingest = shell.grab();
        REQUIRE_FALSE(ingest.isNull());
        REQUIRE(ingest.save(QDir(outputDir()).filePath(dark ? QStringLiteral("ingest-copy-proxies-dark.png")
                                                          : QStringLiteral("ingest-copy-proxies-light.png"))));
        QMetaObject::invokeMethod(popover, "close");
        library->setIngest(false);

        // The column chooser: two columns of switches and the saved layouts.
        library->setView(QStringLiteral("list"));
        library->setColumnVisible(QStringLiteral("created"), true);
        REQUIRE(library->saveColumnPreset(QStringLiteral("Logging")));
        REQUIRE(library->saveColumnPreset(QStringLiteral("Delivery")));
        spin(100);
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* button = byName(root, QStringLiteral("libraryColumnsButton"));
        REQUIRE(button);
        REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
        QObject* chooser = nullptr;
        REQUIRE(waitFor([&] {
            chooser = button->findChild<QObject*>(QStringLiteral("libraryColumnChooser"));
            return chooser && chooser->property("opened").toBool();
        }, 5000));
        spin(200);
        QQuickItem* created = byName(root, QStringLiteral("libraryColumnToggle_created"));
        REQUIRE(created);
        CHECK(created->property("checked").toBool());
        CHECK(byName(root, QStringLiteral("libraryColumnPreset_Logging")) != nullptr);
        CHECK(byName(root, QStringLiteral("libraryColumnPreset_Delivery")) != nullptr);
        QQuickItem* saveButton = byName(root, QStringLiteral("libraryColumnPresetSave"));
        REQUIRE(saveButton);
        CHECK_FALSE(saveButton->isEnabled()); // no name typed yet
        const QImage chooserShot = shell.grab();
        REQUIRE_FALSE(chooserShot.isNull());
        REQUIRE(chooserShot.save(QDir(outputDir()).filePath(dark ? QStringLiteral("library-column-chooser-dark.png")
                                                                : QStringLiteral("library-column-chooser-light.png"))));
        const QRectF where = created->mapRectToScene(QRectF(0, 0, created->width(), created->height()));
        CHECK(QRectF(0, 0, kShotWidth, kShotHeight).contains(where));
        // The smallest window: the chooser scrolls instead of running off the bottom.
        shell.resize(1100, 700);
        spin(300);
        (void)shell.grab();
        QQuickItem* scroll = byName(root, QStringLiteral("libraryColumnChooserScroll"));
        REQUIRE(scroll);
        const QRectF scrollRect = scroll->mapRectToScene(QRectF(0, 0, scroll->width(), scroll->height()));
        INFO("chooser scroll " << scrollRect.top() << " to " << scrollRect.bottom());
        CHECK(scrollRect.bottom() <= 700.5);
        CHECK(scrollRect.top() >= 0.0);
        if (dark) {
            const QImage narrow = shell.grab();
            REQUIRE_FALSE(narrow.isNull());
            REQUIRE(narrow.save(QDir(outputDir()).filePath(QStringLiteral("library-column-chooser-narrow.png"))));
        }
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// ---- 112, 055, 143: the Library's right-click menu ------------------------------------------------------------------

TEST_CASE("Library menu: File Nested Sequences Here on a bin, New Sequence per Clip and Reload Footage on clips (112 055 143)",
          "[app][library][qml][v07]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(project);
    REQUIRE(controller);
    REQUIRE(library);
    project->newProject();
    Timeline tl("V1 |Aaaa|");
    controller->resetProject(tl.state, tl.seq);
    spin(50);
    library->setPersistent(false);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* panel = shell.item("libraryPanel");
    REQUIRE(panel);
    QObject* menu = shell.object("libraryItemMenu");
    if (!menu)
        menu = panel->findChild<QObject*>(QStringLiteral("libraryItemMenu"));
    REQUIRE(menu);
    const auto open = [&](const QString& key, const char* kind) {
        QVariant ignored;
        REQUIRE(QMetaObject::invokeMethod(menu, "openFor", Q_RETURN_ARG(QVariant, ignored),
                                          Q_ARG(QVariant, QVariant::fromValue<QObject*>(panel)), Q_ARG(QVariant, key),
                                          Q_ARG(QVariant, QString::fromLatin1(kind)), Q_ARG(QVariant, QPointF(40, 40)),
                                          Q_ARG(QVariant, -1)));
        spin(100);
    };
    const auto item = [&](const char* name) {
        QObject* found = menu->findChild<QObject*>(QString::fromLatin1(name));
        REQUIRE(found);
        return found;
    };

    // A bin: the nest-bin row says what it does and flips.
    const QString bin = library->newBin(QStringLiteral("Nests"));
    REQUIRE_FALSE(bin.isEmpty());
    library->select(bin, LibraryBridge::Replace);
    open(bin, "bin");
    CHECK(item("libraryMenuNestBin")->property("visible").toBool());
    CHECK(item("libraryMenuNestBin")->property("text").toString() == QStringLiteral("File Nested Sequences Here"));
    CHECK_FALSE(item("libraryMenuNewSequencePerClip")->property("visible").toBool()); // not for a bin
    CHECK_FALSE(item("libraryMenuReloadFootage")->property("visible").toBool());
    REQUIRE(QMetaObject::invokeMethod(item("libraryMenuNestBin"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    spin(100);
    CHECK(library->details(bin).value(QStringLiteral("nestTarget")).toBool());
    open(bin, "bin");
    CHECK(item("libraryMenuNestBin")->property("text").toString() == QStringLiteral("Stop Filing Nested Sequences Here"));
    REQUIRE(QMetaObject::invokeMethod(item("libraryMenuNestBin"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    spin(100);
    CHECK_FALSE(library->details(bin).value(QStringLiteral("nestTarget")).toBool());

    // One clip: only the single New Sequence From Clip and Reload Footage; two or more: the per-clip rows too.
    const QString a = mediaKey(tl.media('A'));
    const QString b = mediaKey(tl.media('B'));
    library->select(a, LibraryBridge::Replace);
    open(a, "video");
    CHECK(item("libraryMenuNewSequenceFromClip")->property("visible").toBool());
    CHECK(item("libraryMenuReloadFootage")->property("visible").toBool());
    CHECK_FALSE(item("libraryMenuNewSequencePerClip")->property("visible").toBool());
    CHECK_FALSE(item("libraryMenuNewSequenceFromAllClips")->property("visible").toBool());
    QMetaObject::invokeMethod(menu, "close");
    spin(100);
    library->select(b, LibraryBridge::Toggle);
    open(a, "video");
    CHECK(item("libraryMenuNewSequencePerClip")->property("visible").toBool());
    CHECK(item("libraryMenuNewSequencePerClip")->property("text").toString() == QStringLiteral("New Sequence per Clip (2)"));
    CHECK(item("libraryMenuNewSequenceFromAllClips")->property("visible").toBool());
    const std::size_t sequences = controller->snapshot()->sequences.size();
    REQUIRE(QMetaObject::invokeMethod(item("libraryMenuNewSequencePerClip"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    spin(100);
    CHECK(controller->snapshot()->sequences.size() == sequences + 2);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
