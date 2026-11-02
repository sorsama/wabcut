// 0.6 (P1 round, stream 3B): the monitors.
//   * #16 Paused Resolution: MediaReadiness.pausedQuality (Full by default) is the resolution while neither monitor
//     plays; previewDivisor follows the transport (the Program's and the Source Monitor's), the pill and badge keep the
//     playback resolution; the Source Monitor's own decoder follows preview changes; the four keymap commands, the
//     quality pill's and the wrench's Paused Resolution submenus and View > Paused Resolution.
//   * #17 the Program Monitor's edit overlay: a Library clip or the Source Monitor's picture dragged over the monitor
//     shows five zones; a drop on Insert, Overwrite, Overlay (a free track above, else a new one), Replace or Append at
//     End edits it in at the playhead, one undo step named after the zone. The drags here are Qt drag events sent to
//     the window, the path a Library card's drag takes into QML; the real OLE drag is left for a real-mouse check.
//   * #18 captions in the Program Monitor: a click picks the cue (Captions selects it, no clip stays selected), a
//     double-click asks the Captions panel to open its text, a drag moves the text block (placement Custom, kept in
//     the safe area) in one undo step.
// Screenshots (dark and light): the pill menu's Paused Resolution submenu, the edit overlay, a picked caption.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandJob.h"
#include "CommandPalette.h"
#include "GraphicCanvasBridge.h"
#include "MediaReadinessBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"

#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/playback/video/Scheduler.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <tuple>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;
using Catch::Approx;

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

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

/// The real shell (Main.qml, hidden, 1600 x 960, reduced motion) over a project from a timeline picture.
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
    std::size_t undo() const { return controller->session().undoCount(); }
    std::string undoLabel() const { return controller->session().undoLabel().value_or(std::string()); }
    const Sequence& sequence() const { return *controller->currentSequence(); }
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
    void runCommand(const char* id) const {
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
    }
};

/// The media of the clip covering `frame` on video track `index` (V1 = 0); invalid when nothing is there.
MediaId mediaOn(const Sequence& seq, std::size_t index, int frame) {
    if (index >= seq.videoTracks.size())
        return {};
    const Track& t = *seq.videoTracks[index];
    const auto at = t.clipIndexAt(f(frame));
    return at ? t.clips[*at].media : MediaId{};
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- #17: Qt drag events into the window (as a Library card's or the Source Monitor's drag arrives in QML) ----

struct DragPayload {
    QString format;
    QString data;
};

void dragOver(QQuickWindow* window, const DragPayload& payload, QPointF at, QMimeData& mime) {
    mime.setData(payload.format, payload.data.toUtf8());
    QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &enter);
    QDragMoveEvent move(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &move);
}

void dragMove(QQuickWindow* window, QPointF at, QMimeData& mime) {
    QDragMoveEvent move(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &move);
}

