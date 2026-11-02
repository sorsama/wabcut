// 0.6 (P1 round, stream 2D): the Library.
//   * #40 usage: the Video Usage and Audio Usage columns, the card's usage badge and "Unused" mark, the places an item
//     is used (Library.usages: a linked pair is one) and Find in Timeline (the sequence opens, the playhead goes to the
//     clip, the clip is selected and the timeline scrolls to it);
//   * #38 deleting items sequences use: deleteImpact, the confirmation (the plain delete still refuses), Delete and
//     Remove Clips in one undo step, Delete with Options (Ctrl/Cmd+Delete) and the popover in the window;
//   * #46 twirl-down bins in the list view: the tree (each level sorted on its own), Alt+click opens every level, a
//     collapse moves the selection to the bin, Find stays flat, kept per project; the chevron and Right / Left keys.
// Screenshots (dark and light): the usage badge and its menu, the delete confirmation, the list with twirled bins.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QStyleHints>
#include <QJSValue>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <qpa/qwindowsysteminterface.h>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// A Library over a project from a timeline picture: A on V1 + A1 (linked) and again on V1 alone, B nested into a
/// sequence "Nest" (its nested clips, linked, where B was), C..Z unused.
struct UsageRig {
    Timeline tl;
    ProjectController controller;
    LibraryBridge library;
    SequenceId nest;

    UsageRig()
        : tl(R"(
            V1 tgt |AaaaBbbbAaaa|
            A1 tgt |AaaaBbbb....|
        )") {
        tl.linkMatching();
        auto nested = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@4")}, "Nest");
        REQUIRE(nested);
        nest = nested->nested;
        controller.resetProject(nested->edit.state, tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setAnalysisProbe({});
        library.setController(&controller);
    }
    QString key(char letter) const { return mediaKey(tl.media(letter)); }
    QString nestKey() const { return sequenceKey(nest); }
    QString cell(const QString& itemKey, LibraryColumn column) const {
        return library.items()->cellText(library.items()->rowOf(itemKey), column);
    }
    QVariant role(const QString& itemKey, int role) const {
        return library.items()->data(library.items()->index(library.items()->rowOf(itemKey), 0), role);
    }
    QStringList listed() const {
        QStringList out;
        for (int r = 0; r < library.items()->rowCount(); ++r)
            out << library.items()->keyOf(r);
        return out;
    }
    std::size_t undo() { return controller.session().undoCount(); }
    ClipId clip(std::string_view ref) const { return clipAt(*controller.snapshot(), tl.seq, ref); }
    bool hasClipOf(char letter) const {
        bool found = false;
        controller.snapshot()->findSequence(tl.seq)->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips)
                found = found || c.media == tl.media(letter);
        });
        return found;
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

QObject* findObject(QObject* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    return root->findChild<QObject*>(name);
}

QVariantList listOf(const QVariant& value) {
    if (value.userType() == qMetaTypeId<QJSValue>())
        return value.value<QJSValue>().toVariant().toList();
    return value.toList();
}

/// What deleteConfirmRequested asked: keys, impact, withOptions.
struct DeleteAsk {
    QStringList keys;
    QVariantMap impact;
    bool withOptions = false;
};
/// A real click through the window system path: the pointer moves there first, then presses and releases.
void click(QQuickWindow* window, QPointF at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    spin(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 30);
    const QPointF global = window->mapToGlobal(at);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        window, at, global, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, modifiers);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        window, at, global, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress, modifiers);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        window, at, global, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease, modifiers);
    spin(30);
}

void key(QQuickWindow* window, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers);
    QCoreApplication::sendEvent(window, &release);
    spin(30);
}

/// Saves the part of `image` under `item` as `name` in the UI test output (grown by `margin` scene pixels).
void saveItemShot(const QImage& image, const QQuickItem* item, const QString& name, double margin = 0) {
    const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                            .adjusted(-margin, -margin, margin, margin)
                            .intersected(QRectF(0, 0, image.width() / image.devicePixelRatio(),
                                                image.height() / image.devicePixelRatio()));
    const double dpr = image.devicePixelRatio();
    REQUIRE(image.copy(QRect(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                             static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr)))
                .save(QDir(outputDir()).filePath(name)));
}

/// The popup (menu, popover) holding `item`: its item under the overlay.
QQuickItem* popupOf(QQuickItem* item) {
    QQuickItem* at = item;
    while (at && at->parentItem() && !QByteArray(at->parentItem()->metaObject()->className()).contains("Overlay"))
        at = at->parentItem();
    return at;
}

/// Leaves the shell's sample project for one from a timeline picture (as UsageRig's, with the nest), not the demo.
SequenceId useOwnProject(Shell& shell, Timeline& tl) {
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(project);
    REQUIRE(controller);
    project->newProject();
    tl.linkMatching();
    auto nested = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@4")}, "Nest");
    REQUIRE(nested);
    controller->resetProject(nested->edit.state, tl.seq);
    spin(50);
    REQUIRE_FALSE(project->demo());
    return nested->nested;
}

} // namespace

