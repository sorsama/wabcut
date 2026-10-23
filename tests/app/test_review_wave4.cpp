// Regression tests for the fourth-wave review (docs/internal/reviews-wave4.md), UI items. Each failed before its fix:
//   P1-8  closing a popover gave focus back to its trigger after the user had moved it (to a text field, or to the
//         modal dialog the popover opened), so typed keys ran editing shortcuts;
//   P2-41 the Timeline list claimed Delete with any modifier, so Shift+Delete (Ripple Delete) deleted plainly;
//   P2-42 presses on the list's heading strip fell through to the hidden lanes;
//   P2-44 the time-remap panel's selected key was a bare index kept across clips and key deletions;
//   P2-46 every media change (a Source Monitor mark) rebuilt the whole timeline and every list row;
//   P2-47 the interface size showed the raw stored value instead of what was applied;
//   P2-48 the export provenance sidecar threw on invalid UTF-8;
//   P2-24 (app parts) a remapped clip's sourceIn/sourceOut are curve times, yet clip markers and duplicate-frame bands
//         read them as media time, and paste dropped the remap.
// Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "ClipAccessModel.h"
#include "ExportController.h"
#include "ProjectController.h"
#include "TimeRemapBridge.h"
#include "TimelineAnalysis.h"
#include "TimelineView.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/TimeRemap.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Spins the event loop until `done` (or `ms` pass): waits for animations that a loaded machine runs late.
bool waitUntil(const std::function<bool()>& done, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms)
        spin(20);
    return done();
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

QObject* findObject(QObject* root, const QString& name) {
    for (QObject* o : root->findChildren<QObject*>()) {
        if (o->objectName() == name)
            return o;
    }
    return nullptr;
}

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

