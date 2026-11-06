#pragma once

// MediaReadinessBridge: progressive media readiness for QML (singleton `MediaReadiness`). Owner: Graphics/playback
// (I.3). The work happens in wabcut_editor (ProxyManager, ResourceGovernor, CacheInspector); this class keeps the
// user's settings, feeds the governor and the queue's priorities from the UI, and reports state to QML:
//
//   * Proxy policy Off / Auto / Always, the proxy format (ProRes Proxy, DNxHR LB, H.264 intra) and size (1/2, 1/4).
//   * Preview quality Full / 1/2 / 1/4 / Auto (Auto: the coarsest resolution that still fills the monitor's pixels)
//     and "Use proxies": both go to the scheduler (decode resolution, proxies) and the program monitor (composite
//     resolution). `qualityBadge` says what the picture really is made of ("Proxy 1/2", "1/2", "Full", ...).
//   * P1 (#16), Premiere's Paused Resolution: `pausedQuality` ("same", "full", "half", "quarter"; default "full") is the
//     resolution while neither the Program nor the Source Monitor plays; `previewDivisor` follows the transport (the
//     parked frame is re-rendered at the paused resolution), `playbackDivisor` and the badge keep the playback one.
//   * Per-media proxy state for the library cards (`mediaStates`), the "Proxies 3/12" pill, Create / Reveal / Delete.
//   * Background work pauses while the transport plays or a timeline gesture runs (the resource governor); the queue
//     ranks media under the playhead, then on the sequence, then selected in the library.
//   * Cache inspector: disk use of proxies, thumbnails, waveforms and analysis results, per-cache limits, cleaning.
//
// Exports never use proxies (exporter::ExportJob resolves originals and reports offline ones).
//
// 0.7, background rendering while idle (editor/IdleRender.h): the red and yellow stretches of the current sequence are
// rendered into the same preview cache by a worker thread (IdleRenderer) while the editor is quiet: no input for the idle
// time, nothing playing, no gesture, export, manual render or proxy job, not on battery, other programs not busy. This
// bridge feeds the worker plain data (the project snapshot, the current sequence and playhead, the activity flags, the
// time of the last input from an application-wide event filter) at its 250 ms poll and reads its status back; it holds the
// worker back (held) after Delete Render Files and Stop until the project changes. The worker runs only on a real platform
// (not the offscreen/minimal test platforms) unless a test allows it. Settings: render/background*, Settings > Media & cache.
//
// 0.1.1: Sequence > Render In to Out (Enter). The project's preview render cache (model/PreviewCache.h, in
// "<project> Preview Files", editor::previewFolderFor) goes to playback with the other preview settings, so the
// monitor plays a rendered file where its segment is still valid (PreviewCache::validAt: an edit that changes the
// segment makes playback composite live again). renderInToOut() runs render.previewInOut off the GUI thread
// (CommandJob) with progress and cancel; previewServices() gives the same cache and renderer to the assistant.
//
// P1 round (#7, #58, stream 2A): Sequence > Render Selection (the selected video clips' ranges, one render.previewInOut
// per merged range in one job), Delete Render Files and Delete Render Files In to Out (render.deletePreviewFiles off the
// GUI thread; the render bar turns red or yellow again), their menu and palette states (commandAvailability), the
// project's render files as a row of the disk caches ("renderFiles": its size; Clean deletes them, never by the size
// limits), and the cache limits kept automatically: a quiet enforce-and-scan 30 s after start and after each proxy batch
// (QSettings cache/autoEnforce), skipped and tried again later while a render, a proxy job, playback or an export runs.
// Fix round 1 (F1A, ARGON-04): only that job deletes (scanCaches() only measures), and only caches no project refers to
// have a limit (thumbnails, waveforms); proxies and analysis results are deleted by their row's Clean alone.

#include "CommandJob.h"

#include <wabcut/editor/CacheInspector.h>
#include <wabcut/editor/IdleRender.h>
#include <wabcut/editor/ProxyManager.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut {
class PreviewCache;
class PreviewRenderer;
} // namespace wabcut

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;
class ProjectLifecycle;
class SourceMonitorBridge;

class MediaReadinessBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(MediaReadiness)
    QML_SINGLETON

    /// "off", "auto" or "always".
    Q_PROPERTY(QString policy READ policy WRITE setPolicy NOTIFY settingsChanged FINAL)
    /// "prores" (ProRes Proxy), "dnxhr" (DNxHR LB) or "h264" (all-intra H.264, NVENC when available).
    Q_PROPERTY(QString proxyFormat READ proxyFormat WRITE setProxyFormat NOTIFY settingsChanged FINAL)
    /// 2 = half resolution, 4 = quarter.
    Q_PROPERTY(int proxyDivisor READ proxyDivisor WRITE setProxyDivisor NOTIFY settingsChanged FINAL)
    Q_PROPERTY(bool useProxies READ useProxies WRITE setUseProxies NOTIFY previewChanged FINAL)
    /// "auto", "full", "half" or "quarter".
    Q_PROPERTY(QString previewQuality READ previewQuality WRITE setPreviewQuality NOTIFY previewChanged FINAL)
    /// Monitor pixels per sequence pixel (set by the monitor panel): drives the Auto quality.
    Q_PROPERTY(double displayScale READ displayScale WRITE setDisplayScale NOTIFY previewChanged FINAL)
    /// The resolution divisor the preview uses now (1, 2 or 4): ProgramMonitor.previewDivisor and decoding. P1 (#16):
    /// the paused one while no monitor plays.
    Q_PROPERTY(int previewDivisor READ previewDivisor NOTIFY previewChanged FINAL)
    /// P1 (#16), Paused Resolution: "same" (as playback), "full", "half" or "quarter" (QSettings preview/pausedQuality).
    Q_PROPERTY(QString pausedQuality READ pausedQuality WRITE setPausedQuality NOTIFY previewChanged FINAL)
    /// P1 (#16): the divisor while playing (previewQuality, Auto resolved): the quality pill and badge show it.
    Q_PROPERTY(int playbackDivisor READ playbackDivisor NOTIFY previewChanged FINAL)
    /// P1 (#16): whether a monitor plays (the Program's transport or the Source Monitor's).
    Q_PROPERTY(bool monitorPlaying READ monitorPlaying NOTIFY previewChanged FINAL)
    /// What the monitor shows, honestly: "Full", "1/2", "Proxy 1/2", "Proxy 1/2 + 1 original", ...
    Q_PROPERTY(QString qualityBadge READ qualityBadge NOTIFY badgeChanged FINAL)
    Q_PROPERTY(QString qualityTooltip READ qualityTooltip NOTIFY badgeChanged FINAL)
    /// "full", "reduced", "proxy" or "offline" (an offline original is shown from its proxy).
    Q_PROPERTY(QString qualityState READ qualityState NOTIFY badgeChanged FINAL)
    /// "Proxies 3/12" while the project wants proxies; "" when it has none.
    Q_PROPERTY(QString statusText READ statusText NOTIFY proxiesChanged FINAL)
    /// "2 queued · paused while playing", "1 failed", ...
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY proxiesChanged FINAL)
    Q_PROPERTY(bool working READ working NOTIFY proxiesChanged FINAL)
    Q_PROPERTY(bool pausedForForeground READ pausedForForeground NOTIFY proxiesChanged FINAL)
    /// { "m12": { state: "generating", progress: 0.42, detail: "...", file: "..." } } for media that has proxy
    /// state (queued, generating, paused, ready, stale, failed).
    Q_PROPERTY(QVariantMap mediaStates READ mediaStates NOTIFY proxiesChanged FINAL)
    /// Where new proxies go ("<project> Proxies").
    Q_PROPERTY(QString proxyFolder READ proxyFolder NOTIFY proxiesChanged FINAL)
    /// P1 (#48), Settings > Media & cache, app-wide (CacheFolders.h). The media cache folder chosen ("" = the default,
    /// defaultMediaCacheFolder); it applies when Wabcut starts, so mediaCacheFolderInUse may still be the old one.
    Q_PROPERTY(QString mediaCacheFolder READ mediaCacheFolder WRITE setMediaCacheFolder NOTIFY locationsChanged FINAL)
    Q_PROPERTY(QString defaultMediaCacheFolder READ defaultMediaCacheFolder CONSTANT FINAL)
    /// The folder the thumbnails and waveforms use now (a production's own media cache wins), native separators.
    Q_PROPERTY(QString mediaCacheFolderInUse READ mediaCacheFolderInUse NOTIFY locationsChanged FINAL)
    /// The AI models and analysis folder chosen ("" = the default, defaultAiCacheFolder); applies when Wabcut starts.
    Q_PROPERTY(QString aiCacheFolder READ aiCacheFolder WRITE setAiCacheFolder NOTIFY locationsChanged FINAL)
    Q_PROPERTY(QString defaultAiCacheFolder READ defaultAiCacheFolder CONSTANT FINAL)
    /// "project" (next to the project) or "folder" (proxyRootFolder): where new proxies go, at once.
    Q_PROPERTY(QString proxyLocation READ proxyLocation WRITE setProxyLocation NOTIFY locationsChanged FINAL)
    Q_PROPERTY(QString proxyRootFolder READ proxyRootFolder WRITE setProxyRootFolder NOTIFY locationsChanged FINAL)
    /// "412 GB free on Data" for the media cache (the chosen folder), the AI cache and the proxy folder; "" unknown.
    Q_PROPERTY(QString mediaCacheFreeText READ mediaCacheFreeText NOTIFY locationsChanged FINAL)
    Q_PROPERTY(QString aiCacheFreeText READ aiCacheFreeText NOTIFY locationsChanged FINAL)
    Q_PROPERTY(QString proxyFreeText READ proxyFreeText NOTIFY locationsChanged FINAL)
    /// A media cache or AI folder change waits for a restart.
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY locationsChanged FINAL)
    /// [{ id, name, folder, bytes, sizeText, files, inUseText, limitBytes, limited, limitText, freeText }] (scanCaches());
    /// `limited`: the row has a size limit (F1A: thumbnails and waveforms only).
    /// P1 (#48): with the proxy folder moved, "proxiesNearProject" lists the proxies left next to the project.
    Q_PROPERTY(QVariantList caches READ caches NOTIFY cachesChanged FINAL)
    Q_PROPERTY(QString cacheTotalText READ cacheTotalText NOTIFY cachesChanged FINAL)
    Q_PROPERTY(bool cacheBusy READ cacheBusy NOTIFY cachesChanged FINAL)
    /// 0.1.1, Render In to Out: running, its progress (0..1, -1 unknown) and its last message ("Cancelled", an error).
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderChanged FINAL)
    Q_PROPERTY(double renderProgress READ renderProgress NOTIFY renderChanged FINAL)
    Q_PROPERTY(QString renderMessage READ renderMessage NOTIFY renderChanged FINAL)
    /// P1 (#7): what the render running (or the last one) covers: "inToOut" or "selection" (the toast and Try Again).
    Q_PROPERTY(QString renderScope READ renderScope NOTIFY renderChanged FINAL)
    /// P1 (#58): the cache limits are kept without opening Settings (QSettings cache/autoEnforce, default true).
    Q_PROPERTY(bool autoEnforce READ autoEnforce WRITE setAutoEnforce NOTIFY settingsChanged FINAL)

    // ---- 0.7: background rendering while idle (editor/IdleRender.h) ----
    /// Render the red and yellow stretches of the current sequence by itself when nothing else happens (QSettings
    /// render/background, default true): Settings > Media & cache > Background rendering, Sequence > Background Rendering.
    Q_PROPERTY(bool backgroundRender READ backgroundRender WRITE setBackgroundRender NOTIFY backgroundSettingsChanged FINAL)
    /// Seconds without input before it starts (3..600, QSettings render/backgroundIdleSeconds, default 10).
    Q_PROPERTY(int backgroundIdleSeconds READ backgroundIdleSeconds WRITE setBackgroundIdleSeconds NOTIFY backgroundSettingsChanged FINAL)
    /// Wait on battery power (render/backgroundPauseOnBattery, default true).
    Q_PROPERTY(bool backgroundPauseOnBattery READ backgroundPauseOnBattery WRITE setBackgroundPauseOnBattery NOTIFY backgroundSettingsChanged FINAL)
    /// Wait while other programs use the CPU or an NVIDIA GPU (render/backgroundPauseWhenBusy, default true).
    Q_PROPERTY(bool backgroundPauseWhenBusy READ backgroundPauseWhenBusy WRITE setBackgroundPauseWhenBusy NOTIFY backgroundSettingsChanged FINAL)
    /// "off", "waiting", "rendering", "paused", "upToDate" or "full" (IdleRenderStatus::Phase).
    Q_PROPERTY(QString backgroundState READ backgroundState NOTIFY backgroundChanged FINAL)
    /// One sentence for the settings page and the background tasks popover: what it does now or what it waits for.
    Q_PROPERTY(QString backgroundText READ backgroundText NOTIFY backgroundChanged FINAL)
    /// The short form of it for a row that already says "Background rendering": "3 segments left", "Paused: playback is running."
    Q_PROPERTY(QString backgroundDetail READ backgroundDetail NOTIFY backgroundChanged FINAL)
    /// A segment is being rendered or waits half rendered (the popover lists it).
    Q_PROPERTY(bool backgroundActive READ backgroundActive NOTIFY backgroundChanged FINAL)
    /// Progress of the segment in hand, 0..1.
    Q_PROPERTY(double backgroundProgress READ backgroundProgress NOTIFY backgroundChanged FINAL)
    /// Segments still to render (the one in hand included), and segments finished since the project was opened.
    Q_PROPERTY(int backgroundRemaining READ backgroundRemaining NOTIFY backgroundChanged FINAL)
    Q_PROPERTY(int backgroundRendered READ backgroundRendered NOTIFY backgroundChanged FINAL)