// ---- #40 usage ----------------------------------------------------------------------------------------------------

TEST_CASE("Library usage: Video and Audio Usage columns and the uses a linked pair counts once", "[app][library][p1][usage]") {
    UsageRig r;
    // A: on V1 + A1 (linked: one use) and on V1 alone (a second use).
    const LibraryUsage a = r.library.usage().at(r.tl.media('A').value);
    CHECK(a.video == 2);
    CHECK(a.audio == 1);
    CHECK(a.uses == 2);
    // B lives in Nest now; Nest itself is used once in the main sequence (its linked nested pair).
    CHECK(r.library.usage().at(r.tl.media('B').value).uses == 1);
    const LibraryUsage nest = r.library.sequenceUsage().at(r.nest.value);
    CHECK(nest.video == 1);
    CHECK(nest.audio == 1);
    CHECK(nest.uses == 1);
    CHECK_FALSE(r.library.usage().contains(r.tl.media('C').value));

    // The columns: hidden until chosen, numbers (blank for bins), sorted by count.
    bool listed = false;
    for (const QVariant& c : r.library.allColumns()) {
        const QVariantMap column = c.toMap();
        if (column.value("id").toString() == QStringLiteral("videoUsage")) {
            listed = true;
            CHECK_FALSE(column.value("visible").toBool());
            CHECK(column.value("title").toString() == QStringLiteral("Video Usage"));
        }
    }
    CHECK(listed);
    CHECK(r.cell(r.key('A'), LibraryColumn::VideoUsage) == QStringLiteral("2"));
    CHECK(r.cell(r.key('A'), LibraryColumn::AudioUsage) == QStringLiteral("1"));
    CHECK(r.cell(r.key('C'), LibraryColumn::VideoUsage) == QStringLiteral("0"));
    CHECK(r.cell(r.nestKey(), LibraryColumn::VideoUsage) == QStringLiteral("1"));
    r.library.setColumnVisible(QStringLiteral("videoUsage"), true);
    r.library.sortBy(QStringLiteral("videoUsage"));
    r.library.sortBy(QStringLiteral("videoUsage")); // descending: the most used media first (after the sequences)
    const QStringList order = r.listed();
    CHECK(order.indexOf(r.key('A')) < order.indexOf(r.key('B')));
    CHECK(order.indexOf(r.key('B')) < order.indexOf(r.key('C')));

    // The roles the card reads: the badge's count and the "Unused" mark.
    CHECK(r.role(r.key('A'), LibraryModel::UsageRole).toInt() == 2);
    CHECK(r.role(r.key('A'), LibraryModel::VideoUsageRole).toInt() == 2);
    CHECK(r.role(r.key('C'), LibraryModel::UsageRole).toInt() == 0);
    CHECK(r.role(r.key('C'), LibraryModel::UnusedRole).toBool());
    CHECK_FALSE(r.role(r.key('A'), LibraryModel::UnusedRole).toBool());
    CHECK(r.role(r.nestKey(), LibraryModel::UsageRole).toInt() == 1);

    // An edit that takes one use away updates the counts (the same media were used before and after).
    ProjectState next = *r.controller.snapshot();
    auto lifted = edit::deleteClips(next, r.tl.seq, std::vector<ClipId>{r.clip("V1@8")});
    REQUIRE(lifted);
    REQUIRE(r.controller.session().commit("Lift", lifted->state));
    spin(10);
    CHECK(r.cell(r.key('A'), LibraryColumn::VideoUsage) == QStringLiteral("1"));
    CHECK(r.role(r.key('A'), LibraryModel::UsageRole).toInt() == 1);
}