void dropAt(QQuickWindow* window, QPointF at, QMimeData& mime) {
    QDragMoveEvent move(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &move);
    QDropEvent drop(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(window, &drop);
    spin(30);
}

/// The middle of an edit overlay zone, in scene coordinates.
QPointF zonePoint(QQuickItem* overlay, const char* zone) {
    const std::map<std::string, QPointF> at{{"overlay", {0.5, 0.08}},
                                            {"insert", {0.08, 0.5}},
                                            {"overwrite", {0.5, 0.5}},
                                            {"append", {0.92, 0.5}},
                                            {"replace", {0.5, 0.92}}};
    const QPointF fraction = at.at(zone);
    return overlay->mapToScene(QPointF(overlay->width() * fraction.x(), overlay->height() * fraction.y()));
}

// ---- #18: mouse events on the Program Monitor's picture ----

void mouse(QQuickWindow* window, QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
    QMouseEvent event(type, at, at, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QCoreApplication::sendEvent(window, &event);
}

/// Two media cues on a caption track named English (the shown one).
CaptionTrackId addCaptions(ProjectController& c) {
    const auto made = CommandJob::runNow(&c, nullptr, "captions.addTrack", Json{{"name", "English"}, {"language", "en"}});
    INFO(commandErrorText(made).toStdString());
    REQUIRE(made.ok());
    REQUIRE_FALSE(made.createdIds.empty());
    const CaptionTrackId id{made.createdIds.front()};
    for (const auto& [from, to, text] : {std::tuple{0, 50, "Clutch round incoming"}, std::tuple{50, 100, "Second cue"}}) {
        const auto r = CommandJob::runNow(&c, nullptr, "captions.addCue",
                                          Json{{"captionTrack", id.value}, {"start", f(from).value}, {"end", f(to).value}, {"text", text}});
        INFO(commandErrorText(r).toStdString());
        REQUIRE(r.ok());
    }
    return id;
}

} // namespace

// ---- #16 ---------------------------------------------------------------------------------------------------------------

TEST_CASE("Paused Resolution: a parked monitor draws at its own resolution while the pill keeps the playback one",
          "[app][monitor][p1][3b]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )", {{'S', 200}});
    auto* readiness = p.singleton<MediaReadinessBridge>("MediaReadiness");
    auto* program = p.singleton<PlaybackBridge>("Playback");
    auto* source = p.singleton<SourceMonitorBridge>("SourceMonitor");
    auto* palette = p.singleton<CommandPalette>("CommandPalette");

    // Premiere's default: Full while parked, whatever the playback resolution.
    CHECK(readiness->pausedQuality() == QStringLiteral("full"));
    readiness->setPreviewQuality(QStringLiteral("quarter"));
    CHECK(readiness->playbackDivisor() == 4);
    CHECK_FALSE(readiness->monitorPlaying());
    CHECK(readiness->previewDivisor() == 1);
    CHECK(program->previewSettings().decodeDivisor == 1);
    // The badge and the pill show the playback resolution (the paused one is in the tooltip).
    readiness->pollNow();
    CHECK(readiness->qualityBadge() == QStringLiteral("1/4"));
    CHECK(readiness->qualityTooltip().contains(QStringLiteral("Paused: Full")));
    QQuickItem* pill = p.item(QStringLiteral("previewQualityButton"));
    REQUIRE(pill);
    CHECK(pill->property("text").toString() == QStringLiteral("1/4"));

    // Playing: the playback resolution, in the monitor and the decoders; parked again: the paused one.
    program->play();
    REQUIRE(waitFor([&] { return program->playing() && readiness->monitorPlaying(); }, 5000));
    CHECK(readiness->previewDivisor() == 4);
    CHECK(program->previewSettings().decodeDivisor == 4);
    readiness->pollNow();
    CHECK(readiness->qualityBadge() == QStringLiteral("1/4"));
    CHECK(pill->property("text").toString() == QStringLiteral("1/4"));
    program->pause();
    REQUIRE(waitFor([&] { return !readiness->monitorPlaying(); }, 5000));
    CHECK(readiness->previewDivisor() == 1);
    CHECK(program->previewSettings().decodeDivisor == 1);

    // Auto: the pill names what Auto chose for playback, not the paused Full.
    readiness->setPreviewQuality(QStringLiteral("auto"));
    CHECK(readiness->previewDivisor() == 1);
    spin(30);
    const int chosen = readiness->playbackDivisor();
    CHECK(pill->property("text").toString()
          == QStringLiteral("Auto ") + (chosen == 1 ? QStringLiteral("Full") : QStringLiteral("1/%1").arg(chosen)));
    readiness->setPreviewQuality(QStringLiteral("half"));

    // Same as playback (the behaviour before 0.6), remembered; an unknown value changes nothing.
    readiness->setPausedQuality(QStringLiteral("same"));
    CHECK(readiness->previewDivisor() == 2);
    CHECK(QSettings().value(QStringLiteral("preview/pausedQuality")).toString() == QStringLiteral("same"));
    readiness->setPausedQuality(QStringLiteral("eighth"));
    CHECK(readiness->pausedQuality() == QStringLiteral("same"));

    // The Source Monitor plays from the same setting: its own decoder follows a change made after it loaded, and its
    // transport counts as playing.
    readiness->setPausedQuality(QStringLiteral("quarter"));
    readiness->setPreviewQuality(QStringLiteral("full"));
    REQUIRE(source->load(mediaKey(p.tl.media('S'))));
    REQUIRE(source->playback());
    CHECK(source->playback()->video().scheduler().preview().decodeDivisor == 4);
    readiness->setPausedQuality(QStringLiteral("half"));
    CHECK(source->playback()->video().scheduler().preview().decodeDivisor == 2);
    source->play();
    REQUIRE(waitFor([&] { return source->playing() && readiness->monitorPlaying(); }, 5000));
    CHECK(readiness->previewDivisor() == 1);
    CHECK(source->playback()->video().scheduler().preview().decodeDivisor == 1);
    source->pause();
    REQUIRE(waitFor([&] { return !readiness->monitorPlaying(); }, 5000));
    CHECK(source->playback()->video().scheduler().preview().decodeDivisor == 2);

    // The four commands: known, ready, no default key; each sets the paused resolution.
    for (const auto& [id, value] : {std::pair{"monitor.pausedResolutionFull", "full"}, std::pair{"monitor.pausedResolutionHalf", "half"},
                                    std::pair{"monitor.pausedResolutionQuarter", "quarter"},
                                    std::pair{"monitor.pausedResolutionSame", "same"}}) {
        INFO(id);
        const QVariantMap state = palette->commandState(QString::fromLatin1(id));
        CHECK(state.value(QStringLiteral("known")).toBool());
        CHECK(state.value(QStringLiteral("enabled")).toBool());
        p.runCommand(id);
        CHECK(readiness->pausedQuality() == QLatin1String(value));
    }

    // The quality pill's menu: a Paused Resolution submenu whose checks follow the setting.
    readiness->setPausedQuality(QStringLiteral("full"));
    QObject* qualityMenu = p.object(QStringLiteral("previewQualityMenu"));
    QObject* pausedMenu = p.object(QStringLiteral("quality_pausedResolutionMenu"));
    REQUIRE(qualityMenu);
    REQUIRE(pausedMenu);
    CHECK(pausedMenu->property("title").toString() == QStringLiteral("Paused Resolution"));
    QObject* half = pausedMenu->findChild<QObject*>(QStringLiteral("quality_pausedResolution_half"));
    QObject* full = pausedMenu->findChild<QObject*>(QStringLiteral("quality_pausedResolution_full"));
    REQUIRE(half);
    REQUIRE(full);
    CHECK(full->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(half, "triggered"));
    CHECK(readiness->pausedQuality() == QStringLiteral("half"));
    CHECK(half->property("checked").toBool());
    CHECK_FALSE(full->property("checked").toBool());
    // The wrench has the same submenu.
    QObject* wrenchRow = p.object(QStringLiteral("program_pausedResolution_quarter"));
    REQUIRE(wrenchRow);
    REQUIRE(QMetaObject::invokeMethod(wrenchRow, "triggered"));
    CHECK(readiness->pausedQuality() == QStringLiteral("quarter"));

    // View > Paused Resolution: four rows running the commands.
    QObject* bar = p.object(QStringLiteral("appMenuBar"));
    REQUIRE(bar);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 6), Q_ARG(bool, false)));
    QObject* view = p.object(QStringLiteral("viewMenu"));
    REQUIRE(view);
    REQUIRE(waitFor([&] { return opened(view); }));
    QObject* viewPaused = view->findChild<QObject*>(QStringLiteral("viewPausedResolutionMenu"));
    REQUIRE(viewPaused);
    CHECK(viewPaused->property("title").toString() == QStringLiteral("Paused Resolution"));
    QObject* viewQuarter = view->findChild<QObject*>(QStringLiteral("menuCommand_monitor.pausedResolutionQuarter"));
    QObject* viewFull = view->findChild<QObject*>(QStringLiteral("menuCommand_monitor.pausedResolutionFull"));
    REQUIRE(viewQuarter);
    REQUIRE(viewFull);
    CHECK(viewQuarter->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(viewPaused, "open"));
    REQUIRE(waitFor([&] { return opened(viewPaused); }));
    REQUIRE(QMetaObject::invokeMethod(viewFull, "clicked"));
    REQUIRE(waitFor([&] { return !shown(view); }));
    spin(30);
    CHECK(readiness->pausedQuality() == QStringLiteral("full"));
    CHECK(viewFull->property("checked").toBool());

    // Screenshots: the pill menu with its Paused Resolution submenu open, dark and light.
    readiness->setPreviewQuality(QStringLiteral("half"));
    for (const bool dark : {true, false}) {
        p.shell.setAppearance(dark, "ultraviolet");
        spin(50);
        REQUIRE(QMetaObject::invokeMethod(pill, "clicked"));
        REQUIRE(waitFor([&] { return opened(qualityMenu); }));
        REQUIRE(QMetaObject::invokeMethod(pausedMenu, "open"));
        REQUIRE(waitFor([&] { return opened(pausedMenu); }));
        spin(200);
        auto* menuItem = qobject_cast<QQuickItem*>(qualityMenu->property("contentItem").value<QObject*>());
        auto* subItem = qobject_cast<QQuickItem*>(pausedMenu->property("contentItem").value<QObject*>());
        REQUIRE(menuItem);
        REQUIRE(subItem);
        p.shot(sceneRect(menuItem->parentItem()).united(sceneRect(subItem->parentItem())).united(sceneRect(pill)).adjusted(-8, -8, 8, 8),
               dark ? QStringLiteral("p1-3b-paused-resolution-dark.png") : QStringLiteral("p1-3b-paused-resolution-light.png"));
        REQUIRE(QMetaObject::invokeMethod(pausedMenu, "close"));
        REQUIRE(QMetaObject::invokeMethod(qualityMenu, "close"));
        REQUIRE(waitFor([&] { return !shown(pausedMenu) && !shown(qualityMenu); }));
    }
    checkNoMessages();
    resetSettings();
}

