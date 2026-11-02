#include "MonitorBridge.h"
#include "BridgeIds.h"

#include "CommandJob.h"
#include "GraphicCanvasBridge.h"
#include "MediaReadinessBridge.h"
#include "MotionBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"

#include <wabcut/command/Engine.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/export/StillExport.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

namespace wabcut::app {

namespace {

constexpr const char* kTransmitKey = "monitor/transmit";
constexpr const char* kTransmitScreenKey = "monitor/transmitScreen";
constexpr const char* kTransmitBackgroundKey = "monitor/disableTransmitInBackground";
constexpr const char* kRulersKey = "monitor/showRulers";
constexpr const char* kRulerPercentKey = "monitor/rulerPercent";
constexpr const char* kShowGuidesKey = "monitor/showGuides";
constexpr const char* kLockGuidesKey = "monitor/lockGuides";
constexpr const char* kSnapKey = "monitor/snap";
constexpr const char* kProgramGridKey = "monitor/programTransparencyGrid";
constexpr const char* kSourceGridKey = "monitor/sourceTransparencyGrid";
constexpr const char* kDroppedIndicatorKey = "monitor/droppedFrameIndicator";
constexpr const char* kOverlaysKey = "monitor/overlays";
constexpr const char* kAudioUnitsKey = "monitor/audioTimeUnits";
constexpr const char* kGuidesKey = "monitor/guides";
constexpr const char* kTemplatesKey = "monitor/guideTemplates";

constexpr const char* kExportFolderKey = "monitor/exportFrameFolder";
constexpr const char* kExportFormatKey = "monitor/exportFrameFormat";
constexpr const char* kExportImportKey = "monitor/exportFrameImport";
// P1 (#23): the size, crop, JPEG quality and 2 MB cap of Export Frame; the markers Export Frames at Markers takes.
constexpr const char* kExportSizeKey = "monitor/exportFrameSize";
constexpr const char* kExportWidthKey = "monitor/exportFrameWidth";
constexpr const char* kExportHeightKey = "monitor/exportFrameHeight";
constexpr const char* kExportFillKey = "monitor/exportFrameFill";
constexpr const char* kExportQualityKey = "monitor/exportFrameQuality";
constexpr const char* kExportKeepUnderKey = "monitor/exportFrameKeepUnder";
constexpr const char* kExportMarkersKey = "monitor/exportFramesMarkers";
/// YouTube's thumbnail limit ("2 MB"), in bytes.
constexpr std::int64_t kThumbnailBytes = 2'000'000;

/// The Export Frame sizes: Sequence size, YouTube thumbnail, Vertical, Custom.
struct FrameSize {
    const char* value;
    const char* label;
    int width;
    int height;
};
constexpr std::array<FrameSize, 4> kFrameSizes{{
    {"sequence", QT_TRANSLATE_NOOP("wabcut::app::MonitorBridge", "Sequence size"), 0, 0},
    {"thumbnail", QT_TRANSLATE_NOOP("wabcut::app::MonitorBridge", "YouTube thumbnail 1280×720"), 1280, 720},
    {"vertical", QT_TRANSLATE_NOOP("wabcut::app::MonitorBridge", "Vertical 1080×1920"), 1080, 1920},
    {"custom", QT_TRANSLATE_NOOP("wabcut::app::MonitorBridge", "Custom"), 0, 0},
}};

/// A folder from a URL or a path ("" for neither).
QString folderOf(const QVariant& value) {
    if (value.typeId() == QMetaType::QUrl)
        return value.toUrl().isLocalFile() ? value.toUrl().toLocalFile() : value.toUrl().toString();
    const QString text = value.toString();
    if (text.startsWith(QLatin1String("file:")))
        return QUrl(text).toLocalFile();
    return text;
}

/// The still options of Export Frame's `options` as command params (width, height, fit, quality, maxBytes); "" or why
/// not (a custom size out of bounds).
QString addStillOptions(const QVariantMap& options, bool jpeg, command::Json& params) {
    const QString size = options.value(QStringLiteral("size"), QStringLiteral("sequence")).toString();
    int width = 0;
    int height = 0;
    for (const FrameSize& f : kFrameSizes) {
        if (size == QLatin1String(f.value)) {
            width = f.width;
            height = f.height;
        }
    }
    if (size == QLatin1String("custom")) {
        width = options.value(QStringLiteral("width")).toInt();
        height = options.value(QStringLiteral("height")).toInt();
        if (width < 16 || height < 16 || width > 16384 || height > 16384)
            return QCoreApplication::translate("wabcut::app::MonitorBridge", "Give a custom size of 16 to 16384 pixels a side");
    }
    if (width > 0 && height > 0) {
        params["width"] = width;
        params["height"] = height;
        params["fit"] = options.value(QStringLiteral("fill"), true).toBool() ? "fill" : "fit";
    }
    if (jpeg) {
        const int quality = options.value(QStringLiteral("quality"), 100).toInt();
        if (quality >= 1 && quality < 100)
            params["quality"] = quality;
        if (options.value(QStringLiteral("keepUnder"), false).toBool())
            params["maxBytes"] = kThumbnailBytes;
    }
    return {};
}

/// Remembers Export Frame's size choices for next time.
void rememberStillOptions(const QVariantMap& options) {
    QSettings settings;
    const QString size = options.value(QStringLiteral("size"), QStringLiteral("sequence")).toString();
    settings.setValue(QLatin1String(kExportSizeKey), size);
    if (size == QLatin1String("custom")) {
        settings.setValue(QLatin1String(kExportWidthKey), options.value(QStringLiteral("width")).toInt());
        settings.setValue(QLatin1String(kExportHeightKey), options.value(QStringLiteral("height")).toInt());
    }
    if (options.contains(QStringLiteral("fill")))
        settings.setValue(QLatin1String(kExportFillKey), options.value(QStringLiteral("fill")).toBool());
    if (options.contains(QStringLiteral("quality")))
        settings.setValue(QLatin1String(kExportQualityKey), options.value(QStringLiteral("quality")).toInt());
    if (options.contains(QStringLiteral("keepUnder")))
        settings.setValue(QLatin1String(kExportKeepUnderKey), options.value(QStringLiteral("keepUnder")).toBool());
}

QString pathOf(const QUrl& url) { return url.isLocalFile() ? url.toLocalFile() : url.toString(); }

/// A file name from `text`: characters Windows refuses become "_", timecode colons ";" (Wabcut's frame names).
QString fileNameOf(QString text) {
    text.replace(QLatin1Char(':'), QLatin1Char(';'));
    for (const QChar bad : {QLatin1Char('/'), QLatin1Char('\\'), QLatin1Char('*'), QLatin1Char('?'), QLatin1Char('"'),
                            QLatin1Char('<'), QLatin1Char('>'), QLatin1Char('|')})
        text.replace(bad, QLatin1Char('_'));
    return text.trimmed();
}

} // namespace

MonitorBridge::MonitorBridge(QObject* parent) : QObject(parent) {
    QSettings settings;
    m_transmit = settings.value(QLatin1String(kTransmitKey), false).toBool();
    m_transmitScreen = settings.value(QLatin1String(kTransmitScreenKey), -1).toInt();
    m_disableInBackground = settings.value(QLatin1String(kTransmitBackgroundKey), true).toBool();
    m_showRulers = settings.value(QLatin1String(kRulersKey), false).toBool();
    m_rulerPercent = settings.value(QLatin1String(kRulerPercentKey), false).toBool();
    m_showGuides = settings.value(QLatin1String(kShowGuidesKey), true).toBool();
    m_lockGuides = settings.value(QLatin1String(kLockGuidesKey), false).toBool();
    m_snap = settings.value(QLatin1String(kSnapKey), true).toBool();
    m_programGrid = settings.value(QLatin1String(kProgramGridKey), false).toBool();
    m_sourceGrid = settings.value(QLatin1String(kSourceGridKey), false).toBool();
    m_droppedIndicator = settings.value(QLatin1String(kDroppedIndicatorKey), false).toBool();
    m_overlays = settings.value(QLatin1String(kOverlaysKey), false).toBool();
    m_audioUnits = settings.value(QLatin1String(kAudioUnitsKey), false).toBool();
    // Guides and templates from the settings: untrusted like any file (bounded and checked); bad data is dropped.
    const auto stored = guides::fromJson(QJsonDocument::fromJson(settings.value(QLatin1String(kGuidesKey)).toByteArray()).array());
    if (stored)
        m_guides = *stored;
    const auto templates =
        guides::templatesFromJson(QJsonDocument::fromJson(settings.value(QLatin1String(kTemplatesKey)).toByteArray()).array());
    if (templates)
        m_templates = *templates;
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::screenAdded, this, [this] {
            emit screensChanged();
            emit outputChanged();
        });
        connect(app, &QGuiApplication::screenRemoved, this, [this] {
            emit screensChanged();
            emit outputChanged();
        });
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            m_foreground = state == Qt::ApplicationActive;
            if (m_transmit && m_disableInBackground)
                emit outputChanged();
        });
    }
}

