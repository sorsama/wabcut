#pragma once

// MediaReadinessBridge: progressive media readiness for QML (singleton `MediaReadiness`). Owner: Graphics/playback
// (I.3). The work happens in wabcut_editor (ProxyManager, ResourceGovernor, CacheInspector); this class keeps the
// user's settings, feeds the governor and the queue's priorities from the UI, and reports state to QML:
//
//   * Proxy policy Off / Auto / Always, the proxy format (ProRes Proxy, DNxHR LB, H.264 intra) and size (1/2, 1/4).
//   * Preview quality Full / 1/2 / 1/4 / Auto (Auto: the coarsest resolution that still fills the monitor's pixels)
//     and "Use proxies": both go to the scheduler (decode resolution, proxies) and the program monitor (composite
//     resolution). `qualityBadge` says what the picture really is made of ("Proxy 1/2", "1/2", "Full", ...).
//   * Per-media proxy state for the library cards (`mediaStates`), the "Proxies 3/12" pill, Create / Reveal / Delete.
//   * Background work pauses while the transport plays or a timeline gesture runs (the resource governor); the queue
//     ranks media under the playhead, then on the sequence, then selected in the library.
//   * Cache inspector: disk use of proxies, thumbnails, waveforms and analysis results, per-cache limits, cleaning.
//
// Exports never use proxies (exporter::ExportJob resolves originals and reports offline ones).
//
// 0.1.1: Sequence > Render In to Out (Enter). The project's preview render cache (model/PreviewCache.h, in
// "<project> Preview Files", editor::previewFolderFor) goes to playback with the other preview settings, so the
// monitor plays a rendered file where its segment is still valid (PreviewCache::validAt: an edit that changes the
// segment makes playback composite live again). renderInToOut() runs render.previewInOut off the GUI thread
// (CommandJob) with progress and cancel; previewServices() gives the same cache and renderer to the assistant.

#include "CommandJob.h"

#include <wabcut/editor/CacheInspector.h>
#include <wabcut/editor/ProxyManager.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <memory>
#include <mutex>

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
    /// The resolution divisor the preview uses now (1, 2 or 4): ProgramMonitor.previewDivisor and decoding.
    Q_PROPERTY(int previewDivisor READ previewDivisor NOTIFY previewChanged FINAL)
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
    /// [{ id, name, folder, bytes, sizeText, files, inUseText, limitBytes, limitText }] (scanCaches()).
    Q_PROPERTY(QVariantList caches READ caches NOTIFY cachesChanged FINAL)
    Q_PROPERTY(QString cacheTotalText READ cacheTotalText NOTIFY cachesChanged FINAL)
    Q_PROPERTY(bool cacheBusy READ cacheBusy NOTIFY cachesChanged FINAL)
    /// 0.1.1, Render In to Out: running, its progress (0..1, -1 unknown) and its last message ("Cancelled", an error).
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderChanged FINAL)
    Q_PROPERTY(double renderProgress READ renderProgress NOTIFY renderChanged FINAL)
    Q_PROPERTY(QString renderMessage READ renderMessage NOTIFY renderChanged FINAL)

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
    QString qualityBadge() const { return m_badge; }
    QString qualityTooltip() const { return m_badgeTooltip; }
    QString qualityState() const { return m_badgeState; }
    QString statusText() const { return m_statusText; }
    QString statusDetail() const { return m_statusDetail; }
    bool working() const { return m_working; }
    bool pausedForForeground() const { return m_pausedForForeground; }
    QVariantMap mediaStates() const { return m_mediaStates; }
    QString proxyFolder() const;
    QVariantList caches() const { return m_caches; }
    QString cacheTotalText() const { return m_cacheTotal; }
    bool cacheBusy() const { return m_cacheBusy; }
    bool rendering() const { return m_rendering; }
    double renderProgress() const { return m_renderProgress; }
    QString renderMessage() const { return m_renderMessage; }

    /// The open project's preview render cache (made when first asked for; a new one when the project file changes).
    std::shared_ptr<PreviewCache> previewCache();
    /// command::PreviewServices over that cache and the export path's renderer (render.previewInOut and friends).
    std::shared_ptr<command::CommandServices> previewServices();
    /// Tests: renders with this renderer instead of editor::makeExportPreviewRenderer, and on the calling thread.
    void setPreviewRendererForTests(std::shared_ptr<PreviewRenderer> renderer);
    void setRenderSynchronous(bool synchronous) { m_render.setSynchronous(synchronous); }

    // ---- QML ----
    /// Sequence > Render In to Out (Enter): renders the current sequence's red and yellow segments between In and Out
    /// (the whole sequence without marks). Returns "" when it started, else why not.
    Q_INVOKABLE QString renderInToOut();
    /// Stops the render at its next checkpoint; the segments rendered so far stay.
    Q_INVOKABLE void cancelRender();
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
    /// Deletes the regenerable files of one cache ("" = every cache), never a proxy the project uses.
    Q_INVOKABLE void cleanCache(const QString& id);
    /// 0 = unlimited. Over-limit files (least recently written first) are deleted at the next scan.
    Q_INVOKABLE void setCacheLimit(const QString& id, double bytes);
    /// Opens Settings > Media & cache (Main.qml hosts the dialog).
    Q_INVOKABLE void openSettings();

signals:
    void settingsChanged();
    void previewChanged();
    void badgeChanged();
    void proxiesChanged();
    void cachesChanged();
    void settingsRequested();
    void renderChanged();

private:
    void attachDocument();
    void applyPreview(bool force = false);
    void ensurePreviewCache();
    void updateProxyState();
    void updateBadge();
    void updateFocus();
    void configureCaches();
    void runCacheJob(std::function<std::vector<editor::CacheUsage>(editor::CacheInspector&)> job);

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
    bool m_cacheBusy = false;

    std::shared_ptr<PreviewCache> m_previews;
    std::filesystem::path m_previewFolder;
    std::filesystem::path m_previewProject;
    std::shared_ptr<PreviewRenderer> m_renderer;
    std::shared_ptr<PreviewRenderer> m_testRenderer;
    std::shared_ptr<PreviewCache> m_appliedPreviews;
    CommandJob m_render;
    bool m_rendering = false;
    double m_renderProgress = -1.0;
    QString m_renderMessage;
};

} // namespace wabcut::app
