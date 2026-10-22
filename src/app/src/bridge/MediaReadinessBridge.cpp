#include "MediaReadinessBridge.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/editor/PreviewRender.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/PreviewCache.h>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>

namespace wabcut::app {

namespace {

constexpr int kPollMs = 250;

QString toQString(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.generic_u16string());
}

std::filesystem::path toPath(const QString& s) {
    return std::filesystem::path(s.toStdU16String());
}

exporter::ProxyCodec codecOf(const QString& format) {
    if (format == QLatin1String("dnxhr")) {
        return exporter::ProxyCodec::DnxhrLb;
    }
    if (format == QLatin1String("h264")) {
        return exporter::ProxyCodec::H264Intra;
    }
    return exporter::ProxyCodec::ProResProxy;
}

QString formatOf(exporter::ProxyCodec codec) {
    switch (codec) {
    case exporter::ProxyCodec::DnxhrLb:
        return QStringLiteral("dnxhr");
    case exporter::ProxyCodec::H264Intra:
        return QStringLiteral("h264");
    case exporter::ProxyCodec::ProResProxy:
        break;
    }
    return QStringLiteral("prores");
}

std::optional<MediaId> parseMediaKey(const QString& text) {
    return detail::parseId<MediaId>(QLatin1Char('m'), text);
}

std::vector<MediaId> parseIds(const QStringList& ids) {
    std::vector<MediaId> out;
    for (const QString& id : ids) {
        if (const auto media = parseMediaKey(id)) {
            out.push_back(*media);
        }
    }
    return out;
}

} // namespace

MediaReadinessBridge::MediaReadinessBridge(ProjectLifecycle* lifecycle, PlaybackBridge* playback, QObject* parent)
    : QObject(parent), m_lifecycle(lifecycle), m_playback(playback) {
    m_cacheGate->bridge = this;
    QSettings settings;
    m_useProxies = settings.value(QStringLiteral("preview/useProxies"), true).toBool();
    const QString quality = settings.value(QStringLiteral("preview/quality"), QStringLiteral("auto")).toString();
    m_quality = QStringList{QStringLiteral("auto"), QStringLiteral("full"), QStringLiteral("half"), QStringLiteral("quarter")}
                        .contains(quality)
                    ? quality
                    : QStringLiteral("auto");

    if (m_lifecycle) {
        editor::ProxyManagerOptions options;
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        options.untitledProxyRoot = toPath(cache);
        options.governor = m_governor;
        options.policy = editor::parsePolicy(settings.value(QStringLiteral("proxies/policy"), QStringLiteral("auto"))
                                                 .toString()
                                                 .toStdString())
                             .value_or(editor::ProxyPolicy::Auto);
        options.settings.codec = codecOf(settings.value(QStringLiteral("proxies/format"), QStringLiteral("prores")).toString());
        options.settings.divisor = settings.value(QStringLiteral("proxies/divisor"), 2).toInt() == 4 ? 4 : 2;
        m_manager = std::make_unique<editor::ProxyManager>(m_lifecycle->document(), std::move(options));
        connect(m_lifecycle, &ProjectLifecycle::documentChanged, this, &MediaReadinessBridge::attachDocument);
        // 0.2 (P.3): the production's Production Settings may keep the previews in another folder.
        connect(m_lifecycle, &ProjectLifecycle::scratchDisksChanged, this, [this] {
            if (m_render.running()) {
                return; // the render finishing reapplies its cache
            }
            m_previewProject.clear();
            applyPreview(true);
        });
        connect(m_lifecycle, &ProjectLifecycle::mediaStatusChanged, this, [this] {
            if (m_manager) {
                m_manager->refresh(true);
            }
        });
    }
    configureCaches();
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &MediaReadinessBridge::pollNow);
    m_poll.start();
    attachDocument();
}

MediaReadinessBridge::~MediaReadinessBridge() {
    {
        std::lock_guard lock(m_cacheGate->mutex);
        m_cacheGate->bridge = nullptr;
    }
    m_poll.stop();
    m_render.stop();   // joins the render worker
    m_manager.reset(); // joins the proxy worker
}