TEST_CASE("Library usage: the places an item is used and Find in Timeline opens the sequence at the clip", "[app][library][p1][usage]") {
    UsageRig r;
    const QVariantList uses = r.library.usages(r.key('A'));
    REQUIRE(uses.size() == 2);
    const QVariantMap first = uses[0].toMap();
    const QVariantMap second = uses[1].toMap();
    CHECK(first.value("sequenceKey").toString() == sequenceKey(r.tl.seq));
    CHECK(first.value("timecode").toString() == QStringLiteral("00:00:00:00"));
    CHECK(first.value("tracks").toString() == QStringLiteral("V1, A1"));
    CHECK(first.value("clipId").toString() == clipKey(r.clip("V1@0"))); // the video clip of the linked pair
    CHECK(second.value("timecode").toString() == QStringLiteral("00:00:00:08"));
    CHECK(second.value("tracks").toString() == QStringLiteral("V1"));
    CHECK(r.library.usageText(r.key('A')) == QStringLiteral("Used 2 times in 1 sequence (video 2, audio 1)"));
    CHECK(r.library.usageText(r.key('C')) == QStringLiteral("Not used in any sequence"));
    CHECK(r.library.usages(r.key('C')).isEmpty());
    // B is used inside Nest; Nest is used (nested) in the main sequence.
    const QVariantList inNest = r.library.usages(r.key('B'));
    REQUIRE(inNest.size() == 1);
    CHECK(inNest[0].toMap().value("sequenceName").toString() == QStringLiteral("Nest"));
    const QVariantList nestUses = r.library.usages(r.nestKey());
    REQUIRE(nestUses.size() == 1);
    CHECK(nestUses[0].toMap().value("tracks").toString() == QStringLiteral("V1, A1"));

    // Find in Timeline: the sequence opens (here: from Nest back to the main sequence), the playhead goes to the clip's
    // start, the clip is selected with its linked partner, and the window is asked to scroll to it.
    QStringList reveal;
    QObject::connect(&r.library, &LibraryBridge::revealInTimelineRequested, &r.library,
                     [&reveal](const QString& clip) { reveal << clip; });
    REQUIRE(r.library.findInTimeline(inNest[0].toMap().value("sequenceKey").toString(),
                                     inNest[0].toMap().value("clipId").toString()));
    CHECK(r.controller.currentSequenceId() == r.nest);
    REQUIRE(r.library.findInTimeline(second.value("sequenceKey").toString(), second.value("clipId").toString()));
    CHECK(r.controller.currentSequenceId() == r.tl.seq);
    CHECK(r.controller.playhead() == f(8));
    CHECK(r.controller.selection() == std::vector<ClipId>{r.clip("V1@8")});
    REQUIRE(reveal.size() == 2);
    CHECK(reveal.back() == second.value("clipId").toString());

    // A clip that is gone says so.
    CHECK_FALSE(r.library.findInTimeline(sequenceKey(r.tl.seq), clipKey(ClipId{987654})));
    CHECK_FALSE(r.library.message().isEmpty());
}

// ---- #38 delete --------------------------------------------------------------------------------------------------

TEST_CASE("Library delete: media in use asks first then goes with its clips in one undo step", "[app][library][p1][delete]") {
    UsageRig r;
    const QVariantMap impact = r.library.deleteImpact({r.key('A')});
    CHECK(impact.value("clips").toInt() == 3); // V1@0 with its linked A1@0, and V1@8
    CHECK(impact.value("sequences").toInt() == 1);
    CHECK(impact.value("firstName").toString() == QStringLiteral("A"));
    CHECK(impact.value("usedItems").toInt() == 1);
    CHECK(r.library.deleteImpact({r.key('C')}).value("clips").toInt() == 0);

    // The plain delete still refuses (and says why); the request asks instead of deleting.
    const auto before = r.undo();
    CHECK_FALSE(r.library.deleteItems({r.key('A')}));
    CHECK_FALSE(r.library.message().isEmpty());
    std::vector<DeleteAsk> asked;
    QObject::connect(&r.library, &LibraryBridge::deleteConfirmRequested, &r.library,
                     [&asked](const QStringList& keys, const QVariantMap& impact, bool withOptions) {
                         asked.push_back({keys, impact, withOptions});
                     });
    CHECK(r.library.requestDelete({r.key('A')}));
    REQUIRE(asked.size() == 1);
    CHECK(asked.front().keys == QStringList{r.key('A')});
    CHECK(asked.front().impact.value("clips").toInt() == 3);
    CHECK_FALSE(asked.front().withOptions);
    CHECK(r.undo() == before);
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('A')) != nullptr);
    // Unused media goes at once (no question).
    CHECK(r.library.requestDelete({r.key('C')}));
    CHECK(asked.size() == 1);
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('C')) == nullptr);
    CHECK(r.undo() == before + 1);
    // Delete with Options with "Remove their clips" off (fix round 1, ARGON-12): only the items no sequence uses go;
    // the impact names them (the panel's Delete Unused Items deletes those keys).
    r.controller.undo();
    REQUIRE(r.controller.snapshot()->findMedia(r.tl.media('C')) != nullptr);
    const QVariantMap mixed = r.library.deleteImpact({r.key('A'), r.key('C')});
    CHECK(mixed.value("clips").toInt() == 3);
    CHECK(mixed.value("unusedKeys").toStringList() == QStringList{r.key('C')});
    CHECK(r.library.deleteImpact({r.key('A')}).value("unusedKeys").toStringList().isEmpty());
    REQUIRE(r.library.deleteItems(mixed.value("unusedKeys").toStringList(), false));
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('C')) == nullptr);
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('A')) != nullptr);
    CHECK(r.hasClipOf('A'));
    CHECK(r.undo() == before + 1);

    // Confirmed: A and its three clips go in one step ("Delete and Remove Clips"); the gaps stay (no ripple).
    REQUIRE(r.library.deleteItems({r.key('A')}, true));
    CHECK(r.undo() == before + 2);
    CHECK(r.controller.session().undoLabel() == "Delete and Remove Clips");
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('A')) == nullptr);
    CHECK_FALSE(r.hasClipOf('A'));
    const Sequence* main = r.controller.snapshot()->findSequence(r.tl.seq);
    REQUIRE(main != nullptr);
    CHECK(main->videoTracks.front()->clips.size() == 1); // the nested clip, still at frame 4
    CHECK(main->videoTracks.front()->clips.front().start() == f(4));
    r.controller.undo();
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('A')) != nullptr);
    CHECK(r.hasClipOf('A'));

    // A bin holding media in use, and a sequence nested in another, go the same way.
    const QString bin = r.library.newBin(QStringLiteral("Selects"));
    REQUIRE(r.library.moveItems({r.key('A')}, bin));
    CHECK(r.library.deleteImpact({bin}).value("clips").toInt() == 3);
    CHECK_FALSE(r.library.deleteItems({bin}));
    REQUIRE(r.library.deleteItems({bin}, true));
    CHECK_FALSE(r.hasClipOf('A'));
    r.controller.undo();
    CHECK(r.library.deleteImpact({r.nestKey()}).value("clips").toInt() == 2);
    REQUIRE(r.library.deleteItems({r.nestKey()}, true));
    CHECK(r.controller.snapshot()->findSequence(r.nest) == nullptr);
    CHECK(r.controller.snapshot()->findSequence(r.tl.seq)->videoTracks.front()->clips.size() == 2); // A, A
}