public:
    MediaReadinessBridge(ProjectLifecycle* lifecycle, PlaybackBridge* playback, QObject* parent = nullptr);
    ~MediaReadinessBridge() override;
    static MediaReadinessBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // ---- C++ ----
    editor::ProxyManager* manager() { return m_manager.get(); }
    /// One poll now (the timer does it every 250 ms): governor, priorities, finished jobs, badge.
    void pollNow();

    QString policy() const;
    void setPolicy(const QString& policy);
    QString proxyFormat() const;
    void setProxyFormat(const QString& format);
    int proxyDivisor() const;
    void setProxyDivisor(int divisor);
    bool useProxies() const { return m_useProxies; }
    void setUseProxies(bool use);
    QString previewQuality() const { return m_quality; }
    void setPreviewQuality(const QString& quality);
    double displayScale() const { return m_displayScale; }
    void setDisplayScale(double scale);
    int previewDivisor() const;
    QString pausedQuality() const { return m_pausedQuality; }
    void setPausedQuality(const QString& quality);
    int playbackDivisor() const;
    bool monitorPlaying() const { return m_monitorPlaying; }
    /// P1 (#16): the Source Monitor's transport counts as playing too (set from the singleton in create(); tests that
    /// build the bridge themselves may pass theirs). Its preview follows Playback's (PlaybackBridge forwards it).
    void setSourceMonitor(SourceMonitorBridge* source);
    QString qualityBadge() const { return m_badge; }
    QString qualityTooltip() const { return m_badgeTooltip; }
    QString qualityState() const { return m_badgeState; }
    QString statusText() const { return m_statusText; }
    QString statusDetail() const { return m_statusDetail; }
    bool working() const { return m_working; }
    bool pausedForForeground() const { return m_pausedForForeground; }
    QVariantMap mediaStates() const { return m_mediaStates; }
    QString proxyFolder() const;
    QString mediaCacheFolder() const;
    void setMediaCacheFolder(const QString& folder);
    QString defaultMediaCacheFolder() const;
    QString mediaCacheFolderInUse() const;
    QString aiCacheFolder() const;
    void setAiCacheFolder(const QString& folder);
    QString defaultAiCacheFolder() const;
    QString proxyLocation() const;
    void setProxyLocation(const QString& location);
    QString proxyRootFolder() const;
    void setProxyRootFolder(const QString& folder);
    QString mediaCacheFreeText() const;
    QString aiCacheFreeText() const;
    QString proxyFreeText() const;
    bool restartNeeded() const;
    QVariantList caches() const { return m_caches; }
    QString cacheTotalText() const { return m_cacheTotal; }
    bool cacheBusy() const { return m_cacheBusy; }
    bool rendering() const { return m_rendering; }
    double renderProgress() const { return m_renderProgress; }
    QString renderMessage() const { return m_renderMessage; }
    QString renderScope() const { return m_renderScope; }
    bool autoEnforce() const;
    void setAutoEnforce(bool on);

    // ---- 0.7: background rendering while idle ----
    bool backgroundRender() const { return m_idleSettings.enabled; }
    void setBackgroundRender(bool on);
    int backgroundIdleSeconds() const { return static_cast<int>(m_idleSettings.idleSeconds); }
    void setBackgroundIdleSeconds(int seconds);
    bool backgroundPauseOnBattery() const { return m_idleSettings.pauseOnBattery; }
    void setBackgroundPauseOnBattery(bool on);
    bool backgroundPauseWhenBusy() const { return m_idleSettings.pauseWhenBusy; }
    void setBackgroundPauseWhenBusy(bool on);
    QString backgroundState() const { return m_bgState; }
    QString backgroundText() const { return m_bgText; }
    QString backgroundDetail() const { return m_bgDetail; }
    bool backgroundActive() const { return m_bgActive; }
    double backgroundProgress() const { return m_bgProgress; }
    int backgroundRemaining() const { return m_bgRemaining; }
    int backgroundRendered() const { return m_bgRendered; }
    /// The worker (null until the first look with the feature on, and never in the offscreen test platform unless a test
    /// allows it): tests change its settings and polling directly.
    editor::IdleRenderer* backgroundRenderer() { return m_idle.get(); }
    /// The application (main.cpp) says that background rendering may run in this process: a process that does not (the test
    /// programs) never starts the worker, whatever the setting says.
    static void setBackgroundRenderAvailable(bool available);
    /// Tests: lets the worker run in a process that has not opted in, with this load probe.
    void setBackgroundRenderAllowedForTests(bool allowed, std::shared_ptr<editor::SystemLoadProbe> probe = {});
    /// Tests: counts as input now (the event filter does it for real input).
    void noteInput(double secondsAgo = 0.0);

    /// P1 (#7, #58): the Sequence menu's render rows this bridge answers (Render Selection, Delete Render Files, Delete
    /// Render Files In to Out), for CommandPalette::commandState: {handled, enabled, reason}.
    static bool handlesCommand(const QString& id);
    QVariantMap commandAvailability(const QString& id);
    /// P1 (#58): the quiet enforce-limits-and-scan job (the timer's). Returns false when it is off, or skipped because
    /// work that may use the caches runs (then it is tried again in a minute).
    bool autoEnforceCaches();
    /// Whether an automatic enforce is waiting on its timer (tests).
    bool autoEnforcePending() const { return m_autoEnforce.isActive(); }
    /// What says an export runs (create() asks the ExportController singleton); tests may set their own.
    void setExportBusyProbe(std::function<bool()> probe) { m_exportBusy = std::move(probe); }

    /// The open project's preview render cache (made when first asked for; a new one when the project file changes).
    std::shared_ptr<PreviewCache> previewCache();
    /// command::PreviewServices over that cache and the export path's renderer (render.previewInOut and friends).
    std::shared_ptr<command::CommandServices> previewServices();
    /// Tests: renders with this renderer instead of editor::makeExportPreviewRenderer, and on the calling thread.
    void setPreviewRendererForTests(std::shared_ptr<PreviewRenderer> renderer);
    /// Tests: renders and deletes render files on the calling thread.
    void setRenderSynchronous(bool synchronous) {
        m_render.setSynchronous(synchronous);
        m_deleteJob.setSynchronous(synchronous);
    }

    // ---- QML ----
    /// Sequence > Render In to Out (Enter): renders the current sequence's red and yellow segments between In and Out
    /// (the whole sequence without marks). Returns "" when it started, else why not.
    Q_INVOKABLE QString renderInToOut();
    /// P1 (#7): Sequence > Render Selection: renders the red and yellow segments under the selected video clips (each
    /// run of overlapping or touching clips once). Returns "" when it started, else why not.
    Q_INVOKABLE QString renderSelection();
    /// Stops the render at its next checkpoint; the segments rendered so far stay.
    Q_INVOKABLE void cancelRender();
    /// 0.7: Sequence > Background Rendering (the checkable row): turns the feature on or off.
    Q_INVOKABLE void toggleBackgroundRender();
    /// 0.7: the background tasks popover's Stop: gives up the segment in hand; nothing is rendered in the background
    /// again until the project changes (the next edit) or the feature is turned off and on.
    Q_INVOKABLE void stopBackgroundRender();
    /// P1 (#7, #58): what Delete Render Files (In to Out) would delete in the current sequence, as the command's dry run
    /// counts it: { enabled, reason, files, bytes, sizeText ("1.2 GB"), sequenceName }. In to Out: the files that meet
    /// [In, Out) (a missing mark is the sequence's start or end; neither set: not enabled).
    Q_INVOKABLE QVariantMap previewFilesInfo(bool inToOut);
    /// P1 (#7, #58): Sequence > Delete Render Files (In to Out): render.deletePreviewFiles on the current sequence, off
    /// the GUI thread; then the monitor and the render bar look again and renderFilesDeleted says what went. Returns ""
    /// when it started, else why not. A file the monitor still has open goes once it is let go (PreviewCache).
    Q_INVOKABLE QString deleteRenderFiles(bool inToOut);
    /// Library > Create proxy (regardless of the policy). Returns how many were queued.
    Q_INVOKABLE int generateProxies(const QStringList& mediaIds);
    /// Library > Delete proxy: the proxy files go (originals untouched); they are not made again automatically.
    Q_INVOKABLE int deleteProxies(const QStringList& mediaIds);
    /// Shows the proxy file in the system file manager.
    Q_INVOKABLE bool revealProxy(const QString& mediaId);
    Q_INVOKABLE bool revealProxyFolder();
    /// The library selection (a priority hint for the queue).
    Q_INVOKABLE void setLibrarySelection(const QStringList& mediaIds);
    /// Re-measures the caches in the background (cachesChanged when done).
    Q_INVOKABLE void scanCaches();
    /// Deletes the files of one cache ("" = every cache that can be rebuilt), never a file the project uses. P1 (#58):
    /// "renderFiles" deletes every render file of the project (render.deletePreviewFiles all); "" leaves them. Fix F2C:
    /// only files Wabcut made are ever deleted (proxies its proxy index records, analysis cache entries); "" leaves
    /// "analysisKept" (AI object mask mattes, imported chat logs: they cannot be rebuilt). The dialog confirms first.
    /// A row of `caches` carries what Clean deletes: cleanableBytes, cleanableFiles, cleanableText; rebuildable;
    /// proxies (a proxy folder's row).
    Q_INVOKABLE void cleanCache(const QString& id);
    /// "1.2 GB" (editor::formatBytes), for the confirmations' totals.
    Q_INVOKABLE QString sizeText(double bytes) const;
    /// 0 = unlimited. Over-limit files (least recently written first) are deleted at the next scan.
    Q_INVOKABLE void setCacheLimit(const QString& id, double bytes);
    /// Opens Settings > Media & cache (Main.qml hosts the dialog).
    Q_INVOKABLE void openSettings();
    /// P1 (#48): opens a cache location ("media", "ai" or "proxies": where it is now) in the file manager.
    Q_INVOKABLE bool revealLocation(const QString& which);