MediaReadinessBridge* MediaReadinessBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* lifecycle = qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* playback = qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    return new MediaReadinessBridge(lifecycle, playback);
}

void MediaReadinessBridge::attachDocument() {
    if (m_manager) {
        m_manager->refresh(true);
    }
    configureCaches();
    applyPreview();
    updateProxyState();
    updateBadge();
}

// ---- Settings ---------------------------------------------------------------------------------------------------

QString MediaReadinessBridge::policy() const {
    return m_manager ? QString::fromLatin1(editor::policyName(m_manager->policy()).data()) : QStringLiteral("auto");
}

void MediaReadinessBridge::setPolicy(const QString& policy) {
    const auto parsed = editor::parsePolicy(policy.toStdString());
    if (!parsed || !m_manager || *parsed == m_manager->policy()) {
        return;
    }
    QSettings().setValue(QStringLiteral("proxies/policy"), policy);
    m_manager->setPolicy(*parsed);
    emit settingsChanged();
    updateProxyState();
}

QString MediaReadinessBridge::proxyFormat() const {
    return m_manager ? formatOf(m_manager->settings().codec) : QStringLiteral("prores");
}

void MediaReadinessBridge::setProxyFormat(const QString& format) {
    if (!m_manager || format == proxyFormat()) {
        return;
    }
    exporter::ProxySettings s = m_manager->settings();
    s.codec = codecOf(format);
    m_manager->setSettings(s);
    QSettings().setValue(QStringLiteral("proxies/format"), formatOf(s.codec));
    emit settingsChanged();
}

int MediaReadinessBridge::proxyDivisor() const {
    return m_manager ? m_manager->settings().divisor : 2;
}

void MediaReadinessBridge::setProxyDivisor(int divisor) {
    divisor = divisor == 4 ? 4 : 2;
    if (!m_manager || divisor == proxyDivisor()) {
        return;
    }
    exporter::ProxySettings s = m_manager->settings();
    s.divisor = divisor;
    m_manager->setSettings(s);
    QSettings().setValue(QStringLiteral("proxies/divisor"), divisor);
    emit settingsChanged();
}

void MediaReadinessBridge::setUseProxies(bool use) {
    if (use == m_useProxies) {
        return;
    }
    m_useProxies = use;
    QSettings().setValue(QStringLiteral("preview/useProxies"), use);
    emit previewChanged();
    applyPreview();
}

void MediaReadinessBridge::setPreviewQuality(const QString& quality) {
    if (quality == m_quality ||
        !QStringList{QStringLiteral("auto"), QStringLiteral("full"), QStringLiteral("half"), QStringLiteral("quarter")}
             .contains(quality)) {
        return;
    }
    m_quality = quality;
    QSettings().setValue(QStringLiteral("preview/quality"), quality);
    emit previewChanged();
    applyPreview();
    updateBadge();
}

void MediaReadinessBridge::setDisplayScale(double scale) {
    if (!(scale > 0.0) || std::abs(scale - m_displayScale) < 1e-4) {
        return;
    }
    const int before = previewDivisor();
    m_displayScale = scale;
    emit previewChanged();
    if (previewDivisor() != before) {
        applyPreview();
        updateBadge();
    }
}

int MediaReadinessBridge::previewDivisor() const {
    if (m_quality == QLatin1String("full")) {
        return 1;
    }
    if (m_quality == QLatin1String("half")) {
        return 2;
    }
    if (m_quality == QLatin1String("quarter")) {
        return 4;
    }
    // Auto: the coarsest resolution that still has at least one sequence pixel per monitor pixel.
    int d = 1;
    while (d < 4 && 1.0 / (2.0 * d) >= m_displayScale - 1e-3) {
        d *= 2;
    }
    return d;
}

QString MediaReadinessBridge::proxyFolder() const {
    return m_manager ? QDir::toNativeSeparators(toQString(m_manager->proxyFolder())) : QString();
}

// ---- Polling ----------------------------------------------------------------------------------------------------