void MonitorBridge::setFullScreen(bool on) {
    if (on == m_fullScreen)
        return;
    m_fullScreen = on;
    emit outputChanged();
}

void MonitorBridge::setTransmit(bool on) {
    if (on == m_transmit)
        return;
    m_transmit = on;
    QSettings().setValue(QLatin1String(kTransmitKey), on);
    emit outputChanged();
}

void MonitorBridge::setTransmitScreen(int index) {
    index = std::max(-1, index);
    if (index == m_transmitScreen)
        return;
    m_transmitScreen = index;
    QSettings().setValue(QLatin1String(kTransmitScreenKey), index);
    emit outputChanged();
}

void MonitorBridge::setDisableTransmitInBackground(bool on) {
    if (on == m_disableInBackground)
        return;
    m_disableInBackground = on;
    QSettings().setValue(QLatin1String(kTransmitBackgroundKey), on);
    emit outputChanged();
}

void MonitorBridge::setPresentWindows(bool present) {
    if (present == m_presentWindows)
        return;
    m_presentWindows = present;
    emit outputChanged();
}

QVariantList MonitorBridge::screens() const {
    QVariantList out;
    const QList<QScreen*> all = QGuiApplication::screens();
    const QScreen* primary = QGuiApplication::primaryScreen();
    for (int i = 0; i < all.size(); ++i) {
        const QRect g = all[i]->geometry();
        out << QVariantMap{{QStringLiteral("index"), i},
                           {QStringLiteral("name"), all[i]->name()},
                           {QStringLiteral("width"), g.width()},
                           {QStringLiteral("height"), g.height()},
                           {QStringLiteral("primary"), all[i] == primary}};
    }
    return out;
}

bool MonitorBridge::transmitScreenValid() const {
    return m_transmitScreen >= 0 && m_transmitScreen < QGuiApplication::screens().size();
}

bool MonitorBridge::outputActive() const { return m_fullScreen || (m_transmit && transmitScreenValid()); }

bool MonitorBridge::outputVisible() const {
    if (!m_presentWindows || !outputActive())
        return false;
    // Transmit alone goes dark while Wabcut is in the background (when asked); full screen is Wabcut itself.
    return m_fullScreen || m_foreground || !m_disableInBackground;
}

int MonitorBridge::outputScreen() const {
    // Full screen covers the display the main window is on; transmit uses its own.
    return m_fullScreen || !transmitScreenValid() ? -1 : m_transmitScreen;
}

void MonitorBridge::placeOutputWindow(QQuickWindow* output, QQuickWindow* mainWindow) const {
    if (!output) {
        return;
    }
    const int index = outputScreen();
    const QList<QScreen*> all = QGuiApplication::screens();
    QScreen* screen = index >= 0 && index < all.size() ? all[index]
                      : mainWindow                     ? mainWindow->screen()
                                                       : QGuiApplication::primaryScreen();
    if (screen && output->screen() != screen) {
        output->setScreen(screen);
        output->setGeometry(screen->geometry()); // full screen covers the display it is on
    }
}

