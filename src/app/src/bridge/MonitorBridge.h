#pragma once

// MonitorBridge: Premiere's Source and Program Monitor settings and keys (singleton `Monitors`, P.2 E6;
// docs/monitors.md). Main.qml hands it every keymap command after the Source Monitor's own (runCommand):
//   * Loop (monitor.loop): the Program Monitor's Loop (Playback.loop); the Source Monitor toggles its own while it has
//     the focus (SourceMonitor.runCommand).
//   * Play In to Out (transport.playInToOut) in the Program Monitor.
//   * Nudge Selected Object (programMonitor.nudge*: Ctrl+arrows, Ctrl+Shift+arrows by five): the clip whose Motion is
//     selected (ProgramMotion), else the selected graphic layer (GraphicCanvas), by one or five pixels.
//   * The second program output (ProgramOutputWindow.qml, a top-level window showing the program through a playback
//     mirror): Toggle Full Screen (monitor.toggleFullScreen, Ctrl+`) puts it full screen on the main window's display;
//     Enable Transmit (monitor.enableTransmit; Premiere's Mercury Transmit) keeps it full screen on `transmitScreen`
//     (Playback > Video Device: a display), hidden while Wabcut is in the background when
//     `disableTransmitInBackground` is on (Premiere's "Disable video output when in the background", on by default).
//   * Rulers and guides (Program Monitor > View, MonitorGuides.h): Show Rulers (Ctrl+R; pixels or percent), Show
//     Guides (Ctrl+;), Lock Guides (Ctrl+Alt+Shift+R), Add Guide..., Clear Guides, Guide Templates (Save Guides as
//     Template..., Manage Guides...: apply, delete, export, import) and Snap in Program Monitor (Ctrl+Shift+;): Motion
//     drags snap to the guides and the frame's edges and centre. Guides persist across projects and sessions, as in
//     Premiere.
//   * Transparency Grid (each monitor's settings menu): a checkerboard where the picture is transparent.
//   * Export Frame (Ctrl+Shift+E, the camera in both monitors; ExportFrameDialog.qml): the Program Monitor's frame at
//     the playhead (sequence.exportFrame) or the Source Monitor's at its position (source.exportFrame), as DPX, JPEG,
//     PNG, TIFF, Targa, BMP, GIF or OpenEXR, optionally imported into the project; the last name pattern's folder,
//     format and Import choice are remembered.
//   * The Program Monitor's view settings: Premiere's zoom levels (Fit, 10 % to 1600 %), Show Dropped Frame Indicator,
//     Overlays (the sequence and source timecode over the picture) and Show Audio Time Units (the readout counts audio
//     samples instead of frames).
//   * P1 (#19): Toggle Proxies (View > Use Proxies) and Playback Resolution: Auto / Full / 1/2 / 1/4 (View > Playback
//     Resolution): the same two preview settings as the Program Monitor's quality menu (the singleton MediaReadiness),
//     so the keys, the View menu and that menu show the same state. P1 (#16): Paused Resolution: Full / 1/2 / 1/4 /
//     Same as Playback (View > Paused Resolution, MediaReadiness.pausedQuality).
//   * P1 (#17): the Program Monitor's edit overlay (EditOverlay.qml): where an Overlay drop goes (overlayTrack), the
//     sequence's end for Append at End (sequenceEndText) and the clip Replace replaces (replaceTarget).

#include "MonitorGuides.h"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class CommandJob;
class GraphicCanvasBridge;
class MediaReadinessBridge;
class MotionBridge;
class PlaybackBridge;
class ProjectController;
class SourceMonitorBridge;

