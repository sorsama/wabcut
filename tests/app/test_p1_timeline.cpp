// 0.6 (P1 round, stream 1A): timeline editing - the Square timeline corners (Settings > Appearance), selected edit points
// taking E / Ctrl+D / Effects-panel transitions, Edit Marker / Add Chapter Marker / M on a marker and the marker menus,
// Trim and Extend Previous / Next Edit to Playhead, Shift-click track header toggles with Sync Lock and the target-all
// keys, the Clip Mismatch Warning and the out-of-sync tags with Move / Slip into Sync. C++ rigs (TimelineHarness) for
// the edits, the real shell (Main.qml, hidden) for the QML. Screenshots under test-output/ui (p1-timeline-*.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "MarkersBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
#include "TimelineView.h"
#include "TrimBridge.h"

#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>

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

void mouseAt(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons,
             Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, scene, scene, button, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

/// A left click on `item`'s centre (real mouse events through the window), with `mods` held.
void clickItem(QQuickWindow* window, QQuickItem* item, Qt::KeyboardModifiers mods = {}) {
    REQUIRE(item);
    const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, mods);
    mouseAt(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
    spin(30);
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

/// The real shell (Main.qml, hidden, dark, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    Timeline tl;
    QObject* keymap = nullptr;
    QObject* palette = nullptr;

    explicit ShellProject(std::string_view picture, bool link = true) : tl(picture) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        if (link)
            tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        keymap = shell.engine.singletonInstance<QObject*>("Wabcut", "KeymapDispatcher");
        palette = shell.engine.singletonInstance<QObject*>("Wabcut", "CommandPalette");
        REQUIRE(keymap);
        REQUIRE(palette);
        spin(200);
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    QObject* object(const QString& name) const { return shell.window->findChild<QObject*>(name); }
    std::size_t undo() const { return controller->session().undoCount(); }
    const Sequence& sequence() const { return *controller->currentSequence(); }
    TimelineView* view() const {
        auto* v = qobject_cast<TimelineView*>(item(QStringLiteral("timelineView")));
        REQUIRE(v);
        return v;
    }
    void run(const char* command) const {
        REQUIRE(QMetaObject::invokeMethod(keymap, "trigger", Q_ARG(QString, QString::fromLatin1(command))));
        spin(50);
    }
    QVariantMap state(const char* command) const {
        QVariantMap out;
        REQUIRE(QMetaObject::invokeMethod(palette, "commandState", Q_RETURN_ARG(QVariantMap, out),
                                          Q_ARG(QString, QString::fromLatin1(command))));
        return out;
    }
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

/// A 2560 x 1440, 60 fps recording (an OBS capture) in place of media `letter`, and the sequence emptied.
ProjectState withObsMedia(const Timeline& tl, char letter) {
    ProjectState state = tl.state;
    const MediaItem* item = state.findMedia(tl.media(letter));
    REQUIRE(item);
    MediaItem obs = *item;
    obs.name = "2026-10-31 20-00-00.mp4";
    obs.info.video.front().width = 2560;
    obs.info.video.front().height = 1440;
    obs.info.video.front().frameRate = Rate{60, 1};
    for (MediaPtr& m : state.media) {
        if (m->id == obs.id)
            m = std::make_shared<const MediaItem>(obs);
    }
    std::vector<ClipId> clips;
    state.findSequence(tl.seq)->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips)
            clips.push_back(c.id);
    });
    auto emptied = edit::deleteClips(state, tl.seq, clips, edit::LinkMode::IgnoreLinks);
    REQUIRE(emptied);
    return std::move(emptied->state);
}

} // namespace

// ---- #0: selected edit points ----------------------------------------------------------------------------------------