signals:
    void settingsChanged();
    void previewChanged();
    void badgeChanged();
    void proxiesChanged();
    void cachesChanged();
    void settingsRequested();
    /// P1 (#48): a cache location setting changed (or the free space was read again).
    void locationsChanged();
    void renderChanged();
    /// 0.2: the preview cache's files may have changed (a render finished, or the project's cache was made anew): the
    /// timeline's render bar looks again.
    void previewFilesChanged();
    /// UX rework (L): Render In to Out ended (after renderChanged): `outcome` "done", "cancelled" or "failed";
    /// `message` is renderMessage ("" when done). For the editor's toast.
    void renderFinished(const QString& outcome, const QString& message);
    /// P1 (#29, stream 1C): Create proxy was not started: the proxy folder's drive has less room than the proxies need
    /// (x 1.2). Render In to Out says the same through renderFinished("failed", ...).
    void diskSpaceLow(const QString& message);
    /// P1 (#7, #58): Delete Render Files ended: `ok`, and "Deleted 1.2 GB of render files (12 files)." or the error.
    void renderFilesDeleted(bool ok, const QString& message);
    /// 0.7: the background rendering settings changed.
    void backgroundSettingsChanged();
    /// 0.7: what the background rendering does changed (state, text, progress, counts).
    void backgroundChanged();