class MonitorBridge : public QObject {
    Q_OBJECT
    /// Toggle Full Screen: the program fills the main window's display.
    Q_PROPERTY(bool fullScreen READ fullScreen WRITE setFullScreen NOTIFY outputChanged FINAL)
    /// Enable Transmit: the program fills `transmitScreen` (remembered).
    Q_PROPERTY(bool transmit READ transmit WRITE setTransmit NOTIFY outputChanged FINAL)
    /// The display transmit uses, an index into `screens` (-1: none; remembered).
    Q_PROPERTY(int transmitScreen READ transmitScreen WRITE setTransmitScreen NOTIFY outputChanged FINAL)
    /// Premiere's "Disable video output when in the background" for transmit (remembered; on by default).
    Q_PROPERTY(bool disableTransmitInBackground READ disableTransmitInBackground WRITE setDisableTransmitInBackground
                   NOTIFY outputChanged FINAL)
    /// The displays: [{ index, name, width, height, primary }].
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged FINAL)
    /// The output window is wanted (full screen, or transmit to a display that exists): the mirror runs.
    Q_PROPERTY(bool outputActive READ outputActive NOTIFY outputChanged FINAL)
    /// The output window is on screen (outputActive, Wabcut in the foreground or transmit allowed in the background,
    /// and windows allowed: tests turn them off).
    Q_PROPERTY(bool outputVisible READ outputVisible NOTIFY outputChanged FINAL)
    /// The display the output window goes on (-1: the main window's).
    Q_PROPERTY(int outputScreen READ outputScreen NOTIFY outputChanged FINAL)

    // ---- Rulers and guides (remembered) ----
    Q_PROPERTY(bool showRulers READ showRulers WRITE setShowRulers NOTIFY viewChanged FINAL)
    /// The rulers count percent of the frame instead of pixels (the ruler's menu).
    Q_PROPERTY(bool rulerPercent READ rulerPercent WRITE setRulerPercent NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool showGuides READ showGuides WRITE setShowGuides NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool lockGuides READ lockGuides WRITE setLockGuides NOTIFY viewChanged FINAL)
    /// Snap in Program Monitor: Motion drags snap to the guides and the frame's edges and centre.
    Q_PROPERTY(bool snap READ snap WRITE setSnap NOTIFY viewChanged FINAL)
    /// [{ index, orientation: "horizontal" | "vertical", position (fraction of the frame), value, units: "pixels" |
    ///    "percent", color }].
    Q_PROPERTY(QVariantList guides READ guides NOTIFY guidesChanged FINAL)
    Q_PROPERTY(QStringList guideTemplates READ guideTemplates NOTIFY guideTemplatesChanged FINAL)
    /// The colours Add Guide offers ("#rrggbb"), the default first.
    Q_PROPERTY(QStringList guideColors READ guideColors CONSTANT FINAL)
    /// The sequence frame the guides and rulers measure (pixels).
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY guidesChanged FINAL)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY guidesChanged FINAL)
    /// Transparency Grid in the Program and the Source Monitor (remembered; off by default).
    Q_PROPERTY(bool programTransparencyGrid READ programTransparencyGrid WRITE setProgramTransparencyGrid NOTIFY
                   viewChanged FINAL)
    Q_PROPERTY(bool sourceTransparencyGrid READ sourceTransparencyGrid WRITE setSourceTransparencyGrid NOTIFY
                   viewChanged FINAL)
    /// Show Dropped Frame Indicator, Overlays, Show Audio Time Units (remembered; off by default).
    Q_PROPERTY(bool droppedFrameIndicator READ droppedFrameIndicator WRITE setDroppedFrameIndicator NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool overlays READ overlays WRITE setOverlays NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool audioTimeUnits READ audioTimeUnits WRITE setAudioTimeUnits NOTIFY viewChanged FINAL)
    /// The playhead as the Program Monitor's readout shows it: a timecode, or hh:mm:ss:samples with audio time units.
    Q_PROPERTY(QString programTimeText READ programTimeText NOTIFY timeChanged FINAL)
    /// What Overlays draw: { sequence: timecode, source: the topmost clip's source timecode or "", clip: its name }.
    Q_PROPERTY(QVariantMap overlayText READ overlayText NOTIFY timeChanged FINAL)
    /// Premiere's zoom levels besides Fit, as monitor pixels per sequence pixel: 10 % to 1600 %.
    Q_PROPERTY(QVariantList zoomLevels READ zoomLevels CONSTANT FINAL)
    /// The last problem (a template that cannot be saved, a file that cannot be read), "" otherwise.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// P1 (#23, stream 1C): Export Frames at Markers runs (in the background): its progress (0..1, -1 unknown) and text.
    Q_PROPERTY(bool framesBusy READ framesBusy NOTIFY framesChanged FINAL)
    Q_PROPERTY(double framesProgress READ framesProgress NOTIFY framesChanged FINAL)
    Q_PROPERTY(QString framesStatus READ framesStatus NOTIFY framesChanged FINAL)