MonitorBridge* MonitorBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new MonitorBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
        bridge->setMotion(qmlEngine->singletonInstance<MotionBridge*>("Wabcut", "ProgramMotion"));
        bridge->setGraphicCanvas(qmlEngine->singletonInstance<GraphicCanvasBridge*>("Wabcut", "GraphicCanvas"));
        bridge->setSourceMonitor(qmlEngine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor"));
        // P1 (#19): Toggle Proxies and Playback Resolution are the preview settings MediaReadiness owns.
        bridge->setMediaReadiness(qmlEngine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness"));
    }
    return bridge;
}

void MonitorBridge::setPlayback(PlaybackBridge* playback) { m_playback = playback; }
void MonitorBridge::setSourceMonitor(SourceMonitorBridge* source) { m_source = source; }
void MonitorBridge::setGraphicCanvas(GraphicCanvasBridge* canvas) { m_canvas = canvas; }
void MonitorBridge::setMediaReadiness(MediaReadinessBridge* readiness) { m_readiness = readiness; }

void MonitorBridge::setMotion(MotionBridge* motion) {
    m_motion = motion;
    syncSnapping();
}

void MonitorBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        controller->setAudioTimeUnits(m_audioUnits); // 0.2: the playhead moves by samples while they are shown
        connect(controller, &ProjectController::sequenceChanged, this, [this] {
            emit guidesChanged(); // pixel guides sit at other fractions of another frame
            syncSnapping();
            emit timeChanged();
        });
        connect(controller, &ProjectController::playheadChanged, this, &MonitorBridge::timeChanged);
        connect(controller, &ProjectController::revisionChanged, this, [this] {
            if (m_overlays)
                emit timeChanged(); // the clip under the playhead may have changed
        });
    }
    emit guidesChanged();
    syncSnapping();
}

bool MonitorBridge::handlesCommand(const QString& id) {
    static const QSet<QString> kCommands{
        QStringLiteral("monitor.loop"),
        QStringLiteral("programMonitor.showRulers"),
        QStringLiteral("programMonitor.showGuides"),
        QStringLiteral("programMonitor.lockGuides"),
        QStringLiteral("programMonitor.snap"),
        QStringLiteral("programMonitor.addGuide"),
        QStringLiteral("programMonitor.clearGuides"),
        QStringLiteral("programMonitor.saveGuides"),
        QStringLiteral("programMonitor.manageGuides"),
        QStringLiteral("programMonitor.transparencyGrid"),
        QStringLiteral("sourceMonitor.transparencyGrid"),
        QStringLiteral("file.exportFrame"),
        QStringLiteral("marker.exportFrames"), // P1 (#23): Markers > Export Frames at Markers…
        QStringLiteral("programMonitor.droppedFrameIndicator"),
        QStringLiteral("programMonitor.overlays"),
        QStringLiteral("programMonitor.audioTimeUnits"),
        QStringLiteral("monitor.toggleFullScreen"),
        QStringLiteral("monitor.toggleProxies"), // P1 (#19): View > Use Proxies
        QStringLiteral("monitor.playbackResolutionAuto"),
        QStringLiteral("monitor.playbackResolutionFull"),
        QStringLiteral("monitor.playbackResolutionHalf"),
        QStringLiteral("monitor.playbackResolutionQuarter"),
        QStringLiteral("monitor.pausedResolutionSame"), // P1 (#16): View > Paused Resolution
        QStringLiteral("monitor.pausedResolutionFull"),
        QStringLiteral("monitor.pausedResolutionHalf"),
        QStringLiteral("monitor.pausedResolutionQuarter"),
        QStringLiteral("monitor.toggleCropDirectManipulation"),
        QStringLiteral("monitor.enableTransmit"),
        QStringLiteral("transport.playInToOut"),
        QStringLiteral("transport.playFromPlayheadToOut"), // 0.4: Ctrl+Space
        QStringLiteral("programMonitor.nudgeUp"),
        QStringLiteral("programMonitor.nudgeDown"),
        QStringLiteral("programMonitor.nudgeLeft"),
        QStringLiteral("programMonitor.nudgeRight"),
        QStringLiteral("programMonitor.nudgeUpFive"),
        QStringLiteral("programMonitor.nudgeDownFive"),
        QStringLiteral("programMonitor.nudgeLeftFive"),
        QStringLiteral("programMonitor.nudgeRightFive"),
    };
    return kCommands.contains(id);
}

