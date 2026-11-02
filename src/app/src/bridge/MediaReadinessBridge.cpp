#include "MediaReadinessBridge.h"

#include "BridgeIds.h"
#include "CacheFolders.h"
#include "DiskSpace.h"
#include "ExportController.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
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
// P1 (#58): the automatic cache limits: after start, after a proxy batch, and again while other work ran.
constexpr int kAutoEnforceAfterStartMs = 30'000;
constexpr int kAutoEnforceAfterBatchMs = 2'000;
constexpr int kAutoEnforceRetryMs = 60'000;
const QString kAutoEnforceKey = QStringLiteral("cache/autoEnforce");
const QString kRenderFilesId = QStringLiteral("renderFiles");

/// Fix F1A (ARGON-04): the caches whose size limit Wabcut keeps by deleting files: only those no project refers to
/// (thumbnails and waveforms: pictures made from the media, made again when missing). Proxies (project files name their
/// proxies, other projects may share a proxy folder, and a proxy can be the only playable copy while the originals are
/// offline) and analysis results (an AI object mask names its matte there; transcripts and other results serve
/// projects Wabcut cannot see while they are closed) are never deleted by a limit, only by their row's Clean.
bool keptByLimit(const std::string& id) {
    return id == "thumbnails" || id == "peaks";
}

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
    // P1 (#16): Premiere's Paused Resolution defaults to Full.
    const QString paused = settings.value(QStringLiteral("preview/pausedQuality"), QStringLiteral("full")).toString();
    m_pausedQuality = QStringList{QStringLiteral("same"), QStringLiteral("full"), QStringLiteral("half"), QStringLiteral("quarter")}
                              .contains(paused)
                          ? paused
                          : QStringLiteral("full");
    // P1 (#16): the paused resolution starts and ends with the transport (not only at the next poll).
    if (m_playback) {
        connect(m_playback, &PlaybackBridge::transportChanged, this, &MediaReadinessBridge::updatePlaying);
    }

    m_startMediaCache = cachefolders::mediaCacheFolder();
    m_startAiCache = QDir::cleanPath(toQString(ai::userCacheDir()));
    if (m_lifecycle) {
        editor::ProxyManagerOptions options;
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        options.untitledProxyRoot = toPath(cache);
        // P1 (#48): Settings > Media & cache > Proxies: In this folder.
        if (const QString root = cachefolders::proxyRoot(); !root.isEmpty()) {
            options.proxyRoot = toPath(root);
        }
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
    // P1 (#58): the cache limits are kept without Settings being opened: a quiet enforce-and-scan after start.
    m_autoEnforce.setSingleShot(true);
    connect(&m_autoEnforce, &QTimer::timeout, this, [this] { (void)autoEnforceCaches(); });
    if (autoEnforce()) {
        m_autoEnforce.start(kAutoEnforceAfterStartMs);
    }
    attachDocument();
}

MediaReadinessBridge::~MediaReadinessBridge() {
    {
        std::lock_guard lock(m_cacheGate->mutex);
        m_cacheGate->bridge = nullptr;
    }
    m_poll.stop();
    m_autoEnforce.stop();
    m_deleteJob.stop(); // joins a render-file delete
    m_render.stop();   // joins the render worker
    m_manager.reset(); // joins the proxy worker
}

MediaReadinessBridge* MediaReadinessBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* lifecycle = qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* playback = qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    auto* bridge = new MediaReadinessBridge(lifecycle, playback);
    bridge->setSourceMonitor(qmlEngine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor"));
    // P1 (#58): the automatic cache limits wait while an export runs (looked up when asked: the export queue is made
    // by the editor's own first use).
    bridge->setExportBusyProbe([engine = QPointer<QQmlEngine>(qmlEngine)] {
        auto* exports = engine ? engine->singletonInstance<ExportController*>("Wabcut", "ExportController") : nullptr;
        return exports && exports->busy();
    });
    return bridge;
}

void MediaReadinessBridge::setSourceMonitor(SourceMonitorBridge* source) {
    if (m_source == source) {
        return;
    }
    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
    }
    m_source = source;
    if (source) {
        connect(source, &SourceMonitorBridge::transportChanged, this, &MediaReadinessBridge::updatePlaying);
        connect(source, &SourceMonitorBridge::sourceChanged, this, &MediaReadinessBridge::updatePlaying);
    }
    updatePlaying();
}

