// 0.7 (batch A9, card 124): track groups in the app - the rows of the lanes (a header row above a group's tracks, its tracks
// folded into it while it is collapsed), the controller's group operations (one undo step each, with their names), the
// keymap commands, the header rows and menus of the Tracks panel and the group strip of the Audio Track Mixer, in the real
// shell with dark and light screenshots (test-output/ui: trackgroups-*.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..Bbb...|
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
    A2     |.Bbbbbb.|
    A3     |..Cccc..|
)";

QString key(const Timeline& tl, const char* label) { return trackKey(tl.track(label)); }

void settleRig(Rig& rig) {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
}

const TimelineView::Row* rowOf(const TimelineView& view, TrackId id) {
    for (const TimelineView::Row& row : view.rows()) {
        if (row.track->id == id)
            return &row;
    }
    return nullptr;
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

QQuickItem* headerOf(QQuickItem* part) {
    for (QQuickItem* item = part; item; item = item->parentItem()) {
        if (item->property("trackId").isValid())
            return item;
    }
    return nullptr;
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

void clickAt(QQuickWindow* window, QPointF scene) {
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, scene, scene, Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, {});
        QCoreApplication::sendEvent(window, &event);
    }
}

/// The real shell (Main.qml, hidden, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    Timeline tl;

    explicit ShellProject(std::string_view picture) : tl(picture) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    TimelineView* view() const {
        auto* v = qobject_cast<TimelineView*>(item(QStringLiteral("timelineView")));
        REQUIRE(v);
        return v;
    }
    std::size_t undo() const { return controller->session().undoCount(); }
    void settle() const {
        REQUIRE_FALSE(shell.grab().isNull());
        spin(30);
    }
    void shot(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

} // namespace

