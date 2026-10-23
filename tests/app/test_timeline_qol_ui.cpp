// UX rework 2026-10-23, stream T (timeline and monitor QoL), the QML side in the real shell (Main.qml, hidden): the
// typed timecode entry of the Timeline and both monitors, the track header menu (Add Track, Add Tracks..., Rename,
// Delete Track, Delete Empty Tracks, Sync Lock), the timeline transition and gap menus, marker names and tooltips on
// the ruler and the Program time bar, the Source Monitor's drops, the Library card's In/Out band, Reveal in Project
// from the clip menu and the remembered Snap toggle. Screenshots under test-output/ui (timeline-*.png and others).

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "EditingBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "SequenceTimeBar.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDropEvent>
#include <QHoverEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <string_view>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

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

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

/// The real shell (Main.qml, hidden, 1600 x 960, dark, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    Timeline tl;

    explicit ShellProject(std::string_view picture, const std::map<char, std::int64_t>& media = {})
        : tl(picture, media) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    template <class T>
    T* singleton(const char* name) {
        T* found = shell.engine.singletonInstance<T*>("Wabcut", name);
        REQUIRE(found);
        return found;
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    QObject* object(const QString& name) const { return shell.window->findChild<QObject*>(name); }
    QString track(std::string_view label) const { return trackKey(tl.track(label)); }
    std::size_t undo() const { return controller->session().undoCount(); }
    const Sequence& sequence() const { return *controller->currentSequence(); }
    /// The Tracks panel's header of track `shortName` ("V1", "A2"); headers are rebuilt when tracks come and go.
    QQuickItem* header(const QString& shortName) const {
        QQuickItem* name = item(QStringLiteral("trackName_") + shortName);
        return name ? name->parentItem() : nullptr;
    }
    /// Lays the window out once (a popup takes its final place, pushed inside the window, on its first polish), so
    /// rectangles read afterwards are where the next grab draws them.
    void settle() const {
        REQUIRE_FALSE(shell.grab().isNull());
        spin(30);
    }
    /// Saves `rect` (scene coordinates) of one grab of the window.
    void shot(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

/// A popup's (menu, dialog, tooltip) rectangle in the scene: its background.
QRectF popupRect(QObject* popup) {
    auto* background = popup->property("background").value<QQuickItem*>();
    REQUIRE(background);
    return sceneRect(background);
}

QObject* waitOpened(QObject* popup) {
    REQUIRE(popup);
    REQUIRE(waitFor([&] { return popup->property("opened").toBool(); }, 5000));
    return popup;
}

void waitClosed(QObject* popup) {
    REQUIRE(waitFor([&] { return !popup->property("visible").toBool(); }, 5000));
    spin(50);
}

QObject* child(QObject* parent, const char* name) {
    QObject* found = parent->findChild<QObject*>(QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

/// A control inside a dialog's content (WabcutDialog: the body and the buttons).
QQuickItem* inDialog(QObject* dialog, const char* name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* found = findNamed(content->parentItem() ? content->parentItem() : content, QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

void trigger(QObject* item) {
    REQUIRE(item);
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
}

void click(QObject* button) {
    REQUIRE(button);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
}

void key(QQuickWindow* window, int key, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &release);
}

/// The pointer enters `item` (a MouseArea with hover) at `scene` and moves there. A hidden window does not deliver
/// hover from pointer moves, so the hover events go to the item itself, in its own coordinates.
void hoverItem(QQuickItem* item, QPointF scene) {
    const QPointF local = item->mapFromScene(scene);
    QHoverEvent enter(QEvent::HoverEnter, local, scene, QPointF(-1, -1));
    QCoreApplication::sendEvent(item, &enter);
    QHoverEvent move(QEvent::HoverMove, local, scene, local);
    QCoreApplication::sendEvent(item, &move);
}
void leaveItem(QQuickItem* item) {
    QHoverEvent leave(QEvent::HoverLeave, QPointF(-1, -1), QPointF(-1, -1), QPointF(-1, -1));
    QCoreApplication::sendEvent(item, &leave);
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

// ---- Typed timecode entry -------------------------------------------------------------------------------------------

TEST_CASE("Typed timecode entry in the shell: the Timeline and both monitors seek and Esc cancels",
          "[app][qml][timecodeEntry][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )", {{'S', 50}});
    ProjectController& c = *p.controller;
    QQuickWindow* window = p.shell.window.get();

    // The Timeline's corner: a click opens the field with the playhead selected.
    QQuickItem* entry = p.item(QStringLiteral("timelineTimecode"));
    REQUIRE(entry);
    c.setPlayhead(f(10));
    CHECK(entry->property("text").toString() == c.playheadText());
    QQuickItem* field = p.item(QStringLiteral("timelineTimecodeField"));
    REQUIRE(field);
    CHECK_FALSE(field->isVisible());
    REQUIRE(QMetaObject::invokeMethod(entry, "startEditing", Q_ARG(QString, QString())));
    CHECK(entry->property("editing").toBool());
    CHECK(field->isVisible());
    CHECK(field->hasActiveFocus());
    CHECK(field->property("text").toString() == QStringLiteral("00:00:00:10"));
    CHECK(field->property("selectedText").toString() == QStringLiteral("00:00:00:10"));

    // An offset: the hint says where Enter goes; Enter goes there.
    field->setProperty("text", QStringLiteral("+15"));
    CHECK(entry->property("previewText").toString() == QStringLiteral("00:00:01:00"));
    QObject* hint = child(entry, "timelineTimecodeHint");
    REQUIRE(waitFor([&] { return hint->property("opened").toBool(); }, 3000));
    spin(100);
    p.settle();
    const QRectF corner = sceneRect(entry).united(popupRect(hint)).adjusted(-24, -12, 160, 24);
    p.shot(corner, QStringLiteral("timeline-timecode-entry.png"));
    REQUIRE(QMetaObject::invokeMethod(entry, "commit"));
    CHECK(c.playhead() == f(25));
    CHECK_FALSE(entry->property("editing").toBool());
    CHECK(entry->property("text").toString() == QStringLiteral("00:00:01:00"));

    // Not a time: the field stays open and says so; Esc cancels and nothing moves.
    REQUIRE(QMetaObject::invokeMethod(entry, "startEditing", Q_ARG(QString, QString())));
    field->setProperty("text", QStringLiteral("1:2:3:4:5"));
    REQUIRE(QMetaObject::invokeMethod(entry, "commit"));
    CHECK(entry->property("editing").toBool());
    CHECK(entry->property("invalid").toBool());
    CHECK(c.playhead() == f(25));
    REQUIRE(waitFor([&] { return hint->property("opened").toBool(); }, 3000));
    p.settle();
    p.shot(sceneRect(entry).united(popupRect(hint)).adjusted(-24, -12, 160, 24),
           QStringLiteral("timeline-timecode-entry-invalid.png"));
    REQUIRE(field->hasActiveFocus());
    key(window, Qt::Key_Escape);
    CHECK_FALSE(entry->property("editing").toBool());
    CHECK(c.playhead() == f(25));

    // From the keyboard: Tab to it, type a digit (it opens seeded with it), Enter.
    entry->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(entry->hasActiveFocus());
    key(window, Qt::Key_5, QStringLiteral("5"));
    REQUIRE(entry->property("editing").toBool());
    CHECK(field->property("text").toString() == QStringLiteral("5"));
    key(window, Qt::Key_Return);
    CHECK_FALSE(entry->property("editing").toBool());
    CHECK(c.playhead() == f(5));
    CHECK(entry->hasActiveFocus()); // focus went back where it was

    // The Program Monitor's pill: an absolute time.
    QQuickItem* program = p.item(QStringLiteral("programTimecode"));
    REQUIRE(program);
    REQUIRE(program->isVisible());
    REQUIRE(QMetaObject::invokeMethod(program, "startEditing", Q_ARG(QString, QString())));
    QQuickItem* programField = p.item(QStringLiteral("programTimecodeField"));
    REQUIRE(programField);
    CHECK(programField->property("text").toString() == c.playheadText());
    programField->setProperty("text", QStringLiteral("2."));
    REQUIRE(QMetaObject::invokeMethod(program, "commit"));
    CHECK(c.playhead() == f(50));
    REQUIRE(QMetaObject::invokeMethod(program, "startEditing", Q_ARG(QString, QString())));
    programField->setProperty("text", QStringLiteral("-1:00"));
    spin(50);
    p.settle();
    p.shot(sceneRect(program).adjusted(-24, -60, 200, 24), QStringLiteral("program-timecode-entry.png"));
    REQUIRE(QMetaObject::invokeMethod(program, "commit"));
    CHECK(c.playhead() == f(25));

    // The Source Monitor's pill: off without a source; with one, the source's own time and offsets.
    QQuickItem* source = p.item(QStringLiteral("sourceTimecode"));
    REQUIRE(source);
    CHECK_FALSE(source->isEnabled());
    REQUIRE(QMetaObject::invokeMethod(source, "startEditing", Q_ARG(QString, QString())));
    CHECK_FALSE(source->property("editing").toBool());
    auto* monitor = p.singleton<SourceMonitorBridge>("SourceMonitor");
    REQUIRE(monitor->load(mediaKey(p.tl.media('S'))));
    REQUIRE(waitFor([&] { return source->isEnabled(); }, 2000));
    REQUIRE(QMetaObject::invokeMethod(source, "startEditing", Q_ARG(QString, QString())));
    QQuickItem* sourceField = p.item(QStringLiteral("sourceTimecodeField"));
    REQUIRE(sourceField);
    sourceField->setProperty("text", QStringLiteral("+10"));
    REQUIRE(QMetaObject::invokeMethod(source, "commit"));
    CHECK(monitor->positionTicks() == f(10));
    CHECK(c.playhead() == f(25)); // the sequence did not move

    checkNoMessages();
}

// ---- Track header menu ----------------------------------------------------------------------------------------------

TEST_CASE("Track header menu in the shell: Add Track and Add Tracks and Rename and Delete Track and Delete Empty Tracks",
          "[app][qml][tracks][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V2     |............|
        V1 tgt |Aaaa........|
        A1 tgt |Aaaa........|
        A2     |............|
    )");
    ProjectController& c = *p.controller;
    QQuickWindow* window = p.shell.window.get();
    const auto openMenu = [&](const QString& shortName) -> QObject* {
        QQuickItem* header = p.header(shortName);
        INFO(shortName.toStdString());
        REQUIRE(header);
        REQUIRE(QMetaObject::invokeMethod(header, "openMenu", Q_ARG(QVariant, QVariant::fromValue<QObject*>(header)),
                                          Q_ARG(QVariant, QPointF(60, header->height() / 2))));
        QObject* loader = child(header, "trackHeaderMenuLoader");
        QObject* menu = loader->property("item").value<QObject*>();
        REQUIRE(menu);
        return waitOpened(menu);
    };

    // Both kinds have the menu now; a video track's has no channel formats.
    QObject* menu = openMenu(QStringLiteral("V1"));
    CHECK(child(menu, "trackMenuAddTrack")->property("text").toString() == QStringLiteral("Add Video Track Above"));
    CHECK(child(menu, "trackMenuDelete")->property("enabled").toBool());
    CHECK(child(menu, "trackMenuDelete")->property("text").toString() == QStringLiteral("Delete Track…"));
    CHECK(child(menu, "trackMenuDeleteEmpty")->property("enabled").toBool());
    CHECK(child(menu, "trackMenuDeleteEmpty")->property("text").toString() == QStringLiteral("Delete Empty Tracks (2)"));
    CHECK(child(menu, "trackMenuSyncLock")->property("checked").toBool());
    CHECK_FALSE(child(menu, "trackFormat_stereo")->property("visible").toBool());
    p.settle();
    p.shot(sceneRect(p.header(QStringLiteral("V1"))).united(popupRect(menu)).adjusted(-16, -16, 16, 16),
           QStringLiteral("timeline-track-menu.png"));

    // Add Video Track Above: V2 is new (above V1), the old V2 is V3. One undo step.
    std::size_t steps = p.undo();
    trigger(child(menu, "trackMenuAddTrack"));
    waitClosed(menu);
    REQUIRE(waitFor([&] { return p.sequence().videoTracks.size() == 3; }, 3000));
    CHECK(p.undo() == steps + 1);
    CHECK(p.sequence().videoTracks[0]->id == p.tl.track("V1"));
    CHECK(p.sequence().videoTracks[2]->id == p.tl.track("V2"));
    spin(100);

    // Rename... (after the menu closed): the field over the header; Enter keeps the name, which shows beside the chip.
    menu = openMenu(QStringLiteral("V1"));
    trigger(child(menu, "trackMenuRename"));
    waitClosed(menu);
    QQuickItem* v1 = p.header(QStringLiteral("V1"));
    REQUIRE(v1);
    REQUIRE(waitFor([&] { return v1->property("renaming").toBool(); }, 3000));
    QQuickItem* nameField = findNamed(v1, QStringLiteral("trackRenameField"));
    REQUIRE(nameField);
    REQUIRE(waitFor([&] { return nameField->hasActiveFocus(); }, 2000));
    steps = p.undo();
    nameField->setProperty("text", QStringLiteral("Dialogue"));
    key(window, Qt::Key_Return);
    REQUIRE(waitFor([&] { return !v1->property("renaming").toBool(); }, 2000));
    CHECK(p.undo() == steps + 1);
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->name == "Dialogue");
    QQuickItem* nameLabel = p.item(QStringLiteral("trackName_V1"));
    REQUIRE(nameLabel);
    REQUIRE(waitFor([&] { return nameLabel->isVisible(); }, 2000));
    CHECK(nameLabel->property("text").toString() == QStringLiteral("Dialogue"));
    {
        QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
        REQUIRE(panel);
        p.settle();
        const QRectF headers = sceneRect(v1).united(sceneRect(p.header(QStringLiteral("A2"))));
        p.shot(QRectF(sceneRect(panel).left(), headers.top() - 12, 640, headers.height() + 24),
               QStringLiteral("timeline-track-names.png"));
    }
    // Esc leaves the name as it was.
    REQUIRE(QMetaObject::invokeMethod(v1, "startRename"));
    nameField = findNamed(v1, QStringLiteral("trackRenameField"));
    REQUIRE(nameField);
    REQUIRE(waitFor([&] { return nameField->hasActiveFocus(); }, 2000));
    nameField->setProperty("text", QStringLiteral("Something else"));
    key(window, Qt::Key_Escape);
    REQUIRE(waitFor([&] { return !v1->property("renaming").toBool(); }, 2000));
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->name == "Dialogue");

    // The dialogs in the light theme (the dark one is in the menus' screenshots).
    p.shell.setAppearance(false, "ultraviolet");
    spin(300);

    // Add Tracks...: the dialog; from A1's menu one audio track goes after A1 by default. Ask for a video track as well
    // and mono audio: one undo step.
    menu = openMenu(QStringLiteral("A1"));
    CHECK(child(menu, "trackFormat_stereo")->property("visible").toBool()); // an audio track's formats
    CHECK(child(menu, "trackMenuAddTrack")->property("text").toString() == QStringLiteral("Add Audio Track Below"));
    trigger(child(menu, "trackMenuAddTracks"));
    waitClosed(menu);
    QObject* dialog = waitOpened(p.object(QStringLiteral("addTracksDialog")));
    QQuickItem* videoCount = inDialog(dialog, "addTracksVideoCount");
    QQuickItem* audioCount = inDialog(dialog, "addTracksAudioCount");
    CHECK(videoCount->property("value").toDouble() == 0);
    CHECK(audioCount->property("value").toDouble() == 1);
    // The header's kind has the focus with its count selected: a typed 1 replaces it (not 11).
    REQUIRE(waitFor([&] { return audioCount->hasActiveFocus(); }, 2000));
    CHECK(audioCount->property("selectedText").toString() == QStringLiteral("1"));
    key(window, Qt::Key_1, QStringLiteral("1"));
    CHECK(audioCount->property("text").toString() == QStringLiteral("1"));
    CHECK(inDialog(dialog, "addTracksAudioPlacement")->property("text").toString() == QStringLiteral("After A1 (below it)"));
    CHECK_FALSE(inDialog(dialog, "addTracksVideoPlacement")->isEnabled());
    videoCount->setProperty("value", 1);
    inDialog(dialog, "addTracksAudioFormat")->setProperty("currentIndex", 1); // mono
    CHECK(inDialog(dialog, "addTracksVideoPlacement")->isEnabled());
    CHECK(inDialog(dialog, "addTracksVideoPlacement")->property("text").toString() == QStringLiteral("After V3 (above it)"));
    spin(100);
    p.settle();
    p.shot(popupRect(dialog).adjusted(-16, -16, 16, 16), QStringLiteral("timeline-add-tracks-dialog.png"));
    steps = p.undo();
    click(inDialog(dialog, "addTracksOk"));
    waitClosed(dialog);
    CHECK(p.undo() == steps + 1);
    REQUIRE(p.sequence().videoTracks.size() == 4);
    REQUIRE(p.sequence().audioTracks.size() == 3);
    CHECK(p.sequence().audioTracks[0]->id == p.tl.track("A1"));
    CHECK(p.sequence().audioTracks[1]->format == ChannelFormat::Mono); // the new A2
    CHECK(p.sequence().audioTracks[2]->id == p.tl.track("A2"));

    // Typing replaces the selected count and Enter adds: from V1's menu, 2 then Enter is two video tracks above V1 in one
    // undo step (undone again for what follows).
    menu = openMenu(QStringLiteral("V1"));
    trigger(child(menu, "trackMenuAddTracks"));
    waitClosed(menu);
    dialog = waitOpened(p.object(QStringLiteral("addTracksDialog")));
    videoCount = inDialog(dialog, "addTracksVideoCount");
    REQUIRE(waitFor([&] { return videoCount->hasActiveFocus(); }, 2000));
    CHECK(videoCount->property("selectedText").toString() == QStringLiteral("1"));
    key(window, Qt::Key_2, QStringLiteral("2"));
    steps = p.undo();
    key(window, Qt::Key_Return);
    waitClosed(dialog);
    CHECK(p.undo() == steps + 1);
    REQUIRE(p.sequence().videoTracks.size() == 6);
    CHECK(p.sequence().videoTracks[0]->id == p.tl.track("V1"));
    REQUIRE(c.undo());
    REQUIRE(p.sequence().videoTracks.size() == 4);

    // Delete Track on a track with clips asks first; Delete removes it and its clips (one undo step).
    menu = openMenu(QStringLiteral("V1"));
    trigger(child(menu, "trackMenuDelete"));
    waitClosed(menu);
    dialog = waitOpened(p.object(QStringLiteral("deleteTrackDialog")));
    CHECK(dialog->property("title").toString() == QStringLiteral("Delete V1?"));
    spin(100);
    p.settle();
    p.shot(popupRect(dialog).adjusted(-16, -16, 16, 16), QStringLiteral("timeline-delete-track-dialog.png"));
    steps = p.undo();
    click(inDialog(dialog, "deleteTrackOk"));
    waitClosed(dialog);
    CHECK(p.undo() == steps + 1);
    CHECK(p.sequence().findTrack(p.tl.track("V1")) == nullptr);
    CHECK(p.sequence().videoTracks.size() == 3);

    // Delete Empty Tracks: every empty track but one of each kind's... V1 to V3 are empty now (one stays), A2 and A3.
    menu = openMenu(QStringLiteral("A1"));
    QObject* deleteEmpty = child(menu, "trackMenuDeleteEmpty");
    REQUIRE(deleteEmpty->property("enabled").toBool());
    steps = p.undo();
    trigger(deleteEmpty);
    waitClosed(menu);
    REQUIRE(waitFor([&] { return p.sequence().audioTracks.size() == 1; }, 3000));
    CHECK(p.undo() == steps + 1);
    CHECK(p.sequence().videoTracks.size() == 1);
    CHECK(p.sequence().audioTracks[0]->id == p.tl.track("A1"));

    // Sync Lock toggles (one undo step); the last track of a kind cannot go (the item says why).
    menu = openMenu(QStringLiteral("A1"));
    CHECK_FALSE(child(menu, "trackMenuDelete")->property("enabled").toBool());
    CHECK(child(menu, "trackMenuDeleteReason")->property("visible").toBool());
    steps = p.undo();
    trigger(child(menu, "trackMenuSyncLock"));
    waitClosed(menu);
    CHECK(p.undo() == steps + 1);
    CHECK_FALSE(p.sequence().findTrack(p.tl.track("A1"))->syncLocked);
    REQUIRE(c.undo());
    CHECK(p.sequence().findTrack(p.tl.track("A1"))->syncLocked);

    checkNoMessages();
}

// ---- Transition and gap menus ---------------------------------------------------------------------------------------

TEST_CASE("Timeline transition menu and gap menu in the shell: Set Duration and Replace with Default and Ripple Delete",
          "[app][qml][transition][gap][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaaaaaaaaaaaaaB20bbbbbbbbbbbbbbbbb........|
        A1 tgt |Cccc....Dddd................................|
    )");
    ProjectController& c = *p.controller;
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    auto* editing = p.singleton<EditingBridge>("Editing");
    view->setPixelsPerSecond(400);
    view->setScrollX(0);
    c.setPlayhead(f(16));
    REQUIRE(c.applyDefaultTransition(false));
    const auto transitions = [&] { return p.sequence().findTrack(p.tl.track("V1"))->transitions; };
    REQUIRE(transitions().size() == 1);
    const QString id = transitionKey(transitions()[0].id);
    spin(100);

    // The right click (the view selected the transition): the menu with its facts.
    QObject* menu = p.object(QStringLiteral("transitionContextMenu"));
    REQUIRE(menu);
    const auto openMenu = [&] {
        c.selectTransition(id);
        const double x = f(16).toSeconds() * view->pixelsPerSecond() - view->scrollX();
        emit view->transitionContextMenuRequested(id, x, 20);
        waitOpened(menu);
    };
    openMenu();
    CHECK(child(menu, "transitionMenuTitle")->property("text").toString().startsWith(QStringLiteral("Cross Dissolve")));
    CHECK_FALSE(child(menu, "transitionMenuReplaceDefault")->property("enabled").toBool()); // it is the default
    CHECK_FALSE(child(menu, "transitionMenuSetDefault")->property("enabled").toBool());
    CHECK(child(menu, "transitionMenuAlignCenter")->property("visible").toBool()); // between two clips
    CHECK(child(menu, "transitionMenuClear")->property("enabled").toBool());
    p.settle();
    p.shot(popupRect(menu).adjusted(-420, -120, 40, 40), QStringLiteral("timeline-transition-menu.png"));

    // Set Duration...: the dialog after the menu closed, prefilled; 10 frames, one undo step.
    trigger(child(menu, "transitionMenuDuration"));
    waitClosed(menu);
    QObject* dialog = waitOpened(p.object(QStringLiteral("transitionDurationDialog")));
    QQuickItem* duration = inDialog(dialog, "transitionDurationField");
    CHECK(duration->property("text").toString() == c.transitionInfo(id).value(QStringLiteral("durationText")).toString());
    duration->setProperty("text", QStringLiteral("10"));
    std::size_t steps = p.undo();
    click(inDialog(dialog, "transitionDurationOk"));
    waitClosed(dialog);
    CHECK(p.undo() == steps + 1);
    CHECK(c.transitionInfo(id).value(QStringLiteral("durationText")).toString() == QStringLiteral("00:00:00:10"));

    // With a feet+frames Display Format the field shows feet+frames and reads them back: Set Duration on the untouched
    // field keeps the length (it used to be read as a timecode and refused).
    const auto displayFormat = [&](VideoTimeDisplay display) {
        edit::SequenceSettingsChange change;
        change.videoDisplay = display;
        REQUIRE(c.applyEditFn("Display Format", [&](const ProjectState& s) {
            return edit::setSequenceSettings(s, p.tl.seq, change);
        }));
    };
    displayFormat(VideoTimeDisplay::Feet35mm);
    openMenu();
    trigger(child(menu, "transitionMenuDuration"));
    waitClosed(menu);
    dialog = waitOpened(p.object(QStringLiteral("transitionDurationDialog")));
    duration = inDialog(dialog, "transitionDurationField");
    CHECK(duration->property("text").toString() == QStringLiteral("0000+10"));
    click(inDialog(dialog, "transitionDurationOk"));
    waitClosed(dialog);
    CHECK(c.transitionInfo(id).value(QStringLiteral("durationText")).toString() == QStringLiteral("0000+10"));
    displayFormat(VideoTimeDisplay::Timecode);
    CHECK(c.transitionInfo(id).value(QStringLiteral("durationText")).toString() == QStringLiteral("00:00:00:10"));

    // Another transition, then Replace with Default puts Cross Dissolve back; Set as Default makes the other the default.
    QString other;
    for (const QVariant& choice : editing->transitionChoices(QStringLiteral("video"))) {
        const QString choiceId = choice.toMap().value(QStringLiteral("id")).toString();
        if (choiceId != QStringLiteral("crossDissolve")) {
            other = choiceId;
            break;
        }
    }
    REQUIRE_FALSE(other.isEmpty());
    c.selectTransition(id);
    c.effectControls()->setTransitionEffect(other);
    REQUIRE(c.transitionInfo(id).value(QStringLiteral("effect")).toString() == other);
    openMenu();
    QObject* replace = child(menu, "transitionMenuReplaceDefault");
    CHECK(replace->property("enabled").toBool());
    CHECK(replace->property("text").toString() == QStringLiteral("Replace with Cross Dissolve"));
    trigger(replace);
    waitClosed(menu);
    CHECK(c.transitionInfo(id).value(QStringLiteral("effect")).toString() == QStringLiteral("crossDissolve"));
    c.effectControls()->setTransitionEffect(other);
    openMenu();
    trigger(child(menu, "transitionMenuSetDefault"));
    waitClosed(menu);
    CHECK(editing->defaultVideoTransition() == other);
    CHECK(c.transitionInfo(id).value(QStringLiteral("isDefault")).toBool());
    REQUIRE(editing->setDefaultTransition(QStringLiteral("crossDissolve")));

    // Clear removes it.
    openMenu();
    steps = p.undo();
    trigger(child(menu, "transitionMenuClear"));
    waitClosed(menu);
    CHECK(transitions().empty());
    CHECK(p.undo() == steps + 1);

    // A gap on A1 (frames 4 to 8): its menu; V1 is sync-locked and has a clip there, so Ripple Delete is refused and the
    // reason shows as a toast; the gap stays selected.
    REQUIRE(c.selectGapAt(p.tl.track("A1"), f(5)));
    spin(50);
    QObject* gapMenu = p.object(QStringLiteral("gapContextMenu"));
    REQUIRE(gapMenu);
    const double gapX = f(6).toSeconds() * view->pixelsPerSecond() - view->scrollX();
    emit view->gapContextMenuRequested(p.track("A1"), gapX, 60);
    waitOpened(gapMenu);
    CHECK(child(gapMenu, "gapMenuTitle")->property("text").toString() == QStringLiteral("Gap on A1 · 00:00:00:04"));
    p.settle();
    p.shot(popupRect(gapMenu).adjusted(-260, -120, 40, 40), QStringLiteral("timeline-gap-menu.png"));
    steps = p.undo();
    trigger(child(gapMenu, "gapMenuRippleDelete"));
    waitClosed(gapMenu);
    CHECK(p.undo() == steps);
    QQuickItem* toast = p.item(QStringLiteral("toast"));
    REQUIRE(toast);
    REQUIRE(waitFor([&] { return toast->property("shown").toBool(); }, 2000));
    CHECK(toast->property("text").toString() == c.lastError());
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK_FALSE(c.selectedGap().isEmpty());

    // V1's sync lock off. With the keyboard focus in another panel (the Library), Delete (the keymap's edit.clear, an
    // application key) leaves the gap alone: the click that selected it was only a deselect for the user.
    c.setTrackFlag(p.track("V1"), QStringLiteral("syncLocked"), false);
    REQUIRE(c.selectGapAt(p.tl.track("A1"), f(5)));
    QQuickItem* libraryPanel = p.item(QStringLiteral("libraryPanel"));
    REQUIRE(libraryPanel);
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "focusPanel", Q_ARG(QString, QStringLiteral("library"))));
    REQUIRE(waitFor([&] {
        QQuickItem* focused = p.shell.window->activeFocusItem();
        return focused && (focused == libraryPanel || libraryPanel->isAncestorOf(focused));
    }, 3000));
    steps = p.undo();
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("edit.clear"))));
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("edit.rippleDelete"))));
    CHECK(p.undo() == steps);
    CHECK(p.sequence().findTrack(p.tl.track("A1"))->clips[1].start() == f(8));
    CHECK_FALSE(c.selectedGap().isEmpty()); // still selected: back in the Timeline, Delete closes it

    // In the Timeline, Delete closes it.
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "focusPanel", Q_ARG(QString, QStringLiteral("timeline"))));
    QQuickItem* timelinePanel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(timelinePanel);
    REQUIRE(waitFor([&] {
        QQuickItem* focused = p.shell.window->activeFocusItem();
        return focused && timelinePanel->isAncestorOf(focused);
    }, 3000));
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("edit.clear"))));
    CHECK(p.undo() == steps + 1);
    const Track& a1 = *p.sequence().findTrack(p.tl.track("A1"));
    REQUIRE(a1.clips.size() == 2);
    CHECK(a1.clips[1].start() == f(4));
    CHECK(c.selectedGap().isEmpty());

    checkNoMessages();
}