void MediaReadinessBridge::pollNow() {
    if (!m_manager || !m_lifecycle) {
        return;
    }
    EditSession& session = m_lifecycle->document().session();
    m_governor->setActive(editor::ResourceGovernor::Playback, m_playback && m_playback->playing());
    m_governor->setActive(editor::ResourceGovernor::Gesture, session.inGesture());
    if (session.revision() != m_seenRevision) {
        m_seenRevision = session.revision();
        m_manager->refresh();
    }
    updateFocus();
    if (m_playback && m_playback->session()) {
        m_manager->setLateFrames(m_playback->session()->video().scheduler().stats().lateFramesByMedia);
    }
    m_manager->poll();
    applyPreview();
    updateProxyState();
    updateBadge();
}

void MediaReadinessBridge::updateFocus() {
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    if (!controller || !m_manager) {
        return;
    }
    std::vector<MediaId> under;
    std::vector<MediaId> onTimeline;
    if (const SequencePtr seq = controller->currentSequence()) {
        const Ticks at = m_playback && m_playback->session() ? m_playback->session()->position() : controller->playhead();
        for (const TrackPtr& track : seq->videoTracks) {
            if (!track) {
                continue;
            }
            for (const Clip& clip : track->clips) {
                onTimeline.push_back(clip.media);
            }
            if (const auto index = track->clipIndexAt(at)) {
                under.push_back(track->clips[*index].media);
            }
        }
    }
    m_manager->setFocus(under, onTimeline, parseIds(m_selection));
}

void MediaReadinessBridge::applyPreview(bool force) {
    if (!m_playback) {
        return;
    }
    ensurePreviewCache();
    const auto table = m_manager ? m_manager->table() : nullptr;
    const int divisor = previewDivisor();
    if (!force && table == m_appliedTable && divisor == m_appliedDivisor && m_useProxies == m_appliedUse &&
        m_previews == m_appliedPreviews) {
        return;
    }
    m_appliedTable = table;
    m_appliedDivisor = divisor;
    m_appliedUse = m_useProxies;
    m_appliedPreviews = m_previews;
    playback::PreviewSettings preview;
    preview.useProxies = m_useProxies;
    preview.decodeDivisor = divisor;
    preview.proxies = table;
    // 0.1.1: rendered previews play where still valid. Setting the same cache again (force, after a render) is how the
    // scheduler learns its files changed.
    preview.previews = m_previews;
    m_playback->setPreviewSettings(std::move(preview));
}

// ---- Render In to Out (0.1.1) -----------------------------------------------------------------------------------

void MediaReadinessBridge::ensurePreviewCache() {
    const std::filesystem::path project = m_lifecycle ? toPath(m_lifecycle->filePath()) : std::filesystem::path();
    if (m_previews && project == m_previewProject) {
        return;
    }
    // An untitled project keeps its previews in the app's cache folder until it is saved somewhere.
    const std::filesystem::path folder =
        project.empty() ? toPath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)) / "Untitled Preview Files"
                        : editor::previewFolderFor(project);
    m_previewProject = project;
    m_previewFolder = folder;
    m_previews = std::make_shared<PreviewCache>(folder);
    (void)m_previews->load(); // a missing index is an empty cache; a malformed one leaves it empty
    if (m_testRenderer) {
        m_renderer = m_testRenderer;
    } else {
        editor::PreviewRenderOptions options;
        options.projectFile = project;
        m_renderer = editor::makeExportPreviewRenderer(std::move(options));
    }
    // Queued: callers are often in the middle of reading the cache (the timeline's render bar).
    QMetaObject::invokeMethod(this, &MediaReadinessBridge::previewFilesChanged, Qt::QueuedConnection);
}

std::shared_ptr<PreviewCache> MediaReadinessBridge::previewCache() {
    ensurePreviewCache();
    return m_previews;
}

std::shared_ptr<command::CommandServices> MediaReadinessBridge::previewServices() {
    ensurePreviewCache();
    return std::make_shared<command::PreviewServices>(m_previews, m_renderer);
}

void MediaReadinessBridge::setPreviewRendererForTests(std::shared_ptr<PreviewRenderer> renderer) {
    m_testRenderer = std::move(renderer);
    m_previews.reset(); // made again with it
    m_previewProject.clear();
    ensurePreviewCache();
}