TEST_CASE("Selected edit points take E and Ctrl+D and Effects panel transitions", "[app][timeline][trim][editpoints]") {
    Rig rig(R"(
        V1 tgt |AaaaaaB9bbbbb..|
        A1 tgt |AaaaaaB9bbbbb..|
    )");
    TrimBridge trim;
    trim.setController(&rig.controller);
    ProjectController& c = rig.controller;
    const ClipId a = rig.clip("V1@0");

    // A click on A's tail as a roll: the point and its linked audio partner's go to the controller.
    trim.selectEditPoint(clipKey(a), QStringLiteral("tail"), QStringLiteral("roll"), 0);
    REQUIRE(trim.active());
    REQUIRE(c.trimEditPoints().size() == 2);
    CHECK_FALSE(c.trimEditPoints().front().alignment); // a roll: the default (centred)
    CHECK(c.selection().empty());
    // E and Ctrl+D are available without a selected clip or a targeted track.
    CHECK(c.commandAvailability(QStringLiteral("sequence.extendEditToPlayhead")).value(QStringLiteral("enabled")).toBool());
    CHECK(c.commandAvailability(QStringLiteral("sequence.applyVideoTransition")).value(QStringLiteral("enabled")).toBool());

    SECTION("Ctrl+D puts the default video transition on the selected cut only") {
        const std::size_t steps = rig.undoSteps();
        REQUIRE(c.runCommand(QStringLiteral("sequence.applyVideoTransition")));
        const Sequence& seq = *c.currentSequence();
        REQUIRE(seq.videoTracks[0]->transitions.size() == 1);
        CHECK(seq.videoTracks[0]->transitions[0].outgoing == a);
        CHECK(seq.videoTracks[0]->transitions[0].before > Ticks{}); // across the cut
        CHECK(seq.videoTracks[0]->transitions[0].after > Ticks{});
        CHECK(seq.audioTracks[0]->transitions.empty());
        CHECK(rig.undoSteps() == steps + 1);
        // Ctrl+Shift+D: the audio point.
        REQUIRE(c.runCommand(QStringLiteral("sequence.applyAudioTransition")));
        CHECK(c.currentSequence()->audioTracks[0]->transitions.size() == 1);
        REQUIRE(c.undo());
        REQUIRE(c.undo());
        CHECK(rig.picture() == normalize(rig.original));
        CHECK(c.currentSequence()->videoTracks[0]->transitions.empty());
    }
    SECTION("a ripple point on the incoming head starts at the cut") {
        trim.selectEditPoint(clipKey(rig.clip("V1@6")), QStringLiteral("head"), QStringLiteral("ripple"), 0);
        REQUIRE(c.trimEditPoints().front().alignment == TransitionAlignment::StartAtCut);
        REQUIRE(c.applyDefaultTransition(false));
        const Transition& t = c.currentSequence()->videoTracks[0]->transitions[0];
        CHECK(t.before == Ticks{}); // it starts at the cut
        CHECK(t.after > Ticks{});
    }
    SECTION("E moves the selected edit to the playhead with the trim type") {
        c.setPlayhead(f(8));
        const std::size_t steps = rig.undoSteps();
        REQUIRE(trim.runCommand(QStringLiteral("sequence.extendEditToPlayhead")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaaaaaB11bb..|
            A1 |AaaaaaaaB11bb..|
        )"));
        CHECK(rig.undoSteps() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Extend Edit"));
        CHECK(trim.active()); // the points stay selected
        CHECK(trim.editTime() == f(8));
        REQUIRE(c.undo());
        CHECK(rig.picture() == normalize(rig.original));
        // Without edit points E is the controller's (the selected clip's nearest edge).
        trim.clear();
        CHECK(c.trimEditPoints().empty());
        CHECK_FALSE(trim.runCommand(QStringLiteral("sequence.extendEditToPlayhead")));
    }
    SECTION("an Effects panel transition goes on the edit points") {
        const QVariantMap result = c.effectControls()->applyToSelectedClips(QStringLiteral("effect:dipToBlack"));
        INFO(result.value(QStringLiteral("error")).toString().toStdString());
        REQUIRE(result.value(QStringLiteral("ok")).toBool());
        CHECK(result.value(QStringLiteral("message")).toString() == QStringLiteral("Added Dip to Black to the edit point"));
        const Sequence& seq = *c.currentSequence();
        REQUIRE(seq.videoTracks[0]->transitions.size() == 1);
        CHECK(seq.videoTracks[0]->transitions[0].effect == "dipToBlack");
        CHECK(c.undoText() == QStringLiteral("Undo Apply Transition"));
    }
}

// ---- #11: non-ripple Q / W ---------------------------------------------------------------------------------------------

TEST_CASE("Trim and Extend Previous and Next Edit to Playhead are one undo step each", "[app][timeline][qw]") {
    Rig rig(R"(
        V1 tgt |AaaaB4bbbb|
        A1 tgt |AaaaB4bbbb|
    )");
    ProjectController& c = rig.controller;
    c.setPlayhead(f(6));
    const struct {
        const char* command;
        const char* undo;
        const char* picture;
    } rows[] = {
        {"sequence.trimPreviousToPlayhead", "Undo Trim Previous Edit", R"(
            V1 |Aaaa..B6bb|
            A1 |Aaaa..B6bb|
        )"},
        {"sequence.trimNextToPlayhead", "Undo Trim Next Edit", R"(
            V1 |AaaaB4....|
            A1 |AaaaB4....|
        )"},
        {"sequence.extendPreviousToPlayhead", "Undo Extend Previous Edit", R"(
            V1 |AaaaaaB6bb|
            A1 |AaaaaaB6bb|
        )"},
        {"sequence.extendNextToPlayhead", "Undo Extend Next Edit", R"(
            V1 |AaaaB4....|
            A1 |AaaaB4....|
        )"},
    };
    for (const auto& row : rows) {
        INFO(row.command);
        CHECK(c.commandAvailability(QString::fromLatin1(row.command)).value(QStringLiteral("enabled")).toBool());
        const std::size_t steps = rig.undoSteps();
        REQUIRE(c.runCommand(QString::fromLatin1(row.command)));
        CHECK(rig.picture() == normalize(row.picture));
        CHECK(c.playhead() == f(6)); // the playhead stays
        if (rig.picture() != normalize(rig.original)) {
            CHECK(rig.undoSteps() == steps + 1);
            CHECK(c.undoText() == QString::fromLatin1(row.undo));
            REQUIRE(c.undo());
        }
        CHECK(rig.picture() == normalize(rig.original));
    }
    // Extend Next Edit from 2: the cut at 4 comes back to the playhead (B shows earlier media).
    c.setPlayhead(f(2));
    REQUIRE(c.extendNextToPlayhead());
    CHECK(rig.picture() == normalize(R"(
        V1 |AaB2bbbbbb|
        A1 |AaB2bbbbbb|
    )"));
    REQUIRE(c.undo());
    // Untargeted: the commands say why.
    c.setTrackFlagAll(QStringLiteral("video"), QStringLiteral("targeted"), false);
    c.setTrackFlagAll(QStringLiteral("audio"), QStringLiteral("targeted"), false);
    const QVariantMap off = c.commandAvailability(QStringLiteral("sequence.trimPreviousToPlayhead"));
    CHECK_FALSE(off.value(QStringLiteral("enabled")).toBool());
    CHECK(off.value(QStringLiteral("reason")).toString() == QStringLiteral("No track is targeted"));
}

// ---- #12: track flags on every track --------------------------------------------------------------------------------

TEST_CASE("Track flags set on every track of a kind in one undo step", "[app][timeline][tracks]") {
    Rig rig(R"(
        V2     |..........|
        V1 tgt |Aaaa......|
        A1 tgt |Aaaa......|
        A2     |..........|
    )");
    ProjectController& c = rig.controller;
    const std::size_t steps = rig.undoSteps();
    REQUIRE(c.setTrackFlagAll(QStringLiteral("audio"), QStringLiteral("muted"), true));
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(c.currentSequence()->audioTracks[0]->muted);
    CHECK(c.currentSequence()->audioTracks[1]->muted);
    CHECK_FALSE(c.currentSequence()->videoTracks[0]->locked);
    CHECK(c.undoText() == QStringLiteral("Undo Mute All Tracks"));
    REQUIRE(c.undo());
    CHECK_FALSE(c.currentSequence()->audioTracks[1]->muted);
    // Toggle All Video Targets: V2 is not targeted, so both get it; again, neither.
    REQUIRE(c.runCommand(QStringLiteral("timeline.toggleAllVideoTargets")));
    CHECK(c.currentSequence()->videoTracks[0]->targeted);
    CHECK(c.currentSequence()->videoTracks[1]->targeted);
    REQUIRE(c.runCommand(QStringLiteral("timeline.toggleAllVideoTargets")));
    CHECK_FALSE(c.currentSequence()->videoTracks[0]->targeted);
    CHECK_FALSE(c.currentSequence()->videoTracks[1]->targeted);
    CHECK(c.currentSequence()->audioTracks[0]->targeted); // audio untouched
    CHECK_FALSE(c.setTrackFlagAll(QStringLiteral("subtitles"), QStringLiteral("muted"), true));
    CHECK_FALSE(c.setTrackFlagAll(QStringLiteral("video"), QStringLiteral("loud"), true));
}

// ---- #13: out-of-sync tags ----------------------------------------------------------------------------------------------

TEST_CASE("Out of sync linked clips show a red tag and move or slip into sync", "[app][timeline][sync]") {
    Rig rig(R"(
        V1 tgt |Aaaa......|
        A1 tgt |..Aaaa....|
    )", 40.0, false);
    rig.tl.link("V1@0 A1@2");
    rig.controller.resetProject(rig.tl.state, rig.tl.seq);
    rig.harness.frame();
    rig.harness.frame();
    ProjectController& c = rig.controller;
    const ClipId video = rig.clip("V1@0");
    const ClipId audio = rig.clip("A1@2");
    CHECK(c.syncOffsetFrames(clipKey(audio)) == 2);
    CHECK(c.syncOffsetFrames(clipKey(video)) == -2);
    CHECK(rig.view->stats().syncBadges.value(audio.value) == QStringLiteral("+2"));
    CHECK(rig.view->stats().syncBadges.value(video.value) == QStringLiteral("-2"));
    // The clip tooltip says it too.
    const QVariantMap hover = rig.view->clipHoverInfo(clipKey(audio));
    CHECK(hover.value(QStringLiteral("syncFrames")).toInt() == 2);
    CHECK(hover.value(QStringLiteral("text")).toString().contains(QStringLiteral("2 frames late against its linked clip")));
    // Nothing selected: the commands say so; the clip menu rows show only on such clips.
    CHECK_FALSE(c.commandAvailability(QStringLiteral("clip.moveIntoSync")).value(QStringLiteral("enabled")).toBool());

    SECTION("Move into Sync: with both halves selected the sound moves to the picture") {
        c.selectClipById(video, ProjectController::Replace); // linked selection: both
        REQUIRE(c.selection().size() == 2);
        CHECK(c.commandAvailability(QStringLiteral("clip.moveIntoSync")).value(QStringLiteral("enabled")).toBool());
        const std::size_t steps = rig.undoSteps();
        REQUIRE(c.moveIntoSync());
        CHECK(rig.picture() == normalize(R"(
            V1 |Aaaa......|
            A1 |Aaaa......|
        )"));
        CHECK(rig.undoSteps() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Move into Sync"));
        rig.harness.frame();
        CHECK(rig.view->stats().syncBadges.isEmpty());
        CHECK(c.syncOffsetFrames(clipKey(audio)) == 0);
        REQUIRE(c.undo());
        rig.harness.frame();
        CHECK(rig.view->stats().syncBadges.value(audio.value) == QStringLiteral("+2"));
    }
    SECTION("Slip into Sync: the sound stays and plays the matching source") {
        c.selectClipById(audio, ProjectController::Replace, false);
        REQUIRE(c.slipIntoSync());
        CHECK(rig.picture() == normalize(R"(
            V1 |Aaaa......|
            A1 |..A2aa....|
        )"));
        CHECK(c.undoText() == QStringLiteral("Undo Slip into Sync"));
        CHECK(c.syncOffsetFrames(clipKey(audio)) == 0);
        REQUIRE(c.undo());
    }
    SECTION("in sync: nothing to do") {
        c.selectClipById(video, ProjectController::Replace);
        REQUIRE(c.moveIntoSync());
        CHECK_FALSE(c.moveIntoSync());
        CHECK(c.lastError() == QStringLiteral("No selected clip is out of sync"));
    }
}

// ---- #10: Clip Mismatch Warning (C++) -------------------------------------------------------------------------------

TEST_CASE("Clip mismatch matches an empty sequence to the first clip in the drop's undo step", "[app][timeline][mismatch]") {
    Rig rig(R"(
        V1 tgt |Oooo|
        A1 tgt |Oooo|
    )");
    ProjectController& c = rig.controller;
    c.resetProject(withObsMedia(rig.tl, 'O'), rig.tl.seq);
    TimelineView& view = *rig.view;
    REQUIRE(view.sequenceEmpty());
    CHECK(view.clipMismatch().isEmpty());
    const int width = c.currentSequence()->width;
    const Rate rate = c.currentSequence()->frameRate;
    editor::MediaDrop drop;
    drop.media = rig.tl.media('O');
    drop.sequence = rig.tl.seq;
    drop.mode = editor::DropMode::Overwrite;
    REQUIRE(editor::dropMedia(c.session(), drop));
    CHECK_FALSE(view.sequenceEmpty());
    const std::size_t steps = rig.undoSteps();
    const QVariantMap mismatch = view.clipMismatch();
    CHECK(mismatch.value(QStringLiteral("media")).toString() == mediaKey(rig.tl.media('O')));
    CHECK(mismatch.value(QStringLiteral("mediaText")).toString() == QStringLiteral("2560 x 1440, 60 fps"));
    CHECK(mismatch.value(QStringLiteral("sequenceText")).toString().startsWith(QStringLiteral("1920 x 1080")));
    REQUIRE(view.matchSequenceToClip(true, c.revision()));
    CHECK(c.currentSequence()->width == 2560);
    CHECK(c.currentSequence()->height == 1440);
    CHECK(c.currentSequence()->frameRate == Rate{60, 1});
    CHECK(rig.undoSteps() == steps); // one step with the drop
    CHECK(c.undoText() == QStringLiteral("Undo Overwrite"));
    CHECK(view.clipMismatch().isEmpty());
    REQUIRE(c.undo());
    CHECK(view.sequenceEmpty());
    CHECK(c.currentSequence()->width == width);
    CHECK(c.currentSequence()->frameRate == rate);
}

// ---- The shell ----------------------------------------------------------------------------------------------------------

TEST_CASE("Timeline corners Square squares the timeline in the dark and light themes",
          "[app][qml][timeline][corners][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V2     |..Cccccc....Dddd....|
        V1 tgt |AaaaaaaaBbbbbbbb....|
        A1 tgt |AaaaaaaaBbbbbbbb....|
    )");
    if (!dark)
        p.shell.setAppearance(false, "ultraviolet");
    TimelineView* view = p.view();
    view->setPixelsPerSecond(500);
    view->setScrollX(0);
    // The default: rounded (the Echo look).
    CHECK(view->parsedStyle().clipRadius > 0);
    CHECK(view->parsedStyle().laneRadius > 0);
    p.controller->selectClipById(p.tl.clip("V1@8"), ProjectController::Replace);
    spin(300);
    p.settle();
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    if (dark)
        p.shot(sceneRect(panel), QStringLiteral("p1-timeline-corners-rounded.png"));
    // Settings > Appearance > Timeline corners: Square, live.
    p.shell.preferences->setProperty("timelineCorners", QStringLiteral("square"));
    spin(50);
    CHECK(p.shell.theme->property("squareTimeline").toBool());
    CHECK(p.shell.theme->property("radiusTimeline").toInt() == 0);
    CHECK(view->parsedStyle().clipRadius == 0);
    CHECK(view->parsedStyle().laneRadius == 0);
    CHECK(view->parsedStyle().chipRadius == 0);
    CHECK(waitFor([] { return QSettings().value(QStringLiteral("appearance/timelineCorners")).toString() == QStringLiteral("square"); },
                  3000));
    p.settle();
    p.shot(sceneRect(panel), dark ? QStringLiteral("p1-timeline-corners-square.png")
                                  : QStringLiteral("p1-timeline-corners-square-light.png"));
    // Back to Rounded.
    p.shell.preferences->setProperty("timelineCorners", QStringLiteral("rounded"));
    spin(50);
    CHECK(view->parsedStyle().clipRadius > 0);
    CHECK_FALSE(p.shell.theme->property("squareTimeline").toBool());
    if (dark) {
        // Settings > Appearance > Timeline corners and Settings > Timeline & editing > Clip mismatch warning.
        REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "openSettings", Q_ARG(QString, QStringLiteral("appearance")),
                                          Q_ARG(QString, QString())));
        QObject* loader = p.object(QStringLiteral("settingsWindowLoader"));
        REQUIRE(loader);
        QObject* settings = loader->property("item").value<QObject*>();
        REQUIRE(settings);
        REQUIRE(waitFor([&] { return settings->property("opened").toBool(); }));
        auto* content = settings->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        QQuickItem* corners = nullptr;
        REQUIRE(waitFor([&] { return (corners = findNamed(content, QStringLiteral("timelineCornersTabs"))) != nullptr; }));
        CHECK(corners->property("currentIndex").toInt() == 0);
        REQUIRE(QMetaObject::invokeMethod(corners, "activated", Q_ARG(int, 1)));
        CHECK(p.shell.preferences->property("timelineCorners").toString() == QStringLiteral("square"));
        CHECK(view->parsedStyle().clipRadius == 0);
        REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "openSettings", Q_ARG(QString, QStringLiteral("timeline")),
                                          Q_ARG(QString, QStringLiteral("clipMismatch"))));
        QQuickItem* mismatch = nullptr;
        REQUIRE(waitFor([&] { return (mismatch = findNamed(content, QStringLiteral("settingsClipMismatch"))) != nullptr; }));
        CHECK(mismatch->property("currentIndex").toInt() == 2); // the test shell keeps settings ("keep")
        REQUIRE(QMetaObject::invokeMethod(mismatch, "activated", Q_ARG(int, 0)));
        CHECK(p.shell.preferences->property("clipMismatch").toString() == QStringLiteral("ask"));
        REQUIRE(QMetaObject::invokeMethod(settings, "close"));
    }
    resetSettings();
    checkNoMessages();
}

TEST_CASE("Edit Marker and Add Chapter Marker and M on a marker open the marker editor",
          "[app][qml][markers][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )");
    ProjectController& c = *p.controller;
    TimelineView* view = p.view();
    view->setPixelsPerSecond(100);
    view->setScrollX(0);
    auto* markers = p.shell.engine.singletonInstance<MarkersBridge*>("Wabcut", "Markers");
    REQUIRE(markers);
    QQuickItem* ruler = p.item(QStringLiteral("timelineRuler"));
    REQUIRE(ruler);
    QObject* editor = ruler->findChild<QObject*>(QStringLiteral("markerEditor"));
    REQUIRE(editor);
    c.setPlayhead(f(10));
    REQUIRE(markers->add(false) == QStringLiteral("sequence"));
    REQUIRE(p.sequence().markers.size() == 1);
    // Edit Marker… is available on the marker's frame, not elsewhere.
    CHECK(p.state("marker.edit").value(QStringLiteral("enabled")).toBool());
    CHECK(p.state("marker.addChapter").value(QStringLiteral("enabled")).toBool());

    // M with the playhead on the marker: its editor opens, no second marker.
    const std::size_t steps = p.undo();
    p.run("marker.add");
    waitOpened(editor);
    CHECK(editor->property("markerIndex").toInt() == 0);
    CHECK(p.sequence().markers.size() == 1);
    CHECK(p.undo() == steps);
    REQUIRE(QMetaObject::invokeMethod(editor, "close"));
    waitClosed(editor);
    // Markers > Edit Marker…: the same.
    p.run("marker.edit");
    waitOpened(editor);
    REQUIRE(QMetaObject::invokeMethod(editor, "close"));
    waitClosed(editor);

    // Elsewhere: Edit Marker… is off with the reason; Add Chapter Marker… adds a chapter there and opens its editor.
    c.setPlayhead(f(40));
    spin(50);
    const QVariantMap off = p.state("marker.edit");
    CHECK_FALSE(off.value(QStringLiteral("enabled")).toBool());
    CHECK(off.value(QStringLiteral("reason")).toString() == QStringLiteral("No marker at the playhead"));
    p.run("marker.addChapter");
    REQUIRE(p.sequence().markers.size() == 2);
    CHECK(p.sequence().markers[1].type == MarkerType::Chapter);
    CHECK(p.sequence().markers[1].start == f(40));
    CHECK(c.undoText() == QStringLiteral("Undo Add Chapter Marker"));
    waitOpened(editor);
    CHECK(editor->property("markerIndex").toInt() == 1);
    p.settle();
    p.shot(popupRect(editor).united(sceneRect(ruler).intersected(popupRect(editor).adjusted(-200, -60, 200, 0)))
               .adjusted(-12, -12, 12, 12),
           QStringLiteral("p1-timeline-chapter-marker-editor.png"));
    REQUIRE(QMetaObject::invokeMethod(editor, "close"));
    waitClosed(editor);

    // The ruler's flag menu: Marker Colour and Marker Type submenus (one undo step each) and Go to Marker.
    QObject* menu = p.object(QStringLiteral("rulerContextMenu"));
    REQUIRE(menu);
    QQuickItem* flag = findNamed(ruler, QStringLiteral("rulerMarker_0"));
    REQUIRE(flag);
    const QPointF onFlag = flag->mapToScene(QPointF(0, flag->height() - 8));
    mouseAt(p.shell.window.get(), QEvent::MouseButtonPress, onFlag, Qt::RightButton, Qt::RightButton);
    mouseAt(p.shell.window.get(), QEvent::MouseButtonRelease, onFlag, Qt::RightButton, Qt::NoButton);
    waitOpened(menu);
    auto* colorMenu = menu->property("colorMenu").value<QObject*>();
    REQUIRE(colorMenu);
    int at = -1;
    REQUIRE(QMetaObject::invokeMethod(menu, "submenuIndex", Q_RETURN_ARG(int, at),
                                      Q_ARG(QVariant, QVariant::fromValue(colorMenu))));
    CHECK(at == 1); // right after Edit Marker…
    CHECK(child(menu, "rulerMenuGoToMarker")->property("visible").toBool());
    p.settle();
    p.shot(popupRect(menu).adjusted(-12, -12, 12, 12), QStringLiteral("p1-timeline-ruler-marker-menu.png"));
    trigger(child(colorMenu, "markerColor_1"));
    waitClosed(menu);
    CHECK(p.sequence().markers[0].colorIndex == 1);
    CHECK(c.undoText() == QStringLiteral("Undo Edit Marker"));
    // The ruler's own menu (off a flag) has no marker submenus.
    const QPointF offFlag = ruler->mapToScene(QPointF(ruler->width() - 20, ruler->height() / 2));
    mouseAt(p.shell.window.get(), QEvent::MouseButtonPress, offFlag, Qt::RightButton, Qt::RightButton);
    mouseAt(p.shell.window.get(), QEvent::MouseButtonRelease, offFlag, Qt::RightButton, Qt::NoButton);
    waitOpened(menu);
    REQUIRE(QMetaObject::invokeMethod(menu, "submenuIndex", Q_RETURN_ARG(int, at),
                                      Q_ARG(QVariant, QVariant::fromValue(colorMenu))));
    CHECK(at == -1);
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    waitClosed(menu);

    // The Program time bar's marker menu: Type, then Delete Marker (one undo step).
    QQuickItem* timeBar = p.item(QStringLiteral("programTimeBar"));
    REQUIRE(timeBar);
    QObject* barMenu = timeBar->findChild<QObject*>(QStringLiteral("markerContextMenu"));
    REQUIRE(barMenu);
    markers->setScope(QStringLiteral("sequence"));
    REQUIRE(QMetaObject::invokeMethod(barMenu, "openAt", Q_ARG(QQuickItem*, timeBar),
                                      Q_ARG(QPointF, QPointF(20, 4)), Q_ARG(int, 1)));
    waitOpened(barMenu);
    p.settle();
    p.shot(popupRect(barMenu).adjusted(-12, -12, 12, 12), QStringLiteral("p1-marker-context-menu.png"));
    auto* typeMenu = barMenu->findChild<QObject*>(QStringLiteral("markerTypeSubmenu"));
    REQUIRE(typeMenu);
    CHECK(child(typeMenu, "markerType_chapter")->property("checked").toBool());
    trigger(child(typeMenu, "markerType_comment"));
    waitClosed(barMenu);
    CHECK(p.sequence().markers[1].type == MarkerType::Comment);
    REQUIRE(QMetaObject::invokeMethod(barMenu, "openAt", Q_ARG(QQuickItem*, timeBar),
                                      Q_ARG(QPointF, QPointF(20, 4)), Q_ARG(int, 1)));
    waitOpened(barMenu);
    const std::size_t before = p.undo();
    trigger(child(barMenu, "markerMenuDelete"));
    waitClosed(barMenu);
    CHECK(p.sequence().markers.size() == 1);
    CHECK(p.undo() == before + 1);
    REQUIRE(c.undo());
    CHECK(p.sequence().markers.size() == 2);

    // During playback M adds (the marker editor stays shut).
    resetSettings();
    checkNoMessages();
}