// ---- Markers on the ruler and the Program time bar ------------------------------------------------------------------

TEST_CASE("Marker names on the ruler and marker tooltips on the ruler and the Program time bar",
          "[app][qml][markers][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectController& c = *p.controller;
    Marker hook;
    hook.start = f(10);
    hook.name = "Hook";
    hook.comment = "Start the story here";
    Marker chorus;
    chorus.start = f(40);
    chorus.duration = f(25);
    chorus.name = "Chorus with a long name that does not fit before the next marker";
    chorus.colorIndex = 2;
    Marker outro;
    outro.start = f(48);
    outro.name = "Outro";
    outro.colorIndex = 4;
    for (const Marker& m : {hook, chorus, outro})
        REQUIRE(c.applyEditFn("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, p.tl.seq, m); }));
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    view->setPixelsPerSecond(500);
    view->setScrollX(0);
    spin(200);

    // The names beside the flags: a long one is cut short before the next marker.
    QQuickItem* ruler = p.item(QStringLiteral("timelineRuler"));
    REQUIRE(ruler);
    QQuickItem* first = findNamed(ruler, QStringLiteral("rulerMarker_0"));
    QQuickItem* second = findNamed(ruler, QStringLiteral("rulerMarker_1"));
    REQUIRE(first);
    REQUIRE(second);
    QQuickItem* hookName = findNamed(ruler, QStringLiteral("rulerMarkerName_0"));
    QQuickItem* chorusName = findNamed(ruler, QStringLiteral("rulerMarkerName_1"));
    REQUIRE(hookName);
    REQUIRE(chorusName);
    CHECK(hookName->isVisible());
    CHECK(chorusName->isVisible());
    const QVariantList items = view->markerItems();
    REQUIRE(items.size() == 3);
    const double nextX = items[1].toMap().value(QStringLiteral("nextX")).toDouble();
    CHECK(sceneRect(chorusName).right() <= ruler->mapToScene(QPointF(nextX, 0)).x());

    // Hover over the first flag: the tooltip with the name, comment and time.
    const QPointF flag = first->mapToScene(QPointF(0, first->height() - 8));
    QQuickItem* flagArea = findNamed(first, QStringLiteral("rulerMarkerArea"));
    REQUIRE(flagArea);
    hoverItem(flagArea, flag);
    REQUIRE(flagArea->property("containsMouse").toBool());
    CHECK(std::abs(sceneRect(first).x() - flag.x()) <= 1); // a hover moves nothing (only a drag moves a marker)
    QObject* tooltip = nullptr;
    REQUIRE(waitFor([&] {
        tooltip = first->findChild<QObject*>(QStringLiteral("rulerMarkerTooltip"));
        return tooltip && tooltip->property("opened").toBool();
    }, 4000));
    CHECK(tooltip->property("text").toString()
          == QStringLiteral("Hook\nStart the story here\n00:00:00:10"));
    spin(100);
    p.settle();
    {
        QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
        REQUIRE(panel);
        const QRectF tip = popupRect(tooltip);
        const QRectF r = sceneRect(ruler);
        INFO("ruler " << r.x() << "," << r.y() << " " << r.width() << " flag0 " << sceneRect(first).x() << " flag1 "
                      << sceneRect(second).x() << " pps " << view->pixelsPerSecond() << " scrollX " << view->scrollX());
        CHECK(std::abs(sceneRect(first).x() - (r.x() + f(10).toSeconds() * view->pixelsPerSecond() - view->scrollX())) <= 2);
        p.shot(QRectF(sceneRect(panel).left(), std::min(tip.top(), r.top()) - 8, sceneRect(panel).width(),
                      r.bottom() - std::min(tip.top(), r.top()) + 40),
               QStringLiteral("timeline-ruler-markers.png"));
    }
    leaveItem(flagArea);

    // Hover over the name: the same tooltip.
    QQuickItem* nameArea = findNamed(hookName, QStringLiteral("rulerMarkerNameArea"));
    REQUIRE(nameArea);
    const QPointF onName = hookName->mapToScene(QPointF(hookName->width() / 2, hookName->height() / 2));
    hoverItem(nameArea, onName);
    REQUIRE(waitFor([&] {
        tooltip = first->findChild<QObject*>(QStringLiteral("rulerMarkerTooltip"));
        return tooltip && tooltip->property("opened").toBool();
    }, 4000));
    CHECK(tooltip->property("text").toString() == QStringLiteral("Hook\nStart the story here\n00:00:00:10"));
    leaveItem(nameArea);

    // A press on the name scrubs like anywhere off the flag (only the flag moves a marker): the playhead goes where the
    // pointer is and follows a drag; the marker stays where it was and there is nothing to undo.
    {
        const auto mouse = [&](QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, scene, scene, button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(p.shell.window.get(), &event);
        };
        const auto frameAt = [&](double rulerX) { return (rulerX + view->scrollX()) / view->pixelsPerSecond() * 25.0; };
        const double nameX = ruler->mapFromScene(onName).x();
        const std::size_t steps = p.undo();
        mouse(QEvent::MouseButtonPress, onName, Qt::LeftButton, Qt::LeftButton);
        spin(20);
        INFO("name x " << nameX << " playhead " << c.playhead().toSeconds() * 25.0 << " expected " << frameAt(nameX));
        CHECK(std::abs(c.playhead().toSeconds() * 25.0 - frameAt(nameX)) <= 1.0);
        CHECK(c.playhead() != f(10)); // not the marker's time: the click was beside it
        for (int i = 1; i <= 6; ++i) {
            mouse(QEvent::MouseMove, onName + QPointF(10.0 * i, 0), Qt::NoButton, Qt::LeftButton);
            spin(5);
        }
        mouse(QEvent::MouseButtonRelease, onName + QPointF(60, 0), Qt::LeftButton, Qt::NoButton);
        spin(50);
        CHECK(std::abs(c.playhead().toSeconds() * 25.0 - frameAt(nameX + 60)) <= 1.0);
        CHECK(p.undo() == steps);
        REQUIRE(p.sequence().markers.size() == 3);
        CHECK(p.sequence().markers[0].start == f(10));
    }

    // The playhead knob over a name stays visible: the name pill is drawn under it.
    {
        view->seekToX(ruler->mapFromScene(onName).x());
        spin(50);
        QQuickItem* cap = findNamed(ruler, QStringLiteral("playheadCap"));
        REQUIRE(cap);
        REQUIRE(sceneRect(cap).intersects(sceneRect(hookName)));
        const QImage image = p.shell.grab();
        REQUIRE_FALSE(image.isNull());
        const QColor knob = pixelAt(image, cap, QPointF(cap->width() / 2, cap->height() / 2), image.devicePixelRatio());
        INFO("knob centre " << describe(knob));
        CHECK(sameColor(knob, QColor(QStringLiteral("#FFFFFF")), 24));
        p.shot(sceneRect(hookName).united(sceneRect(cap)).adjusted(-60, -24, 120, 24),
               QStringLiteral("timeline-ruler-playhead-over-name.png"));
    }

    // A marker that goes takes its name with it (the pill lives in the ruler's name layer).
    REQUIRE(c.undo()); // the Outro marker
    REQUIRE(waitFor([&] { return findNamed(ruler, QStringLiteral("rulerMarkerName_2")) == nullptr; }, 2000));

    // The Program Monitor's time bar: over a marker, its name and time in the hover pill.
    auto* bar = qobject_cast<SequenceTimeBar*>(p.item(QStringLiteral("sequenceTimeBar")));
    REQUIRE(bar);
    REQUIRE(bar->isVisible());
    double markerX = -1;
    for (double x = 0; x < bar->width() && markerX < 0; x += 1) {
        if (bar->markerAt(x, 1).value(QStringLiteral("index")).toInt() == 0 && !bar->markerAt(x, 1).isEmpty())
            markerX = x;
    }
    REQUIRE(markerX >= 0);
    QQuickItem* barArea = p.item(QStringLiteral("programTimeBarArea"));
    REQUIRE(barArea);
    hoverItem(barArea, bar->mapToScene(QPointF(markerX, bar->height() / 2)));
    QQuickItem* hoverPill = p.item(QStringLiteral("programTimeHover"));
    QQuickItem* markerRow = p.item(QStringLiteral("programTimeHoverMarker"));
    REQUIRE(hoverPill);
    REQUIRE(markerRow);
    INFO("area hovered " << barArea->property("containsMouse").toBool() << " mouseX "
                         << barArea->property("mouseX").toDouble() << " markerX " << markerX << " bar.x " << bar->x()
                         << " pill " << hoverPill->isVisible() << " atMarker " << hoverPill->property("atMarker").toBool()
                         << " marker keys " << hoverPill->property("marker").toMap().keys().join(QLatin1Char(',')).toStdString()
                         << " cpp " << bar->markerAt(markerX).keys().join(QLatin1Char(',')).toStdString()
                         << " bar w " << bar->width() << " active " << bar->isEnabled());
    REQUIRE(waitFor([&] { return hoverPill->isVisible() && markerRow->isVisible(); }, 3000));
    spin(100);
    p.settle();
    p.shot(sceneRect(hoverPill).united(sceneRect(bar)).adjusted(-16, -16, 16, 16).intersected(
               sceneRect(hoverPill).adjusted(-300, -16, 300, 60)),
           QStringLiteral("program-timebar-marker.png"));
    leaveItem(barArea);

    checkNoMessages();
}

// ---- The Source Monitor's drops and the Library card's marks --------------------------------------------------------

TEST_CASE("The Source Monitor opens a Library clip or a file dropped on it and Library cards show the marked range",
          "[app][qml][source][drop][library][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
    )", {{'S', 50}});
    ProjectController& c = *p.controller;
    QQuickWindow* window = p.shell.window.get();
    auto* monitor = p.singleton<SourceMonitorBridge>("SourceMonitor");
    QQuickItem* well = p.item(QStringLiteral("sourceMonitorWell"));
    REQUIRE(well);
    REQUIRE(well->isVisible());
    const QPoint center = well->mapToScene(QPointF(well->width() / 2, well->height() / 2)).toPoint();
    QQuickItem* highlight = p.item(QStringLiteral("sourceMonitorDropHighlight"));
    REQUIRE(highlight);

    // A sequence from the Library cannot open here: the drag is refused.
    {
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-wabcut-media"), sequenceKey(p.tl.seq).toUtf8());
        QDragEnterEvent enter(center, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &enter);
        QDragMoveEvent move(center, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &move);
        CHECK_FALSE(highlight->isVisible());
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(window, &leave);
    }

    // A media item: the frame shows while it is over the picture; the drop opens it (and adds it to Recent).
    const QString key = mediaKey(p.tl.media('S'));
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-wabcut-media"), key.toUtf8());
    QDragEnterEvent enter(center, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &enter);
    QDragMoveEvent move(center, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &move);
    REQUIRE(waitFor([&] { return highlight->isVisible(); }, 2000));
    spin(50);
    p.settle();
    p.shot(sceneRect(well).adjusted(-8, -8, 8, 8), QStringLiteral("source-monitor-drop.png"));
    QDropEvent drop(center, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &drop);
    spin(50);
    CHECK_FALSE(highlight->isVisible());
    CHECK(monitor->mediaKeyText() == key);
    CHECK(monitor->recent().size() == 1);

    // The Library card of an item with Source Monitor marks: the band from In to Out and the marked length.
    REQUIRE(c.applyEditFn("Set Source Marks", [&](const ProjectState& s) {
        return edit::setSourceMarks(s, p.tl.media('S'), {f(10), f(35)});
    }));
    // The item shown in the Library (its bin, no filter: as Reveal in Project shows it).
    auto* library = p.singleton<LibraryBridge>("Library");
    REQUIRE(library->showItem(key));
    spin(200);
    REQUIRE_FALSE(p.shell.grab().isNull()); // the Library grid makes its cards when it is laid out
    QQuickItem* card = p.item(QStringLiteral("libraryCard_") + key);
    REQUIRE(card);
    QQuickItem* band = findNamed(card, QStringLiteral("libraryCardMarks"));
    REQUIRE(band);
    REQUIRE(waitFor([&] { return band->isVisible(); }, 2000));
    QQuickItem* thumb = band->parentItem();
    REQUIRE(thumb);
    CHECK(std::abs(band->x() - 0.2 * thumb->width()) <= 2);
    CHECK(std::abs(band->width() - 0.5 * thumb->width()) <= 3);
    CHECK(card->property("marked").toBool());
    p.settle();
    p.shot(sceneRect(card).adjusted(-8, -8, 8, 8), QStringLiteral("library-card-marks.png"));

    // A file from File Explorer: imported, then opened here. The sample project has an edit (the marks), so leaving it
    // for a new project asks about it before the import; the file opens once the answer is in.
    REQUIRE(c.canUndo());
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto dropFile = [&](const QString& name, Qt::GlobalColor color) {
        const QString still = dir.filePath(name);
        QImage picture(64, 36, QImage::Format_RGB32);
        picture.fill(color);
        REQUIRE(picture.save(still));
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(still)});
        QDragEnterEvent enterFiles(center, Qt::CopyAction, &files, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &enterFiles);
        CHECK(enterFiles.isAccepted());
        QDropEvent dropFiles(center, Qt::CopyAction, &files, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &dropFiles);
        spin(100);
    };
    dropFile(QStringLiteral("Card.png"), Qt::darkCyan);
    QObject* unsaved = waitOpened(p.object(QStringLiteral("unsavedChangesDialog")));
    CHECK(monitor->name() != QStringLiteral("Card.png")); // not imported yet
    click(inDialog(unsaved, "unsavedDiscardButton"));
    waitClosed(unsaved);
    REQUIRE(waitFor([&] { return monitor->name() == QStringLiteral("Card.png"); }, 3000));

    // In a project of its own the next file imports and opens at once.
    dropFile(QStringLiteral("Title.png"), Qt::darkMagenta);
    CHECK(monitor->name() == QStringLiteral("Title.png"));

    // Stills' Library thumbnails cannot decode here (as in the timeline file-drop test): not this feature's.
    QStringList messages = takeMessages();
    messages.removeIf([](const QString& m) { return m.contains(QStringLiteral("No decoder for png")); });
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Reveal in Project, the remembered Snap ------------------------------------------------------------------------