QString MediaReadinessBridge::renderInToOut() {
    if (m_render.running()) {
        return tr("Already rendering");
    }
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    if (!controller) {
        return tr("No project is open");
    }
    const SequenceId sequence = controller->currentSequenceId();
    if (!sequence.isValid()) {
        return tr("Open a sequence first");
    }
    ensurePreviewCache();
    std::error_code ec;
    std::filesystem::create_directories(m_previewFolder, ec);
    if (ec) {
        return tr("The preview folder cannot be made: %1").arg(QString::fromStdString(ec.message()));
    }
    m_rendering = true;
    m_renderProgress = 0.0;
    m_renderMessage = tr("Rendering previews…");
    emit renderChanged();
    QPointer<MediaReadinessBridge> self(this);
    const bool started = m_render.start(
        this, controller, previewServices(), {"render.previewInOut", command::Json{{"sequence", sequence.value}}, "Render In to Out"},
        [self](const QString& text, double progress) {
            if (!self)
                return;
            self->m_renderProgress = progress;
            if (!text.isEmpty())
                self->m_renderMessage = text;
            emit self->renderChanged();
        },
        [self](const command::CommandResult& result) {
            if (!self)
                return;
            self->m_rendering = false;
            self->m_renderProgress = -1.0;
            self->m_renderMessage = result.ok() ? QString() : commandErrorText(result);
            self->applyPreview(true); // the monitor picks up the new files
            emit self->renderChanged();
            emit self->previewFilesChanged(); // the render bar turns green there (0.2)
        });
    if (!started) {
        m_rendering = false;
        m_renderProgress = -1.0;
        m_renderMessage = tr("Already rendering");
        emit renderChanged();
        return m_renderMessage;
    }
    return {};
}

void MediaReadinessBridge::cancelRender() {
    m_render.cancel();
}

void MediaReadinessBridge::updateProxyState() {
    if (!m_manager) {
        return;
    }
    QVariantMap states;
    bool paused = false;
    for (const editor::ProxyStatus& s : m_manager->statuses()) {
        if (s.state == editor::ProxyState::None) {
            continue;
        }
        paused = paused || s.state == editor::ProxyState::Paused;
        states.insert(mediaKey(s.media), QVariantMap{
                                             {QStringLiteral("state"), QString::fromLatin1(editor::stateName(s.state).data())},
                                             {QStringLiteral("progress"), s.progress},
                                             {QStringLiteral("detail"), QString::fromStdString(s.detail)},
                                             {QStringLiteral("file"), QDir::toNativeSeparators(toQString(s.file))},
                                             {QStringLiteral("originalOffline"), s.originalOffline},
                                         });
    }
    const editor::ProxyCounts c = m_manager->counts();
    //: Library status pill: proxies ready / proxies the project wants, e.g. "Proxies 3/12"
    const QString text = c.wanted > 0 ? tr("Proxies %1/%2").arg(c.ready).arg(c.wanted) : QString();
    QStringList parts;
    if (c.generating > 0) {
        if (paused) {
            const unsigned busy = m_governor->activities();
            parts << ((busy & editor::ResourceGovernor::Playback) ? tr("paused while playing")
                      : (busy & editor::ResourceGovernor::Gesture) ? tr("paused while editing")
                                                                   : tr("paused"));
        } else {
            parts << tr("%n generating", "", c.generating);
        }
    }
    if (c.queued > 0) {
        parts << tr("%n queued", "", c.queued);
    }
    if (c.stale > 0) {
        parts << tr("%n out of date", "", c.stale);
    }
    if (c.failed > 0) {
        parts << tr("%n failed", "", c.failed);
    }
    const QString detail = parts.join(QStringLiteral(" · "));
    const bool working = c.generating + c.queued > 0;
    if (states != m_mediaStates || text != m_statusText || detail != m_statusDetail || working != m_working ||
        paused != m_pausedForForeground) {
        m_mediaStates = std::move(states);
        m_statusText = text;
        m_statusDetail = detail;
        m_working = working;
        m_pausedForForeground = paused;
        emit proxiesChanged();
    }
}