// Fix round 3 (NEW-03): Delete Unused Items is a fixed point over the whole selection - a sequence that only another
// selected and unused sequence nests goes in the same step, instead of being left behind for a second Delete.
TEST_CASE("Library delete: Delete Unused Items takes a nested sequence only an unused selected sequence nests",
          "[app][library][p1][delete]") {
    UsageRig r;
    const QString mainKey = sequenceKey(r.tl.seq); // nothing nests the main sequence
    CHECK(r.library.deleteImpact({mainKey}).value("unusedKeys").toStringList() == QStringList{mainKey});
    // "Nest" alone is used by the main sequence, which is not in the selection: it stays.
    CHECK(r.library.deleteImpact({r.nestKey()}).value("unusedKeys").toStringList().isEmpty());
    // Selected together the main sequence goes too, so its only use of "Nest" does not count: both are unused.
    const QVariantMap both = r.library.deleteImpact({mainKey, r.nestKey()});
    CHECK(both.value("unusedKeys").toStringList() == QStringList({mainKey, r.nestKey()}));
    // Both sequences leave in one step (the main one first, freeing the nested one); the media stays in the library.
    REQUIRE(r.library.deleteItems(both.value("unusedKeys").toStringList(), false));
    CHECK(r.controller.snapshot()->findSequence(r.tl.seq) == nullptr);
    CHECK(r.controller.snapshot()->findSequence(r.nest) == nullptr);
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('B')) != nullptr);
}

TEST_CASE("Library delete: Delete with Options asks every time and Ctrl+Delete runs it", "[app][library][p1][delete]") {
    UsageRig r;
    CHECK(LibraryBridge::handlesCommand(QStringLiteral("project.deleteWithOptions")));
    CHECK_FALSE(r.library.commandAvailability(QStringLiteral("project.deleteWithOptions")).value("enabled").toBool());
    r.library.select(r.key('C'), LibraryBridge::Replace);
    CHECK(r.library.commandAvailability(QStringLiteral("project.deleteWithOptions")).value("enabled").toBool());
    std::vector<DeleteAsk> asked;
    QObject::connect(&r.library, &LibraryBridge::deleteConfirmRequested, &r.library,
                     [&asked](const QStringList& keys, const QVariantMap& impact, bool withOptions) {
                         asked.push_back({keys, impact, withOptions});
                     });
    CHECK(r.library.runCommand(QStringLiteral("project.deleteWithOptions")));
    REQUIRE(asked.size() == 1);
    CHECK(asked.front().withOptions);
    CHECK(asked.front().impact.value("clips").toInt() == 0);
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('C')) != nullptr); // asked, not deleted
    // project.delete (Delete / Backspace) on media in use asks too.
    r.library.select(r.key('A'), LibraryBridge::Replace);
    CHECK(r.library.runCommand(QStringLiteral("project.delete")));
    CHECK(asked.size() == 2);
    CHECK_FALSE(asked.back().withOptions);
    CHECK(r.controller.snapshot()->findMedia(r.tl.media('A')) != nullptr);
}

// ---- #46 twirl-down bins -----------------------------------------------------------------------------------------