void click(QQuickWindow* window, QPointF scenePos) {
    QMouseEvent press(QEvent::MouseButtonPress, scenePos, scenePos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, scenePos, scenePos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
}

QPointF centreOf(QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

bool isInside(QQuickItem* item, QQuickItem* ancestor) {
    for (QQuickItem* p = item; p; p = p->parentItem()) {
        if (p == ancestor)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("Review P1-8: a popover never takes focus back from a field the user clicked into", "[app][review][focus][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    // Motion stays on: the bug needs the popover's exit transition to end after the field took focus.
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);

    QQuickItem* button = findNamed(w->contentItem(), QStringLiteral("timelineDisplayButton"));
    REQUIRE(button);
    button->forceActiveFocus(Qt::TabFocusReason);
    tap(w, Qt::Key_Space, {}, QStringLiteral(" "));
    QObject* popover = nullptr;
    // Waits for states, not times: under a parallel test run the transitions can end late.
    REQUIRE(waitUntil([&] {
        popover = findObject(button, QStringLiteral("timelineDisplayPopover"));
        return popover && popover->property("opened").toBool();
    }, 5000));

    QQuickItem* field = findNamed(w->contentItem(), QStringLiteral("librarySearchField"));
    REQUIRE(field);
    REQUIRE(field->isVisible());
    click(w, centreOf(field));
    // The exit transition ends (the old popover then forced focus back onto its button), and a moment after.
    REQUIRE(waitUntil([&] { return !popover->property("visible").toBool(); }, 5000));
    spin(100);
    CHECK(field->hasActiveFocus());
    CHECK_FALSE(button->hasActiveFocus());

    const auto undo = controller->session().undoCount();
    const int revision = controller->revision();
    for (const QChar c : QStringLiteral("eqw"))
        tap(w, c.toUpper().unicode(), {}, QString(c));
    CHECK(field->property("text").toString() == QStringLiteral("eqw"));
    CHECK(controller->session().undoCount() == undo); // E / Q / W never reached the timeline
    CHECK(controller->revision() == revision);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Review P1-8: a dialog opened from Settings keeps the focus", "[app][review][focus][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());

    // 0.1.3: the Settings window (it replaced the Settings popover) on Playback & performance. UX rework: from the
    // keyboard through the menu bar - Tab to Edit, Space opens it, Settings… (once the menu has closed).
    QQuickItem* settings = findNamed(w->contentItem(), QStringLiteral("menuBar_edit"));
    REQUIRE(settings);
    settings->forceActiveFocus(Qt::TabFocusReason);
    tap(w, Qt::Key_Space, {}, QStringLiteral(" "));
    QObject* editMenu = shell.object("editMenu");
    REQUIRE(editMenu);
    REQUIRE(waitUntil([&] { return editMenu->property("opened").toBool(); }, 5000));
    QObject* settingsItem = shell.object("editMenuSettings");
    REQUIRE(settingsItem);
    REQUIRE(QMetaObject::invokeMethod(settingsItem, "clicked"));
    QObject* popover = nullptr;
    QObject* settingsLoader = shell.object("settingsWindowLoader");
    REQUIRE(settingsLoader);
    // Waits for states, not times: under a parallel test run the window can open late.
    REQUIRE(waitUntil([&] {
        popover = settingsLoader->property("item").value<QObject*>();
        return popover && popover->property("opened").toBool();
    }, 5000));
    auto* contentItem = popover->property("contentItem").value<QQuickItem*>();
    REQUIRE(contentItem);
    QQuickItem* category = findNamed(contentItem, QStringLiteral("settingsCategory_performance"));
    REQUIRE(category);
    click(w, centreOf(category));
    QQuickItem* media = nullptr;
    REQUIRE(waitUntil([&] {
        media = findNamed(contentItem, QStringLiteral("mediaCacheSettingsButton"));
        return media && media->isVisible() && media->width() > 0;
    }, 5000));
    click(w, centreOf(media));
    QObject* loader = shell.object("mediaCacheSettingsLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitUntil([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool() && !popover->property("visible").toBool();
    }, 5000));
    CHECK_FALSE(popover->property("visible").toBool());
    auto* dialogContent = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(dialogContent);
    CHECK_FALSE(settings->hasActiveFocus()); // not behind the modal dialog
    QQuickItem* focused = w->activeFocusItem();
    CHECK((focused && isInside(focused, dialogContent->parentItem())));
    QMetaObject::invokeMethod(dialog, "close");
    spin(300);
    takeMessages();
}

namespace {

/// The demo project in list mode, the list focused.
struct ListShell {
    Shell shell;
    QQuickWindow* w = nullptr;
    ProjectController* controller = nullptr;
    QQuickItem* list = nullptr;

    ListShell() {
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        w = shell.window.get();
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        QQuickItem* panel = shell.item("timelinePanel");
        REQUIRE(panel);
        panel->setProperty("listMode", true);
        spin(50);
        REQUIRE_FALSE(shell.grab().isNull());
        list = findNamed(w->contentItem(), QStringLiteral("timelineListView"));
        REQUIRE(list);
        REQUIRE(list->hasActiveFocus());
    }
};

} // namespace

TEST_CASE("Review P2-41: Shift+Delete in the Timeline list is Ripple Delete", "[app][review][a11y][qml]") {
    resetSettings();
    clearMessages();
    ListShell s;
    tap(s.w, Qt::Key_Home);
    tap(s.w, Qt::Key_Return);
    REQUIRE_FALSE(s.controller->selection().empty());
    s.list->forceActiveFocus();
    tap(s.w, Qt::Key_Delete, Qt::ShiftModifier);
    spin(30);
    INFO(s.controller->undoText().toStdString());
    CHECK(s.controller->undoText().contains(QStringLiteral("Ripple Delete")));
    takeMessages();
}

TEST_CASE("Review P2-42: presses on the Timeline list's heading strip do not reach the hidden lanes",
          "[app][review][a11y][qml]") {
    resetSettings();
    clearMessages();
    ListShell s;
    auto* lanes = qobject_cast<TimelineView*>(findNamed(s.w->contentItem(), QStringLiteral("timelineView")));
    REQUIRE(lanes);
    int pressed = 0;
    QObject::connect(lanes, &TimelineView::pressed, [&] { ++pressed; });
    QQuickItem* root = findNamed(s.w->contentItem(), QStringLiteral("timelineList"));
    REQUIRE(root);
    const auto selection = s.controller->selection();
    for (const QPointF local : {QPointF(root->width() / 2, 4), QPointF(root->width() - 3, root->height() / 2)}) {
        click(s.w, root->mapToScene(local));
        spin(30);
    }
    CHECK(pressed == 0);
    CHECK(s.controller->selection() == selection);
    takeMessages();
}

TEST_CASE("Review P2-44: the time-remap panel forgets its key on another clip and after deleting it",
          "[app][review][remap][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* bridge = shell.engine.singletonInstance<TimeRemapBridge*>("Wabcut", "TimeRemap");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(bridge);
    REQUIRE(controller);
    REQUIRE(bridge->setEnabled(true));
    REQUIRE(bridge->addKey(0.5));
    QQuickItem* panel = findNamed(shell.window->contentItem(), QStringLiteral("timeRemapControls"));
    REQUIRE(panel);
    panel->setProperty("expanded", true);
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    const QVariantList keys = bridge->keys();
    REQUIRE(keys.size() >= 2);
    const int middle = keys[1].toMap().value(QStringLiteral("index")).toInt();

    SECTION("another clip") {
        panel->setProperty("selectedKey", middle);
        const ClipId current = *controller->propertyTarget(StreamKind::Video);
        ClipId other;
        for (const TrackPtr& t : controller->currentSequence()->videoTracks) {
            for (const Clip& c : t->clips) {
                if (c.id != current && !other)
                    other = c.id;
            }
        }
        REQUIRE(other);
        controller->selectClip(clipKey(other));
        spin(30);
        CHECK(panel->property("selectedKey").toInt() == -1);
    }
    SECTION("Delete removes the selected key and clears the selection") {
        panel->setProperty("selectedKey", middle);
        QQuickItem* graph = findNamed(panel, QStringLiteral("timeRemapGraph"));
        REQUIRE(graph);
        graph->forceActiveFocus();
        const auto before = bridge->keys().size();
        tap(shell.window.get(), Qt::Key_Delete);
        spin(30);
        CHECK(bridge->keys().size() == before - 1);
        CHECK(panel->property("selectedKey").toInt() == -1);
    }
    takeMessages();
}

TEST_CASE("Review P2-46: a media change that the lanes do not draw rebuilds nothing", "[app][review][timeline]") {
    Rig rig("V1 |Aaaaaaaaaa|");
    rig.harness.frame();
    int rows = 0;
    QObject::connect(rig.controller.clipList(), &QAbstractItemModel::dataChanged, [&] { ++rows; });
    const MediaId a = rig.tl.media('A');

    // Source Monitor marks live on the media item: nothing on the lanes changes.
    REQUIRE(rig.controller.applyEditFn("Marks", [&](const ProjectState& s) {
        return edit::setSourceMarks(s, a, {f(2), f(6)});
    }));
    rig.harness.frame();
    CHECK_FALSE(rig.view->stats().fullRebuild);
    CHECK(rows == 0);

    // Provenance is drawn (the AI badge) and spoken: that change still rebuilds.
    ProjectState generated = *rig.controller.snapshot();
    for (MediaPtr& m : generated.media) {
        if (m->id == a) {
            MediaItem item = *m;
            item.generated = GeneratedProvenance{"fal", "model", std::string(64, 'a'), "2026-10-11T09:30:00Z", "video",
                                                 "text-to-video", {}};
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(rig.controller.session().commit("Provenance", generated));
    rig.harness.frame();
    CHECK(rig.view->stats().fullRebuild);
    CHECK(rows >= 1);
}

TEST_CASE("Review P2-47: the interface size shows what was applied, not a raw stored value", "[app][review][qml]") {
    resetSettings();
    QSettings().setValue(QStringLiteral("appearance/uiScale"), 110); // not a size Wabcut offers: applied as 100
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    CHECK(shell.theme->property("uiScale").toInt() == 100);
    // 0.1.3: the sizes are in Settings > Appearance.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, QStringLiteral("appearance")),
                                      Q_ARG(QString, QString())));
    QObject* settings = shell.object("settingsWindowLoader")->property("item").value<QObject*>();
    REQUIRE(settings);
    spin(50);
    QQuickItem* tabs = findNamed(settings->property("contentItem").value<QQuickItem*>(), QStringLiteral("uiScaleTabs"));
    REQUIRE(tabs);
    CHECK(tabs->property("currentIndex").toInt() == 0);
    resetSettings();
    takeMessages();
}

TEST_CASE("Review P2-48: the provenance sidecar survives invalid UTF-8", "[app][review]") {
    ai::Json provenance{{"provider", std::string("fal\xff\xfe")}, {"model", "m"}};
    CHECK_THROWS(provenance.dump(2)); // what the export controller did, in a slot
    std::string text;
    CHECK_NOTHROW(text = ExportController::provenanceSidecarText(provenance));
    CHECK(text.find("\"model\"") != std::string::npos);
}

// =====================================================================================================================
// P2-24 (app parts): remapped clips read as media time

namespace {

/// A remap curve key: curve time `curve` shows media frame `frame`.
Keyframe remapKey(Ticks curve, std::int64_t frame) {
    Keyframe k;
    k.time = curve;
    k.value = static_cast<double>(f(frame).value);
    return k;
}

/// Remaps `clip` (curve times from 0) with `keys`: its sourceIn/sourceOut stay curve times while it shows other media.
ProjectState remapped(const Timeline& tl, ClipId clip, std::vector<Keyframe> keys) {
    Curve curve;
    curve.keys = std::move(keys);
    auto out = edit::setTimeRemap(tl.state, tl.seq, clip, std::move(curve), f(0));
    REQUIRE(out);
    return std::move(out->state);
}

/// Curve frames 0-10 show media frames 100-110 (100 % forward, offset).
std::vector<Keyframe> hundredOn() { return {remapKey(f(0), 100), remapKey(f(10), 110)}; }

const Clip& clipIn(const ProjectState& state, SequenceId seq, ClipId id) {
    const Sequence& sequence = *state.findSequence(seq);
    for (const TrackPtr& t : sequence.videoTracks) {
        if (const Clip* c = t->findClip(id))
            return *c;
    }
    FAIL("no clip " << id.value);
    static const Clip none;
    return none;
}

} // namespace

TEST_CASE("Review P2-24: a remapped clip shows its media's markers where it shows their frames",
          "[app][review][timeline][remap]") {
    Timeline tl("V1 tgt |Aaaaaaaaaa|");
    const ClipId id = clipAt(tl.state, tl.seq, "V1@0");
    const ProjectState state = remapped(tl, id, hundredOn());
    const Clip& clip = clipIn(state, tl.seq, id);
    REQUIRE(clip.shownSourceRange(clip.start(), clip.end()) == TimeRange{f(100), f(10)});

    std::vector<Marker> markers(3); // media time, sorted
    markers[0].start = f(5);        // inside the clip's curve range, but never shown
    markers[0].colorIndex = 5;
    markers[1].start = f(102);      // shown over timeline frames 2-6
    markers[1].duration = f(4);
    markers[1].colorIndex = 2;
    markers[2].start = f(105);      // shown at timeline frame 5
    markers[2].colorIndex = 3;
    const auto marks = clipMarkerMarks(clip, markers);
    REQUIRE(marks.size() == 2);
    CHECK(marks[0].colorIndex == 2);
    CHECK(marks[0].at == f(2));
    CHECK(marks[0].ranged);
    REQUIRE(marks[0].end);
    CHECK(*marks[0].end == f(6));
    CHECK(marks[1].colorIndex == 3);
    CHECK(marks[1].at == f(5));
    CHECK_FALSE(marks[1].ranged);

    // A plain clip of the same frames shows them at the same places.
    Timeline plain("V1 tgt |A100aaaaaa|");
    const auto plainMarks = clipMarkerMarks(clipIn(plain.state, plain.seq, clipAt(plain.state, plain.seq, "V1@0")),
                                            markers);
    REQUIRE(plainMarks.size() == 2);
    CHECK(plainMarks[0].at == f(2));
    CHECK(plainMarks[1].at == f(5));
}

TEST_CASE("Review P2-24: duplicate-frame bands read the media a remapped clip shows", "[app][review][timeline][remap]") {
    // V1: A remapped to show media frames 100-110; V2: A plainly from frame 100. They share every frame.
    Timeline tl(R"(
        V2 |..........A100aaaaaa|
        V1 |Aaaaaaaaaa..........|
    )");
    const ClipId remap = clipAt(tl.state, tl.seq, "V1@0");
    const ClipId plain = clipAt(tl.state, tl.seq, "V2@10");
    {
        const ProjectState state = remapped(tl, remap, hundredOn());
        const auto dup = duplicateFrameRanges(*state.findSequence(tl.seq));
        REQUIRE(dup.contains(remap.value));
        REQUIRE(dup.contains(plain.value));
        REQUIRE(dup.at(remap.value).size() == 1);
        CHECK(dup.at(remap.value)[0].range == TimeRange{f(0), f(10)});
        REQUIRE(dup.at(plain.value).size() == 1);
        CHECK(dup.at(plain.value)[0].range == TimeRange{f(10), f(10)});
        CHECK(dup.at(remap.value)[0].colorIndex == dup.at(plain.value)[0].colorIndex);
    }
    {
        // A hold: frames 100-104, then 105 held for five frames. The plain clip shares 100-105 (its first six frames);
        // every frame of the remapped clip shows one of them.
        const ProjectState state = remapped(tl, remap, {remapKey(f(0), 100), remapKey(f(5), 105), remapKey(f(10), 105)});
        const auto dup = duplicateFrameRanges(*state.findSequence(tl.seq));
        REQUIRE(dup.contains(remap.value));
        REQUIRE(dup.contains(plain.value));
        REQUIRE(dup.at(remap.value).size() == 1);
        CHECK(dup.at(remap.value)[0].range == TimeRange{f(0), f(10)});
        REQUIRE(dup.at(plain.value).size() == 1);
        CHECK(dup.at(plain.value)[0].range == TimeRange{f(10), f(6)});
    }
}

TEST_CASE("Review P2-24: copy and paste keep a clip's time remap", "[app][review][remap]") {
    Timeline tl("V1 tgt |Aaaaaaaaaa....................|");
    const ClipId id = clipAt(tl.state, tl.seq, "V1@0");
    ProjectController controller;
    controller.resetProject(remapped(tl, id, hundredOn()), tl.seq);
    controller.selectClipById(id, ProjectController::Replace);
    controller.copySelection();
    controller.setPlayhead(f(20));
    controller.paste();

    const SequencePtr seq = controller.currentSequence();
    const Track& v1 = *seq->videoTracks.front();
    REQUIRE(v1.clips.size() == 2);
    const Clip& original = v1.clips[0];
    const Clip& pasted = v1.clips[1];
    CHECK(pasted.start() == f(20));
    CHECK(pasted.duration() == f(10));
    REQUIRE(pasted.timeRemap());
    CHECK(pasted.timeRemap()->keys == original.timeRemap()->keys);
    CHECK(pasted.sourceIn == original.sourceIn);
    CHECK(pasted.shownSourceRange(pasted.start(), pasted.end()) == TimeRange{f(100), f(10)});
    // One undo step takes the paste back.
    controller.undo();
    CHECK(controller.currentSequence()->videoTracks.front()->clips.size() == 1);
}