void MediaReadinessBridge::updateBadge() {
    playback::PlaybackStats s;
    if (m_playback) {
        if (playback::PlaybackController* controller = m_playback->activeController()) {
            s = controller->stats();
        }
    }
    const int preview = previewDivisor();
    const bool decoded = s.proxyLayers + s.originalLayers > 0;
    const int res = decoded ? std::max(preview, s.decodeDivisor) : preview;
    const QString resText = res == 1 ? tr("Full") : QStringLiteral("1/%1").arg(res);
    QString badge;
    QString state;
    if (s.proxyLayers > 0) {
        //: Monitor quality badge: the picture comes from proxies at this resolution, e.g. "Proxy 1/2"
        badge = tr("Proxy %1").arg(resText);
        if (s.originalLayers > 0) {
            badge += QStringLiteral(" + ") + tr("%n original(s)", "", s.originalLayers);
        }
        state = s.offlineProxyLayers > 0 ? QStringLiteral("offline") : QStringLiteral("proxy");
    } else {
        badge = resText;
        state = res == 1 ? QStringLiteral("full") : QStringLiteral("reduced");
    }
    QStringList tip;
    const QString setting = m_quality == QLatin1String("auto")   ? tr("Auto (%1)").arg(preview == 1 ? tr("Full") : QStringLiteral("1/%1").arg(preview))
                            : m_quality == QLatin1String("full") ? tr("Full")
                            : m_quality == QLatin1String("half") ? QStringLiteral("1/2")
                                                                 : QStringLiteral("1/4");
    tip << tr("Preview quality: %1").arg(setting);
    if (decoded) {
        tip << tr("Decoded from %1 proxy layer(s) and %2 original layer(s), at %3 of full resolution")
                   .arg(s.proxyLayers)
                   .arg(s.originalLayers)
                   .arg(res == 1 ? tr("all") : QStringLiteral("1/%1").arg(res));
    }
    if (s.offlineProxyLayers > 0) {
        tip << tr("%n original(s) offline: shown from the proxy. Relink before exporting.", "", s.offlineProxyLayers);
    }
    tip << tr("Exports always use the original media at full quality.");
    const QString tooltip = tip.join(QLatin1Char('\n'));
    if (badge != m_badge || tooltip != m_badgeTooltip || state != m_badgeState) {
        m_badge = badge;
        m_badgeTooltip = tooltip;
        m_badgeState = state;
        emit badgeChanged();
    }
}

// ---- Actions ----------------------------------------------------------------------------------------------------

int MediaReadinessBridge::generateProxies(const QStringList& mediaIds) {
    if (!m_manager) {
        return 0;
    }
    const int queued = m_manager->generate(parseIds(mediaIds));
    updateProxyState();
    return queued;
}

int MediaReadinessBridge::deleteProxies(const QStringList& mediaIds) {
    if (!m_manager) {
        return 0;
    }
    const int deleted = m_manager->deleteProxies(parseIds(mediaIds));
    applyPreview();
    updateProxyState();
    return deleted;
}

bool MediaReadinessBridge::revealProxy(const QString& mediaId) {
    const auto media = parseMediaKey(mediaId);
    if (!m_manager || !media) {
        return false;
    }
    const auto file = m_manager->proxyFile(*media);
    if (!file) {
        return false;
    }
    const QString path = QDir::toNativeSeparators(toQString(*file));
#if defined(Q_OS_WIN)
    return QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + path});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