bool MonitorBridge::runCommand(const QString& id, bool sourceFocused) {
    if (!handlesCommand(id))
        return false;
    if (id == QLatin1String("monitor.loop")) {
        if (m_playback)
            m_playback->setLoop(!m_playback->loop());
        return true;
    }
    if (id == QLatin1String("programMonitor.showRulers")) {
        setShowRulers(!m_showRulers);
        return true;
    }
    if (id == QLatin1String("programMonitor.showGuides")) {
        setShowGuides(!m_showGuides);
        return true;
    }
    if (id == QLatin1String("programMonitor.lockGuides")) {
        setLockGuides(!m_lockGuides);
        return true;
    }
    if (id == QLatin1String("programMonitor.snap")) {
        setSnap(!m_snap);
        return true;
    }
    if (id == QLatin1String("programMonitor.transparencyGrid")) {
        setProgramTransparencyGrid(!m_programGrid);
        return true;
    }
    if (id == QLatin1String("sourceMonitor.transparencyGrid")) {
        setSourceTransparencyGrid(!m_sourceGrid);
        return true;
    }
    if (id == QLatin1String("programMonitor.droppedFrameIndicator")) {
        setDroppedFrameIndicator(!m_droppedIndicator);
        return true;
    }
    if (id == QLatin1String("programMonitor.overlays")) {
        setOverlays(!m_overlays);
        return true;
    }
    if (id == QLatin1String("programMonitor.audioTimeUnits")) {
        setAudioTimeUnits(!m_audioUnits);
        return true;
    }
    if (id == QLatin1String("file.exportFrame")) {
        // Ctrl+Shift+E: the Source Monitor's frame while it has the focus, else the program's.
        requestExportFrame(sourceFocused);
        return true;
    }
    if (id == QLatin1String("marker.exportFrames")) {
        requestExportFramesAtMarkers(false);
        return true;
    }
    if (id == QLatin1String("programMonitor.clearGuides")) {
        clearGuides();
        return true;
    }
    if (id == QLatin1String("programMonitor.addGuide")) {
        emit dialogRequested(QStringLiteral("addGuide"), -1);
        return true;
    }
    if (id == QLatin1String("programMonitor.saveGuides")) {
        emit dialogRequested(QStringLiteral("saveGuides"), -1);
        return true;
    }
    if (id == QLatin1String("programMonitor.manageGuides")) {
        emit dialogRequested(QStringLiteral("manageGuides"), -1);
        return true;
    }
    if (id == QLatin1String("monitor.toggleCropDirectManipulation")) {
        if (m_motion)
            m_motion->toggleCrop();
        return true;
    }
    if (id == QLatin1String("monitor.toggleFullScreen")) {
        setFullScreen(!m_fullScreen);
        return true;
    }
    if (id == QLatin1String("monitor.toggleProxies")) {
        // P1 (#19): Premiere's View > Use Proxies; the preview settings are MediaReadiness's.
        if (m_readiness)
            m_readiness->setUseProxies(!m_readiness->useProxies());
        return true;
    }
    if (id.startsWith(QLatin1String("monitor.playbackResolution"))) {
        // Playback Resolution: Auto / Full / 1/2 / 1/4 (MediaReadiness's preview quality).
        if (m_readiness)
            m_readiness->setPreviewQuality(id.mid(QStringLiteral("monitor.playbackResolution").size()).toLower());
        return true;
    }
    if (id.startsWith(QLatin1String("monitor.pausedResolution"))) {
        // P1 (#16): Paused Resolution: Same as playback / Full / 1/2 / 1/4 (MediaReadiness's paused quality).
        if (m_readiness)
            m_readiness->setPausedQuality(id.mid(QStringLiteral("monitor.pausedResolution").size()).toLower());
        return true;
    }
    if (id == QLatin1String("monitor.enableTransmit")) {
        setTransmit(!m_transmit);
        return true;
    }
    if (id == QLatin1String("transport.playInToOut")) {
        if (m_playback)
            m_playback->playInToOut();
        return true;
    }
    if (id == QLatin1String("transport.playFromPlayheadToOut")) { // 0.4, Premiere's Play from Playhead to Out
        if (m_playback)
            m_playback->playToOut();
        return true;
    }
    if (id.startsWith(QLatin1String("programMonitor.nudge"))) {
        const double step = id.endsWith(QLatin1String("Five")) ? 5.0 : 1.0;
        const QString direction = id.mid(QStringLiteral("programMonitor.nudge").size()).remove(QStringLiteral("Five"));
        const double dx = direction == QLatin1String("Left") ? -step : direction == QLatin1String("Right") ? step : 0.0;
        const double dy = direction == QLatin1String("Up") ? -step : direction == QLatin1String("Down") ? step : 0.0;
        nudge(dx, dy);
        return true;
    }
    return false;
}

// ---- Rulers and guides ----------------------------------------------------------------------------------------

void MonitorBridge::setShowRulers(bool on) {
    if (on == m_showRulers)
        return;
    m_showRulers = on;
    QSettings().setValue(QLatin1String(kRulersKey), on);
    emit viewChanged();
}

void MonitorBridge::setRulerPercent(bool on) {
    if (on == m_rulerPercent)
        return;
    m_rulerPercent = on;
    QSettings().setValue(QLatin1String(kRulerPercentKey), on);
    emit viewChanged();
}

void MonitorBridge::setShowGuides(bool on) {
    if (on == m_showGuides)
        return;
    m_showGuides = on;
    QSettings().setValue(QLatin1String(kShowGuidesKey), on);
    emit viewChanged();
    syncSnapping();
}

void MonitorBridge::setLockGuides(bool on) {
    if (on == m_lockGuides)
        return;
    m_lockGuides = on;
    QSettings().setValue(QLatin1String(kLockGuidesKey), on);
    emit viewChanged();
}

void MonitorBridge::setSnap(bool on) {
    if (on == m_snap)
        return;
    m_snap = on;
    QSettings().setValue(QLatin1String(kSnapKey), on);
    emit viewChanged();
    syncSnapping();
}

void MonitorBridge::setDroppedFrameIndicator(bool on) {
    if (on == m_droppedIndicator)
        return;
    m_droppedIndicator = on;
    QSettings().setValue(QLatin1String(kDroppedIndicatorKey), on);
    emit viewChanged();
}

void MonitorBridge::setOverlays(bool on) {
    if (on == m_overlays)
        return;
    m_overlays = on;
    QSettings().setValue(QLatin1String(kOverlaysKey), on);
    emit viewChanged();
    emit timeChanged();
}

void MonitorBridge::setAudioTimeUnits(bool on) {
    if (on == m_audioUnits)
        return;
    m_audioUnits = on;
    QSettings().setValue(QLatin1String(kAudioUnitsKey), on);
    if (m_controller)
        m_controller->setAudioTimeUnits(on); // 0.2: one setting for the monitor and the timeline (Premiere)
    emit viewChanged();
    emit timeChanged();
}

QVariantList MonitorBridge::zoomLevels() {
    // Premiere's Select Zoom Level menu (cmd.monitor.output.zoom.*), besides Fit.
    return {0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0};
}

QString MonitorBridge::programTimeText() const {
    if (!m_controller)
        return {};
    if (!m_audioUnits)
        return m_controller->formatTimecode(m_controller->playhead());
    // Show Audio Time Units: hours, minutes, seconds and the audio sample within the second (or milliseconds, the
    // sequence's audio display format), to the sample the playhead stands on.
    return m_controller->formatAudioTime(m_controller->finePlayhead());
}