TEST_CASE("Track header Shift-click toggles every track and the Sync Lock button",
          "[app][qml][timeline][tracks][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V2     |..........|
        V1 tgt |Aaaaaaaa..|
        A1 tgt |Aaaaaaaa..|
        A2     |..........|
    )");
    ProjectController& c = *p.controller;
    QQuickWindow* window = p.shell.window.get();
    // Each click is aimed at the layout a grab polished (the header column positions its rows when polished).
    const auto click = [&](const char* name, Qt::KeyboardModifiers mods = {}) {
        p.settle();
        clickItem(window, p.item(QString::fromLatin1(name)), mods);
    };
    QQuickItem* syncA1 = p.item(QStringLiteral("trackSyncLock_A1"));
    QQuickItem* syncV1 = p.item(QStringLiteral("trackSyncLock_V1"));
    REQUIRE(syncA1);
    REQUIRE(syncV1);
    CHECK(syncA1->isVisible());
    CHECK(syncA1->property("checked").toBool()); // sync lock is on by default

    // A plain click: that track only, one undo step.
    std::size_t steps = p.undo();
    click("trackSyncLock_A1");
    CHECK_FALSE(p.sequence().audioTracks[0]->syncLocked);
    CHECK(p.sequence().audioTracks[1]->syncLocked);
    CHECK(p.undo() == steps + 1);
    spin(50);
    CHECK_FALSE(p.item(QStringLiteral("trackSyncLock_A1"))->property("checked").toBool());

    // Shift-click on A1's mute: every audio track, one step.
    steps = p.undo();
    click("trackMute_A1", Qt::ShiftModifier);
    CHECK(p.sequence().audioTracks[0]->muted);
    CHECK(p.sequence().audioTracks[1]->muted);
    CHECK(p.undo() == steps + 1);
    // Shift-click on V1's lock: every video track.
    click("trackLock_V1", Qt::ShiftModifier);
    CHECK(p.sequence().videoTracks[0]->locked);
    CHECK(p.sequence().videoTracks[1]->locked);
    CHECK_FALSE(p.sequence().audioTracks[0]->locked);
    // A later plain click is one track again (the Shift was used up).
    click("trackLock_V2");
    CHECK_FALSE(p.sequence().videoTracks[1]->locked);
    CHECK(p.sequence().videoTracks[0]->locked);
    // Shift-click on the target chip of V2: every video track targeted.
    p.settle();
    QQuickItem* header = p.item(QStringLiteral("trackName_V2"))->parentItem();
    REQUIRE(header);
    clickItem(window, findNamed(header, QStringLiteral("trackTarget")), Qt::ShiftModifier);
    CHECK(p.sequence().videoTracks[0]->targeted);
    CHECK(p.sequence().videoTracks[1]->targeted);

    // The keys: Toggle All Audio Targets (A2 is not targeted: all on, then all off).
    p.run("timeline.toggleAllAudioTargets");
    CHECK(p.sequence().audioTracks[0]->targeted);
    CHECK(p.sequence().audioTracks[1]->targeted);
    p.run("timeline.toggleAllAudioTargets");
    CHECK_FALSE(p.sequence().audioTracks[0]->targeted);
    CHECK_FALSE(p.sequence().audioTracks[1]->targeted);
    // Toggle All Source Video with no source loaded: nothing to switch, nothing breaks.
    steps = p.undo();
    p.run("timeline.toggleAllVideoSources");
    CHECK(p.undo() == steps);

    p.settle();
    QQuickItem* headers = p.item(QStringLiteral("trackName_V2"))->parentItem()->parentItem();
    REQUIRE(headers);
    p.shot(sceneRect(headers).adjusted(-4, -4, 4, 4), QStringLiteral("p1-timeline-track-headers.png"));
    (void)c;
    resetSettings();
    checkNoMessages();
}