public:
    explicit MonitorBridge(QObject* parent = nullptr);
    ~MonitorBridge() override;
    static MonitorBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setPlayback(PlaybackBridge* playback);
    void setMotion(MotionBridge* motion);
    void setController(ProjectController* controller);
    void setSourceMonitor(SourceMonitorBridge* source);
    void setGraphicCanvas(GraphicCanvasBridge* canvas);
    /// P1 (#19): the preview settings Toggle Proxies and Playback Resolution change (singleton `MediaReadiness`).
    void setMediaReadiness(MediaReadinessBridge* readiness);

    bool fullScreen() const { return m_fullScreen; }
    void setFullScreen(bool on);
    bool transmit() const { return m_transmit; }
    void setTransmit(bool on);
    int transmitScreen() const { return m_transmitScreen; }
    void setTransmitScreen(int index);
    bool disableTransmitInBackground() const { return m_disableInBackground; }
    void setDisableTransmitInBackground(bool on);
    QVariantList screens() const;
    bool outputActive() const;
    bool outputVisible() const;
    int outputScreen() const;
    /// Puts the output window on its display: `outputScreen`, else the main window's (QML's Window.screen takes a
    /// Screen, not the QScreen the bridge knows).
    Q_INVOKABLE void placeOutputWindow(QQuickWindow* output, QQuickWindow* mainWindow) const;
    /// Tests: whether output windows may appear on screen (they never do in a test run).
    void setPresentWindows(bool present);

    bool showRulers() const { return m_showRulers; }
    void setShowRulers(bool on);
    bool rulerPercent() const { return m_rulerPercent; }
    void setRulerPercent(bool on);
    bool showGuides() const { return m_showGuides; }
    void setShowGuides(bool on);
    bool lockGuides() const { return m_lockGuides; }
    void setLockGuides(bool on);
    bool snap() const { return m_snap; }
    void setSnap(bool on);
    bool droppedFrameIndicator() const { return m_droppedIndicator; }
    void setDroppedFrameIndicator(bool on);
    bool overlays() const { return m_overlays; }
    void setOverlays(bool on);
    bool audioTimeUnits() const { return m_audioUnits; }
    void setAudioTimeUnits(bool on);
    QString programTimeText() const;
    QVariantMap overlayText() const;
    static QVariantList zoomLevels();
    bool programTransparencyGrid() const { return m_programGrid; }
    void setProgramTransparencyGrid(bool on);
    bool sourceTransparencyGrid() const { return m_sourceGrid; }
    void setSourceTransparencyGrid(bool on);
    QVariantList guides() const;
    QStringList guideTemplates() const;
    static QStringList guideColors();
    int frameWidth() const;
    int frameHeight() const;
    QString message() const { return m_message; }
    const std::vector<guides::Guide>& guideList() const { return m_guides; }

    /// Add Guide: `orientation` "horizontal" | "vertical", `value` in `units` ("pixels" from the frame's top / left
    /// edge, or "percent" of it), `color` "#rrggbb" ("" the default). Returns the new guide's index, -1 when refused.
    Q_INVOKABLE int addGuide(const QString& orientation, double value, const QString& units, const QString& color = {});
    /// Edit Guide (the dialog on a double-clicked guide).
    Q_INVOKABLE bool setGuide(int index, const QString& orientation, double value, const QString& units,
                              const QString& color);
    /// Drags a guide to `fraction` of the frame (in its own units). Refused while the guides are locked.
    Q_INVOKABLE bool moveGuide(int index, double fraction);
    Q_INVOKABLE bool removeGuide(int index);
    /// A double-clicked guide: the host opens Edit Guide on it.
    Q_INVOKABLE void editGuide(int index) { emit dialogRequested(QStringLiteral("editGuide"), index); }
    /// The camera buttons: the host opens Export Frame for the Program (`source` false) or the Source Monitor.
    Q_INVOKABLE void requestExportFrame(bool source) { emit dialogRequested(QStringLiteral("exportFrame"), source ? 1 : 0); }
    Q_INVOKABLE void clearGuides();
    /// Save Guides as Template: "" or why not (an empty name, no guides; an existing name is replaced).
    Q_INVOKABLE QString saveGuidesAsTemplate(const QString& name);
    /// Choosing a template under View > Guide Templates: its guides replace the current ones.
    Q_INVOKABLE bool applyGuideTemplate(const QString& name);
    Q_INVOKABLE bool removeGuideTemplate(const QString& name);
    /// Manage Guides > Export: the named templates (all when empty) into a guides file; "" or the error.
    Q_INVOKABLE QString exportGuideTemplates(const QStringList& names, const QUrl& file);
    /// Manage Guides > Import: the file's templates join the user's (same names replaced); "" or the error.
    Q_INVOKABLE QString importGuideTemplates(const QUrl& file);

    /// Export Frame's starting values for the Program (`source` false) or the Source Monitor: { name, folder (URL),
    /// format, importIntoProject, formats: [{ value, label }], available (a frame to export) }.
    Q_INVOKABLE QVariantMap exportFrameDefaults(bool source) const;
    /// Export Frame: the Program Monitor's frame at the playhead, or the Source Monitor's at its position, written as
    /// `format` to `folder`/`name` (the extension added); with `importIntoProject` the still joins the project (one undo
    /// step). Returns the file written, or "" (the reason in `message`).
    Q_INVOKABLE QString exportFrame(bool source, const QString& name, const QString& format, const QUrl& folder,
                                    bool importIntoProject, const QVariantMap& options = {});

    // ---- P1 (#23, stream 1C): Export Frame sizes and Export Frames at Markers ----
    // exportFrame's `options`: { size: "sequence" | "thumbnail" (1280 x 720) | "vertical" (1080 x 1920) | "custom",
    // width, height (custom), fill (crop to fill instead of black bars), quality (JPEG 1..100), keepUnder (JPEG under
    // 2 MB) } - remembered with the rest; exportFrameDefaults() adds them and `sizes` ([{ value, label, width, height }]).
    /// Markers > Export Frames at Markers… and the Highlights panel: the host opens the dialog in its markers mode
    /// (`highlights`: the highlight markers' peaks).
    Q_INVOKABLE void requestExportFramesAtMarkers(bool highlights) {
        emit dialogRequested(QStringLiteral("exportFramesAtMarkers"), highlights ? 1 : 0);
    }
    /// How many of the current sequence's markers each choice exports: { all, chapters, highlights }.
    Q_INVOKABLE QVariantMap markerFrameCounts() const;
    /// Export Frames at Markers (sequence.exportFramesAtMarkers) in the background: `options` = exportFrame's plus
    /// { format, folder (URL), markers: "all" | "chapters" | "highlights", importIntoProject }. False (why in `message`)
    /// when it cannot start; framesFinished(ok, message) when done.
    Q_INVOKABLE bool exportFramesAtMarkers(const QVariantMap& options);
    Q_INVOKABLE void cancelFramesAtMarkers();
    bool framesBusy() const { return m_framesBusy; }
    double framesProgress() const { return m_framesProgress; }
    QString framesStatus() const { return m_framesStatus; }
    /// Tests: exportFramesAtMarkers runs on the calling thread.
    void setFramesSynchronous(bool synchronous) { m_framesSynchronous = synchronous; }

    /// The keymap commands runCommand() runs (the command palette lists them as available).
    static bool handlesCommand(const QString& id);
    /// Runs a monitor keymap command; false when it is not one (the caller routes it on). `sourceFocused`: the Source
    /// Monitor has the focus (Export Frame exports its frame then).
    Q_INVOKABLE bool runCommand(const QString& id, bool sourceFocused = false);

    // ---- P1 (#17): the Program Monitor's edit overlay ----
    /// Overlay: the first unlocked video track, from the topmost targeted one (else V1) up, with nothing where the drop
    /// lands - at the playhead, as long as the clip the drop makes (`mediaKey`, a Library media or sequence key, with
    /// its Source Monitor marks; `fromSource`: the Source Monitor's clip, `mediaKey` ignored). "" when every one is
    /// taken there (the overlay then adds a video track on top in the same undo step) or nothing can be dropped.
    Q_INVOKABLE QString overlayTrack(const QString& mediaKey, bool fromSource) const;
    /// Append at End: the timecode where the sequence's last clip ends (00:00:00:00 when it is empty).
    Q_INVOKABLE QString sequenceEndText() const;
    /// Replace: the clip under the playhead on the topmost targeted video track that has one there, else on the
    /// topmost video track that has one ("" when there is none).
    Q_INVOKABLE QString replaceTarget() const;