TEST_CASE("A track group has a header row above its tracks; collapsing folds the tracks into it (card 124)",
          "[app][timeline][trackgroup]") {
    Rig rig(kPicture, 10.0, false);
    ProjectController& c = rig.controller;
    const TimelineView& view = *rig.view;
    settleRig(rig);
    const double gapPx = view.parsedStyle().trackGap;
    const double a1Before = rowOf(view, rig.tl.track("A1"))->y;
    const double a3Before = rowOf(view, rig.tl.track("A3"))->y;
    const double contentBefore = view.contentHeight();
    CHECK(view.groupRows().empty());

    // Group A1 and A2: a row appears above A1, the audio tracks below it shift down by its height.
    const std::size_t steps = c.session().undoCount();
    const QString group = c.createTrackGroup({key(rig.tl, "A1"), key(rig.tl, "A2")}, QStringLiteral("Dialogue"));
    REQUIRE_FALSE(group.isEmpty());
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Group Tracks"));
    settleRig(rig);
    REQUIRE(view.groupRows().size() == 1);
    const TimelineView::GroupRow row = view.groupRows().front();
    const double groupHeight = view.parsedStyle().groupHeaderHeight;
    CHECK(trackGroupKey(row.id) == group);
    CHECK(row.height == groupHeight);
    CHECK_FALSE(row.collapsed);
    CHECK(rowOf(view, rig.tl.track("A1"))->y == a1Before + groupHeight + gapPx); // under the header row
    CHECK(rowOf(view, rig.tl.track("A1"))->y == row.y + row.height + gapPx);
    CHECK(rowOf(view, rig.tl.track("A3"))->y == a3Before + groupHeight + gapPx);
    CHECK(view.contentHeight() == contentBefore + groupHeight + gapPx);
    const QVariantMap item = view.groupRowItems().front().toMap();
    CHECK(item.value(QStringLiteral("id")).toString() == group);
    CHECK(item.value(QStringLiteral("name")).toString() == QStringLiteral("Dialogue"));
    CHECK(item.value(QStringLiteral("kind")).toString() == QStringLiteral("audio"));
    CHECK(item.value(QStringLiteral("count")).toInt() == 2);
    CHECK(item.value(QStringLiteral("y")).toDouble() == row.y);

    // Collapse: the two tracks fold away (no height, nothing under the pointer), A3 moves up under the header row.
    const double a1Height = rowOf(view, rig.tl.track("A1"))->height;
    const double a2Height = rowOf(view, rig.tl.track("A2"))->height;
    const double expandedContent = view.contentHeight();
    const double a1Y = rowOf(view, rig.tl.track("A1"))->y;
    CHECK(c.setTrackGroupCollapsed(group, true));
    CHECK(c.session().undoCount() == steps + 2);
    CHECK(c.undoText() == QStringLiteral("Undo Collapse Group"));
    settleRig(rig);
    REQUIRE(view.groupRows().size() == 1);
    CHECK(view.groupRows().front().collapsed);
    CHECK(rowOf(view, rig.tl.track("A1"))->hidden);
    CHECK(rowOf(view, rig.tl.track("A2"))->hidden);
    CHECK(rowOf(view, rig.tl.track("A1"))->height == 0);
    CHECK_FALSE(rowOf(view, rig.tl.track("A3"))->hidden);
    CHECK(rowOf(view, rig.tl.track("A3"))->y == row.y + row.height + gapPx); // right under the group's row
    CHECK(view.contentHeight() == expandedContent - a1Height - a2Height - 2 * gapPx);
    CHECK_FALSE(view.trackVisible(key(rig.tl, "A1")));
    CHECK(view.trackVisible(key(rig.tl, "A3")));
    CHECK(view.trackHeightOf(key(rig.tl, "A1")) == a1Height); // a folded track keeps the height it has when shown
    CHECK(view.trackYOf(key(rig.tl, "A3")) == rowOf(view, rig.tl.track("A3"))->y);
    // Where A1 was, the pointer is over A3 or the group's row, never the folded track.
    CHECK(view.clipAt(rig.x(3), a1Y - view.scrollY() + 2) != clipKey(rig.clip("A1@0")));
    // The same state again adds no step; the tracks themselves are untouched.
    CHECK(c.setTrackGroupCollapsed(group, true));
    CHECK(c.session().undoCount() == steps + 2);
    CHECK(c.currentSequence()->findTrack(rig.tl.track("A1"))->clips.size() == 1);

    // Expand again (undo): every row is where it was.
    REQUIRE(c.undo());
    settleRig(rig);
    CHECK_FALSE(rowOf(view, rig.tl.track("A1"))->hidden);
    CHECK(view.contentHeight() == expandedContent);
    CHECK(rowOf(view, rig.tl.track("A1"))->y == a1Y);
    REQUIRE(c.undo());
    settleRig(rig);
    CHECK(view.groupRows().empty());
    CHECK(rowOf(view, rig.tl.track("A1"))->y == a1Before);
    CHECK(view.contentHeight() == contentBefore);
}