void MediaReadinessBridge::updatePlaying() {
    const bool playing = (m_playback && m_playback->playing()) || (m_source && m_source->playing());
    if (playing == m_monitorPlaying) {
        return;
    }
    const int before = previewDivisor();
    m_monitorPlaying = playing;
    emit previewChanged();
    if (previewDivisor() != before) {
        applyPreview(); // the scheduler bumps its preview version: the parked frame is drawn again
        updateBadge();
    }
}

void MediaReadinessBridge::attachDocument() {
    if (m_manager) {
        m_manager->refresh(true);
    }
    configureCaches();
    applyPreview();
    updateProxyState();
    updateBadge();
    emit locationsChanged(); // the proxy folder follows the project
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

void MediaReadinessBridge::setPausedQuality(const QString& quality) {
    if (quality == m_pausedQuality ||
        !QStringList{QStringLiteral("same"), QStringLiteral("full"), QStringLiteral("half"), QStringLiteral("quarter")}
             .contains(quality)) {
        return;
    }
    m_pausedQuality = quality;
    QSettings().setValue(QStringLiteral("preview/pausedQuality"), quality);
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
    // P1 (#16): parked, the paused resolution (Premiere's Paused Resolution); "same" keeps the playback one.
    if (!m_monitorPlaying) {
        if (m_pausedQuality == QLatin1String("full")) {
            return 1;
        }
        if (m_pausedQuality == QLatin1String("half")) {
            return 2;
        }
        if (m_pausedQuality == QLatin1String("quarter")) {
            return 4;
        }
    }
    return playbackDivisor();
}

int MediaReadinessBridge::playbackDivisor() const {
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
    updatePlaying(); // P1 (#16): normally already done on the transport's signal
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
    return startRender({}, QStringLiteral("inToOut"));
}

QString MediaReadinessBridge::renderSelection() {
    if (m_render.running()) {
        return tr("Already rendering");
    }
    const std::vector<TimeRange> ranges = selectionRanges();
    if (ranges.empty()) {
        ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
        return controller && controller->currentSequence() ? tr("Select a video clip first") : tr("Open a sequence first");
    }
    return startRender(ranges, QStringLiteral("selection"));
}

std::vector<TimeRange> MediaReadinessBridge::selectionRanges() const {
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    const SequencePtr seq = controller ? controller->currentSequence() : nullptr;
    if (!seq || controller->selection().empty()) {
        return {};
    }
    // Previews are pictures: the video clips count (a linked audio partner adds nothing beyond its video).
    std::vector<TimeRange> ranges;
    for (const TrackPtr& track : seq->videoTracks) {
        if (!track) {
            continue;
        }
        for (const Clip& clip : track->clips) {
            if (controller->isSelected(clip.id) && clip.timelineRange.duration > Ticks{}) {
                ranges.push_back(clip.timelineRange);
            }
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : ranges) {
        if (!merged.empty() && r.start <= merged.back().end()) {
            merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

QString MediaReadinessBridge::startRender(const std::vector<TimeRange>& ranges, const QString& scope) {
    if (m_render.running()) {
        return tr("Already rendering");
    }
    if (m_deleteJob.running()) {
        return tr("Render files are being deleted");
    }
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    if (!controller) {
        return tr("No project is open");
    }
    const SequenceId sequence = controller->currentSequenceId();
    if (!sequence.isValid()) {
        return tr("Open a sequence first");
    }
    if (m_renderScope != scope) {
        m_renderScope = scope;
        emit renderChanged();
    }
    ensurePreviewCache();
    std::error_code ec;
    std::filesystem::create_directories(m_previewFolder, ec);
    if (ec) {
        return tr("The preview folder cannot be made: %1").arg(QString::fromStdString(ec.message()));
    }
    // P1 (#29, stream 1C): the previews (ProRes LT, about 102 Mb/s at 1080p30, scaled by the pixel rate) between In and
    // Out need room on the preview folder's drive; when it is short the render does not start, and the toast says why.
    if (const SequencePtr seq = controller->currentSequence()) {
        double seconds = 0.0;
        if (ranges.empty()) {
            const Ticks in = seq->markIn.value_or(Ticks{});
            const Ticks out = seq->markOut.value_or(std::max(seq->duration, seq->contentEnd()));
            seconds = std::max(0.0, (out - in).toSeconds());
        } else {
            for (const TimeRange& r : ranges) { // P1 (#7): Render Selection's ranges
                seconds += std::max(0.0, r.duration.toSeconds());
            }
        }
        const double pixelRate = static_cast<double>(seq->width) * seq->height * seq->frameRate.toDouble();
        const double bytes = 102e6 * pixelRate / (1920.0 * 1080.0 * 29.97) * seconds / 8.0;
        const QString low = diskspace::lowSpaceMessage(toQString(m_previewFolder), bytes, tr("the previews"));
        if (!low.isEmpty()) {
            m_renderMessage = low;
            emit renderChanged();
            emit renderFinished(QStringLiteral("failed"), low);
            return low;
        }
    }
    m_rendering = true;
    m_renderProgress = 0.0;
    m_renderMessage = tr("Rendering previews…");
    emit renderChanged();
    QPointer<MediaReadinessBridge> self(this);
    // P1 (#7): Render Selection is one render.previewInOut per range, in one job (one progress, one Cancel).
    std::vector<CommandJob::Request> requests;
    if (ranges.empty()) {
        requests.push_back({"render.previewInOut", command::Json{{"sequence", sequence.value}}, "Render In to Out"});
    } else {
        for (const TimeRange& r : ranges) {
            requests.push_back({"render.previewInOut",
                                command::Json{{"sequence", sequence.value}, {"start", r.start.value}, {"end", r.end().value}},
                                "Render Selection"});
        }
    }
    const bool started = m_render.startBatch(
        this, controller, previewServices(), std::move(requests),
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
            self->updateRenderFilesRow();     // P1 (#58): Settings > Media & cache shows their size
            // UX rework (L): the toast's cue.
            const QString outcome = result.ok() ? QStringLiteral("done")
                                    : result.error->code == command::ErrorCode::Cancelled ? QStringLiteral("cancelled")
                                                                                          : QStringLiteral("failed");
            emit self->renderFinished(outcome, self->m_renderMessage);
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

// ---- Delete Render Files (P1 #7, #58) ---------------------------------------------------------------------------

QVariantMap MediaReadinessBridge::previewFilesInfo(bool inToOut) {
    QVariantMap info{{QStringLiteral("enabled"), false}, {QStringLiteral("reason"), QString()},
                     {QStringLiteral("files"), 0},      {QStringLiteral("bytes"), 0.0},
                     {QStringLiteral("sizeText"), QString::fromStdString(editor::formatBytes(0))},
                     {QStringLiteral("sequenceName"), QString()}};
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    const SequencePtr seq = controller ? controller->currentSequence() : nullptr;
    if (!seq) {
        info[QStringLiteral("reason")] = tr("Open a sequence first");
        return info;
    }
    info[QStringLiteral("sequenceName")] = QString::fromStdString(seq->name);
    std::optional<TimeRange> range;
    if (inToOut) {
        if (!seq->markIn && !seq->markOut) {
            info[QStringLiteral("reason")] = tr("Set an In or an Out in the sequence first");
            return info;
        }
        // As Render In to Out reads them: a missing mark is the sequence's start or end.
        const Ticks start = seq->markIn.value_or(Ticks{});
        const Ticks end = seq->markOut.value_or(std::max(seq->duration, seq->contentEnd()));
        if (end <= start) {
            info[QStringLiteral("reason")] = tr("The Out mark is before the In mark");
            return info;
        }
        range = TimeRange::fromStartEnd(start, end);
    }
    // What render.deletePreviewFiles deletes (its dry run counts the same entries).
    int files = 0;
    std::int64_t bytes = 0;
    for (const PreviewEntry& e : previewCache()->entries()) {
        if (e.sequence == seq->id && (!range || e.range.overlaps(*range))) {
            ++files;
            bytes += e.bytes;
        }
    }
    info[QStringLiteral("files")] = files;
    info[QStringLiteral("bytes")] = static_cast<double>(bytes);
    info[QStringLiteral("sizeText")] = QString::fromStdString(editor::formatBytes(bytes));
    if (files == 0) {
        info[QStringLiteral("reason")] = inToOut ? tr("There are no render files between In and Out")
                                                 : tr("This sequence has no render files");
        return info;
    }
    if (m_render.running()) {
        info[QStringLiteral("reason")] = tr("Wait for the render to finish");
        return info;
    }
    if (m_deleteJob.running()) {
        info[QStringLiteral("reason")] = tr("Render files are being deleted");
        return info;
    }
    info[QStringLiteral("enabled")] = true;
    return info;
}

QString MediaReadinessBridge::deleteRenderFiles(bool inToOut) {
    const QVariantMap info = previewFilesInfo(inToOut);
    if (!info.value(QStringLiteral("enabled")).toBool()) {
        return info.value(QStringLiteral("reason")).toString();
    }
    ProjectController* controller = m_lifecycle->controller();
    const SequencePtr seq = controller->currentSequence();
    command::Json params{{"sequence", seq->id.value}};
    if (inToOut) {
        params["start"] = seq->markIn.value_or(Ticks{}).value;
        params["end"] = seq->markOut.value_or(std::max(seq->duration, seq->contentEnd())).value;
    }
    return startDelete(std::move(params));
}

QString MediaReadinessBridge::startDelete(command::Json params) {
    if (m_render.running()) {
        return tr("Wait for the render to finish");
    }
    if (m_deleteJob.running()) {
        return tr("Render files are being deleted");
    }
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    if (!controller) {
        return tr("No project is open");
    }
    QPointer<MediaReadinessBridge> self(this);
    // Off the GUI thread: a long sequence can have thousands of files, on a slow drive. PreviewCache is thread-safe; a
    // file the monitor's decoder still has open is deleted once it is let go (PreviewCache::retryDeletes).
    const bool started = m_deleteJob.start(
        this, controller, previewServices(), {"render.deletePreviewFiles", std::move(params), "Delete Render Files"}, {},
        [self](const command::CommandResult& result) {
            if (!self)
                return;
            self->applyPreview(true);         // the monitor composites live where the files went
            emit self->previewFilesChanged(); // the render bar turns red or yellow again
            self->updateRenderFilesRow();
            if (!result.ok()) {
                emit self->renderFilesDeleted(false, tr("The render files could not be deleted: %1").arg(commandErrorText(result)));
                return;
            }
            const command::Json& data = result.data;
            const int files = data.is_object() ? data.value("files", 0) : 0;
            const std::int64_t bytes = data.is_object() ? data.value("bytes", std::int64_t{0}) : 0;
            const QString count = files == 1 ? tr("1 file") : tr("%1 files").arg(files);
            //: %1 is a size such as "1.2 GB", %2 how many files ("12 files")
            emit self->renderFilesDeleted(true, tr("Deleted %1 of render files (%2).")
                                                    .arg(QString::fromStdString(editor::formatBytes(bytes)), count));
        });
    return started ? QString() : tr("Render files are being deleted");
}

bool MediaReadinessBridge::handlesCommand(const QString& id) {
    return id == QLatin1String("sequence.renderSelection") || id == QLatin1String("sequence.deleteRenderFiles") ||
           id == QLatin1String("sequence.deleteRenderFilesInToOut");
}

QVariantMap MediaReadinessBridge::commandAvailability(const QString& id) {
    const auto state = [](bool enabled, const QString& reason) {
        return QVariantMap{{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("reason"), enabled ? QString() : reason}};
    };
    if (!handlesCommand(id)) {
        return {};
    }
    if (id == QLatin1String("sequence.renderSelection")) {
        ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
        if (!controller || !controller->currentSequence()) {
            return state(false, tr("Open a sequence first"));
        }
        if (m_render.running()) {
            return state(false, tr("A render is running"));
        }
        if (m_deleteJob.running()) {
            return state(false, tr("Render files are being deleted"));
        }
        return state(!selectionRanges().empty(), tr("Select a video clip first"));
    }
    const QVariantMap info = previewFilesInfo(id == QLatin1String("sequence.deleteRenderFilesInToOut"));
    return state(info.value(QStringLiteral("enabled")).toBool(), info.value(QStringLiteral("reason")).toString());
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
        // P1 (#58): a proxy batch finished: the caches are brought within their limits again (soon, quietly).
        if (m_working && !working && autoEnforce() &&
            (!m_autoEnforce.isActive() || m_autoEnforce.remainingTime() > kAutoEnforceAfterBatchMs)) {
            m_autoEnforce.start(kAutoEnforceAfterBatchMs);
        }
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
    // P1 (#16): the badge shows the playback resolution, so it does not change at every play and pause (the paused one
    // is in the tooltip).
    const int preview = playbackDivisor();
    const bool decoded = s.proxyLayers + s.originalLayers > 0;
    // A coarser paused resolution's decodes do not count (the badge would change at every pause).
    const int decodedDivisor = !m_monitorPlaying && previewDivisor() > preview ? preview : s.decodeDivisor;
    const int res = decoded ? std::max(preview, decodedDivisor) : preview;
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
    if (m_pausedQuality != QLatin1String("same")) {
        //: Monitor quality tooltip: Premiere's Paused Resolution, e.g. "Paused: Full"
        tip << tr("Paused: %1").arg(m_pausedQuality == QLatin1String("full")   ? tr("Full")
                                    : m_pausedQuality == QLatin1String("half") ? QStringLiteral("1/2")
                                                                               : QStringLiteral("1/4"));
    }
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
    // P1 (#29, stream 1C): the proxies (ProRes Proxy about 45 Mb/s, DNxHR LB about 36 Mb/s at 1080p30, at the proxy size)
    // need room on the proxy folder's drive; when it is short none is queued and a toast says why.
    if (ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr) {
        if (const ProjectSnapshot snap = controller->snapshot()) {
            const double rate = proxyFormat() == QLatin1String("prores") ? 45e6 : 36e6;
            const int divisor = std::max(1, proxyDivisor());
            double bytes = 0;
            for (const MediaId id : parseIds(mediaIds)) {
                const MediaItem* item = snap->findMedia(id);
                if (!item || item->info.video.empty())
                    continue;
                const VideoStreamInfo& v = item->info.video.front();
                const double fps = v.frameRate.isValid() ? v.frameRate.toDouble() : 30.0;
                const double pixelRate = static_cast<double>(v.width / divisor) * (v.height / divisor) * fps;
                bytes += rate * pixelRate / (1920.0 * 1080.0 * 29.97) * std::max(0.0, item->info.duration.toSeconds()) / 8.0;
            }
            const QString low = diskspace::lowSpaceMessage(proxyFolder(), bytes, tr("the proxies"));
            if (!low.isEmpty()) {
                emit diskSpaceLow(low);
                return 0;
            }
        }
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

// ---- Cache locations (P1 #48) -----------------------------------------------------------------------------------

namespace {

/// ai::userCacheDir() without WABCUT_CACHE_DIR (the rule in ModelStore.cpp).
QString platformAiCacheFolder() {
#if defined(Q_OS_WIN)
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    return local.isEmpty() ? QDir::tempPath() + QStringLiteral("/Wabcut/cache")
                           : QDir::fromNativeSeparators(local) + QStringLiteral("/Wabcut/cache");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Caches/Wabcut");
#else
    const QString xdg = qEnvironmentVariable("XDG_CACHE_HOME");
    return xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache/wabcut") : xdg + QStringLiteral("/wabcut");
#endif
}

} // namespace

QString MediaReadinessBridge::mediaCacheFolder() const {
    return QDir::toNativeSeparators(cachefolders::chosenMediaCacheFolder());
}

void MediaReadinessBridge::setMediaCacheFolder(const QString& folder) {
    if (cachefolders::localFolder(folder) == cachefolders::chosenMediaCacheFolder()) {
        return;
    }
    cachefolders::setChosenMediaCacheFolder(folder);
    emit locationsChanged();
}

QString MediaReadinessBridge::defaultMediaCacheFolder() const {
    return QDir::toNativeSeparators(cachefolders::defaultMediaCacheFolder());
}

QString MediaReadinessBridge::mediaCacheFolderInUse() const {
    const QString inUse = m_lifecycle && m_lifecycle->controller() ? m_lifecycle->controller()->mediaCacheDirectory()
                                                                   : m_startMediaCache;
    return QDir::toNativeSeparators(inUse);
}

QString MediaReadinessBridge::aiCacheFolder() const {
    return QDir::toNativeSeparators(cachefolders::chosenAiCacheFolder());
}

void MediaReadinessBridge::setAiCacheFolder(const QString& folder) {
    if (cachefolders::localFolder(folder) == cachefolders::chosenAiCacheFolder()) {
        return;
    }
    cachefolders::setChosenAiCacheFolder(folder);
    emit locationsChanged();
}

QString MediaReadinessBridge::defaultAiCacheFolder() const {
    return QDir::toNativeSeparators(platformAiCacheFolder());
}

QString MediaReadinessBridge::proxyLocation() const {
    return cachefolders::proxyLocation();
}

void MediaReadinessBridge::setProxyLocation(const QString& location) {
    if (location == cachefolders::proxyLocation()) {
        return;
    }
    cachefolders::setProxyLocation(location);
    applyProxyRoot();
}

QString MediaReadinessBridge::proxyRootFolder() const {
    return QDir::toNativeSeparators(cachefolders::proxyFolderSetting());
}

void MediaReadinessBridge::setProxyRootFolder(const QString& folder) {
    if (cachefolders::localFolder(folder) == cachefolders::proxyFolderSetting()) {
        return;
    }
    cachefolders::setProxyFolderSetting(folder);
    applyProxyRoot();
}

void MediaReadinessBridge::applyProxyRoot() {
    const QString root = cachefolders::proxyRoot();
    if (m_manager && toPath(root) != m_manager->proxyRoot()) {
        m_manager->setProxyRoot(toPath(root));
        m_manager->refresh(true);
        updateProxyState();
        emit proxiesChanged(); // proxyFolder
    }
    emit locationsChanged();
    scanCaches();
}

QString MediaReadinessBridge::mediaCacheFreeText() const {
    return cachefolders::freeSpaceText(cachefolders::mediaCacheFolder());
}

QString MediaReadinessBridge::aiCacheFreeText() const {
    const QString chosen = cachefolders::chosenAiCacheFolder();
    return cachefolders::freeSpaceText(chosen.isEmpty() ? platformAiCacheFolder() : chosen);
}

QString MediaReadinessBridge::proxyFreeText() const {
    return m_manager ? cachefolders::freeSpaceText(toQString(m_manager->proxyFolder())) : QString();
}

bool MediaReadinessBridge::restartNeeded() const {
    if (QString::compare(cachefolders::mediaCacheFolder(), m_startMediaCache, Qt::CaseInsensitive) != 0) {
        return true;
    }
    const QString ai = cachefolders::chosenAiCacheFolder();
    if (ai.isEmpty() && qEnvironmentVariableIsSet("WABCUT_CACHE_DIR")) {
        return false; // a folder set by hand in the environment, not by this setting
    }
    const QString wantedAi = QDir::cleanPath(ai.isEmpty() ? platformAiCacheFolder() : ai);
    return QString::compare(wantedAi, m_startAiCache, Qt::CaseInsensitive) != 0;
}

bool MediaReadinessBridge::revealLocation(const QString& which) {
    QString folder;
    if (which == QLatin1String("media")) {
        folder = QDir::fromNativeSeparators(mediaCacheFolderInUse());
    } else if (which == QLatin1String("ai")) {
        folder = toQString(ai::userCacheDir());
    } else if (which == QLatin1String("proxies") && m_manager) {
        folder = toQString(m_manager->proxyFolder());
    }
    if (folder.isEmpty()) {
        return false;
    }
    QDir().mkpath(folder);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

// ---- Caches -----------------------------------------------------------------------------------------------------

void MediaReadinessBridge::configureCaches() {
    QSettings settings;
    const auto limit = [&](const char* id) -> qlonglong {
        // Fix F1A: no limit at all on a cache whose files projects may still use (keptByLimit).
        return keptByLimit(id) ? settings.value(QStringLiteral("cache/limit/") + QLatin1String(id), 0).toLongLong() : 0;
    };
    const QString media = m_lifecycle && m_lifecycle->controller() ? m_lifecycle->controller()->mediaCacheDirectory()
                                                                   : QString();
    std::vector<editor::CacheLocation> locations;
    // Fix F2C: the proxy rows list (and Clean deletes) only the proxies Wabcut made there (CacheKind::WabcutProxies):
    // never a user's own file in the folder.
    locations.push_back({"proxies", tr("Proxies").toStdString(), m_manager ? m_manager->proxyFolder() : std::filesystem::path{},
                         limit("proxies"), {".mov"}, editor::CacheKind::WabcutProxies});
    // P1 (#48): the proxy folder moved elsewhere; the proxies made next to the project before stay there and can be
    // cleaned from here (the row shows only while that folder holds any).
    if (m_manager && !m_manager->proxyRoot().empty() && m_lifecycle && !m_lifecycle->document().file().empty()) {
        const std::filesystem::path& file = m_lifecycle->document().file();
        std::filesystem::path name = file.stem();
        name += " Proxies";
        if (const std::filesystem::path near = file.parent_path() / name; near != m_manager->proxyFolder()) {
            locations.push_back({"proxiesNearProject", tr("Proxies next to the project").toStdString(), near,
                                 limit("proxies"), {".mov"}, editor::CacheKind::WabcutProxies});
        }
    }
    locations.push_back({"thumbnails", tr("Thumbnails").toStdString(),
                         media.isEmpty() ? std::filesystem::path{} : toPath(media) / "thumbnails", limit("thumbnails"),
                         {".wthumb"}});
    locations.push_back({"peaks", tr("Waveforms").toStdString(),
                         media.isEmpty() ? std::filesystem::path{} : toPath(media) / "peaks", limit("peaks"),
                         {".wpeaks"}});
    locations.push_back({"analysis", tr("Analysis results").toStdString(), ai::userCacheDir() / "analysis",
                         limit("analysis"), {}, editor::CacheKind::Analysis});
    // Fix F2C: AI object mask mattes and imported chat logs cannot be rebuilt from the media: their own row, never part
    // of "Clean regenerable data", and their Clean asks first (MediaCacheSettings.qml).
    locations.push_back({"analysisKept", tr("AI masks and chat logs").toStdString(), ai::userCacheDir() / "analysis", 0, {},
                         editor::CacheKind::KeptAnalysis});
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
                    if ((u.id == "proxiesNearProject" || u.id == "analysisKept") && u.files == 0) {
                        continue; // nothing left next to the project; no AI mask or chat log kept
                    }
                    // Fix F2C: what Clean deletes (the confirmations say how much) and whether it can be made again.
                    const std::int64_t cleanableBytes = u.bytes - u.inUseBytes;
                    const std::int64_t cleanableFiles = u.files - u.inUseFiles;
                    total += u.bytes;
                    list.append(QVariantMap{
                        {QStringLiteral("freeText"), u.dir.empty() ? QString() : cachefolders::freeSpaceText(toQString(u.dir))},
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
                        {QStringLiteral("limited"), keptByLimit(u.id)},
                        {QStringLiteral("cleanableBytes"), static_cast<double>(cleanableBytes)},
                        {QStringLiteral("cleanableFiles"), static_cast<double>(cleanableFiles)},
                        {QStringLiteral("cleanableText"), QString::fromStdString(editor::formatBytes(cleanableBytes))},
                        {QStringLiteral("rebuildable"), u.kind != editor::CacheKind::KeptAnalysis},
                        {QStringLiteral("proxies"), u.kind == editor::CacheKind::WabcutProxies},
                        {QStringLiteral("limitText"), u.limitBytes > 0
                                                          ? QString::fromStdString(editor::formatBytes(u.limitBytes))
                                                          : tr("No limit")},
                    });
                }
                // P1 (#58): the project's render files, listed with the caches (their own Clean; no size limit).
                const QVariantMap render = self->renderFilesRow();
                self->m_measuredBytes = total;
                list.append(render);
                total += static_cast<std::int64_t>(render.value(QStringLiteral("bytes")).toDouble());
                self->m_caches = std::move(list);
                self->m_cacheTotal = QString::fromStdString(editor::formatBytes(total));
                self->m_cacheBusy = false;
                emit self->cachesChanged();
            },
            Qt::QueuedConnection);
    });
}

void MediaReadinessBridge::scanCaches() {
    // Fix F1A (ARGON-04): measuring never deletes anything (Settings opening, a limit or a folder changed): only the
    // automatic keeper does, with its switch on and nothing running (autoEnforceCaches).
    const auto inUse = m_manager ? m_manager->filesInUse() : std::set<std::filesystem::path>{};
    runCacheJob([inUse](editor::CacheInspector& inspector) { return inspector.scan(inUse); });
}

void MediaReadinessBridge::cleanCache(const QString& id) {
    if (id == kRenderFilesId) {
        // P1 (#58): every render file of the project (not only the current sequence's), through the command.
        const QString why = startDelete(command::Json{{"all", true}});
        if (!why.isEmpty()) {
            emit renderFilesDeleted(false, why);
        }
        return;
    }
    const auto inUse = m_manager ? m_manager->filesInUse() : std::set<std::filesystem::path>{};
    runCacheJob([inUse, id = id.toStdString()](editor::CacheInspector& inspector) {
        inspector.clean(id, inUse);
        return inspector.scan(inUse);
    });
}

QString MediaReadinessBridge::sizeText(double bytes) const {
    return QString::fromStdString(editor::formatBytes(static_cast<std::int64_t>(std::max(0.0, bytes))));
}

void MediaReadinessBridge::setCacheLimit(const QString& id, double bytes) {
    QSettings().setValue(QStringLiteral("cache/limit/") + id, static_cast<qlonglong>(std::max(0.0, bytes)));
    scanCaches();
    // Fix F1A: a new limit is kept by the automatic keeper (soon, with its switch on and nothing running), never here.
    if (autoEnforce() && (!m_autoEnforce.isActive() || m_autoEnforce.remainingTime() > kAutoEnforceAfterBatchMs)) {
        m_autoEnforce.start(kAutoEnforceAfterBatchMs);
    }
}

// ---- Render files as a cache, automatic limits (P1 #58) -----------------------------------------------------------

QVariantMap MediaReadinessBridge::renderFilesRow() {
    // From the preview cache's index (it sweeps files it does not name when it loads): no folder walk, and never a
    // file deleted behind its back - the size limits do not apply to render files.
    const std::shared_ptr<PreviewCache> cache = previewCache();
    const std::vector<PreviewEntry> entries = cache ? cache->entries() : std::vector<PreviewEntry>{};
    std::int64_t bytes = 0;
    for (const PreviewEntry& e : entries) {
        bytes += e.bytes;
    }
    const QString folder = cache ? toQString(cache->folder()) : QString();
    return QVariantMap{
        {QStringLiteral("freeText"), folder.isEmpty() ? QString() : cachefolders::freeSpaceText(folder)},
        {QStringLiteral("id"), kRenderFilesId},
        {QStringLiteral("name"), tr("Render files (this project)")},
        {QStringLiteral("folder"), QDir::toNativeSeparators(folder)},
        {QStringLiteral("bytes"), static_cast<double>(bytes)},
        {QStringLiteral("sizeText"), QString::fromStdString(editor::formatBytes(bytes))},
        {QStringLiteral("files"), static_cast<double>(entries.size())},
        {QStringLiteral("inUseText"), QString()},
        {QStringLiteral("limitBytes"), 0.0},
        {QStringLiteral("limited"), false},
        {QStringLiteral("limitText"), QString()},
        {QStringLiteral("cleanableBytes"), static_cast<double>(bytes)},
        {QStringLiteral("cleanableFiles"), static_cast<double>(entries.size())},
        {QStringLiteral("cleanableText"), QString::fromStdString(editor::formatBytes(bytes))},
        {QStringLiteral("rebuildable"), true},
        {QStringLiteral("proxies"), false},
    };
}

void MediaReadinessBridge::updateRenderFilesRow() {
    if (m_caches.isEmpty()) {
        return; // not measured yet: the next scan lists them
    }
    const QVariantMap render = renderFilesRow();
    bool replaced = false;
    for (QVariant& row : m_caches) {
        if (row.toMap().value(QStringLiteral("id")).toString() == kRenderFilesId) {
            row = render;
            replaced = true;
        }
    }
    if (!replaced) {
        m_caches.append(render);
    }
    m_cacheTotal = QString::fromStdString(
        editor::formatBytes(m_measuredBytes + static_cast<std::int64_t>(render.value(QStringLiteral("bytes")).toDouble())));
    emit cachesChanged();
}

bool MediaReadinessBridge::autoEnforce() const {
    return QSettings().value(kAutoEnforceKey, true).toBool();
}

void MediaReadinessBridge::setAutoEnforce(bool on) {
    if (on == autoEnforce()) {
        return;
    }
    QSettings().setValue(kAutoEnforceKey, on);
    if (on) {
        m_autoEnforce.start(kAutoEnforceAfterBatchMs);
    } else {
        m_autoEnforce.stop();
    }
    emit settingsChanged();
}

bool MediaReadinessBridge::cacheWorkRunning() const {
    return m_cacheBusy || m_render.running() || m_deleteJob.running() || m_working ||
           (m_playback && m_playback->playing()) || (m_source && m_source->playing()) || (m_exportBusy && m_exportBusy());
}

bool MediaReadinessBridge::autoEnforceCaches() {
    m_autoEnforce.stop();
    if (!autoEnforce()) {
        return false;
    }
    // Never while something may be reading or writing the caches: a render, a proxy job, playback, an export (it keeps
    // the disk quiet for them; files in use are never deleted anyway).
    if (cacheWorkRunning()) {
        m_autoEnforce.start(kAutoEnforceRetryMs);
        return false;
    }
    // enforceLimits(inUse) + scan, on the thread pool. Only the caches no project refers to have a limit here
    // (keptByLimit, configureCaches): proxies and analysis results are never deleted by it (fix F1A, ARGON-04).
    const auto inUse = m_manager ? m_manager->filesInUse() : std::set<std::filesystem::path>{};
    runCacheJob([inUse](editor::CacheInspector& inspector) {
        inspector.enforceLimits(inUse);
        return inspector.scan(inUse);
    });
    return true;
}

} // namespace wabcut::app