QVariantMap MonitorBridge::overlayText() const {
    QVariantMap out;
    if (!m_controller)
        return out;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return out;
    const Ticks t = m_controller->playhead();
    out.insert(QStringLiteral("sequence"), m_controller->formatTimecode(t));
    // Premiere's default overlay: the source timecode of the topmost visible clip at the playhead.
    const ProjectSnapshot snap = m_controller->snapshot();
    for (auto it = seq->videoTracks.rbegin(); it != seq->videoTracks.rend(); ++it) {
        const Track* track = it->get();
        if (!track || !track->enabled)
            continue;
        for (const Clip& c : track->clips) {
            if (!c.enabled || t < c.start() || t >= c.end())
                continue;
            out.insert(QStringLiteral("clip"), QString::fromStdString(c.name()));
            const MediaItem* item = snap->findMedia(c.media);
            if (item && !item->info.video.empty()) {
                const VideoStreamInfo& v = item->info.video.front();
                const Rate rate = v.frameRate.isValid() && v.frameRate.isExact() ? v.frameRate : seq->frameRate;
                const Ticks source = c.sourceTimeAt(t) + item->info.startTimecode.value_or(Ticks{});
                out.insert(QStringLiteral("source"), QString::fromStdString(ticksToTimecode(source, rate, false)));
            }
            return out;
        }
    }
    return out;
}

void MonitorBridge::setProgramTransparencyGrid(bool on) {
    if (on == m_programGrid)
        return;
    m_programGrid = on;
    QSettings().setValue(QLatin1String(kProgramGridKey), on);
    emit viewChanged();
}

void MonitorBridge::setSourceTransparencyGrid(bool on) {
    if (on == m_sourceGrid)
        return;
    m_sourceGrid = on;
    QSettings().setValue(QLatin1String(kSourceGridKey), on);
    emit viewChanged();
}

int MonitorBridge::frameWidth() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->width > 0 ? seq->width : 1920;
}

int MonitorBridge::frameHeight() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->height > 0 ? seq->height : 1080;
}

QVariantList MonitorBridge::guides() const {
    QVariantList out;
    const double w = frameWidth();
    const double h = frameHeight();
    for (std::size_t i = 0; i < m_guides.size(); ++i) {
        const guides::Guide& g = m_guides[i];
        out << QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                           {QStringLiteral("orientation"), g.vertical ? QStringLiteral("vertical") : QStringLiteral("horizontal")},
                           {QStringLiteral("position"), g.fraction(w, h)},
                           {QStringLiteral("value"), g.value},
                           {QStringLiteral("units"), g.percent ? QStringLiteral("percent") : QStringLiteral("pixels")},
                           {QStringLiteral("color"), g.color.name(QColor::HexRgb)}};
    }
    return out;
}

QStringList MonitorBridge::guideColors() {
    // Cyan (the default), magenta, amber, green, red, white: readable over most pictures.
    return {guides::defaultColor().name(QColor::HexRgb), QStringLiteral("#e040fb"), QStringLiteral("#ffd740"),
            QStringLiteral("#69f0ae"), QStringLiteral("#ff5252"), QStringLiteral("#ffffff")};
}

QStringList MonitorBridge::guideTemplates() const {
    QStringList out;
    for (const guides::Template& t : m_templates)
        out << t.name;
    return out;
}

int MonitorBridge::addGuide(const QString& orientation, double value, const QString& units, const QString& color) {
    if (static_cast<int>(m_guides.size()) >= guides::kMaxGuides) {
        setMessage(tr("There are already %1 guides").arg(guides::kMaxGuides));
        return -1;
    }
    m_guides.emplace_back();
    const int index = static_cast<int>(m_guides.size()) - 1;
    if (!setGuide(index, orientation, value, units, color)) {
        m_guides.pop_back();
        return -1;
    }
    return index;
}

bool MonitorBridge::setGuide(int index, const QString& orientation, double value, const QString& units,
                             const QString& color) {
    if (index < 0 || index >= static_cast<int>(m_guides.size()))
        return false;
    guides::Guide g;
    if (orientation != QLatin1String("horizontal") && orientation != QLatin1String("vertical"))
        return false;
    g.vertical = orientation == QLatin1String("vertical");
    g.percent = units == QLatin1String("percent");
    if (!std::isfinite(value) || std::abs(value) > (g.percent ? guides::kMaxPercent : guides::kMaxPixels)) {
        setMessage(tr("That position is out of range"));
        return false;
    }
    g.value = value;
    g.color = color.isEmpty() ? guides::defaultColor() : QColor::fromString(color);
    if (!g.color.isValid())
        g.color = guides::defaultColor();
    g.color.setAlpha(255);
    m_guides[static_cast<std::size_t>(index)] = g;
    setMessage({});
    storeGuides();
    return true;
}

bool MonitorBridge::moveGuide(int index, double fraction) {
    if (m_lockGuides || index < 0 || index >= static_cast<int>(m_guides.size()) || !std::isfinite(fraction))
        return false;
    guides::Guide& g = m_guides[static_cast<std::size_t>(index)];
    double value = g.valueAt(std::clamp(fraction, -1.0, 2.0), frameWidth(), frameHeight());
    if (!g.percent)
        value = std::round(value); // whole pixels, as Premiere's guides
    else
        value = std::round(value * 100.0) / 100.0;
    g.value = value;
    storeGuides();
    return true;
}

bool MonitorBridge::removeGuide(int index) {
    if (m_lockGuides || index < 0 || index >= static_cast<int>(m_guides.size()))
        return false;
    m_guides.erase(m_guides.begin() + index);
    storeGuides();
    return true;
}

void MonitorBridge::clearGuides() {
    if (m_guides.empty())
        return;
    m_guides.clear();
    storeGuides();
}