// ---- #17 ---------------------------------------------------------------------------------------------------------------

TEST_CASE("Program Monitor edit overlay: Library and Source Monitor drops Insert Overwrite Overlay Replace or Append",
          "[app][monitor][drop][p1][3b]") {
    resetSettings();
    clearMessages();
    // A (0-10), a gap, B (20-30) on V1, an empty V2; S is 50 frames.
    ShellProject p(R"(
        V2     |..............................|
        V1 tgt |Aaaaaaaaaa..........Bbbbbbbbbb|
        A1 tgt |Aaaaaaaaaa..........Bbbbbbbbbb|
    )", {{'S', 50}});
    QQuickWindow* window = p.shell.window.get();
    ProjectController& c = *p.controller;
    auto* source = p.singleton<SourceMonitorBridge>("SourceMonitor");
    QQuickItem* overlay = p.item(QStringLiteral("programEditOverlay"));
    QQuickItem* well = p.item(QStringLiteral("programMonitorWell"));
    REQUIRE(overlay);
    REQUIRE(well);
    p.settle();
    REQUIRE(overlay->width() > 100);
    CHECK_FALSE(overlay->isVisible());
    const MediaId S = p.tl.media('S');
    const MediaId A = p.tl.media('A');
    const MediaId B = p.tl.media('B');
    const std::string start = wabcut::test::render(*c.snapshot(), p.tl.seq);
    const DragPayload library{QStringLiteral("application/x-wabcut-media"), mediaKey(S)};

    // One drop on a zone at frame 5: one undo step named after the zone; undo puts the timeline back.
    const auto dropOn = [&](const char* zone, const DragPayload& payload, const char* label) {
        INFO(zone);
        const std::size_t before = p.undo();
        QMimeData mime;
        dragOver(window, payload, zonePoint(overlay, "overwrite"), mime);
        REQUIRE(waitFor([&] { return overlay->isVisible(); }, 2000));
        dropAt(window, zonePoint(overlay, zone), mime);
        CHECK_FALSE(overlay->isVisible());
        CHECK(p.undo() == before + 1);
        CHECK(p.undoLabel() == label);
    };
    const auto undoAll = [&] {
        c.undo();
        spin(30);
        CHECK(wabcut::test::render(*c.snapshot(), p.tl.seq) == start);
    };

    // While the drag is over the monitor: the five zones, the one under the pointer lit (screenshots, dark and light).
    c.setPlayhead(f(5));
    for (const bool dark : {true, false}) {
        p.shell.setAppearance(dark, "ultraviolet");
        spin(50);
        QMimeData mime;
        dragOver(window, library, zonePoint(overlay, "insert"), mime);
        REQUIRE(waitFor([&] { return overlay->isVisible(); }, 2000));
        CHECK(overlay->property("zone").toString() == QStringLiteral("insert"));
        dragMove(window, zonePoint(overlay, "overlay"), mime);
        CHECK(overlay->property("zone").toString() == QStringLiteral("overlay"));
        dragMove(window, zonePoint(overlay, "overwrite"), mime);
        CHECK(overlay->property("zone").toString() == QStringLiteral("overwrite"));
        CHECK(overlay->property("replaceAvailable").toBool());
        p.settle();
        p.shot(sceneRect(well).adjusted(-8, -8, 8, 8),
               dark ? QStringLiteral("p1-3b-edit-overlay-dark.png") : QStringLiteral("p1-3b-edit-overlay-light.png"));
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(window, &leave);
        spin(30);
        CHECK_FALSE(overlay->isVisible());
        CHECK(p.undo() == 0);
    }
    p.shell.setAppearance(true, "ultraviolet");

    // Overwrite: S from frame 5 over the rest of A and over B.
    dropOn("overwrite", library, "Overwrite");
    CHECK(mediaOn(p.sequence(), 0, 4) == A);
    CHECK(mediaOn(p.sequence(), 0, 5) == S);
    CHECK(mediaOn(p.sequence(), 0, 54) == S);
    CHECK(mediaOn(p.sequence(), 1, 5) == MediaId{});
    CHECK(p.sequence().videoTracks.size() == 2);
    undoAll();

    // Insert: S at frame 5; A's tail and B move later by its length.
    dropOn("insert", library, "Insert");
    CHECK(mediaOn(p.sequence(), 0, 5) == S);
    CHECK(mediaOn(p.sequence(), 0, 55) == A);
    CHECK(mediaOn(p.sequence(), 0, 70) == B);
    undoAll();

    // Append at End: after the last clip (B ends at 30), whatever the playhead.
    dropOn("append", library, "Append at End");
    CHECK(mediaOn(p.sequence(), 0, 5) == A);
    CHECK(mediaOn(p.sequence(), 0, 30) == S);
    CHECK(mediaOn(p.sequence(), 0, 79) == S);
    undoAll();

    // Overlay: the target V1 is taken at the playhead, V2 above it is free there: S goes on V2, V1 untouched.
    dropOn("overlay", library, "Overlay");
    REQUIRE(p.sequence().videoTracks.size() == 2);
    CHECK(mediaOn(p.sequence(), 0, 5) == A);
    CHECK(mediaOn(p.sequence(), 1, 5) == S);
    CHECK(mediaOn(p.sequence(), 0, 20) == B);
    // Again: V1 and V2 are taken, so a new video track on top takes it - in the same undo step.
    dropOn("overlay", library, "Overlay");
    REQUIRE(p.sequence().videoTracks.size() == 3);
    CHECK(mediaOn(p.sequence(), 2, 5) == S);
    CHECK(mediaOn(p.sequence(), 1, 5) == S);
    c.undo();
    spin(30);
    CHECK(p.sequence().videoTracks.size() == 2);
    undoAll();
    // Where the target itself is free (after B), Overlay uses it.
    c.setPlayhead(f(40));
    dropOn("overlay", library, "Overlay");
    CHECK(mediaOn(p.sequence(), 0, 40) == S);
    CHECK(mediaOn(p.sequence(), 1, 40) == MediaId{});
    undoAll();

    // Replace: the clip under the playhead on V1 (A) becomes S, keeping its place and length.
    c.setPlayhead(f(5));
    dropOn("replace", library, "Replace");
    CHECK(mediaOn(p.sequence(), 0, 0) == S);
    CHECK(mediaOn(p.sequence(), 0, 9) == S);
    CHECK(mediaOn(p.sequence(), 0, 10) == MediaId{});
    CHECK(mediaOn(p.sequence(), 0, 20) == B);
    undoAll();

    // Nothing under the playhead to replace: the Replace zone is dimmed and takes no drop.
    c.setPlayhead(f(15));
    {
        QMimeData mime;
        dragOver(window, library, zonePoint(overlay, "replace"), mime);
        REQUIRE(waitFor([&] { return overlay->isVisible(); }, 2000));
        CHECK_FALSE(overlay->property("replaceAvailable").toBool());
        CHECK(overlay->property("zone").toString().isEmpty());
        dropAt(window, zonePoint(overlay, "replace"), mime);
        CHECK(p.undo() == 0);
    }

    // The Source Monitor's picture: the same zones (Overwrite at the playhead here).
    c.setPlayhead(f(5));
    REQUIRE(source->load(mediaKey(S)));
    dropOn("overwrite", DragPayload{QStringLiteral("application/x-wabcut-source"), QStringLiteral("av")}, "Overwrite");
    CHECK(mediaOn(p.sequence(), 0, 5) == S);
    CHECK(mediaOn(p.sequence(), 0, 4) == A);
    undoAll();
    // ... and Insert of its picture only (a video handle's drag).
    dropOn("insert", DragPayload{QStringLiteral("application/x-wabcut-source"), QStringLiteral("video")}, "Insert");
    CHECK(mediaOn(p.sequence(), 0, 5) == S);
    CHECK(mediaOn(p.sequence(), 0, 55) == A);
    undoAll();

    // A bin (not a clip) is not taken: no overlay.
    {
        QMimeData mime;
        dragOver(window, DragPayload{QStringLiteral("application/x-wabcut-media"), QStringLiteral("b3")}, zonePoint(overlay, "overwrite"), mime);
        spin(50);
        CHECK_FALSE(overlay->isVisible());
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(window, &leave);
    }
    checkNoMessages();
}