TEST_CASE("The group's height setting, switches and states follow its tracks (card 124)", "[app][timeline][trackgroup]") {
    Rig rig(kPicture, 10.0, false);
    ProjectController& c = rig.controller;
    TimelineView& view = *rig.view;
    const QString group = c.createTrackGroup({key(rig.tl, "A1"), key(rig.tl, "A2")}, {});
    REQUIRE_FALSE(group.isEmpty());
    CHECK(c.trackGroupInfo(group).value(QStringLiteral("name")).toString() == QStringLiteral("Group 1"));
    settleRig(rig);

    // Height: one setting for the group's tracks (view state: no undo step).
    const std::size_t steps = c.session().undoCount();
    view.setGroupTrackHeight(group, 70);
    settleRig(rig);
    CHECK(rowOf(view, rig.tl.track("A1"))->height == 70);
    CHECK(rowOf(view, rig.tl.track("A2"))->height == 70);
    CHECK(rowOf(view, rig.tl.track("A3"))->height != 70);
    CHECK(c.session().undoCount() == steps);

    // Switches: none / some / all of the tracks have them (0 / 1 / 2), one undo step each, named after the switch.
    const auto state = [&](const char* flag) { return c.trackGroupInfo(group).value(QLatin1String(flag)).toInt(); };
    CHECK(state("muted") == 0);
    c.setTrackFlag(key(rig.tl, "A1"), QStringLiteral("muted"), true);
    CHECK(state("muted") == 1);
    CHECK(c.setTrackGroupFlag(group, QStringLiteral("muted"), true));
    CHECK(state("muted") == 2);
    CHECK(c.undoText() == QStringLiteral("Undo Mute Group"));
    CHECK(c.currentSequence()->findTrack(rig.tl.track("A2"))->muted);
    CHECK_FALSE(c.currentSequence()->findTrack(rig.tl.track("A3"))->muted); // not in the group
    const std::size_t afterMute = c.session().undoCount();
    CHECK(c.setTrackGroupFlag(group, QStringLiteral("muted"), true)); // all have it: no empty step
    CHECK(c.session().undoCount() == afterMute);
    CHECK(c.setTrackGroupFlag(group, QStringLiteral("solo"), true));
    CHECK(c.undoText() == QStringLiteral("Undo Solo Group"));
    CHECK(c.setTrackGroupFlag(group, QStringLiteral("locked"), true));
    CHECK(c.undoText() == QStringLiteral("Undo Lock Group"));
    CHECK(c.setTrackGroupFlag(group, QStringLiteral("locked"), false));
    CHECK(c.undoText() == QStringLiteral("Undo Unlock Group"));
    CHECK_FALSE(c.setTrackGroupFlag(group, QStringLiteral("nonsense"), true));
    // The header row's item says the same (the lanes' view of the group).
    settleRig(rig);
    const QVariantMap item = view.groupRowItems().front().toMap();
    CHECK(item.value(QStringLiteral("muted")).toInt() == 2);
    CHECK(item.value(QStringLiteral("solo")).toInt() == 2);
    CHECK(item.value(QStringLiteral("locked")).toInt() == 0);

    // Rename and colour are steps too; refusals set the error and change nothing.
    CHECK(c.renameTrackGroup(group, QStringLiteral("  Voices ")));
    CHECK(c.undoText() == QStringLiteral("Undo Rename Group"));
    CHECK(c.trackGroupInfo(group).value(QStringLiteral("name")).toString() == QStringLiteral("Voices"));
    CHECK(c.setTrackGroupColor(group, 6));
    CHECK(c.undoText() == QStringLiteral("Undo Group Color"));
    const std::size_t before = c.session().undoCount();
    CHECK_FALSE(c.renameTrackGroup(group, QStringLiteral("   ")));
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK_FALSE(c.setTrackGroupColor(group, 99));
    CHECK_FALSE(c.setTrackGroupCollapsed(QStringLiteral("g999999"), true));
    CHECK(c.session().undoCount() == before);
    // Route an audio group to a new submix.
    CHECK(c.routeTrackGroup(group, QStringLiteral("newSubmix")));
    CHECK(c.undoText() == QStringLiteral("Undo Route Group to New Submix"));
    CHECK(c.trackGroupInfo(group).value(QStringLiteral("output")).toString() == QStringLiteral("submix"));
    CHECK(c.trackGroupInfo(group).value(QStringLiteral("submixName")).toString() == QStringLiteral("Voices"));
    CHECK(c.routeTrackGroup(group, QStringLiteral("mix")));
    CHECK(c.trackGroupInfo(group).value(QStringLiteral("output")).toString() == QStringLiteral("mix"));
}