TEST_CASE("The clip menu reveals the clip's item in the Library and its file in File Explorer",
          "[app][qml][reveal][library][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    ProjectController& c = *p.controller;
    auto* library = p.singleton<LibraryBridge>("Library");
    QObject* menu = p.object(QStringLiteral("clipContextMenu"));
    REQUIRE(menu);
    QQuickItem* lanes = p.item(QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    const QString clip = clipKey(p.tl.clip("V1@4"));
    c.selectClip(clip);
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, clip), Q_ARG(QVariant, QPointF(200, 20))));
    waitOpened(menu);
    QObject* inProject = child(menu, "revealInProjectMenuItem");
    QObject* inExplorer = child(menu, "revealInExplorerMenuItem");
    CHECK(inProject->property("visible").toBool());
    CHECK(inProject->property("enabled").toBool());
    // The fixture's files do not exist: the item says so and is off.
    CHECK(inExplorer->property("visible").toBool());
    CHECK_FALSE(inExplorer->property("enabled").toBool());
    CHECK(inExplorer->property("text").toString().contains(QStringLiteral("file missing")));
    p.settle();
    p.shot(popupRect(menu).adjusted(-8, -8, 8, 8), QStringLiteral("timeline-clip-menu-reveal.png"));

    trigger(inProject);
    waitClosed(menu);
    CHECK(library->selection() == QStringList{mediaKey(p.tl.media('B'))});
    REQUIRE_FALSE(p.shell.grab().isNull()); // the Library grid makes its cards when it is laid out
    QQuickItem* card = p.item(QStringLiteral("libraryCard_") + mediaKey(p.tl.media('B')));
    REQUIRE(card);
    CHECK(card->isVisible());

    // The keymap command (no default key; the palette lists it) does the same for the selected clip.
    c.selectClip(clipKey(p.tl.clip("V1@0")));
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "runCommand",
                                      Q_ARG(QVariant, QStringLiteral("clip.revealInProject"))));
    CHECK(library->selection() == QStringList{mediaKey(p.tl.media('A'))});

    checkNoMessages();
}