QString MonitorBridge::saveGuidesAsTemplate(const QString& rawName) {
    const QString name = rawName.trimmed();
    if (name.isEmpty())
        return tr("Name the template");
    if (name.size() > guides::kMaxNameLength)
        return tr("The name is longer than %1 characters").arg(guides::kMaxNameLength);
    if (m_guides.empty())
        return tr("There are no guides to save");
    const auto existing = std::find_if(m_templates.begin(), m_templates.end(), [&](const guides::Template& t) {
        return t.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (existing != m_templates.end()) {
        existing->guides = m_guides;
    } else {
        if (static_cast<int>(m_templates.size()) >= guides::kMaxTemplates)
            return tr("There are already %1 guide templates").arg(guides::kMaxTemplates);
        m_templates.push_back({name, m_guides});
    }
    storeTemplates();
    return {};
}

bool MonitorBridge::applyGuideTemplate(const QString& name) {
    for (const guides::Template& t : m_templates) {
        if (t.name == name) {
            m_guides = t.guides;
            storeGuides();
            return true;
        }
    }
    return false;
}

bool MonitorBridge::removeGuideTemplate(const QString& name) {
    const auto before = m_templates.size();
    std::erase_if(m_templates, [&](const guides::Template& t) { return t.name == name; });
    if (m_templates.size() == before)
        return false;
    storeTemplates();
    return true;
}

QString MonitorBridge::exportGuideTemplates(const QStringList& names, const QUrl& file) {
    std::vector<guides::Template> chosen;
    for (const guides::Template& t : m_templates) {
        if (names.isEmpty() || names.contains(t.name))
            chosen.push_back(t);
    }
    if (chosen.empty())
        return tr("Choose the templates to export");
    QString path = pathOf(file);
    if (!path.endsWith(QLatin1String(".guides"), Qt::CaseInsensitive))
        path += QStringLiteral(".guides");
    QSaveFile out(path);
    const QByteArray data = guides::exportFile(chosen);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
        return tr("Cannot write %1: %2").arg(path, out.errorString());
    return {};
}

QString MonitorBridge::importGuideTemplates(const QUrl& file) {
    QFile in(pathOf(file));
    if (!in.open(QIODevice::ReadOnly))
        return tr("Cannot read %1: %2").arg(pathOf(file), in.errorString());
    auto imported = guides::importFile(in.read(4 * 1024 * 1024 + 1));
    if (!imported)
        return imported.error();
    for (guides::Template& t : *imported) {
        const auto existing = std::find_if(m_templates.begin(), m_templates.end(), [&](const guides::Template& o) {
            return o.name.compare(t.name, Qt::CaseInsensitive) == 0;
        });
        if (existing != m_templates.end()) {
            existing->guides = std::move(t.guides);
        } else if (static_cast<int>(m_templates.size()) < guides::kMaxTemplates) {
            m_templates.push_back(std::move(t));
        }
    }
    storeTemplates();
    return {};
}

// ---- Export Frame -------------------------------------------------------------------------------------------------

QVariantMap MonitorBridge::exportFrameDefaults(bool source) const {
    QSettings settings;
    QVariantMap out;
    QString name;
    bool available = false;
    if (source) {
        if (m_source && m_source->loaded()) {
            name = fileNameOf(m_source->name() + QLatin1Char(' ') + m_source->positionText());
            available = m_source->hasVideo();
        }
    } else if (m_controller) {
        if (const SequencePtr seq = m_controller->currentSequence()) {
            name = fileNameOf(QString::fromStdString(seq->name) + QLatin1Char(' ') + m_controller->formatTimecode(m_controller->playhead()));
            available = true;
        }
    }
    if (name.isEmpty())
        name = tr("Frame");
    QString folder = settings.value(QLatin1String(kExportFolderKey)).toString();
    if (folder.isEmpty() && m_controller && !m_controller->projectFile().empty())
        folder = QString::fromStdString(pathToUtf8(m_controller->projectFile().parent_path()));
    if (folder.isEmpty())
        folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QVariantList formats;
    const std::array<std::pair<const char*, const char*>, 8> labels{{
        {"dpx", QT_TR_NOOP("DPX")}, {"jpeg", QT_TR_NOOP("JPEG")}, {"png", QT_TR_NOOP("PNG")}, {"tiff", QT_TR_NOOP("TIFF")},
        {"targa", QT_TR_NOOP("Targa")}, {"bmp", QT_TR_NOOP("BMP")}, {"gif", QT_TR_NOOP("GIF")}, {"openexr", QT_TR_NOOP("OpenEXR")},
    }};
    for (const auto& [value, label] : labels)
        formats << QVariantMap{{QStringLiteral("value"), QString::fromLatin1(value)}, {QStringLiteral("label"), tr(label)}};
    out.insert(QStringLiteral("name"), name);
    out.insert(QStringLiteral("folder"), QUrl::fromLocalFile(folder));
    out.insert(QStringLiteral("format"), settings.value(QLatin1String(kExportFormatKey), QStringLiteral("png")).toString());
    out.insert(QStringLiteral("importIntoProject"), settings.value(QLatin1String(kExportImportKey), false).toBool());
    out.insert(QStringLiteral("formats"), formats);
    out.insert(QStringLiteral("available"), available);
    // P1 (#23): the size, crop, JPEG quality and 2 MB cap picked last.
    QVariantList sizes;
    for (const FrameSize& f : kFrameSizes)
        sizes << QVariantMap{{QStringLiteral("value"), QString::fromLatin1(f.value)}, {QStringLiteral("label"), tr(f.label)},
                             {QStringLiteral("width"), f.width}, {QStringLiteral("height"), f.height}};
    out.insert(QStringLiteral("sizes"), sizes);
    out.insert(QStringLiteral("size"), settings.value(QLatin1String(kExportSizeKey), QStringLiteral("sequence")).toString());
    out.insert(QStringLiteral("width"), settings.value(QLatin1String(kExportWidthKey), 1280).toInt());
    out.insert(QStringLiteral("height"), settings.value(QLatin1String(kExportHeightKey), 720).toInt());
    out.insert(QStringLiteral("fill"), settings.value(QLatin1String(kExportFillKey), true).toBool());
    out.insert(QStringLiteral("quality"), settings.value(QLatin1String(kExportQualityKey), 100).toInt());
    out.insert(QStringLiteral("keepUnder"), settings.value(QLatin1String(kExportKeepUnderKey), true).toBool());
    out.insert(QStringLiteral("markers"), settings.value(QLatin1String(kExportMarkersKey), QStringLiteral("all")).toString());
    return out;
}

QString MonitorBridge::exportFrame(bool source, const QString& rawName, const QString& format, const QUrl& folder,
                                   bool importIntoProject, const QVariantMap& options) {
    if (!m_controller) {
        setMessage(tr("No project"));
        return {};
    }
    const QString dir = pathOf(folder);
    const QString name = fileNameOf(rawName);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        setMessage(tr("Choose a folder for the frame"));
        return {};
    }
    if (name.isEmpty()) {
        setMessage(tr("Name the frame"));
        return {};
    }
    const auto still = exporter::stillFormatFromName(format.toStdString());
    if (!still) {
        setMessage(tr("Choose a format"));
        return {};
    }
    command::Json params{{"format", format.toStdString()},
                         {"path", pathToUtf8(pathFromUtf8(QDir(dir).filePath(name).toStdString()))},
                         {"importIntoProject", importIntoProject}};
    // P1 (#23): a size (YouTube thumbnail, vertical, custom), JPEG quality and the 2 MB cap.
    if (const QString problem = addStillOptions(options, *still == exporter::StillFormat::Jpeg, params); !problem.isEmpty()) {
        setMessage(problem);
        return {};
    }
    command::CommandRequest request;
    if (source) {
        if (!m_source || !m_source->loaded()) {
            setMessage(tr("Load a clip in the Source Monitor first"));
            return {};
        }
        request.command = "source.exportFrame";
        params["media"] = m_source->mediaId().value;
        params["time"] = m_source->positionTicks().value;
    } else {
        const SequencePtr seq = m_controller->currentSequence();
        if (!seq) {
            setMessage(tr("No sequence"));
            return {};
        }
        request.command = "sequence.exportFrame";
        params["sequence"] = seq->id.value;
        params["time"] = m_controller->playhead().value;
    }
    request.params = std::move(params);
    command::EnginePolicy policy;
    const std::filesystem::path projectFile = m_controller->projectFile();
    policy.services = exporter::stillExportServices(
        [projectFile](const std::filesystem::path& file) { return editor::describeMediaFile(file, projectFile); });
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
    const command::CommandResult result = engine.execute(request);
    if (!result.ok()) {
        setMessage(tr("Export Frame failed: %1").arg(QString::fromStdString(result.error->message)));
        return {};
    }
    QSettings settings;
    settings.setValue(QLatin1String(kExportFolderKey), dir);
    settings.setValue(QLatin1String(kExportFormatKey), format);
    settings.setValue(QLatin1String(kExportImportKey), importIntoProject);
    rememberStillOptions(options);
    const QString written = QString::fromStdString(result.data.value("path", std::string()));
    if (result.data.value("overLimit", false))
        setMessage(tr("Frame saved: %1, but it is over 2 MB even at the lowest quality: pick a smaller size")
                       .arg(QFileInfo(written).fileName()));
    else
        setMessage(tr("Frame saved: %1").arg(QFileInfo(written).fileName()));
    return written;
}

MonitorBridge::~MonitorBridge() {
    if (m_framesJob)
        m_framesJob->stop();
}

QVariantMap MonitorBridge::markerFrameCounts() const {
    int all = 0;
    int chapters = 0;
    int highlights = 0;
    if (const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr) {
        for (const Marker& m : seq->markers) {
            ++all;
            chapters += m.type == MarkerType::Chapter ? 1 : 0;
            highlights += m.highlight ? 1 : 0;
        }
    }
    return {{QStringLiteral("all"), all}, {QStringLiteral("chapters"), chapters}, {QStringLiteral("highlights"), highlights}};
}

bool MonitorBridge::exportFramesAtMarkers(const QVariantMap& options) {
    if (!m_controller || !m_controller->currentSequence()) {
        setMessage(tr("Open a sequence first"));
        return false;
    }
    if (m_framesBusy) {
        setMessage(tr("Frames are being exported already"));
        return false;
    }
    const QString dir = folderOf(options.value(QStringLiteral("folder")));
    if (dir.isEmpty() || !QDir(dir).exists()) {
        setMessage(tr("Choose a folder for the frames"));
        return false;
    }
    const QString format = options.value(QStringLiteral("format"), QStringLiteral("jpeg")).toString();
    const auto still = exporter::stillFormatFromName(format.toStdString());
    if (!still) {
        setMessage(tr("Choose a format"));
        return false;
    }
    const QString markers = options.value(QStringLiteral("markers"), QStringLiteral("all")).toString();
    const bool importIntoProject = options.value(QStringLiteral("importIntoProject"), false).toBool();
    command::Json params{{"folder", pathToUtf8(pathFromUtf8(dir.toStdString()))},
                         {"format", format.toStdString()},
                         {"markers", markers == QLatin1String("chapters") || markers == QLatin1String("highlights")
                                         ? markers.toStdString()
                                         : std::string("all")},
                         {"importIntoProject", importIntoProject}};
    if (const QString problem = addStillOptions(options, *still == exporter::StillFormat::Jpeg, params); !problem.isEmpty()) {
        setMessage(problem);
        return false;
    }
    if (!m_framesJob)
        m_framesJob = std::make_unique<CommandJob>();
    m_framesJob->setSynchronous(m_framesSynchronous);
    const std::filesystem::path projectFile = m_controller->projectFile();
    auto services = exporter::stillExportServices(
        [projectFile](const std::filesystem::path& file) { return editor::describeMediaFile(file, projectFile); });
    m_framesBusy = true;
    m_framesProgress = 0.0;
    m_framesStatus = tr("Exporting frames…");
    emit framesChanged();
    const QVariantMap remembered = options;
    const bool started = m_framesJob->start(
        this, m_controller, std::move(services),
        CommandJob::Request{"sequence.exportFramesAtMarkers", std::move(params), tr("Export Frames at Markers").toStdString()},
        [this](const QString& text, double progress) {
            m_framesStatus = text;
            m_framesProgress = progress;
            emit framesChanged();
        },
        [this, dir, remembered, markers](const command::CommandResult& result) {
            m_framesBusy = false;
            m_framesProgress = -1.0;
            m_framesStatus.clear();
            emit framesChanged();
            QString text;
            const bool ok = result.ok();
            if (ok) {
                QSettings settings;
                settings.setValue(QLatin1String(kExportFolderKey), dir);
                settings.setValue(QLatin1String(kExportFormatKey), remembered.value(QStringLiteral("format")).toString());
                settings.setValue(QLatin1String(kExportImportKey), remembered.value(QStringLiteral("importIntoProject")).toBool());
                settings.setValue(QLatin1String(kExportMarkersKey), markers);
                rememberStillOptions(remembered);
                const int count = result.data.value("count", 0);
                const int over = result.data.value("overLimit", 0);
                text = result.data.contains("media") ? tr("%n still(s) saved and imported into the Thumbnails bin", "", count)
                                                     : tr("%n still(s) saved to %1", "", count).arg(QDir::toNativeSeparators(dir));
                if (over > 0)
                    text += tr(" (%n over 2 MB even at the lowest quality)", "", over);
            } else if (result.error && result.error->code == command::ErrorCode::Cancelled) {
                text = tr("Export Frames at Markers stopped: the stills written so far stay in the folder");
            } else {
                text = tr("Export Frames at Markers: %1").arg(commandErrorText(result));
            }
            setMessage(text);
            emit framesFinished(ok, text);
        });
    if (!started) {
        m_framesBusy = false;
        m_framesProgress = -1.0;
        m_framesStatus.clear();
        emit framesChanged();
        setMessage(tr("Frames are being exported already"));
    }
    return started;
}

void MonitorBridge::cancelFramesAtMarkers() {
    if (m_framesJob)
        m_framesJob->cancel();
}

void MonitorBridge::storeGuides() {
    QSettings().setValue(QLatin1String(kGuidesKey), QJsonDocument(guides::toJson(m_guides)).toJson(QJsonDocument::Compact));
    emit guidesChanged();
    syncSnapping();
}

void MonitorBridge::storeTemplates() {
    QSettings().setValue(QLatin1String(kTemplatesKey),
                         QJsonDocument(guides::templatesToJson(m_templates)).toJson(QJsonDocument::Compact));
    emit guideTemplatesChanged();
}

void MonitorBridge::syncSnapping() {
    if (!m_motion)
        return;
    m_motion->setSnapping(m_snap);
    std::vector<double> vertical;
    std::vector<double> horizontal;
    if (m_showGuides) {
        const double w = frameWidth();
        const double h = frameHeight();
        for (const guides::Guide& g : m_guides)
            (g.vertical ? vertical : horizontal).push_back(g.fraction(w, h));
    }
    m_motion->setSnapLines(std::move(vertical), std::move(horizontal));
}

void MonitorBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

bool MonitorBridge::nudge(double dx, double dy) {
    // The clip whose Motion is selected, else the selected graphic layer (Premiere's Nudge Selected Object).
    if (m_motion && m_motion->active())
        return m_motion->nudge(dx, dy);
    if (m_canvas && m_canvas->active() && m_canvas->selectedLayer() != 0)
        return m_canvas->nudge(dx, dy);
    return false;
}

// ---- P1 (#17): the Program Monitor's edit overlay -------------------------------------------------------------------

QString MonitorBridge::overlayTrack(const QString& mediaKeyText, bool fromSource) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!snap || !seq)
        return {};
    // How long the clip the drop makes is: the drop itself, dry-run on a copy of the project (the item's marks, a
    // subclip's range, a still's length, a nested sequence's length all count as the real drop counts them).
    editor::MediaDrop drop;
    if (fromSource) {
        if (!m_source || !m_source->mediaId().isValid())
            return {};
        drop.media = m_source->mediaId();
    } else if (const auto media = detail::parseId<MediaId>(QLatin1Char('m'), mediaKeyText)) {
        drop.media = *media;
    } else if (const auto nested = parseSequenceKey(mediaKeyText)) {
        drop.sourceSequence = *nested;
    } else {
        return {};
    }
    drop.sequence = seq->id;
    drop.at = m_controller->playhead();
    drop.mode = editor::DropMode::Overwrite;
    drop.useSourceMarks = true;
    EditSession trial{ProjectState{*snap}};
    const auto dropped = editor::dropMedia(trial, drop);
    const ProjectSnapshot after = trial.snapshot();
    const Sequence* landed = after ? after->findSequence(seq->id) : nullptr;
    if (!dropped || !landed)
        return {};
    std::optional<TimeRange> range;
    for (const ClipId clip : dropped->placed) {
        if (const auto found = landed->findClip(clip)) {
            const TimeRange r = found->clip().timelineRange;
            range = range ? TimeRange::fromStartEnd(std::min(range->start, r.start), std::max(range->end(), r.end())) : r;
        }
    }
    if (!range || range->isEmpty())
        return {};
    // From the topmost targeted video track (else V1) up: the first unlocked one with nothing there.
    TrackId base;
    for (const TrackId id : edit::targetedTracks(*seq)) {
        const Track* t = seq->findTrack(id);
        if (t && t->kind == TrackKind::Video && !t->locked) {
            base = id;
            break;
        }
    }
    bool reached = !base.isValid();
    for (const TrackPtr& t : seq->videoTracks) {
        if (!t)
            continue;
        reached = reached || t->id == base;
        if (!reached || t->locked)
            continue;
        const bool busy = std::any_of(t->clips.begin(), t->clips.end(),
                                      [&](const Clip& c) { return c.timelineRange.overlaps(*range); });
        if (!busy)
            return trackKey(t->id);
    }
    return {};
}

QString MonitorBridge::sequenceEndText() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq ? m_controller->formatTimecode(seq->contentEnd()) : QString();
}

QString MonitorBridge::replaceTarget() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return {};
    const Ticks at = m_controller->playhead();
    const auto clipOn = [&](const Track* t) -> QString {
        if (!t || t->kind != TrackKind::Video || t->locked)
            return {};
        const auto index = t->clipIndexAt(at);
        return index ? clipKey(t->clips[*index].id) : QString();
    };
    // The targeted video tracks, topmost first (Premiere replaces on the target track).
    std::vector<const Track*> targeted;
    for (const TrackId id : edit::targetedTracks(*seq))
        targeted.push_back(seq->findTrack(id));
    for (auto t = seq->videoTracks.rbegin(); t != seq->videoTracks.rend(); ++t) {
        if (*t && std::find(targeted.begin(), targeted.end(), t->get()) != targeted.end()) {
            if (const QString key = clipOn(t->get()); !key.isEmpty())
                return key;
        }
    }
    for (auto t = seq->videoTracks.rbegin(); t != seq->videoTracks.rend(); ++t) {
        if (const QString key = clipOn(t->get()); !key.isEmpty())
            return key;
    }
    return {};
}

} // namespace wabcut::app