TEST_CASE("Group Targeted Tracks, the keys and the track menu's group facts (card 124)", "[app][timeline][trackgroup]") {
    Rig rig(kPicture, 10.0, false);
    ProjectController& c = rig.controller;
    // V1 and A1 are targeted: one track of each kind, so there is nothing to group yet.
    CHECK_FALSE(c.commandAvailability(QStringLiteral("timeline.groupTargetedTracks")).value(QStringLiteral("enabled")).toBool());
    CHECK(c.groupTargetedTracks().isEmpty());
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK(c.trackInfo(key(rig.tl, "A1")).value(QStringLiteral("targetedFree")).toInt() == 1);

    c.setTrackFlag(key(rig.tl, "A2"), QStringLiteral("targeted"), true);
    CHECK(c.trackInfo(key(rig.tl, "A1")).value(QStringLiteral("targetedFree")).toInt() == 2);
    CHECK(c.commandAvailability(QStringLiteral("timeline.groupTargetedTracks")).value(QStringLiteral("enabled")).toBool());
    const std::size_t steps = c.session().undoCount();
    REQUIRE(c.runCommand(QStringLiteral("timeline.groupTargetedTracks")));
    CHECK(c.session().undoCount() == steps + 1);
    REQUIRE(c.currentSequence()->trackGroups.size() == 1);
    CHECK(c.currentSequence()->trackGroups.front().members.size() == 2);
    CHECK(c.trackInfo(key(rig.tl, "A1")).value(QStringLiteral("groupName")).toString() == QStringLiteral("Group 1"));
    CHECK_FALSE(c.commandAvailability(QStringLiteral("timeline.groupTargetedTracks")).value(QStringLiteral("enabled")).toBool());

    // The menu's facts for a track outside the group: the group it could join; and joining, then leaving.
    const QVariantMap info = c.trackInfo(key(rig.tl, "A3"));
    CHECK(info.value(QStringLiteral("groupId")).toString().isEmpty());
    const QVariantList joinable = info.value(QStringLiteral("groups")).toList();
    REQUIRE(joinable.size() == 1);
    const QString group = joinable.front().toMap().value(QStringLiteral("id")).toString();
    CHECK(c.addTracksToTrackGroup(group, {key(rig.tl, "A3")}));
    CHECK(c.undoText() == QStringLiteral("Undo Add to Group"));
    CHECK(c.currentSequence()->trackGroups.front().members.size() == 3);
    CHECK(c.removeTracksFromTrackGroups({key(rig.tl, "A3")}));
    CHECK(c.undoText() == QStringLiteral("Undo Remove from Group"));
    CHECK(c.currentSequence()->trackGroups.front().members.size() == 2);

    // Collapse / Expand All Track Groups: available only while some group would change; one step for all of them.
    CHECK_FALSE(c.commandAvailability(QStringLiteral("timeline.expandTrackGroups")).value(QStringLiteral("enabled")).toBool());
    CHECK(c.commandAvailability(QStringLiteral("timeline.collapseTrackGroups")).value(QStringLiteral("enabled")).toBool());
    const QString second = c.createTrackGroup({key(rig.tl, "A3")}, QStringLiteral("Music"));
    REQUIRE_FALSE(second.isEmpty());
    const std::size_t before = c.session().undoCount();
    REQUIRE(c.runCommand(QStringLiteral("timeline.collapseTrackGroups")));
    CHECK(c.session().undoCount() == before + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Collapse All Groups"));
    for (const TrackGroup& g : c.currentSequence()->trackGroups)
        CHECK(g.collapsed);
    CHECK_FALSE(c.commandAvailability(QStringLiteral("timeline.collapseTrackGroups")).value(QStringLiteral("enabled")).toBool());
    REQUIRE(c.runCommand(QStringLiteral("timeline.expandTrackGroups")));
    for (const TrackGroup& g : c.currentSequence()->trackGroups)
        CHECK_FALSE(g.collapsed);
    CHECK(c.ungroupTracks(second));
    CHECK(c.undoText() == QStringLiteral("Undo Ungroup Tracks"));
}

TEST_CASE("Track groups in the shell: header rows, collapse, menus and the mixer's group strip, dark and light (card 124)",
          "[app][qml][timeline][trackgroup][screenshot]") {
    resetSettings();
    clearMessages();
    for (const bool dark : {true, false}) {
        ShellProject p(kPicture);
        p.shell.setAppearance(dark, "ultraviolet");
        ProjectController& c = *p.controller;
        const QString dialogue = c.createTrackGroup({key(p.tl, "A1"), key(p.tl, "A2")}, QStringLiteral("Dialogue"));
        const QString picture = c.createTrackGroup({key(p.tl, "V1"), key(p.tl, "V2")}, QStringLiteral("Picture"));
        REQUIRE_FALSE(dialogue.isEmpty());
        REQUIRE_FALSE(picture.isEmpty());
        REQUIRE(c.setTrackGroupColor(dialogue, 11));
        p.settle();
        p.settle();

        // Both header rows are there with their names and member counts, above their tracks' headers.
        QQuickItem* dialogueRow = p.item(QStringLiteral("trackGroupHeader_") + dialogue);
        QQuickItem* pictureRow = p.item(QStringLiteral("trackGroupHeader_") + picture);
        REQUIRE(dialogueRow);
        REQUIRE(pictureRow);
        CHECK(dialogueRow->isVisible());
        QQuickItem* a1Mute = p.item(QStringLiteral("trackMute_A1"));
        REQUIRE(a1Mute);
        CHECK(sceneRect(dialogueRow).bottom() <= sceneRect(headerOf(a1Mute)).top() + 1);
        QQuickItem* name = findNamed(dialogueRow, QStringLiteral("trackGroupName_") + dialogue);
        REQUIRE(name);
        CHECK(name->property("text").toString() == QStringLiteral("Dialogue"));
        QQuickItem* count = findNamed(dialogueRow, QStringLiteral("trackGroupCount_") + dialogue);
        REQUIRE(count);
        CHECK(findNamed(dialogueRow, QStringLiteral("trackGroupStripe_") + dialogue)->isVisible()); // the colour bar
        CHECK_FALSE(findNamed(pictureRow, QStringLiteral("trackGroupStripe_") + picture)->isVisible()); // no label: none
        CHECK(findNamed(pictureRow, QStringLiteral("trackGroupOutput_") + picture)->isVisible());     // video: the eye
        CHECK_FALSE(findNamed(pictureRow, QStringLiteral("trackGroupMute_") + picture)->isVisible());
        CHECK(findNamed(dialogueRow, QStringLiteral("trackGroupMute_") + dialogue)->isVisible());     // audio: mute
        p.shot(QRectF(0, sceneRect(pictureRow).top() - 16, 460, sceneRect(headerOf(p.item(QStringLiteral("trackMute_A2")))).bottom() - sceneRect(pictureRow).top() + 44),
               dark ? QStringLiteral("trackgroups-expanded.png") : QStringLiteral("trackgroups-expanded-light.png"));

        // The switch on the row mutes every track of the group (one undo step); the track headers' own mute buttons follow.
        const std::size_t steps = p.undo();
        QQuickWindow* window = p.shell.window.get();
        QQuickItem* mute = findNamed(dialogueRow, QStringLiteral("trackGroupMute_") + dialogue);
        clickAt(window, sceneRect(mute).center());
        spin(60);
        CHECK(p.undo() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Mute Group"));
        CHECK(c.currentSequence()->findTrack(p.tl.track("A1"))->muted);
        CHECK(c.currentSequence()->findTrack(p.tl.track("A2"))->muted);
        CHECK_FALSE(c.currentSequence()->findTrack(p.tl.track("A3"))->muted);
        clickAt(window, sceneRect(mute).center()); // all muted: the next click unmutes them
        spin(60);
        CHECK_FALSE(c.currentSequence()->findTrack(p.tl.track("A1"))->muted);
        CHECK(p.undo() == steps + 2);

        // The chevron collapses the group: its track headers disappear and the lanes' row folds them.
        QQuickItem* fold = findNamed(dialogueRow, QStringLiteral("trackGroupFold_") + dialogue);
        REQUIRE(fold);
        clickAt(window, sceneRect(fold).center());
        spin(80);
        p.settle();
        CHECK(p.undo() == steps + 3);
        CHECK(c.currentSequence()->trackGroups.front().collapsed);
        CHECK_FALSE(p.item(QStringLiteral("trackMute_A1"))->isVisible());
        CHECK(p.item(QStringLiteral("trackMute_A3"))->isVisible());
        TimelineView* view = p.view();
        CHECK(view->trackVisible(key(p.tl, "A3")));
        CHECK_FALSE(view->trackVisible(key(p.tl, "A1")));
        p.shot(QRectF(0, sceneRect(pictureRow).top() - 16, 460, sceneRect(headerOf(p.item(QStringLiteral("trackMute_A3")))).bottom() - sceneRect(pictureRow).top() + 44),
               dark ? QStringLiteral("trackgroups-collapsed.png") : QStringLiteral("trackgroups-collapsed-light.png"));
        // The lanes beside them: the collapsed group's row shows where its tracks have clips (dark and light shots).
        p.shot(QRectF(0, sceneRect(pictureRow).top() - 16, 900, sceneRect(headerOf(p.item(QStringLiteral("trackMute_A3")))).bottom() - sceneRect(pictureRow).top() + 44),
               dark ? QStringLiteral("trackgroups-lanes.png") : QStringLiteral("trackgroups-lanes-light.png"));

        // The group's menu: rename, label, collapse, track height, the switches, output and ungroup.
        REQUIRE(QMetaObject::invokeMethod(dialogueRow, "openMenu", Q_ARG(QVariant, QVariant::fromValue(dialogueRow)),
                                          Q_ARG(QVariant, QPointF(12, dialogueRow->height()))));
        QObject* menu = dialogueRow->findChild<QObject*>(QStringLiteral("trackGroupMenu"));
        if (!menu)
            menu = p.shell.window->findChild<QObject*>(QStringLiteral("trackGroupMenu"));
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 5000));
        for (const char* row : {"trackGroupMenuRename", "trackGroupMenuCollapse", "trackGroupMenuLabel", "trackGroupMenuHeight",
                                "trackGroupMenuMute", "trackGroupMenuSolo", "trackGroupMenuLock", "trackGroupMenuTarget",
                                "trackGroupMenuOutput", "trackGroupMenuUngroup"})
            CHECK(menu->findChild<QObject*>(QLatin1String(row)) != nullptr);
        QObject* collapse = menu->findChild<QObject*>(QStringLiteral("trackGroupMenuCollapse"));
        REQUIRE(collapse);
        CHECK(collapse->property("text").toString() == QStringLiteral("Expand Group")); // it is collapsed now
        p.settle();
        QQuickItem* menuItem = qobject_cast<QQuickItem*>(menu->property("contentItem").value<QObject*>());
        REQUIRE(menuItem);
        p.shot(sceneRect(menuItem).adjusted(-24, -24, 24, 24).united(sceneRect(dialogueRow).adjusted(-12, -8, 12, 8)),
               dark ? QStringLiteral("trackgroups-menu.png") : QStringLiteral("trackgroups-menu-light.png"));
        QMetaObject::invokeMethod(collapse, "triggered");
        QMetaObject::invokeMethod(menu, "close");
        spin(60);
        CHECK_FALSE(c.currentSequence()->trackGroups.front().collapsed); // the menu row expanded it again
        CHECK(p.undo() == steps + 4);

        // The track header's menu carries the group rows.
        QQuickItem* a3Header = headerOf(p.item(QStringLiteral("trackMute_A3")));
        REQUIRE(a3Header);
        QQuickItem* chip = findNamed(a3Header, QStringLiteral("trackTarget"));
        REQUIRE(chip);
        REQUIRE(QMetaObject::invokeMethod(a3Header, "openMenu", Q_ARG(QVariant, QVariant::fromValue(chip)),
                                          Q_ARG(QVariant, QPointF(0, chip->height()))));
        QObject* trackMenu = a3Header->findChild<QObject*>(QStringLiteral("trackHeaderMenu"));
        if (!trackMenu)
            trackMenu = p.shell.window->findChild<QObject*>(QStringLiteral("trackHeaderMenu"));
        REQUIRE(trackMenu);
        REQUIRE(waitFor([&] { return trackMenu->property("opened").toBool(); }, 5000));
        for (const char* row : {"trackMenuGroupTargeted", "trackMenuGroupNew", "trackMenuGroupAdd", "trackMenuGroupRemove"})
            CHECK(trackMenu->findChild<QObject*>(QLatin1String(row)) != nullptr);
        CHECK_FALSE(trackMenu->findChild<QObject*>(QStringLiteral("trackMenuGroupRemove"))->property("enabled").toBool()); // A3 is in no group
        QMetaObject::invokeMethod(trackMenu, "close");
        spin(60);

        // The Audio Track Mixer: the group's strips carry its tag; collapsed, one group strip stands in for them.
        p.shell.window->setProperty("workspace", 1);
        spin(500);
        p.settle();
        QQuickItem* mixerPanel = p.item(QStringLiteral("audioMixerPanel"));
        REQUIRE(mixerPanel);
        QQuickItem* strip = p.item(QStringLiteral("mixerStrip_A1"));
        REQUIRE(strip);
        CHECK(strip->isVisible());
        QQuickItem* tag = findNamed(strip, QStringLiteral("mixerGroupTag"));
        REQUIRE(tag);
        CHECK(tag->isVisible());
        CHECK_FALSE(findNamed(p.item(QStringLiteral("mixerStrip_A3")), QStringLiteral("mixerGroupTag"))->isVisible()); // A3: no group
        p.shot(sceneRect(mixerPanel), dark ? QStringLiteral("trackgroups-mixer-expanded.png") : QStringLiteral("trackgroups-mixer-expanded-light.png"));
        clickAt(window, sceneRect(tag).center()); // the tag folds the group's strips
        spin(100);
        p.settle();
        CHECK(c.currentSequence()->trackGroups.front().collapsed);
        QQuickItem* groupStrip = p.item(QStringLiteral("mixerGroupStrip_") + dialogue);
        REQUIRE(groupStrip);
        CHECK(groupStrip->isVisible());
        CHECK_FALSE(p.item(QStringLiteral("mixerStrip_A1"))->isVisible());
        CHECK_FALSE(p.item(QStringLiteral("mixerStrip_A2"))->isVisible());
        CHECK(p.item(QStringLiteral("mixerStrip_A3"))->isVisible());
        QQuickItem* groupName = findNamed(groupStrip, QStringLiteral("mixerGroupName"));
        REQUIRE(groupName);
        CHECK(groupName->property("text").toString() == QStringLiteral("Dialogue"));
        QQuickItem* groupMute = findNamed(groupStrip, QStringLiteral("mixerGroupMute_") + dialogue);
        REQUIRE(groupMute);
        const std::size_t mixerSteps = p.undo();
        clickAt(window, sceneRect(groupMute).center());
        spin(60);
        CHECK(p.undo() == mixerSteps + 1);
        CHECK(c.currentSequence()->findTrack(p.tl.track("A1"))->muted);
        // Hiding the group's first track (Show/Hide Tracks) does not take the folded group out of the mixer: the next one carries it.
        auto* mixer = p.shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
        REQUIRE(mixer);
        mixer->setTrackHidden(key(p.tl, "A1"), true);
        spin(100);
        p.settle();
        QQuickItem* carried = p.item(QStringLiteral("mixerGroupStrip_") + dialogue);
        REQUIRE(carried);
        CHECK(carried->isVisible());
        mixer->setTrackHidden(key(p.tl, "A1"), false);
        spin(100);
        p.settle();
        p.shot(sceneRect(mixerPanel), dark ? QStringLiteral("trackgroups-mixer-collapsed.png") : QStringLiteral("trackgroups-mixer-collapsed-light.png"));
        // (The strips are made again when a track changes: look the group strip up again.)
        QQuickItem* expand = findNamed(p.item(QStringLiteral("mixerGroupStrip_") + dialogue), QStringLiteral("mixerGroupExpand_") + dialogue);
        REQUIRE(expand);
        clickAt(window, sceneRect(expand).center());
        spin(100);
        p.settle();
        CHECK_FALSE(c.currentSequence()->trackGroups.front().collapsed);
        CHECK(p.item(QStringLiteral("mixerStrip_A1"))->isVisible());
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
