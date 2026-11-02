// 0.6 (P1 round, stream 1D): the Library and relinking.
//   * #41 Move to Bin (the bin tree), Cut and Paste between bins, and bins dragged into bins with the Library's own
//     drag (real pointer events through the DragHandler: no system drag is involved, so this is the real path);
//   * #43 Link Media… on offline items (and a double-click on one) opens the Relink dialog for them only;
//   * #6 the import report's Relink… button, and its footer stays inside a small window;
//   * #44 proxies for every selected item and for a whole bin;
//   * #45 the File Size and Data Rate columns and the footer's summary line;
//   * #39 Get Properties: the properties of a clip, a sequence and a bin, Copy all, Ctrl+Shift+H and the dialog.
// Screenshots (dark and light): the Properties dialog, the Relink dialog for one item, the Move to Bin menu, the
// import report with Relink….

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlProperty>
#include <QSettings>
#include <QTemporaryDir>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// A project from a timeline picture and a Library over it (no settings, no analysis scan).
struct P1Rig {
    Timeline tl;
    ProjectController controller;
    LibraryBridge library;

    explicit P1Rig(std::string_view picture = R"(
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )")
        : tl(picture) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setAnalysisProbe({});
        library.setController(&controller);
    }
    QString key(char letter) const { return mediaKey(tl.media(letter)); }
    std::size_t undo() { return controller.session().undoCount(); }
    BinId binOf(char letter) const { return controller.snapshot()->findMedia(tl.media(letter))->bin; }
    /// Gives media `letter` a file size (MediaInfo::fileSize) in one commit.
    void setBytes(char letter, std::int64_t bytes) {
        ProjectState next = *controller.snapshot();
        for (MediaPtr& m : next.media) {
            if (m->id == tl.media(letter)) {
                MediaItem item = *m;
                item.info.fileSize = bytes;
                m = std::make_shared<const MediaItem>(std::move(item));
            }
        }
        REQUIRE(controller.session().commit("Size", next));
    }
    QStringList listed() const {
        QStringList out;
        for (int r = 0; r < library.items()->rowCount(); ++r)
            out << library.items()->keyOf(r);
        return out;
    }
};

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

/// A QML list property (a JS array arrives as a QJSValue) as a QVariantList / QStringList.
QVariantList listOf(const QVariant& value) {
    if (value.userType() == qMetaTypeId<QJSValue>())
        return value.value<QJSValue>().toVariant().toList();
    return value.toList();
}
QStringList stringsOf(const QVariant& value) {
    QStringList out;
    for (const QVariant& v : listOf(value))
        out << v.toString();
    return out;
}

void sendMouse(QQuickWindow* window, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, pos, pos, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}