TEST_CASE("Library tree: twirl-down bins list their contents under them sorted within each level", "[app][library][p1][tree]") {
    UsageRig r;
    const QString footage = r.library.newBin(QStringLiteral("Footage"));
    r.library.openBin(QString());
    const QString day = r.library.newBin(QStringLiteral("Day 1"));
    REQUIRE(r.library.moveItems({day}, footage));
    REQUIRE(r.library.moveItems({r.key('A'), r.key('B')}, footage));
    REQUIRE(r.library.moveItems({r.key('C')}, day));
    r.library.setView(QStringLiteral("list"));
    CHECK(r.library.treeShown());

    // Collapsed: as before, the bin shown alone.
    QStringList rows = r.listed();
    CHECK(rows.front() == footage);
    CHECK_FALSE(rows.contains(r.key('A')));
    CHECK(r.role(footage, LibraryModel::ExpandableRole).toBool());
    CHECK_FALSE(r.role(footage, LibraryModel::ExpandedRole).toBool());

    // Footage open: its bin, then its media by name, one level in; Day 1 open too: C two levels in.
    r.library.setExpanded(footage, true);
    rows = r.listed();
    REQUIRE(rows.size() >= 4);
    CHECK(rows.mid(0, 4) == QStringList({footage, day, r.key('A'), r.key('B')}));
    CHECK(r.role(day, LibraryModel::DepthRole).toInt() == 1);
    CHECK(r.role(r.key('A'), LibraryModel::DepthRole).toInt() == 1);
    CHECK(r.role(footage, LibraryModel::DepthRole).toInt() == 0);
    r.library.setExpanded(day, true);
    rows = r.listed();
    CHECK(rows.mid(0, 5) == QStringList({footage, day, r.key('C'), r.key('A'), r.key('B')}));
    CHECK(r.role(r.key('C'), LibraryModel::DepthRole).toInt() == 2);

    // Sorted by name descending: each level on its own (bins first), the children stay under their bin.
    r.library.sortBy(QStringLiteral("name"));
    rows = r.listed();
    CHECK(rows.mid(0, 5) == QStringList({footage, day, r.key('C'), r.key('B'), r.key('A')}));
    r.library.sortBy(QStringLiteral("name"));

    // Shift-range across levels, and a collapse that hides the selection selects the bin instead.
    r.library.select(r.key('C'), LibraryBridge::Replace);
    r.library.select(r.key('B'), LibraryBridge::Range);
    CHECK(r.library.selection() == QStringList({r.key('C'), r.key('A'), r.key('B')}));
    r.library.select(r.key('C'), LibraryBridge::Replace);
    r.library.setExpanded(footage, false);
    CHECK(r.library.selection() == QStringList{footage});
    CHECK_FALSE(r.listed().contains(r.key('C')));
    // Alt+click: every level inside opens.
    r.library.setExpanded(day, false);
    r.library.setExpanded(footage, true, true);
    CHECK(r.library.isExpanded(day));
    CHECK(r.listed().contains(r.key('C')));
    CHECK(r.library.parentOf(r.key('C')) == day);
    CHECK(r.library.parentOf(footage).isEmpty());

    // Find lists the whole project flat; the grid shows the bin alone.
    r.library.setFindText(QStringLiteral("C"));
    CHECK_FALSE(r.library.treeShown());
    CHECK(r.role(r.key('C'), LibraryModel::DepthRole).toInt() == 0);
    r.library.setFindText(QString());
    CHECK(r.role(r.key('C'), LibraryModel::DepthRole).toInt() == 2);
    r.library.setView(QStringLiteral("grid"));
    CHECK_FALSE(r.listed().contains(r.key('C')));
    CHECK_FALSE(r.library.treeShown());
    // Opening a bin in place shows it as the top level.
    r.library.setView(QStringLiteral("list"));
    r.library.openBin(footage);
    CHECK(r.listed().mid(0, 2) == QStringList({day, r.key('C')}));
    CHECK(r.role(day, LibraryModel::DepthRole).toInt() == 0);
}

TEST_CASE("Library tree: the open bins are kept per project", "[app][library][p1][tree]") {
    resetSettings();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    controller.setProjectFile(std::filesystem::path(dir.filePath(QStringLiteral("One.wabcut")).toStdU16String()));
    QString footage;
    {
        LibraryBridge library;
        library.setSynchronous(true);
        library.setAnalysisProbe({});
        library.setController(&controller);
        footage = library.newBin(QStringLiteral("Footage"));
        library.setExpanded(footage, true);
        CHECK(library.isExpanded(footage));
    }
    {
        LibraryBridge again;
        again.setSynchronous(true);
        again.setAnalysisProbe({});
        again.setController(&controller);
        CHECK(again.isExpanded(footage));
    }
    // Another project file has its own.
    controller.setProjectFile(std::filesystem::path(dir.filePath(QStringLiteral("Two.wabcut")).toStdU16String()));
    LibraryBridge other;
    other.setSynchronous(true);
    other.setAnalysisProbe({});
    other.setController(&controller);
    CHECK_FALSE(other.isExpanded(footage));
    resetSettings();
}

// ---- In the window -------------------------------------------------------------------------------------------------