TEST_CASE("Clip Mismatch Warning in the shell asks and changes the sequence settings",
          "[app][qml][timeline][mismatch][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Oooo|
        A1 tgt |Oooo|
    )");
    if (!dark)
        p.shell.setAppearance(false, "ultraviolet");
    ProjectController& c = *p.controller;
    c.resetProject(withObsMedia(p.tl, 'O'), p.tl.seq);
    spin(100);
    p.shell.preferences->setProperty("clipMismatch", QStringLiteral("ask"));
    auto* source = p.shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(source);
    REQUIRE(source->load(mediaKey(p.tl.media('O'))));
    spin(100);
    QObject* dialog = p.object(QStringLiteral("clipMismatchDialog"));
    REQUIRE(dialog);
    const std::size_t steps = p.undo();
    // Overwrite (.) from the Source Monitor into the empty sequence: the clip lands, then the warning asks.
    REQUIRE(QMetaObject::invokeMethod(p.shell.window.get(), "sourceEdit", Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant())));
    waitOpened(dialog);
    CHECK(p.undo() == steps + 1);
    CHECK(inDialog(dialog, "clipMismatchMessage")->property("text").toString().contains(QStringLiteral("2560 x 1440, 60 fps")));
    p.settle();
    p.shot(popupRect(dialog).adjusted(-16, -16, 16, 16), dark ? QStringLiteral("p1-clip-mismatch-dialog.png")
                                                                : QStringLiteral("p1-clip-mismatch-dialog-light.png"));
    if (dark) {
        // Change sequence settings, with Always do this: one undo step with the drop, and the answer is kept.
        REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "clipMismatchRemember"), "toggle"));
        REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "clipMismatchRemember"), "toggled"));
        REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "clipMismatchChange"), "clicked"));
        waitClosed(dialog);
        CHECK(p.sequence().width == 2560);
        CHECK(p.sequence().frameRate == Rate{60, 1});
        CHECK(p.undo() == steps + 1);
        CHECK(p.shell.preferences->property("clipMismatch").toString() == QStringLiteral("change"));
        REQUIRE(c.undo());
        CHECK(p.sequence().width == 1920);
    } else {
        // Keep existing settings: the drop stays as it is.
        REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "clipMismatchKeep"), "clicked"));
        waitClosed(dialog);
        CHECK(p.sequence().width == 1920);
        CHECK(p.undo() == steps + 1);
        CHECK(p.shell.preferences->property("clipMismatch").toString() == QStringLiteral("ask"));
    }
    resetSettings();
    checkNoMessages();
}