/// A press at `from`, moves in small steps to `to` and a release there: what a mouse does.
void dragWithMouse(QQuickWindow* window, QPointF from, QPointF to) {
    sendMouse(window, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    spin(5);
    constexpr int kSteps = 16;
    for (int i = 1; i <= kSteps; ++i) {
        sendMouse(window, QEvent::MouseMove, from + (to - from) * (static_cast<double>(i) / kSteps), Qt::NoButton,
                  Qt::LeftButton);
        spin(5);
    }
    sendMouse(window, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    spin(50);
}

/// Media `key` made offline (Make Offline does the same) in one commit.
void makeOffline(ProjectController* controller, const QString& key) {
    const auto id = parseMediaKey(key);
    REQUIRE(id);
    ProjectState next = *controller->snapshot();
    for (MediaPtr& m : next.media) {
        if (m->id == *id) {
            MediaItem item = *m;
            item.locator.offline = true;
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(controller->session().commit("Offline", next));
}

/// Leaves the shell's sample project (it never counts offline media) for a new project with media A..Z (no files).
void useOwnProject(Shell& shell) {
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(project);
    REQUIRE(controller);
    project->newProject();
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )");
    controller->resetProject(tl.state, tl.seq);
    spin(50);
    REQUIRE_FALSE(project->demo());
}

/// Saves the part of `image` under `item` as `name` in the UI test output.
void saveItemShot(const QImage& image, const QQuickItem* item, const QString& name) {
    const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    const double dpr = image.devicePixelRatio();
    REQUIRE(image.copy(QRect(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                             static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr)))
                .save(QDir(outputDir()).filePath(name)));
}

} // namespace

TEST_CASE("Library P1: Move to Bin lists the bin tree and moves the selection in one undo step", "[app][library][p1]") {
    P1Rig r;
    const QString footage = r.library.newBin(QStringLiteral("Footage"));
    const QString archive = r.library.newBin(QStringLiteral("archive"));
    r.library.openBin(footage);
    const QString day = r.library.newBin(QStringLiteral("Day 1"));
    r.library.openBin(QString());
    const QString search = r.library.newSearchBin(QStringLiteral("Hero"), {{QStringLiteral("text"), QStringLiteral("A")}});
    REQUIRE_FALSE(search.isEmpty());

    // Root first, then depth first by name (case-insensitive): archive, Footage › Day 1, the search bin.
    const QVariantList tree = r.library.binTree({footage});
    REQUIRE(tree.size() == 5);
    const auto at = [&](int i) { return tree[i].toMap(); };
    CHECK(at(0).value(QStringLiteral("key")).toString().isEmpty());
    CHECK(at(0).value(QStringLiteral("depth")).toInt() == 0);
    CHECK(at(1).value(QStringLiteral("key")).toString() == archive);
    CHECK(at(2).value(QStringLiteral("key")).toString() == footage);
    CHECK(at(3).value(QStringLiteral("key")).toString() == day);
    CHECK(at(3).value(QStringLiteral("depth")).toInt() == 2);
    CHECK(at(4).value(QStringLiteral("key")).toString() == search);
    // Moving Footage: it and Day 1 inside it are off, the search bin too; the reasons say why.
    CHECK_FALSE(at(1).value(QStringLiteral("disabled")).toBool());
    CHECK(at(2).value(QStringLiteral("disabled")).toBool());
    CHECK(at(3).value(QStringLiteral("disabled")).toBool());
    CHECK(at(4).value(QStringLiteral("disabled")).toBool());
    CHECK_FALSE(at(4).value(QStringLiteral("reason")).toString().isEmpty());
    CHECK_FALSE(r.library.binTree({r.key('A')})[2].toMap().value(QStringLiteral("disabled")).toBool());

    // Two clips and a bin into Day 1: one step; undo puts them back.
    const auto before = r.undo();
    REQUIRE(r.library.moveItems({r.key('A'), r.key('B'), archive}, day));
    CHECK(r.undo() == before + 1);
    CHECK(r.binOf('A') == *parseBinKey(day));
    CHECK(r.controller.snapshot()->findBin(*parseBinKey(archive))->parent == *parseBinKey(day));
    REQUIRE(r.controller.undo());
    CHECK_FALSE(r.binOf('A').isValid());
}

// Fix round 1 (ARGON-22): moving items to the bin they are in does nothing, so that row is ticked and greyed out.
TEST_CASE("Library P1: Move to Bin greys out the bin the items are already in", "[app][library][p1]") {
    P1Rig r;
    const QString footage = r.library.newBin(QStringLiteral("Footage"));
    const auto row = [](const QVariantList& tree, int i) { return tree[i].toMap(); };
    // A and B at the root: the root ("Project") is theirs.
    QVariantList tree = r.library.binTree({r.key('A'), r.key('B')});
    REQUIRE(tree.size() == 2);
    CHECK(row(tree, 0).value(QStringLiteral("current")).toBool());
    CHECK(row(tree, 0).value(QStringLiteral("disabled")).toBool());
    CHECK(row(tree, 0).value(QStringLiteral("reason")).toString() == QStringLiteral("Already in this bin"));
    CHECK_FALSE(row(tree, 1).value(QStringLiteral("disabled")).toBool());
    // Both in Footage: Footage is theirs, the root is free.
    REQUIRE(r.library.moveItems({r.key('A'), r.key('B')}, footage));
    tree = r.library.binTree({r.key('A'), r.key('B')});
    CHECK_FALSE(row(tree, 0).value(QStringLiteral("disabled")).toBool());
    CHECK(row(tree, 1).value(QStringLiteral("current")).toBool());
    CHECK(row(tree, 1).value(QStringLiteral("disabled")).toBool());
    // A bin moves from where it is: the root is its bin.
    CHECK(row(r.library.binTree({footage}), 0).value(QStringLiteral("current")).toBool());
    // Items in different bins share none: every row can be chosen.
    REQUIRE(r.library.moveItems({r.key('B')}, QString()));
    tree = r.library.binTree({r.key('A'), r.key('B')});
    CHECK_FALSE(row(tree, 0).value(QStringLiteral("disabled")).toBool());
    CHECK_FALSE(row(tree, 1).value(QStringLiteral("disabled")).toBool());
    CHECK_FALSE(row(tree, 1).value(QStringLiteral("current")).toBool());
}

TEST_CASE("Library P1: Cut and Paste move items into the selected bin or the bin shown", "[app][library][p1]") {
    P1Rig r;
    const QString selects = r.library.newBin(QStringLiteral("Selects"));
    const QString music = r.library.newBin(QStringLiteral("Music"));

    // Nothing selected or cut: Cut and Paste are not the Library's (the timeline's run).
    r.library.clearSelection();
    CHECK_FALSE(r.library.commandAvailability(QStringLiteral("edit.cut")).value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(r.library.commandAvailability(QStringLiteral("edit.paste")).value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(r.library.runCommand(QStringLiteral("edit.cut")));
    CHECK_FALSE(r.library.runCommand(QStringLiteral("edit.paste")));

    // Ctrl+X on two clips marks them (the model's cut role dims them); nothing moves yet.
    r.library.select(r.key('A'), LibraryBridge::Replace);
    r.library.select(r.key('C'), LibraryBridge::Add);
    CHECK(r.library.commandAvailability(QStringLiteral("edit.cut")).value(QStringLiteral("enabled")).toBool());
    const auto before = r.undo();
    REQUIRE(r.library.runCommand(QStringLiteral("edit.cut")));
    CHECK(r.undo() == before);
    QStringList cut = r.library.cutKeys();
    QStringList expected{r.key('A'), r.key('C')};
    cut.sort();
    expected.sort();
    CHECK(cut == expected);
    LibraryModel& model = *r.library.items();
    CHECK(model.data(model.index(model.rowOf(r.key('A')), 0), LibraryModel::CutRole).toBool());
    CHECK_FALSE(model.data(model.index(model.rowOf(r.key('B')), 0), LibraryModel::CutRole).toBool());

    // Select the Selects bin and paste: both go into it, one undo step, and the marks are gone.
    r.library.select(selects, LibraryBridge::Replace);
    CHECK(r.library.pasteTarget() == selects);
    CHECK(r.library.commandAvailability(QStringLiteral("edit.paste")).value(QStringLiteral("enabled")).toBool());
    REQUIRE(r.library.runCommand(QStringLiteral("edit.paste")));
    CHECK(r.undo() == before + 1);
    CHECK(r.binOf('A') == *parseBinKey(selects));
    CHECK(r.binOf('C') == *parseBinKey(selects));
    CHECK(r.library.cutKeys().isEmpty());
    REQUIRE(r.controller.undo());
    CHECK_FALSE(r.binOf('A').isValid());

    // With no bin selected, Paste goes into the bin shown; Escape (clearCut) forgets the marks.
    r.library.cutItems({r.key('B')});
    r.library.openBin(music);
    CHECK(r.library.pasteTarget() == music);
    REQUIRE(r.library.pasteItems());
    CHECK(r.binOf('B') == *parseBinKey(music));
    r.library.openBin(QString());
    r.library.cutItems({r.key('D')});
    r.library.clearCut();
    CHECK(r.library.cutKeys().isEmpty());
    CHECK_FALSE(r.library.pasteItems());

    // A bin cut and pasted into itself is refused, and its mark stays for another bin.
    r.library.cutItems({selects});
    r.library.select(selects, LibraryBridge::Replace); // selected but cut: Paste goes to the bin shown (the root)
    CHECK(r.library.pasteTarget().isEmpty());
    r.library.openBin(selects);
    CHECK_FALSE(r.library.pasteItems());
    CHECK(r.library.cutKeys() == QStringList{selects});
    CHECK_FALSE(r.library.message().isEmpty());
}

TEST_CASE("Library P1: bins drag into bins and onto the breadcrumb with the Library's own drag", "[app][library][p1][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(library);
    REQUIRE(controller);
    library->setPersistent(false);
    library->setView(QStringLiteral("grid"));
    const QString moving = library->newBin(QStringLiteral("B-roll"));
    const QString target = library->newBin(QStringLiteral("Selects"));
    library->clearSelection();
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* root = shell.window->contentItem();
    QQuickItem* from = findNamed(root, QStringLiteral("libraryCard_") + moving);
    QQuickItem* onto = findNamed(root, QStringLiteral("libraryCard_") + target);
    REQUIRE(from);
    REQUIRE(onto);

    // A press on B-roll and a drag onto Selects: Selects lights up under the pointer, the chip follows, and the
    // release moves B-roll into it (one undo step).
    QQuickItem* panel = shell.item("libraryPanel");
    REQUIRE(panel);
    const QPointF a = from->mapToScene(QPointF(from->width() / 2, from->height() / 3));
    const QPointF b = onto->mapToScene(QPointF(onto->width() / 2, onto->height() / 3));
    const auto undo = controller->session().undoCount();
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 16; ++i) {
        sendMouse(shell.window.get(), QEvent::MouseMove, a + (b - a) * (i / 16.0), Qt::NoButton, Qt::LeftButton);
        spin(5);
    }
    CHECK(panel->property("binDragging").toBool());
    CHECK(panel->property("binDropValid").toBool());
    CHECK(panel->property("binDropTarget").toString() == target);
    QQuickItem* mark = findNamed(onto, QStringLiteral("libraryCardDropMark"));
    REQUIRE(mark);
    CHECK(mark->isVisible());
    QQuickItem* chip = findNamed(root, QStringLiteral("libraryBinDragChip"));
    REQUIRE(chip);
    CHECK(chip->isVisible());
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
    REQUIRE(waitFor([&] { return controller->snapshot()->findBin(*parseBinKey(moving))->parent == *parseBinKey(target); }, 3000));
    CHECK(controller->session().undoCount() == undo + 1);
    CHECK_FALSE(panel->property("binDragging").toBool());

    // A bin dropped on itself goes nowhere.
    library->openBin(target);
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* inner = findNamed(root, QStringLiteral("libraryCard_") + moving);
    REQUIRE(inner);
    const QPointF c = inner->mapToScene(QPointF(inner->width() / 2, inner->height() / 3));
    dragWithMouse(shell.window.get(), c, c + QPointF(30, 0));
    spin(50);
    CHECK(controller->session().undoCount() == undo + 1);

    // From inside Selects, onto the breadcrumb's "Project": back at the root.
    QQuickItem* crumb = findNamed(root, QStringLiteral("libraryCrumb_root"));
    REQUIRE(crumb);
    inner = findNamed(root, QStringLiteral("libraryCard_") + moving);
    REQUIRE(inner);
    dragWithMouse(shell.window.get(), inner->mapToScene(QPointF(inner->width() / 2, inner->height() / 3)),
                  crumb->mapToScene(QPointF(crumb->width() / 2, crumb->height() / 2)));
    REQUIRE(waitFor([&] { return !controller->snapshot()->findBin(*parseBinKey(moving))->parent.isValid(); }, 3000));
    CHECK(controller->session().undoCount() == undo + 2);
    library->openBin(QString());

    // The list view: a bin row dragged onto another bin row.
    library->setView(QStringLiteral("list"));
    spin(100);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* list = findNamed(root, QStringLiteral("libraryList"));
    REQUIRE(list);
    const int fromRow = library->items()->rowOf(moving);
    const int ontoRow = library->items()->rowOf(target);
    REQUIRE(fromRow >= 0);
    REQUIRE(ontoRow >= 0);
    QQuickItem* table = findNamed(list, QStringLiteral("libraryTable"));
    REQUIRE(table);
    const auto rowPoint = [&](int row) {
        QQuickItem* content = table->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        const double rowHeight = shell.theme->property("rowHeight").toDouble();
        return content->mapToScene(QPointF(60, (row + 0.5) * rowHeight));
    };
    dragWithMouse(shell.window.get(), rowPoint(fromRow), rowPoint(ontoRow));
    REQUIRE(waitFor([&] { return controller->snapshot()->findBin(*parseBinKey(moving))->parent == *parseBinKey(target); }, 3000));
    CHECK(controller->session().undoCount() == undo + 3);

    // The Freeform view, in a bin of its own (room around the cards): a bin card dropped on the empty canvas moves
    // there; dropped on a bin card it goes in.
    library->setView(QStringLiteral("grid"));
    const QString area = library->newBin(QStringLiteral("Area"));
    library->openBin(area);
    const QString extras = library->newBin(QStringLiteral("Extras"));
    const QString keep = library->newBin(QStringLiteral("Keep"));
    library->clearSelection();
    library->setView(QStringLiteral("freeform"));
    spin(100);
    REQUIRE_FALSE(shell.grab().isNull());
    const auto freeformCard = [&](const QString& key) {
        return findNamed(findNamed(root, QStringLiteral("libraryFreeformCell_") + key), QStringLiteral("libraryCard_") + key);
    };
    QQuickItem* extrasCard = freeformCard(extras);
    QQuickItem* selectsCard = freeformCard(keep);
    REQUIRE(extrasCard);
    REQUIRE(selectsCard);
    QQuickItem* view = findNamed(root, QStringLiteral("libraryFreeform"));
    REQUIRE(view);
    const double yBefore = library->freeform().value(extras).toMap().value(QStringLiteral("y")).toDouble();
    // To an empty spot of the canvas below the card (no card there: LibraryFreeformView.keyAt says "").
    const QPointF grip = extrasCard->mapToScene(QPointF(extrasCard->width() / 2, extrasCard->height() / 3));
    QPointF foot(-1, -1);
    const QPointF viewOrigin = view->mapToScene(QPointF(0, 0));
    for (double y = view->height() - 8; y > grip.y() - viewOrigin.y() + 40 && foot.x() < 0; y -= 8) {
        for (double x = 8; x < view->width() - 8 && foot.x() < 0; x += 8) {
            QString under;
            REQUIRE(QMetaObject::invokeMethod(view, "keyAt", Q_RETURN_ARG(QString, under), Q_ARG(double, viewOrigin.x() + x),
                                              Q_ARG(double, viewOrigin.y() + y)));
            if (under.isEmpty())
                foot = QPointF(viewOrigin.x() + x, viewOrigin.y() + y);
        }
    }
    REQUIRE(foot.x() >= 0);
    // The card's own drag (the canvas's selection rectangle never takes it over: it used to, as soon as the card's drag
    // handler was a type of its own).
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, grip, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 16; ++i) {
        sendMouse(shell.window.get(), QEvent::MouseMove, grip + (foot - grip) * (i / 16.0), Qt::NoButton, Qt::LeftButton);
        spin(5);
    }
    CHECK(panel->property("binDragging").toBool());
    CHECK_FALSE(panel->property("binDropValid").toBool());
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, foot, Qt::LeftButton, Qt::NoButton);
    REQUIRE(waitFor([&] { return library->freeform().value(extras).toMap().value(QStringLiteral("y")).toDouble() > yBefore + 20; }, 3000));
    CHECK(controller->snapshot()->findBin(*parseBinKey(extras))->parent == *parseBinKey(area));
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    extrasCard = freeformCard(extras);
    selectsCard = freeformCard(keep);
    REQUIRE(extrasCard);
    REQUIRE(selectsCard);
    dragWithMouse(shell.window.get(), extrasCard->mapToScene(QPointF(extrasCard->width() / 2, extrasCard->height() / 3)),
                  selectsCard->mapToScene(QPointF(selectsCard->width() / 2, selectsCard->height() / 3)));
    REQUIRE(waitFor([&] { return controller->snapshot()->findBin(*parseBinKey(extras))->parent == *parseBinKey(keep); }, 3000));
    library->setView(QStringLiteral("grid"));
    library->openBin(QString());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Library P1: proxies for every selected item and for a whole bin", "[app][library][p1]") {
    P1Rig r;
    // Z becomes sound only, Y a still: neither gets a proxy.
    ProjectState next = *r.controller.snapshot();
    for (MediaPtr& m : next.media) {
        MediaItem item = *m;
        if (m->id == r.tl.media('Z'))
            item.info.video.clear();
        else if (m->id == r.tl.media('Y'))
            item.info.isStillImage = true;
        else
            continue;
        m = std::make_shared<const MediaItem>(std::move(item));
    }
    REQUIRE(r.controller.session().commit("Kinds", next));
    const QString bin = r.library.newBin(QStringLiteral("Day 1"));
    r.library.openBin(bin);
    const QString inner = r.library.newBin(QStringLiteral("Cam B"));
    r.library.openBin(QString());
    REQUIRE(r.library.moveItems({r.key('A'), r.key('Z')}, bin));
    REQUIRE(r.library.moveItems({r.key('B'), r.key('Y')}, inner));

    CHECK(r.library.mediaInBin(bin, false) == QStringList({r.key('A'), r.key('Z')}));
    CHECK(r.library.mediaInBin(bin, true).size() == 4);
    // A bin: its media with what its bins hold, only video.
    CHECK(r.library.proxyTargets(bin) == QStringList({r.key('A'), r.key('B')}));
    // A selection: every selected item that can have a proxy; an item outside the selection: itself.
    r.library.select(r.key('C'), LibraryBridge::Replace);
    r.library.select(r.key('D'), LibraryBridge::Add);
    r.library.select(r.key('X'), LibraryBridge::Add);
    CHECK(r.library.proxyTargets(r.key('C')).size() == 3);
    CHECK(r.library.proxyTargets(r.key('E')) == QStringList{r.key('E')});
    const QVariantMap plan = r.library.proxyPlan(bin);
    CHECK(plan.value(QStringLiteral("targets")).toInt() == 2);
    CHECK(plan.value(QStringLiteral("create")).toInt() == 2);
    CHECK(plan.value(QStringLiteral("ready")).toInt() == 0);
    // Without the proxy service (no MediaReadiness) nothing queues, and the line says so.
    CHECK(r.library.createProxies(bin).startsWith(QStringLiteral("0 queued")));
    CHECK(r.library.createProxies(r.key('Z')).contains(QStringLiteral("video")));
}