TEST_CASE("Library P1 2D: the usage badge lists the places and the delete asks in the window", "[app][library][p1][qml][screenshots]") {
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
        Timeline tl(R"(
            V1 tgt |AaaaBbbbAaaa|
            A1 tgt |AaaaBbbb....|
        )");
        useOwnProject(shell, tl);
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
        REQUIRE(library);
        REQUIRE(controller);
        REQUIRE(keys);
        library->setPersistent(false);
        library->setView(QStringLiteral("grid"));
        const QString a = mediaKey(tl.media('A'));
        const QString c = mediaKey(tl.media('C'));
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* root = shell.window->contentItem();

        // The badge on A says 2; C carries the "Unused" mark instead.
        QQuickItem* badge = findNamed(root, QStringLiteral("libraryUsageBadge_") + a);
        REQUIRE(badge);
        CHECK(badge->isVisible());
        QQuickItem* unusedOnA = findNamed(root, QStringLiteral("libraryUnusedMark_") + a);
        REQUIRE(unusedOnA);
        CHECK_FALSE(unusedOnA->isVisible());
        QQuickItem* unusedOnC = findNamed(root, QStringLiteral("libraryUnusedMark_") + c);
        REQUIRE(unusedOnC);
        CHECK(unusedOnC->isVisible());
        CHECK_FALSE(findNamed(root, QStringLiteral("libraryUsageBadge_") + c)->isVisible());

        // A click on the badge lists both places; the second opens the timeline there.
        click(shell.window.get(), badge->mapToScene(QPointF(badge->width() / 2, badge->height() / 2)));
        QObject* menu = shell.object("libraryUsageMenu");
        if (!menu)
            menu = findObject(shell.item("libraryPanel"), QStringLiteral("libraryUsageMenu"));
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 3000));
        CHECK(listOf(menu->property("usages")).size() == 2);
        spin(150);
        {
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            QQuickItem* row = findNamed(root, QStringLiteral("libraryUsage_1"));
            REQUIRE(row);
            QQuickItem* card = findNamed(root, QStringLiteral("libraryCard_") + a);
            REQUIRE(card);
            const QRectF both = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                                    .united(popupOf(row)->mapRectToScene(QRectF(0, 0, popupOf(row)->width(), popupOf(row)->height())));
            const double dpr = image.devicePixelRatio();
            const QRectF clipped = both.adjusted(-12, -12, 12, 12).intersected(QRectF(0, 0, kShotWidth, kShotHeight));
            REQUIRE(image.copy(QRect(static_cast<int>(clipped.x() * dpr), static_cast<int>(clipped.y() * dpr),
                                     static_cast<int>(clipped.width() * dpr), static_cast<int>(clipped.height() * dpr)))
                        .save(QDir(outputDir()).filePath(dark ? QStringLiteral("library-usage-badge-dark.png")
                                                              : QStringLiteral("library-usage-badge-light.png"))));
        }
        QQuickItem* second = findNamed(root, QStringLiteral("libraryUsage_1"));
        REQUIRE(second);
        QMetaObject::invokeMethod(second, "triggered");
        QMetaObject::invokeMethod(menu, "close");
        REQUIRE(waitFor([&] { return controller->playhead() == f(8); }, 3000));
        CHECK(controller->selection() == std::vector<ClipId>{clipAt(*controller->snapshot(), tl.seq, "V1@8")});

        // Delete on A (in use) through the real key: the confirmation, then Delete and Remove Clips.
        library->select(a, LibraryBridge::Replace);
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);
        REQUIRE(QMetaObject::invokeMethod(panel, "focusContent"));
        spin(50);
        {
            // Delete with Options on A and C with "Remove their clips" off: Delete Unused Items (C only), never a
            // disabled dead end (fix round 1, ARGON-12). Cancelled here.
            library->select(c, LibraryBridge::Toggle);
            REQUIRE(keys->trigger(QStringLiteral("project.deleteWithOptions")));
            QObject* options = findObject(panel, QStringLiteral("libraryDeletePopover"));
            REQUIRE(options);
            REQUIRE(waitFor([&] { return options->property("opened").toBool(); }, 3000));
            QQuickItem* toggle = findNamed(root, QStringLiteral("libraryDeleteRemoveClips"));
            REQUIRE(toggle);
            CHECK(toggle->isVisible());
            toggle->setProperty("checked", false);
            spin(50);
            QQuickItem* confirmUnused = findNamed(root, QStringLiteral("libraryDeleteConfirm"));
            REQUIRE(confirmUnused);
            CHECK(confirmUnused->property("enabled").toBool());
            CHECK(confirmUnused->property("text").toString() == QStringLiteral("Delete Unused Items"));
            QQuickItem* note = findNamed(root, QStringLiteral("libraryDeleteKeepUsedText"));
            REQUIRE(note);
            CHECK(note->isVisible());
            QMetaObject::invokeMethod(findNamed(root, QStringLiteral("libraryDeleteCancel")), "clicked");
            REQUIRE(waitFor([&] { return !options->property("opened").toBool(); }, 3000));
            library->select(a, LibraryBridge::Replace);
            REQUIRE(QMetaObject::invokeMethod(panel, "focusContent"));
            spin(50);
        }
        REQUIRE(keys->trigger(QStringLiteral("project.delete")));
        QObject* popover = findObject(panel, QStringLiteral("libraryDeletePopover"));
        REQUIRE(popover);
        REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 3000));
        QQuickItem* text = findNamed(root, QStringLiteral("libraryDeleteText"));
        REQUIRE(text);
        CHECK(text->property("text").toString().contains(QStringLiteral("is used by 3 clips in")));
        spin(150);
        {
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            saveItemShot(image, popupOf(text), dark ? QStringLiteral("library-delete-in-use-dark.png")
                                                    : QStringLiteral("library-delete-in-use-light.png"), 8);
        }
        QQuickItem* confirm = findNamed(root, QStringLiteral("libraryDeleteConfirm"));
        REQUIRE(confirm);
        CHECK(confirm->property("text").toString() == QStringLiteral("Delete and Remove Clips"));
        QMetaObject::invokeMethod(confirm, "clicked");
        REQUIRE(waitFor([&] { return controller->snapshot()->findMedia(tl.media('A')) == nullptr; }, 3000));
        CHECK(controller->session().undoLabel() == "Delete and Remove Clips");

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Library P1 2D: the list's chevrons and Right and Left keys twirl bins", "[app][library][p1][qml][screenshots]") {
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
        Timeline tl(R"(
            V1 tgt |AaaaBbbbAaaa|
            A1 tgt |AaaaBbbb....|
        )");
        useOwnProject(shell, tl);
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        REQUIRE(library);
        library->setPersistent(false);
        const QString footage = library->newBin(QStringLiteral("Footage"));
        library->openBin(QString());
        const QString day = library->newBin(QStringLiteral("Day 1"));
        REQUIRE(library->moveItems({day}, footage));
        REQUIRE(library->moveItems({mediaKey(tl.media('A')), mediaKey(tl.media('B'))}, footage));
        REQUIRE(library->moveItems({mediaKey(tl.media('C'))}, day));
        library->setView(QStringLiteral("list"));
        library->setColumnVisible(QStringLiteral("videoUsage"), true);
        library->setColumnVisible(QStringLiteral("audioUsage"), true);
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* root = shell.window->contentItem();

        // The chevron of Footage opens it (Alt: every level inside).
        QQuickItem* chevron = findNamed(root, QStringLiteral("libraryTwirl_") + footage);
        REQUIRE(chevron);
        CHECK(chevron->isVisible());
        click(shell.window.get(), chevron->mapToScene(QPointF(chevron->width() / 2, chevron->height() / 2)), Qt::AltModifier);
        REQUIRE(waitFor([&] { return library->isExpanded(footage) && library->isExpanded(day); }, 3000));
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* list = findNamed(root, QStringLiteral("libraryList"));
        REQUIRE(list);
        {
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            saveItemShot(image, list, dark ? QStringLiteral("library-list-twirl-dark.png") : QStringLiteral("library-list-twirl-light.png"));
        }

        // The keys: Left on C goes to Day 1, Left again closes it, Right opens it and goes to C.
        library->select(mediaKey(tl.media('C')), LibraryBridge::Replace);
        list->forceActiveFocus();
        spin(30);
        key(shell.window.get(), Qt::Key_Left);
        CHECK(library->selection() == QStringList{day});
        key(shell.window.get(), Qt::Key_Left);
        CHECK_FALSE(library->isExpanded(day));
        key(shell.window.get(), Qt::Key_Right);
        CHECK(library->isExpanded(day));
        key(shell.window.get(), Qt::Key_Right);
        CHECK(library->selection() == QStringList{mediaKey(tl.media('C'))});

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// ---- Fix round 1 (ARGON-03): audition takes count as uses; a bin and what it holds go in any order -----------------