signals:
    void outputChanged();
    void screensChanged();
    void viewChanged();
    void guidesChanged();
    void guideTemplatesChanged();
    void messageChanged();
    void timeChanged();
    /// The keys and menus ask the host (MonitorHost.qml) for a dialog: "addGuide", "editGuide" (of guide `index`),
    /// "saveGuides", "manageGuides", "exportFrame" (index 1: the Source Monitor's), P1 (#23): "exportFramesAtMarkers"
    /// (index 1: the highlight markers).
    void dialogRequested(const QString& dialog, int index);
    /// P1 (#23): framesBusy, framesProgress or framesStatus changed; Export Frames at Markers finished.
    void framesChanged();
    void framesFinished(bool ok, const QString& message);

private:
    bool nudge(double dx, double dy);
    bool transmitScreenValid() const;
    void storeGuides();
    void storeTemplates();
    /// Hands the snapping setting and the shown guides to ProgramMotion.
    void syncSnapping();
    void setMessage(const QString& message);

    QPointer<PlaybackBridge> m_playback;
    QPointer<MotionBridge> m_motion;
    QPointer<GraphicCanvasBridge> m_canvas;
    /// P1 (#19): the preview settings Toggle Proxies and Playback Resolution change.
    QPointer<MediaReadinessBridge> m_readiness;
    bool m_fullScreen = false;
    bool m_transmit = false;
    int m_transmitScreen = -1;
    bool m_disableInBackground = true;
    bool m_presentWindows = true;
    bool m_foreground = true;

    QPointer<ProjectController> m_controller;
    QPointer<SourceMonitorBridge> m_source;
    bool m_showRulers = false;
    bool m_rulerPercent = false;
    bool m_showGuides = true;
    bool m_lockGuides = false;
    bool m_snap = true;
    bool m_programGrid = false;
    bool m_droppedIndicator = false;
    bool m_overlays = false;
    bool m_audioUnits = false;
    bool m_sourceGrid = false;
    std::vector<guides::Guide> m_guides;
    std::vector<guides::Template> m_templates;
    QString m_message;
    // P1 (#23, 1C): Export Frames at Markers.
    std::unique_ptr<CommandJob> m_framesJob;
    bool m_framesBusy = false;
    double m_framesProgress = -1.0;
    QString m_framesStatus;
    bool m_framesSynchronous = false;
};

struct MonitorBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::MonitorBridge)
    QML_NAMED_ELEMENT(Monitors)
    QML_SINGLETON
public:
    static MonitorBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return MonitorBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