TEST_CASE("Library P1: File Size and Data Rate columns sort by bytes and the footer sums the selection",
          "[app][library][p1]") {
    P1Rig r;
    r.setBytes('A', 3 * 1024 * 1024);
    r.setBytes('B', 500 * 1024 * 1024);
    LibraryModel& model = *r.library.items();
    const auto cell = [&](const QString& k, LibraryColumn c) { return model.cellText(model.rowOf(k), c); };
    // Shown by default (Size), chosen (Data Rate): A is 3 MB over 40 s = 0.6 Mbps.
    bool sizeShown = false;
    for (const QVariant& c : r.library.columns())
        sizeShown = sizeShown || c.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("size");
    CHECK(sizeShown);
    CHECK(cell(r.key('A'), LibraryColumn::FileSize) == librarySizeText(3 * 1024 * 1024));
    CHECK(cell(r.key('A'), LibraryColumn::FileSize).contains(QStringLiteral("MB")));
    CHECK(cell(r.key('A'), LibraryColumn::DataRate).endsWith(QStringLiteral("kbps")));
    CHECK(cell(r.key('B'), LibraryColumn::DataRate).endsWith(QStringLiteral("Mbps")));
    CHECK(cell(r.key('C'), LibraryColumn::FileSize).isEmpty());
    // By bytes: 500 MB sorts after 3 MB (the text "3.0 MB" would sort after "500.0 MB").
    r.library.setColumnVisible(QStringLiteral("dataRate"), true);
    r.library.sortBy(QStringLiteral("size"));
    const QStringList keys = r.listed();
    CHECK(keys.indexOf(r.key('A')) < keys.indexOf(r.key('B')));
    r.library.sortBy(QStringLiteral("size"));
    CHECK(r.listed().indexOf(r.key('B')) < r.listed().indexOf(r.key('A')));

    // The footer: everything listed, then the selection's size and length.
    QString summary = r.library.summaryText();
    CHECK(summary.startsWith(QStringLiteral("27 item")));
    r.library.select(r.key('A'), LibraryBridge::Replace);
    r.library.select(r.key('B'), LibraryBridge::Add);
    summary = r.library.summaryText();
    INFO(summary.toStdString());
    CHECK(summary.contains(QStringLiteral("2 selected")));
    CHECK(summary.contains(librarySizeText(503 * 1024 * 1024)));
    CHECK(summary.endsWith(QStringLiteral("1:20"))); // 2 x 40 s
    // A selected bin counts what it holds.
    const QString bin = r.library.newBin(QStringLiteral("Big"));
    REQUIRE(r.library.moveItems({r.key('B')}, bin));
    r.library.select(bin, LibraryBridge::Replace);
    CHECK(r.library.summaryText().contains(librarySizeText(500 * 1024 * 1024)));
}