TEST_CASE("Library delete: media only an inactive audition take plays asks first and only that take goes",
          "[app][library][p1][delete][auditions]") {
    Timeline tl(R"(
        V3 |Cccc....|
        V2 |Bbbb....|
        V1 |Aaaa....|
    )");
    const ClipId a = tl.clip("V1@0");
    const std::vector<ClipId> ids{a, tl.clip("V2@0"), tl.clip("V3@0")};
    auto audition = edit::createAudition(tl.state, tl.seq, ids); // A active, B and C the other takes
    REQUIRE(audition.has_value());
    ProjectController controller;
    LibraryBridge library;
    controller.resetProject(audition->state, tl.seq);
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setAnalysisProbe({});
    library.setController(&controller);
    const QString b = mediaKey(tl.media('B'));
    const auto auditionClip = [&]() -> Clip {
        const auto loc = controller.snapshot()->findSequence(tl.seq)->findClip(a);
        REQUIRE(loc.has_value());
        return loc->clip();
    };

    // Its uses: Find in Timeline lists the audition and names the take; the badge and Get Properties count it.
    const QVariantList uses = library.usages(b);
    REQUIRE(uses.size() == 1);
    CHECK(uses.front().toMap().value("clipId").toString() == clipKey(a));
    CHECK(uses.front().toMap().value("takeName").toString() == QStringLiteral("B"));
    CHECK(uses.front().toMap().value("clipName").toString().contains(QStringLiteral("B")));
    CHECK(library.usageText(b).contains(QStringLiteral("in 1 sequence")));
    CHECK(library.mediaProperties(b).value("uses").toInt() == 1);

    // Deleting it asks first (it used to skip the question and fail on the model's refusal).
    const QVariantMap impact = library.deleteImpact({b});
    CHECK(impact.value("clips").toInt() == 1);
    CHECK(impact.value("takes").toInt() == 1);
    CHECK(impact.value("usedItems").toInt() == 1);
    CHECK(impact.value("firstName").toString() == QStringLiteral("B"));
    CHECK(impact.value("sequences").toInt() == 1);
    std::vector<DeleteAsk> asked;
    QObject::connect(&library, &LibraryBridge::deleteConfirmRequested, &library,
                     [&asked](const QStringList& keys, const QVariantMap& what, bool withOptions) {
                         asked.push_back({keys, what, withOptions});
                     });
    CHECK(library.requestDelete({b}));
    REQUIRE(asked.size() == 1);
    CHECK(controller.snapshot()->findMedia(tl.media('B')) != nullptr);
    CHECK_FALSE(library.deleteItems({b})); // the plain delete still refuses

    // Confirmed: only B's take leaves the audition, A stays active and C stays a take, in one undo step.
    const auto before = controller.session().undoCount();
    REQUIRE(library.deleteItems({b}, true));
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(controller.snapshot()->findMedia(tl.media('B')) == nullptr);
    CHECK(validate(*controller.snapshot()).has_value());
    {
        const Clip kept = auditionClip();
        CHECK(takeCount(kept) == 2);
        CHECK(kept.media == tl.media('A'));
        CHECK(clipUsesMedia(kept, tl.media('C')));
    }
    controller.undo();
    CHECK(takeCount(auditionClip()) == 3);

    // The active take's media: the next take becomes active, the clip stays.
    REQUIRE(library.deleteItems({mediaKey(tl.media('A'))}, true));
    CHECK(validate(*controller.snapshot()).has_value());
    {
        const Clip next = auditionClip();
        CHECK(takeCount(next) == 2);
        CHECK_FALSE(clipUsesMedia(next, tl.media('A')));
        CHECK(next.media.isValid());
    }
    controller.undo();

    // Every take's media: the audition goes as a clip.
    CHECK(library.deleteImpact({mediaKey(tl.media('A')), b, mediaKey(tl.media('C'))}).value("takes").toInt() == 0);
    REQUIRE(library.deleteItems({mediaKey(tl.media('A')), b, mediaKey(tl.media('C'))}, true));
    CHECK_FALSE(controller.snapshot()->findSequence(tl.seq)->findClip(a).has_value());
    CHECK(validate(*controller.snapshot()).has_value());
}