bool MediaReadinessBridge::revealProxyFolder() {
    if (!m_manager) {
        return false;
    }
    const QString folder = toQString(m_manager->proxyFolder());
    QDir().mkpath(folder);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void MediaReadinessBridge::setLibrarySelection(const QStringList& mediaIds) {
    m_selection = mediaIds;
    updateFocus();
}

void MediaReadinessBridge::openSettings() {
    emit settingsRequested();
}

// ---- Caches -----------------------------------------------------------------------------------------------------

void MediaReadinessBridge::configureCaches() {
    QSettings settings;
    const auto limit = [&](const char* id) {
        return settings.value(QStringLiteral("cache/limit/") + QLatin1String(id), 0).toLongLong();
    };
    const QString media = m_lifecycle && m_lifecycle->controller() ? m_lifecycle->controller()->mediaCacheDirectory()
                                                                   : QString();
    std::vector<editor::CacheLocation> locations;
    locations.push_back({"proxies", tr("Proxies").toStdString(), m_manager ? m_manager->proxyFolder() : std::filesystem::path{},
                         limit("proxies"), {".mov"}});
    locations.push_back({"thumbnails", tr("Thumbnails").toStdString(),
                         media.isEmpty() ? std::filesystem::path{} : toPath(media) / "thumbnails", limit("thumbnails"),
                         {".wthumb"}});
    locations.push_back({"peaks", tr("Waveforms").toStdString(),
                         media.isEmpty() ? std::filesystem::path{} : toPath(media) / "peaks", limit("peaks"),
                         {".wpeaks"}});
    locations.push_back({"analysis", tr("Analysis results").toStdString(), ai::userCacheDir() / "analysis",
                         limit("analysis"), {}});
    m_inspector.setLocations(std::move(locations));
}

void MediaReadinessBridge::runCacheJob(std::function<std::vector<editor::CacheUsage>(editor::CacheInspector&)> job) {
    configureCaches();
    m_cacheBusy = true;
    emit cachesChanged();
    editor::CacheInspector inspector = m_inspector;
    QThreadPool::globalInstance()->start([gate = m_cacheGate, inspector, job = std::move(job)]() mutable {
        std::vector<editor::CacheUsage> usage = job(inspector);
        std::lock_guard lock(gate->mutex);
        MediaReadinessBridge* self = gate->bridge;
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, usage = std::move(usage)] {
                QVariantList list;
                std::int64_t total = 0;
                for (const editor::CacheUsage& u : usage) {
                    total += u.bytes;
                    list.append(QVariantMap{
                        {QStringLiteral("id"), QString::fromStdString(u.id)},
                        {QStringLiteral("name"), QString::fromStdString(u.name)},
                        {QStringLiteral("folder"), QDir::toNativeSeparators(toQString(u.dir))},
                        {QStringLiteral("bytes"), static_cast<double>(u.bytes)},
                        {QStringLiteral("sizeText"), QString::fromStdString(editor::formatBytes(u.bytes))},
                        {QStringLiteral("files"), static_cast<double>(u.files)},
                        {QStringLiteral("inUseText"),
                         u.inUseBytes > 0 ? tr("%1 in use").arg(QString::fromStdString(editor::formatBytes(u.inUseBytes)))
                                          : QString()},
                        {QStringLiteral("limitBytes"), static_cast<double>(u.limitBytes)},
                        {QStringLiteral("limitText"), u.limitBytes > 0
                                                          ? QString::fromStdString(editor::formatBytes(u.limitBytes))
                                                          : tr("No limit")},
                    });
                }
                self->m_caches = std::move(list);
                self->m_cacheTotal = QString::fromStdString(editor::formatBytes(total));
                self->m_cacheBusy = false;
                emit self->cachesChanged();
            },
            Qt::QueuedConnection);
    });
}

void MediaReadinessBridge::scanCaches() {
    const auto inUse = m_manager ? m_manager->filesInUse() : std::set<std::filesystem::path>{};
    runCacheJob([inUse](editor::CacheInspector& inspector) {
        inspector.enforceLimits(inUse);
        return inspector.scan(inUse);
    });
}

void MediaReadinessBridge::cleanCache(const QString& id) {
    const auto inUse = m_manager ? m_manager->filesInUse() : std::set<std::filesystem::path>{};
    runCacheJob([inUse, id = id.toStdString()](editor::CacheInspector& inspector) {
        inspector.clean(id, inUse);
        return inspector.scan(inUse);
    });
}

void MediaReadinessBridge::setCacheLimit(const QString& id, double bytes) {
    QSettings().setValue(QStringLiteral("cache/limit/") + id, static_cast<qlonglong>(std::max(0.0, bytes)));
    scanCaches();
}

} // namespace wabcut::app