// ---- #18 ---------------------------------------------------------------------------------------------------------------

TEST_CASE("Program Monitor captions: a click picks the cue a double-click opens its text and a drag moves it",
          "[app][monitor][captions][p1][3b]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    QQuickWindow* window = p.shell.window.get();
    ProjectController& c = *p.controller;
    auto* captions = p.singleton<CaptionsBridge>("Captions");
    auto* canvas = p.singleton<GraphicCanvasBridge>("GraphicCanvas");
    auto* program = p.singleton<PlaybackBridge>("Playback");
    canvas->setSynchronous(true);
    const CaptionTrackId track = addCaptions(c);
    captions->setTrack(captionTrackKey(track));
    captions->setOverlay(true);
    REQUIRE(waitFor([&] { return program->captionsShown(); }, 3000));
    c.setPlayhead(f(10));
    spin(50);
    QQuickItem* area = p.item(QStringLiteral("programPickArea"));
    QQuickItem* frame = p.item(QStringLiteral("programFrameArea"));
    REQUIRE(area);
    REQUIRE(frame);
    p.settle();
    REQUIRE(area->width() > 100);

    // The caption is an object of the picture, on top: its box, its track and the cue under the playhead.
    const auto findCaption = [&] {
        QVariantMap found;
        for (double y = 0.97; y > 0.03 && found.isEmpty(); y -= 0.01) {
            for (const QVariant& o : canvas->stackAt(0.5, y)) {
                if (o.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("caption")) {
                    found = o.toMap();
                    break;
                }
            }
        }
        return found;
    };
    QVariantMap caption = findCaption();
    REQUIRE_FALSE(caption.isEmpty());
    CHECK(caption.value(QStringLiteral("captionTrack")).toString() == captionTrackKey(track));
    CHECK(caption.value(QStringLiteral("cue")).toInt() == 0);
    CHECK(caption.value(QStringLiteral("text")).toBool());
    CHECK(caption.value(QStringLiteral("name")).toString() == QStringLiteral("Clutch round incoming"));
    double x0 = 1, x1 = 0, y0 = 1, y1 = 0;
    QPointF center;
    const auto measure = [&](const QVariantMap& o) {
        const QVariantList quad = o.value(QStringLiteral("quad")).toList();
        REQUIRE(quad.size() == 8);
        x0 = 1, x1 = 0, y0 = 1, y1 = 0;
        for (int i = 0; i < 8; i += 2) {
            x0 = std::min(x0, quad[i].toDouble());
            x1 = std::max(x1, quad[i].toDouble());
            y0 = std::min(y0, quad[i + 1].toDouble());
            y1 = std::max(y1, quad[i + 1].toDouble());
        }
        center = area->mapToScene(QPointF(area->width() * (x0 + x1) / 2, area->height() * (y0 + y1) / 2));
    };
    measure(caption);
    CHECK(y0 > 0.5); // a bottom caption

    // A click picks the cue: Captions selects it, no clip stays selected, the block is outlined.
    c.selectClipById(p.tl.clip("V1@0"), ProjectController::Replace);
    REQUIRE_FALSE(c.selection().empty());
    mouse(window, QEvent::MouseButtonPress, center, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, center, Qt::NoButton);
    CHECK(captions->selectedCue() == 0);
    CHECK(c.selection().empty());
    CHECK(canvas->pickKey().startsWith(QStringLiteral("caption:")));
    CHECK(canvas->pickOutline().size() == 8);
    for (const bool dark : {true, false}) {
        p.shell.setAppearance(dark, "ultraviolet");
        spin(80);
        p.settle();
        p.shot(sceneRect(frame).adjusted(-8, -8, 8, 8),
               dark ? QStringLiteral("p1-3b-caption-pick-dark.png") : QStringLiteral("p1-3b-caption-pick-light.png"));
    }
    p.shell.setAppearance(true, "ultraviolet");
    // A clip picked elsewhere ends the caption pick.
    c.selectClipById(p.tl.clip("V1@0"), ProjectController::Replace);
    spin(30);
    CHECK_FALSE(canvas->pickKey().startsWith(QStringLiteral("caption:")));

    // A double-click asks the Captions panel to open the cue's text.
    int edited = -1;
    QObject::connect(captions, &CaptionsBridge::editCueRequested, captions, [&](int index) { edited = index; });
    spin(500); // not the second click of the first one
    mouse(window, QEvent::MouseButtonPress, center, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, center, Qt::NoButton);
    mouse(window, QEvent::MouseButtonPress, center, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonDblClick, center, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, center, Qt::NoButton);
    CHECK(edited == 0);
    spin(500);

    // A drag moves the text block: placement Custom at the new centre, one undo step; it stays in the safe area.
    p.settle(); // the Captions panel may have come up beside the monitor: measure again
    caption = findCaption();
    REQUIRE_FALSE(caption.isEmpty());
    measure(caption);
    const std::size_t before = p.undo();
    REQUIRE(captions->style().value(QStringLiteral("placement")).toString() == QStringLiteral("bottom"));
    mouse(window, QEvent::MouseButtonPress, center, Qt::LeftButton);
    const QPointF up(-area->width() * 0.2, -area->height() * 0.4);
    for (int step = 1; step <= 6; ++step)
        mouse(window, QEvent::MouseMove, center + up * (step / 6.0), Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, center + up, Qt::NoButton);
    spin(30);
    const QVariantMap style = captions->style();
    CHECK(style.value(QStringLiteral("placement")).toString() == QStringLiteral("custom"));
    CHECK(style.value(QStringLiteral("positionX")).toDouble() == Approx(0.5 - 0.2).margin(0.02));
    CHECK(style.value(QStringLiteral("positionY")).toDouble() == Approx((y0 + y1) / 2 - 0.4).margin(0.02));
    CHECK(p.undo() == before + 1);
    CHECK(p.undoLabel() == "Move Caption");
    // Dragged far past the frame's corner: kept inside the safe area.
    mouse(window, QEvent::MouseButtonPress, center + up, Qt::LeftButton);
    mouse(window, QEvent::MouseMove, center + up + QPointF(-area->width(), -area->height()), Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, center + up + QPointF(-area->width(), -area->height()), Qt::NoButton);
    spin(30);
    const double safe = captions->style().value(QStringLiteral("safeArea")).toDouble();
    CHECK(captions->style().value(QStringLiteral("positionX")).toDouble() >= safe + (x1 - x0) / 2 - 0.01);
    CHECK(captions->style().value(QStringLiteral("positionY")).toDouble() >= safe + (y1 - y0) / 2 - 0.01);
    CHECK(p.undo() == before + 2);
    c.undo();
    c.undo();
    spin(30);
    CHECK(captions->style().value(QStringLiteral("placement")).toString() == QStringLiteral("bottom"));
    CHECK(p.undo() == before);
    checkNoMessages();
}