TEST_CASE("Library delete: a bin and the media and sequences it nests go together in any selection order",
          "[app][library][p1][delete]") {
    Timeline tl(R"(
        V1 |AaaaCccc|
    )");
    // Inner plays C, Outer nests Inner; then the main sequence no longer plays Outer.
    auto inner = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@4")}, "Inner");
    REQUIRE(inner.has_value());
    auto outer = edit::nestClips(inner->edit.state, tl.seq, inner->clips, "Outer");
    REQUIRE(outer.has_value());
    const SequenceId innerId = inner->nested;
    const SequenceId outerId = outer->nested;
    auto lifted = edit::deleteClips(outer->edit.state, tl.seq, outer->clips);
    REQUIRE(lifted.has_value());
    auto bin = edit::addBin(lifted->state, "Old cuts");
    REQUIRE(bin.has_value());
    auto moved = edit::moveToBin(bin->state, bin->id, {}, {}, std::span(&outerId, 1));
    REQUIRE(moved.has_value());
    const ProjectState s = *moved;
    const MediaId c = tl.media('C');

    // The bin (holding Outer), Inner and C: Inner and C used to be refused as "used" by what goes with them.
    const BinId bins[] = {bin->id};
    const MediaId media[] = {c};
    const SequenceId sequences[] = {innerId};
    const auto all = removeLibraryItems(s, bins, media, sequences, false);
    REQUIRE(all.has_value());
    CHECK(validate(*all).has_value());
    CHECK(all->findBin(bin->id) == nullptr);
    CHECK(all->findSequence(outerId) == nullptr);
    CHECK(all->findSequence(innerId) == nullptr);
    CHECK(all->findMedia(c) == nullptr);
    CHECK(clipsUsingLibraryItems(s, bins, media, sequences).empty());

    // Two nested sequences with the nested one selected first.
    const SequenceId both[] = {innerId, outerId};
    const auto pair = removeLibraryItems(s, {}, {}, both, false);
    REQUIRE(pair.has_value());
    CHECK(pair->findSequence(innerId) == nullptr);
    CHECK(pair->findSequence(outerId) == nullptr);
    CHECK(validate(*pair).has_value());

    // A sequence a sequence that stays nests is still refused, with the reason.
    const auto refused = removeLibraryItems(s, {}, {}, std::span(&innerId, 1), false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("Outer") != std::string::npos);
}