TEST_CASE("The menus list the 0.6 timeline commands with their keys", "[app][qml][menus][timeline]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaB4bbb|
        A1 tgt |AaaaB4bbb|
    )");
    for (const char* name : {"menuCommand_sequence.extendPreviousToPlayhead", "menuCommand_sequence.extendNextToPlayhead",
                             "menuCommand_sequence.trimPreviousToPlayhead", "menuCommand_sequence.trimNextToPlayhead",
                             "markersMenuAddChapter", "markersMenuEdit"}) {
        INFO(name);
        CHECK(p.object(QString::fromLatin1(name)) != nullptr);
    }
#ifdef _WIN32
    QString keys;
    REQUIRE(QMetaObject::invokeMethod(p.keymap, "shortcutText", Q_RETURN_ARG(QString, keys),
                                      Q_ARG(QString, QStringLiteral("sequence.extendPreviousToPlayhead"))));
    CHECK(keys == QStringLiteral("Shift+Q"));
    REQUIRE(QMetaObject::invokeMethod(p.keymap, "shortcutText", Q_RETURN_ARG(QString, keys),
                                      Q_ARG(QString, QStringLiteral("sequence.trimNextToPlayhead"))));
    CHECK(keys == QStringLiteral("Ctrl+Alt+W"));
    REQUIRE(QMetaObject::invokeMethod(p.keymap, "shortcutText", Q_RETURN_ARG(QString, keys),
                                      Q_ARG(QString, QStringLiteral("timeline.toggleAllVideoTargets"))));
    CHECK(keys == QStringLiteral("Ctrl+0"));
#endif
    // Through the keymap: Shift+W's command extends the next edit to the playhead (one undo step).
    p.controller->setPlayhead(f(2));
    const std::size_t steps = p.undo();
    p.run("sequence.extendNextToPlayhead");
    CHECK(p.undo() == steps + 1);
    CHECK(p.sequence().videoTracks[0]->clips[0].end() == f(2));
    resetSettings();
    checkNoMessages();
}