TEST_CASE("Library P1: Get Properties of a clip, a sequence and a bin, and Copy all", "[app][library][p1]") {
    P1Rig r;
    r.setBytes('A', 12 * 1024 * 1024);
    const QVariantMap info = r.library.mediaProperties(r.key('A'));
    REQUIRE_FALSE(info.isEmpty());
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("A"));
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
    CHECK(info.value(QStringLiteral("video")).toMap().value(QStringLiteral("width")).toInt() == 1920);
    CHECK(info.value(QStringLiteral("audio")).toList().size() == 2);
    CHECK(info.value(QStringLiteral("uses")).toInt() == 2); // V1 and A1
    CHECK(info.value(QStringLiteral("bitrate")).toDouble() > 0.0);
    const QVariantList lines = r.library.propertyLines(info);
    QStringList labels;
    for (const QVariant& l : lines)
        labels << l.toMap().value(QStringLiteral("label")).toString();
    INFO(labels.join(QStringLiteral(", ")).toStdString());
    for (const char* wanted : {"Format", "Duration", "Video", "Pixels", "Fields", "Audio 1", "Audio 2", "Size", "Data rate",
                               "Proxy", "Used", "Location"})
        CHECK(labels.contains(QString::fromLatin1(wanted)));

    // A sequence shows its settings; a bin what it holds.
    const QString sequence = sequenceKey(r.tl.seq);
    const QVariantList sequenceLines = r.library.propertyLines(r.library.mediaProperties(sequence));
    REQUIRE_FALSE(sequenceLines.isEmpty());
    CHECK(sequenceLines.front().toMap().value(QStringLiteral("value")).toString() == QStringLiteral("Sequence"));
    const QString bin = r.library.newBin(QStringLiteral("Hero"));
    REQUIRE(r.library.moveItems({r.key('A')}, bin));
    CHECK(r.library.mediaProperties(bin).value(QStringLiteral("media")).toInt() == 1);
    CHECK(r.library.mediaProperties(QStringLiteral("m99999")).isEmpty());

    // Copy all: the name, then "Label: value" lines, on the clipboard.
    const QString text = r.library.propertiesText(r.key('A'));
    CHECK(text.startsWith(QStringLiteral("A\nFormat: ")));
    REQUIRE(r.library.copyProperties(r.key('A')));
    CHECK(QGuiApplication::clipboard()->text() == text);

    // Get Properties for Selection: the Library's selection when it has the focus, else the selected timeline clip.
    r.library.select(r.key('C'), LibraryBridge::Replace);
    CHECK(r.library.propertiesTarget(true) == r.key('C'));
    r.controller.setSelection({r.tl.clip("V1@4")});
    CHECK(r.library.propertiesTarget(false) == r.key('B'));
    CHECK(r.library.commandAvailability(QStringLiteral("file.getProperties")).value(QStringLiteral("enabled")).toBool());
    r.library.clearSelection();
    r.controller.setSelection({});
    CHECK_FALSE(r.library.commandAvailability(QStringLiteral("file.getProperties")).value(QStringLiteral("enabled")).toBool());
}