public:
    /// 0.7 (AI video effects): processed pictures were made or deleted - the monitor evaluates the held picture and the
    /// lookahead again so they show them.
    Q_INVOKABLE void aiPicturesChanged();

private:
    /// 0.7: feeds the worker the editor's state (the poll's) and reads its status back; makes / resets it as needed.
    void updateBackgroundRender();
    void saveBackgroundSettings();
    QString backgroundStatusText(const editor::IdleRenderStatus& status, bool brief = false) const;
    /// Whether this process may run the worker (main.cpp opted in, or a test allows it).
    bool backgroundMayRun() const;
    void attachDocument();
    void applyPreview(bool force = false);
    void ensurePreviewCache();
    void updateProxyState();
    void updateBadge();
    void updateFocus();
    void updatePlaying();
    void configureCaches();
    /// P1 (#48): the proxy folder setting reaches ProxyManager; the caches are measured again.
    void applyProxyRoot();
    void runCacheJob(std::function<std::vector<editor::CacheUsage>(editor::CacheInspector&)> job);
    /// P1 (#7): Render In to Out (no ranges: the command's In-Out default) or Render Selection (these ranges).
    QString startRender(const std::vector<TimeRange>& ranges, const QString& scope);
    /// P1 (#7): the selected video clips' timeline ranges, overlapping or touching ones merged, in time order.
    std::vector<TimeRange> selectionRanges() const;
    /// P1 (#7, #58): runs render.deletePreviewFiles with `params` off the GUI thread ("" = started).
    QString startDelete(command::Json params);
    /// P1 (#58): the "renderFiles" cache row (the project's render files, from the preview cache's index).
    QVariantMap renderFilesRow();
    /// Puts a fresh "renderFiles" row into `caches` once they were measured.
    void updateRenderFilesRow();
    /// P1 (#58): a render, a render-file delete, a proxy job, a cache job, playback or an export runs.
    bool cacheWorkRunning() const;

    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<PlaybackBridge> m_playback;
    std::unique_ptr<editor::ProxyManager> m_manager;
    std::shared_ptr<editor::ResourceGovernor> m_governor = std::make_shared<editor::ResourceGovernor>();
    editor::CacheInspector m_inspector;
    /// Cache scans run on the thread pool; they report back only while the bridge lives.
    struct CacheGate {
        std::mutex mutex;
        MediaReadinessBridge* bridge = nullptr;
    };
    std::shared_ptr<CacheGate> m_cacheGate = std::make_shared<CacheGate>();
    QTimer m_poll;
    std::uint64_t m_seenRevision = ~std::uint64_t{0};
    QStringList m_selection;

    bool m_useProxies = true;
    QString m_quality = QStringLiteral("auto");
    QString m_pausedQuality = QStringLiteral("full");
    QPointer<SourceMonitorBridge> m_source;
    bool m_monitorPlaying = false;
    double m_displayScale = 1.0;
    std::shared_ptr<const playback::ProxyTable> m_appliedTable;
    int m_appliedDivisor = 0;
    bool m_appliedUse = false;

    QString m_badge;
    QString m_badgeTooltip;
    QString m_badgeState;
    QString m_statusText;
    QString m_statusDetail;
    bool m_working = false;
    bool m_pausedForForeground = false;
    QVariantMap m_mediaStates;
    QVariantList m_caches;
    QString m_cacheTotal;
    /// P1 (#48): the media cache and AI cache folders this process started with (a change waits for a restart).
    QString m_startMediaCache;
    QString m_startAiCache;
    bool m_cacheBusy = false;
    /// P1 (#58): the measured caches' bytes without the render files (the total adds them).
    std::int64_t m_measuredBytes = 0;
    QTimer m_autoEnforce;
    std::function<bool()> m_exportBusy;

    std::shared_ptr<PreviewCache> m_previews;
    std::filesystem::path m_previewFolder;
    std::filesystem::path m_previewProject;
    std::shared_ptr<PreviewRenderer> m_renderer;
    std::shared_ptr<PreviewRenderer> m_testRenderer;
    std::shared_ptr<PreviewCache> m_appliedPreviews;
    std::uint64_t m_aiRevision = 0;
    CommandJob m_render;
    bool m_rendering = false;
    double m_renderProgress = -1.0;
    QString m_renderMessage;
    QString m_renderScope = QStringLiteral("inToOut");
    CommandJob m_deleteJob;

    // ---- 0.7: background rendering while idle ----
    /// What the worker thread reads: the editor's focus, the activity flags and the time of the last input. Written by
    /// the GUI thread (the poll, the event filter), never holds a pointer into the GUI's objects.
    struct IdleShared {
        std::mutex mutex;
        editor::IdleRenderFocus focus;
        std::atomic<bool> playing{false};
        std::atomic<bool> gesture{false};
        std::atomic<bool> exporting{false};
        std::atomic<bool> otherWork{false};
        std::atomic<bool> held{false};
        std::atomic<std::int64_t> lastInputNs{0};
    };
    std::shared_ptr<IdleShared> m_idleShared = std::make_shared<IdleShared>();
    /// What render.status shows under "background" (command::PreviewServices::backgroundStatus): written by the GUI thread
    /// when the status changes, read by whichever thread runs the command.
    struct BackgroundBox {
        std::mutex mutex;
        command::Json json = command::Json::object();
    };
    std::shared_ptr<BackgroundBox> m_bgBox = std::make_shared<BackgroundBox>();
    std::unique_ptr<QObject> m_inputWatcher; ///< the application-wide event filter that notes input
    std::unique_ptr<editor::IdleRenderer> m_idle;
    std::shared_ptr<PreviewCache> m_idleCache; ///< the cache the worker was made / reset for
    std::shared_ptr<editor::SystemLoadProbe> m_idleProbe;
    editor::IdleRenderSettings m_idleSettings;
    bool m_idleAllowedForTests = false;
    bool m_idleHeld = false;
    std::uint64_t m_idleHeldRevision = 0;
    std::uint64_t m_idleSeenGeneration = ~std::uint64_t{0};
    int m_idleSeenRendered = 0;
    QString m_bgState = QStringLiteral("off");
    QString m_bgText;
    QString m_bgDetail;
    bool m_bgActive = false;
    double m_bgProgress = 0.0;
    int m_bgRemaining = 0;
    int m_bgRendered = 0;
};

} // namespace wabcut::app