TEST_CASE("The timeline Snap toggle is remembered for the next start", "[app][qml][timeline]") {
    resetSettings();
    clearMessages();
    {
        Shell shell;
        REQUIRE(shell.window);
        QQuickItem* panel = shell.item("timelinePanel");
        REQUIRE(panel);
        CHECK(panel->property("snapping").toBool());
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.snap"))));
        CHECK_FALSE(panel->property("snapping").toBool());
        CHECK_FALSE(qobject_cast<TimelineView*>(shell.item("timelineView"))->snapping());
        CHECK_FALSE(shell.preferences->property("timelineSnapping").toBool());
        // The time remapping lines too.
        panel->setProperty("showSpeed", true);
        CHECK(shell.preferences->property("timelineShowSpeed").toBool());
    }
    spin(50);
    {
        QSettings settings;
        CHECK_FALSE(settings.value(QStringLiteral("timeline/snapping"), true).toBool());
        CHECK(settings.value(QStringLiteral("timeline/showSpeed"), false).toBool());
    }
    resetSettings();
    checkNoMessages();
}

TEST_CASE("Settings Timeline page picks the default video and audio transitions", "[app][qml][settings][transition][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(editing);
    // Opened at the row (the Settings search's reveal): the page scrolls to it.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, QStringLiteral("timeline")),
                                      Q_ARG(QString, QStringLiteral("defaultVideoTransition"))));
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader);
    QObject* window = loader->property("item").value<QObject*>();
    REQUIRE(window);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] {
        REQUIRE(QMetaObject::invokeMethod(window, "page", Q_RETURN_ARG(QQuickItem*, page), Q_ARG(QString, QStringLiteral("timeline"))));
        return page != nullptr && page->isVisible();
    }));
    QQuickItem* video = findNamed(page, QStringLiteral("settingsDefaultVideoTransition"));
    QQuickItem* audio = findNamed(page, QStringLiteral("settingsDefaultAudioTransition"));
    REQUIRE(video);
    REQUIRE(audio);
    CHECK(video->property("text").toString() == QStringLiteral("Cross Dissolve"));
    CHECK(audio->property("text").toString() == QStringLiteral("Constant Power"));

    // Another video transition: the setting changes and the picker shows it.
    QString other;
    QString otherName;
    for (const QVariant& choice : editing->transitionChoices(QStringLiteral("video"))) {
        const QVariantMap c = choice.toMap();
        if (c.value(QStringLiteral("id")).toString() != QStringLiteral("crossDissolve")) {
            other = c.value(QStringLiteral("id")).toString();
            otherName = c.value(QStringLiteral("name")).toString();
            break;
        }
    }
    REQUIRE_FALSE(other.isEmpty());
    REQUIRE(QMetaObject::invokeMethod(video, "chosen", Q_ARG(QString, other)));
    CHECK(editing->defaultVideoTransition() == other);
    CHECK(video->property("text").toString() == otherName);
    CHECK(editing->defaultAudioTransition() == QStringLiteral("constantPower"));
    spin(600); // the reveal's scroll and highlight
    REQUIRE_FALSE(shell.grab().isNull());
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF pickers = sceneRect(video).united(sceneRect(audio));
    const QRectF pageRect = sceneRect(page);
    const QRectF row = QRectF(pageRect.left(), pickers.top() - 64, pageRect.width(), pickers.height() + 88)
                           .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(row.topLeft() * dpr, row.size() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("settings-default-transitions.png"))));
    REQUIRE(editing->setDefaultTransition(QStringLiteral("crossDissolve")));
    checkNoMessages();
}