TEST_CASE("Library P1: Ctrl+Shift+H shows the Properties dialog of the Library's selection", "[app][library][p1][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
        REQUIRE(library);
        REQUIRE(controller);
        REQUIRE(keys);
        library->setPersistent(false);
        const MediaPtr first = controller->snapshot()->media.front();
        const QString item = mediaKey(first->id);
        library->showItem(item);
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);
        REQUIRE(QMetaObject::invokeMethod(panel, "focusContent"));
        spin(50);
        REQUIRE(keys->trigger(QStringLiteral("file.getProperties")));
        QObject* dialog = shell.object("propertiesDialog");
        REQUIRE(dialog);
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
        CHECK(dialog->property("key").toString() == item);
        CHECK(listOf(dialog->property("lines")).size() >= 4);
        CHECK(dialog->property("title").toString().contains(QString::fromStdString(first->name)));
        spin(100);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        QQuickItem* list = findNamed(shell.window->contentItem(), QStringLiteral("propertiesList"));
        REQUIRE(list);
        QQuickItem* card = list;
        while (card->parentItem() && card->parentItem() != shell.window->contentItem() &&
               !QByteArray(card->parentItem()->metaObject()->className()).contains("Overlay"))
            card = card->parentItem();
        saveItemShot(image, card, dark ? QStringLiteral("properties-dialog-dark.png") : QStringLiteral("properties-dialog-light.png"));
        QMetaObject::invokeMethod(dialog, "close");
        spin(150);
        if (dark) {
            // Ctrl+X with the Library focused (Main.runCommand) cuts its selection; Escape forgets it.
            REQUIRE(QMetaObject::invokeMethod(panel, "focusContent"));
            spin(50);
            keys->trigger(QStringLiteral("edit.cut"));
            CHECK(library->cutKeys() == QStringList{item});
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(shell.window.get(), &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(shell.window.get(), &release);
            spin(20);
            CHECK(library->cutKeys().isEmpty());
        }
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// Fix round 1 (ARGON-23): Search… and Locate… open where the last relink found media, not the project folder again.
TEST_CASE("Library P1: the Relink dialog opens its pickers in the last folder used", "[app][library][p1][qml]") {
    resetSettings();
    clearMessages();
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(project);
        QObject* dialog = shell.object("relinkDialog");
        REQUIRE(dialog);
        CHECK(dialog->property("startFolder").toUrl() == project->folder());
        QTemporaryDir drive;
        REQUIRE(drive.isValid());
        const QUrl moved = QUrl::fromLocalFile(drive.path());
        REQUIRE(QMetaObject::invokeMethod(dialog, "rememberFolder", Q_ARG(QVariant, QVariant::fromValue(moved))));
        CHECK(dialog->property("startFolder").toUrl() == moved);
        // Remembered for the next session too (the QML Settings write it out).
        CHECK(waitFor([&] { return QSettings().value(QStringLiteral("relink/lastFolder")).toString() == moved.toString(); }, 5000));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Library P1: Link Media and a double-click open the Relink dialog for offline items only",
          "[app][library][p1][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(library);
        REQUIRE(controller);
        library->setPersistent(false);
        useOwnProject(shell);
        const ProjectSnapshot snap = controller->snapshot();
        REQUIRE(snap->media.size() >= 2);
        QString offline;
        QString online;
        for (const MediaPtr& m : snap->media) {
            if (m->isGenerator())
                continue;
            if (offline.isEmpty())
                offline = mediaKey(m->id);
            else if (online.isEmpty())
                online = mediaKey(m->id);
        }
        REQUIRE_FALSE(offline.isEmpty());
        REQUIRE_FALSE(online.isEmpty());
        makeOffline(controller, offline);
        makeOffline(controller, online); // two offline: the dialog lists only the one asked for
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(project);
        CHECK(project->offlineCount() == 2);
        QObject* dialog = shell.object("relinkDialog");
        REQUIRE(dialog);

        // A double-click on the offline item (LibraryPanel.activate): the dialog for it alone.
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);
        REQUIRE(QMetaObject::invokeMethod(panel, "activate", Q_ARG(QVariant, offline), Q_ARG(QVariant, QStringLiteral("video"))));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
        CHECK(stringsOf(dialog->property("only")) == QStringList{offline});
        CHECK(listOf(dialog->property("shown")).size() == 1);
        QQuickItem* showAll = findNamed(shell.window->contentItem(), QStringLiteral("relinkShowAll"));
        REQUIRE(showAll);
        CHECK(showAll->isVisible());
        spin(100);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        QQuickItem* summary = findNamed(shell.window->contentItem(), QStringLiteral("relinkSummary"));
        REQUIRE(summary);
        QQuickItem* card = summary;
        while (card->parentItem() && card->parentItem() != shell.window->contentItem() &&
               !QByteArray(card->parentItem()->metaObject()->className()).contains("Overlay"))
            card = card->parentItem();
        saveItemShot(image, card, dark ? QStringLiteral("relink-link-media-dark.png") : QStringLiteral("relink-link-media-light.png"));
        // Show all offline media lists both again.
        REQUIRE(QMetaObject::invokeMethod(showAll, "clicked"));
        CHECK(listOf(dialog->property("shown")).size() >= 2);
        QMetaObject::invokeMethod(dialog, "close");
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
        CHECK(stringsOf(dialog->property("only")).isEmpty());

        // Link Media… asks through the bridge (the item menu and the timeline clip menu do): items that are not offline
        // are left out, and with none the dialog stays shut and a toast says why.
        QStringList asked;
        QObject::connect(library, &LibraryBridge::linkMediaRequested, library, [&](const QStringList& k) { asked = k; });
        library->requestLinkMedia({online});
        CHECK(asked == QStringList{online});
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
        QMetaObject::invokeMethod(dialog, "close");
        spin(100);
        asked.clear();
        QString title;
        for (const MediaPtr& m : controller->snapshot()->media) {
            if (m->isGenerator() || mediaKey(m->id) == offline || mediaKey(m->id) == online)
                continue;
            title = mediaKey(m->id);
            break;
        }
        if (!title.isEmpty()) {
            library->requestLinkMedia({title});
            CHECK(asked.isEmpty());
            CHECK_FALSE(library->message().isEmpty());
        }
        // The item menu offers Link Media… on the offline item (first row).
        QObject* menu = shell.object("libraryItemMenu");
        if (!menu)
            menu = panel->findChild<QObject*>(QStringLiteral("libraryItemMenu"));
        REQUIRE(menu);
        library->select(offline, LibraryBridge::Replace);
        QVariant ignored;
        REQUIRE(QMetaObject::invokeMethod(menu, "openFor", Q_RETURN_ARG(QVariant, ignored), Q_ARG(QVariant, QVariant::fromValue<QObject*>(panel)),
                                          Q_ARG(QVariant, offline), Q_ARG(QVariant, QStringLiteral("video")),
                                          Q_ARG(QVariant, QPointF(40, 40)), Q_ARG(QVariant, -1)));
        spin(100);
        CHECK(stringsOf(menu->property("offlineTargets")) == QStringList{offline});
        QMetaObject::invokeMethod(menu, "close");
        spin(100);
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Import report: Relink shows when media is offline and the buttons stay in a small window",
          "[app][library][p1][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(1100, 700); // the smallest window
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        useOwnProject(shell); // an import leaves the sample project for a project of its own
        QString offline;
        for (const MediaPtr& m : controller->snapshot()->media) {
            if (!m->isGenerator()) {
                offline = mediaKey(m->id);
                break;
            }
        }
        REQUIRE_FALSE(offline.isEmpty());
        makeOffline(controller, offline);
        // A long report: many offline files and many entries (what used to push the buttons off the window).
        QVariantList unresolved;
        QVariantList entries;
        for (int i = 0; i < 40; ++i) {
            unresolved.append(QVariantMap{{QStringLiteral("name"), QStringLiteral("Interview_Camera_A_%1.mov").arg(i)},
                                          {QStringLiteral("path"), QStringLiteral("D:/Shoot/Interview_Camera_A_%1.mov").arg(i)}});
            entries.append(QVariantMap{{QStringLiteral("outcome"), QStringLiteral("approximated")},
                                       {QStringLiteral("category"), QStringLiteral("effect")},
                                       {QStringLiteral("reason"), QStringLiteral("Three-Way Color Corrector became Lumetri basic correction")},
                                       {QStringLiteral("sequence"), QStringLiteral("Reel 2 Assembly")},
                                       {QStringLiteral("location"), QStringLiteral("line %1").arg(100 + i)}});
        }
        const QVariantMap report{{QStringLiteral("file"), QStringLiteral("cut.xml")},
                                 {QStringLiteral("format"), QStringLiteral("Final Cut Pro 7 XML")},
                                 {QStringLiteral("sequences"), 1},
                                 {QStringLiteral("media"), 40},
                                 {QStringLiteral("unresolved"), unresolved},
                                 {QStringLiteral("carried"), 120},
                                 {QStringLiteral("approximated"), 40},
                                 {QStringLiteral("dropped"), 0},
                                 {QStringLiteral("entries"), entries},
                                 {QStringLiteral("summary"), QStringLiteral("Final Cut Pro 7 XML · 1 sequence · 40 media files · 40 offline")}};
        QObject* dialog = shell.object("importReportDialog");
        REQUIRE(dialog);
        REQUIRE(QMetaObject::invokeMethod(dialog, "show", Q_ARG(QVariant, report)));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
        spin(100);
        QQuickItem* root = shell.window->contentItem();
        QQuickItem* relink = findNamed(root, QStringLiteral("importReportRelink"));
        QQuickItem* close = findNamed(root, QStringLiteral("importReportClose"));
        REQUIRE(relink);
        REQUIRE(close);
        CHECK(relink->isVisible());
        CHECK(close->property("variant").toString() == QStringLiteral("ghost"));
        // The footer is inside the window.
        const QRectF closeRect = close->mapRectToScene(QRectF(0, 0, close->width(), close->height()));
        INFO("close button at " << closeRect.y() << " + " << closeRect.height());
        CHECK(closeRect.bottom() <= 700);
        CHECK(closeRect.top() >= 0);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("import-report-relink-dark.png")
                                                            : QStringLiteral("import-report-relink-light.png"))));
        // Relink…: the report closes and the Relink dialog opens.
        REQUIRE(QMetaObject::invokeMethod(relink, "clicked"));
        QObject* relinkDialog = shell.object("relinkDialog");
        REQUIRE(relinkDialog);
        REQUIRE(waitFor([&] { return relinkDialog->property("opened").toBool(); }, 3000));
        CHECK_FALSE(dialog->property("opened").toBool());
        QMetaObject::invokeMethod(relinkDialog, "close");
        spin(100);
        // An export report (or nothing offline) has no Relink….
        QVariantMap exported = report;
        exported.insert(QStringLiteral("exported"), true);
        REQUIRE(QMetaObject::invokeMethod(dialog, "show", Q_ARG(QVariant, exported)));
        spin(100);
        CHECK_FALSE(relink->isVisible());
        QMetaObject::invokeMethod(dialog, "close");
        spin(100);
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Screenshots: the Library's Move to Bin menu and its footer line", "[app][library][p1][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(library);
        REQUIRE(controller);
        library->setPersistent(false);
        library->setView(QStringLiteral("grid"));
        const QString footage = library->newBin(QStringLiteral("Footage"));
        library->openBin(footage);
        library->newBin(QStringLiteral("Day 1"));
        library->openBin(QString());
        library->newBin(QStringLiteral("Music"));
        const MediaPtr first = controller->snapshot()->media.front();
        const QString item = mediaKey(first->id);
        library->select(item, LibraryBridge::Replace);
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);
        QQuickItem* summary = findNamed(panel, QStringLiteral("librarySummary"));
        REQUIRE(summary);
        CHECK(summary->isVisible());
        CHECK(summary->property("text").toString().contains(QStringLiteral("1 selected")));
        QObject* menu = panel->findChild<QObject*>(QStringLiteral("libraryItemMenu"));
        REQUIRE(menu);
        QQuickItem* card = findNamed(panel, QStringLiteral("libraryCard_") + item);
        REQUIRE(card);
        QVariant ignored;
        REQUIRE(QMetaObject::invokeMethod(menu, "openFor", Q_RETURN_ARG(QVariant, ignored), Q_ARG(QVariant, QVariant::fromValue<QObject*>(card)),
                                          Q_ARG(QVariant, item), Q_ARG(QVariant, QStringLiteral("video")),
                                          Q_ARG(QVariant, QPointF(20, 20)), Q_ARG(QVariant, -1)));
        spin(150);
        CHECK(listOf(menu->property("bins")).size() == 4); // the root, Footage, Day 1, Music
        QObject* moveMenu = menu->findChild<QObject*>(QStringLiteral("libraryMenuMoveToBin"));
        REQUIRE(moveMenu);
        REQUIRE(QMetaObject::invokeMethod(moveMenu, "open"));
        spin(200);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("library-move-to-bin-dark.png")
                                                            : QStringLiteral("library-move-to-bin-light.png"))));
        QMetaObject::invokeMethod(moveMenu, "close");
        QMetaObject::invokeMethod(menu, "close");
        spin(100);
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Relink: ProjectFile relinks the rest after Locate and offers a recording in another container",
          "[app][library][p1][relink]") {
    const QDir media(QStringLiteral(WABCUT_TEST_MEDIA_DIR));
    for (const char* name : {"h264_1080p60_aac.mp4", "sine_48k.wav", "untitled_three_track.mkv", "obs_hybrid_titled.mp4"}) {
        if (!QFileInfo::exists(media.filePath(QString::fromLatin1(name))))
            SKIP("test media missing");
    }
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QDir dir(temp.path());
    REQUIRE(dir.mkpath(QStringLiteral("shoot/a")));
    REQUIRE(dir.mkpath(QStringLiteral("shoot/b")));
    REQUIRE(dir.mkpath(QStringLiteral("project")));
    REQUIRE(QFile::copy(media.filePath(QStringLiteral("h264_1080p60_aac.mp4")), dir.filePath(QStringLiteral("shoot/a/clip.mp4"))));
    REQUIRE(QFile::copy(media.filePath(QStringLiteral("sine_48k.wav")), dir.filePath(QStringLiteral("shoot/b/sound.wav"))));
    REQUIRE(QFile::copy(media.filePath(QStringLiteral("untitled_three_track.mkv")), dir.filePath(QStringLiteral("shoot/Match 3.mkv"))));
    const QUrl projectFile = QUrl::fromLocalFile(dir.filePath(QStringLiteral("project/p.wabcut")));
    QString clip;
    {
        ProjectController controller;
        ProjectLifecycle project(&controller);
        project.newProject();
        project.importFiles({QUrl::fromLocalFile(dir.filePath(QStringLiteral("shoot/a/clip.mp4"))),
                                                         QUrl::fromLocalFile(dir.filePath(QStringLiteral("shoot/b/sound.wav"))),
                                                         QUrl::fromLocalFile(dir.filePath(QStringLiteral("shoot/Match 3.mkv")))});
        INFO(project.lastError().toStdString());
        REQUIRE(controller.snapshot()->media.size() == 3);
        for (const MediaPtr& m : controller.snapshot()->media) {
            if (QString::fromStdString(m->locator.path).endsWith(QStringLiteral("clip.mp4")))
                clip = mediaKey(m->id);
        }
        REQUIRE(project.saveAs(projectFile));
    }
    REQUIRE_FALSE(clip.isEmpty());
    // The shoot moves into an archive, and the recording is remuxed to .mp4 there.
    REQUIRE(dir.mkpath(QStringLiteral("archive")));
    REQUIRE(QDir().rename(dir.filePath(QStringLiteral("shoot")), dir.filePath(QStringLiteral("archive/shoot"))));
    REQUIRE(QFile::remove(dir.filePath(QStringLiteral("archive/shoot/Match 3.mkv"))));
    REQUIRE(QFile::copy(media.filePath(QStringLiteral("obs_hybrid_titled.mp4")), dir.filePath(QStringLiteral("archive/shoot/Match 3.mp4"))));

    ProjectController controller;
    ProjectLifecycle project(&controller);
    REQUIRE(project.open(projectFile));
    REQUIRE(project.offlineCount() == 3);
    // Locate... on the clip: the sound beside it under the moved root comes with it ("1 other").
    const int others = project.relinkLocated(clip, QUrl::fromLocalFile(dir.filePath(QStringLiteral("archive/shoot/a/clip.mp4"))));
    INFO(project.lastError().toStdString());
    CHECK(others == 1);
    CHECK(project.offlineCount() == 1);
    // A different file is refused with the reason.
    CHECK(project.relinkLocated(clip, QUrl::fromLocalFile(dir.filePath(QStringLiteral("archive/shoot/b/sound.wav")))) == -1);
    CHECK_FALSE(project.lastError().isEmpty());
    // The remuxed recording is offered (.mkv -> .mp4) and relinked in one undo step.
    const QVariantList offers = project.sameRecordings({QUrl::fromLocalFile(dir.filePath(QStringLiteral("archive")))});
    REQUIRE(offers.size() == 1);
    CHECK(offers.front().toMap().value(QStringLiteral("from")).toString() == QStringLiteral(".mkv"));
    CHECK(offers.front().toMap().value(QStringLiteral("to")).toString() == QStringLiteral(".mp4"));
    const auto undo = controller.session().undoCount();
    CHECK(project.relinkSameRecordings(offers) == 1);
    CHECK(project.offlineCount() == 0);
    CHECK(controller.session().undoCount() == undo + 1);
}
