#include "ObjectMaskBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "MaskBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SourceMonitorBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/render/PreparedComposite.h>

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace seg = ai::segment;
using Json = nlohmann::json;

namespace {

QString toQString(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString maskKeyOf(EffectInstanceId effect, std::uint32_t id) { return effectKey(effect) + QLatin1Char('/') + QString::number(id); }

std::optional<std::pair<EffectInstanceId, std::uint32_t>> parseMaskKey(const QString& key) {
    const qsizetype slash = key.indexOf(QLatin1Char('/'));
    const auto effect = slash > 0 ? parseEffectKey(key.left(slash)) : std::nullopt;
    bool ok = false;
    const uint id = slash > 0 ? key.mid(slash + 1).toUInt(&ok) : 0;
    if (!effect || !ok || id == 0) {
        return std::nullopt;
    }
    return std::pair{*effect, static_cast<std::uint32_t>(id)};
}

const EffectInstance* findEffect(const Clip& clip, EffectInstanceId id) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

const Mask* findMask(const Clip& clip, EffectInstanceId effect, std::uint32_t id) {
    const EffectInstance* e = findEffect(clip, effect);
    if (!e) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(e->masks, [&](const Mask& m) { return m.id == id; });
    return it == e->masks.end() ? nullptr : &*it;
}

/// Stops a run from the GUI thread.
class CancelControl final : public command::ExecutionControl {
public:
    explicit CancelControl(std::shared_ptr<std::atomic<bool>> cancelled) : m_cancelled(std::move(cancelled)) {}
    std::optional<command::CommandError> checkpoint(std::string_view) override {
        if (m_cancelled && m_cancelled->load()) {
            return command::CommandError{command::ErrorCode::Cancelled, "cancelled", nullptr};
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};

/// The selected object mask's matte at the playhead (image://wabcut-matte/<serial>).
class MatteImageProvider final : public QQuickImageProvider {
public:
    explicit MatteImageProvider(QPointer<ObjectMaskBridge> bridge) : QQuickImageProvider(QQuickImageProvider::Image), m_bridge(std::move(bridge)) {}

    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage image = m_bridge ? m_bridge->previewImage() : QImage();
        if (image.isNull()) {
            image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
        }
        if (size) {
            *size = image.size();
        }
        return image;
    }

private:
    QPointer<ObjectMaskBridge> m_bridge;
};

/// Matte masks render from the analysis cache's mattes in preview, export, thumbnails and scopes alike: the compositor's
/// matte source, installed when the app starts.
void installObjectMattes() { seg::installMatteSource(); }

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installObjectMattes)

// ---- Placement (as MaskBridge places masks: the clip's Motion and the stream's display rotation) -------------------

struct ObjectMaskBridge::Placement {
    render::Affine2D frameToCanvas;
    render::Affine2D canvasToFrame;
    double frameWidth = 1.0, frameHeight = 1.0, canvasWidth = 1.0, canvasHeight = 1.0;
    Ticks curveTime;

    QPointF toCanvas(double u, double v) const {
        const render::Vec2 p = frameToCanvas.map({u * frameWidth, v * frameHeight});
        return {p.x / canvasWidth, p.y / canvasHeight};
    }
    std::pair<double, double> toFrame(double x, double y) const {
        const render::Vec2 p = canvasToFrame.map({x * canvasWidth, y * canvasHeight});
        return {p.x / frameWidth, p.y / frameHeight};
    }

    static std::optional<Placement> of(const ProjectState& project, const Sequence& seq, const Clip& clip, Ticks t) {
        // The clip's frame: its media stream's picture, or (0.2) for a nested or multicam clip the nested sequence's
        // canvas, which its object masks are made from (square pixels, no stored rotation).
        VideoStreamInfo v;
        if (clip.isNested()) {
            const Sequence* nested = project.findSequence(clip.sequence);
            if (!nested) {
                return std::nullopt;
            }
            v.width = nested->width;
            v.height = nested->height;
        } else {
            const MediaItem* item = project.findMedia(clip.media);
            if (!item || clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size())) {
                return std::nullopt;
            }
            v = item->info.video[static_cast<std::size_t>(clip.stream.index)];
        }
        if (v.width <= 0 || v.height <= 0 || seq.width <= 0 || seq.height <= 0) {
            return std::nullopt;
        }
        const ClipProperties& p = clip.properties();
        render::LayerTransform transform;
        transform.position = {p.value(ParamId::PositionX, t), p.value(ParamId::PositionY, t)};
        transform.scale = p.value(ParamId::Scale, t) * scaleToFrameFactor(project, clip, seq.width, seq.height); // P.2
        transform.rotationDegrees = p.value(ParamId::Rotation, t);
        transform.anchor = {p.value(ParamId::AnchorX, t), p.value(ParamId::AnchorY, t)};
        const int quarter = ((v.rotationDegrees / 90) % 4 + 4) % 4;
        if (quarter != 0) {
            transform.rotationDegrees += 90.0 * quarter;
            const render::Vec2 a = transform.anchor;
            transform.anchor = quarter == 1 ? render::Vec2{a.y, -a.x} : quarter == 2 ? render::Vec2{-a.x, -a.y} : render::Vec2{-a.y, a.x};
        }
        const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
        Placement out;
        out.frameToCanvas = render::layerToCanvas(transform, v.width, v.height, par, {seq.width, seq.height});
        if (std::abs(out.frameToCanvas.determinant()) < 1e-12) {
            return std::nullopt;
        }
        out.canvasToFrame = out.frameToCanvas.inverted();
        out.frameWidth = v.width;
        out.frameHeight = v.height;
        out.canvasWidth = seq.width;
        out.canvasHeight = seq.height;
        out.curveTime = t;
        return out;
    }
};

struct ObjectMaskBridge::Outcome {
    command::CommandResult result;
    ProjectSnapshot state;
};

// ---- Lifetime ----------------------------------------------------------------------------------------------------

ObjectMaskBridge::ObjectMaskBridge(QObject* parent) : QObject(parent) {
    // A singleton cannot create the others while it is being made: attach to the project once the engine is done.
    QMetaObject::invokeMethod(this, &ObjectMaskBridge::refresh, Qt::QueuedConnection);
}

ObjectMaskBridge::~ObjectMaskBridge() {
    if (m_cancel) {
        m_cancel->store(true);
    }
    m_download.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_download.joinable()) {
        m_download.join();
    }
}

ObjectMaskBridge* ObjectMaskBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ObjectMaskBridge(nullptr);
    if (qmlEngine && !qmlEngine->imageProvider(QStringLiteral("wabcut-matte"))) {
        qmlEngine->addImageProvider(QStringLiteral("wabcut-matte"), new MatteImageProvider(bridge));
    }
    return bridge;
}

ProjectController* ObjectMaskBridge::controller() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

MaskBridge* ObjectMaskBridge::masks() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<MaskBridge*>("Wabcut", "Masks") : nullptr;
}

SourceMonitorBridge* ObjectMaskBridge::sourceMonitor() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor") : nullptr;
}

std::filesystem::path ObjectMaskBridge::projectFile() const {
    QQmlEngine* engine = qmlEngine(this);
    auto* file = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    return file ? file->document().file() : std::filesystem::path();
}

void ObjectMaskBridge::attach() {
    if (m_attached) {
        return;
    }
    ProjectController* c = controller();
    MaskBridge* m = masks();
    if (!c || !m) {
        return;
    }
    m_attached = true;
    connect(c, &ProjectController::historyChanged, this, &ObjectMaskBridge::refresh);
    connect(c, &ProjectController::playheadChanged, this, &ObjectMaskBridge::refresh);
    connect(c, &ProjectController::sequenceChanged, this, &ObjectMaskBridge::refresh);
    connect(c, &ProjectController::sessionClosing, this, [this] { cancel(); });
    connect(m, &MaskBridge::stateChanged, this, &ObjectMaskBridge::refresh);
}

// ---- Services ----------------------------------------------------------------------------------------------------

void ObjectMaskBridge::setServices(std::shared_ptr<seg::SegmentServices> services, std::shared_ptr<ai::analysis::AnalysisProvider> shots) {
    m_services = std::move(services);
    m_shots = std::move(shots);
    m_injected = m_services != nullptr;
    emit modelChanged(); // `available` follows the services
}

std::shared_ptr<seg::SegmentServices> ObjectMaskBridge::servicesFor(const std::filesystem::path& file) {
    if (m_injected) {
        return m_services;
    }
    if (!m_services || m_servicesFile != file) {
        seg::MediaSegmentOptions o;
        o.projectFile = file;
        m_services = seg::mediaSegmentServices(std::move(o));
        m_shots.reset();
        m_servicesFile = file;
    }
    return m_services;
}

std::shared_ptr<ai::analysis::AnalysisProvider> ObjectMaskBridge::shotsFor(const std::filesystem::path& file) {
    if (!m_shots && !m_injected) {
        ai::analysis::MediaAnalyzerOptions options;
        options.cacheRoot = ai::AnalysisCache::defaultRoot();
        m_shots = std::make_shared<ai::analysis::MediaAnalysisProvider>(options, file);
    }
    return m_shots;
}

bool ObjectMaskBridge::available() const {
    return m_injected || seg::segmentModelInstalled(ai::ModelStore(ai::ModelStore::defaultRoot()));
}

QString ObjectMaskBridge::modelCard() const {
    std::uint64_t bytes = 0;
    QString card;
    for (const ai::ModelInfo* m : seg::segmentModelFiles()) {
        if (m) {
            bytes += m->size;
            card = toQString(m->licenseCard);
        }
    }
    return tr("SAM 2.1 tiny, %1 MB. %2").arg(bytes / 1000000).arg(card);
}

// ---- State -------------------------------------------------------------------------------------------------------

void ObjectMaskBridge::setArmed(bool on) {
    if (on == m_armed) {
        return;
    }
    m_armed = on;
    if (on) {
        if (MaskBridge* m = masks()) {
            m->setTool({}); // one monitor tool at a time
        }
    }
    emit stateChanged();
}

void ObjectMaskBridge::setPositive(bool on) {
    if (on != m_positive) {
        m_positive = on;
        emit stateChanged();
    }
}

void ObjectMaskBridge::setAcrossShots(bool on) {
    if (on != m_acrossShots) {
        m_acrossShots = on;
        emit stateChanged();
    }
}

void ObjectMaskBridge::setTrackRedactions(bool on) {
    if (on != m_trackRedactions) {
        m_trackRedactions = on;
        emit stateChanged();
    }
}

void ObjectMaskBridge::setPreview(const QString& mode) {
    const QString m = mode == QLatin1String("colour") || mode == QLatin1String("alpha") ? mode : QStringLiteral("off");
    if (m != m_preview) {
        m_preview = m;
        refreshPreview();
    }
}

void ObjectMaskBridge::setStatus(const QString& status) {
    m_status = status;
    emit runChanged();
}

void ObjectMaskBridge::newObject() {
    m_mask.clear();
    m_points.clear();
    m_box.clear();
    emit stateChanged();
    emit geometryChanged();
    refreshPreview();
}

std::optional<ObjectMaskBridge::Placement> ObjectMaskBridge::placement() const {
    ProjectController* c = controller();
    MaskBridge* m = masks();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const auto loc = seq && m && m->clipId().isValid() ? seq->findClip(m->clipId()) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    return Placement::of(*c->snapshot(), *seq, loc->clip(), c->curveTimeAtPlayhead(loc->clip()));
}

std::optional<ObjectMaskBridge::Placement> ObjectMaskBridge::sourcePlacement() const {
    SourceMonitorBridge* sm = sourceMonitor();
    const ProjectSnapshot source = sm ? sm->sourceProject() : nullptr;
    if (!source || source->sequences.empty() || !source->sequences.front()) {
        return std::nullopt;
    }
    // The private project is one sequence with the media's clip on V1 from 0 at 100 %: timeline time = media time.
    const Sequence& seq = *source->sequences.front();
    if (seq.videoTracks.empty() || !seq.videoTracks.front() || seq.videoTracks.front()->clips.empty()) {
        return std::nullopt;
    }
    const Clip& clip = *seq.videoTracks.front()->clips.begin();
    return Placement::of(*source, seq, clip, clip.curveTimeAt(sm->positionTicks()));
}

std::shared_ptr<const seg::MatteTrack> ObjectMaskBridge::trackOf(const std::string& identity) {
    if (const auto it = m_tracks.find(identity); it != m_tracks.end()) {
        return it->second;
    }
    const std::filesystem::path root = m_services ? m_services->cacheRoot : ai::AnalysisCache::defaultRoot();
    auto got = seg::MatteStore(root).get(identity);
    std::shared_ptr<const seg::MatteTrack> track = got && *got ? std::make_shared<const seg::MatteTrack>(std::move(**got)) : nullptr;
    if (track) {
        if (m_tracks.size() > 16) {
            m_tracks.clear();
        }
        m_tracks[identity] = track;
    }
    return track;
}

void ObjectMaskBridge::refresh() {
    attach();
    // The object mask: the mask selected in Masks when it is one (else the one the prompts made, while it exists).
    QString key;
    const Mask* mask = nullptr;
    ProjectController* c = controller();
    MaskBridge* m = masks();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const auto loc = seq && m && m->clipId().isValid() ? seq->findClip(m->clipId()) : std::nullopt;
    for (const QString& candidate : {m ? m->selected() : QString(), m_mask}) {
        const auto parsed = parseMaskKey(candidate);
        const Mask* found = parsed && loc ? findMask(loc->clip(), parsed->first, parsed->second) : nullptr;
        if (found && found->shape == MaskShape::Matte && found->matte) {
            key = candidate;
            mask = found;
            break;
        }
    }
    if (key != m_mask) {
        m_mask = key;
        emit stateChanged();
    }
    // Its prompt on the frame at the playhead.
    m_points.clear();
    m_box.clear();
    const auto where = placement();
    if (mask && where) {
        if (const auto track = trackOf(mask->matte->track)) {
            const Ticks half{std::max<std::int64_t>(1, track->frameDuration.value / 2)};
            for (const seg::Prompt& p : track->prompts) {
                if (std::abs((p.time - where->curveTime).value) >= half.value) {
                    continue;
                }
                for (const seg::PromptPoint& q : p.points) {
                    const QPointF at = where->toCanvas(q.x, q.y);
                    m_points.append(QVariantMap{{QStringLiteral("x"), at.x()}, {QStringLiteral("y"), at.y()}, {QStringLiteral("positive"), q.positive}});
                }
                if (p.box) {
                    const QPointF a = where->toCanvas(p.box->x, p.box->y);
                    const QPointF b = where->toCanvas(p.box->x + p.box->width, p.box->y + p.box->height);
                    m_box = {{QStringLiteral("x0"), a.x()}, {QStringLiteral("y0"), a.y()}, {QStringLiteral("x1"), b.x()}, {QStringLiteral("y1"), b.y()}};
                }
            }
        }
    }
    emit geometryChanged();
    refreshPreview();
}

void ObjectMaskBridge::refreshPreview() {
    QImage image;
    QVariantList quad;
    ProjectController* c = controller();
    MaskBridge* m = masks();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const auto loc = seq && m && m->clipId().isValid() ? seq->findClip(m->clipId()) : std::nullopt;
    const auto parsed = parseMaskKey(m_mask);
    const Mask* mask = parsed && loc ? findMask(loc->clip(), parsed->first, parsed->second) : nullptr;
    const auto where = placement();
    if (m_preview != QLatin1String("off") && mask && mask->matte && where) {
        if (const auto track = trackOf(mask->matte->track)) {
            const seg::MatteFrame* f = track->frameAt(where->curveTime, Ticks{track->frameDuration.value * 3 / 4});
            std::optional<seg::MatteImage> matte;
            if (f) {
                if (auto decoded = track->imageOf(*f)) {
                    matte = std::move(*decoded);
                }
            }
            if (matte) {
                const bool alpha = m_preview == QLatin1String("alpha");
                image = QImage(matte->width, matte->height, alpha ? QImage::Format_ARGB32 : QImage::Format_ARGB32_Premultiplied);
                // The design language's accent (ultraviolet) for the object; the matte in grey for the alpha view.
                const QColor accent(0x8B, 0x5C, 0xF6);
                for (int y = 0; y < matte->height; ++y) {
                    auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
                    for (int x = 0; x < matte->width; ++x) {
                        const int a = matte->alpha[static_cast<std::size_t>(y) * matte->width + x];
                        if (alpha) {
                            line[x] = qRgb(a, a, a);
                        } else {
                            const int k = a * 115 / 255; // 45 % over the picture
                            line[x] = qRgba(accent.red() * k / 255, accent.green() * k / 255, accent.blue() * k / 255, k);
                        }
                    }
                }
                for (const auto [u, v] : {std::pair{0.0, 0.0}, std::pair{1.0, 0.0}, std::pair{0.0, 1.0}}) {
                    const QPointF p = where->toCanvas(u, v);
                    quad << p.x() << p.y();
                }
            }
        }
    }
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->preview = image;
    }
    const QString source = image.isNull() ? QString() : QStringLiteral("image://wabcut-matte/%1").arg(++m_previewSerial);
    if (source != m_previewSource || quad != m_previewQuad) {
        m_previewSource = source;
        m_previewQuad = quad;
        emit previewChanged();
    }
}

QImage ObjectMaskBridge::previewImage() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->preview;
}

// ---- Runs --------------------------------------------------------------------------------------------------------

void ObjectMaskBridge::startRun(QString activity) {
    m_busy = true;
    m_progress = 0.0;
    m_activity = std::move(activity);
    m_confidence.clear();
    m_total = 0;
    m_status.clear();
    emit runChanged();
}

void ObjectMaskBridge::finishRun(QString status) {
    m_busy = false;
    m_activity.clear();
    m_status = std::move(status);
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
    emit runChanged();
    refresh();
}

void ObjectMaskBridge::onProgress(const seg::SegmentProgress& p) {
    m_progress = std::clamp(p.fraction, 0.0, 1.0);
    if (!p.message.empty()) {
        m_activity = toQString(p.message);
    }
    if (p.frame) {
        m_confidence.append(p.present ? static_cast<double>(p.confidence) : -1.0);
        m_total = std::max(m_total, static_cast<int>(m_confidence.size()));
    }
    emit runChanged();
}

bool ObjectMaskBridge::runCommand(const char* command, Json params, QString activity, std::function<QString(const Outcome&)> apply) {
    ProjectController* c = controller();
    if (m_busy || !c) {
        return false;
    }
    const std::filesystem::path file = projectFile();
    auto base = servicesFor(file);
    if (!base) {
        return false;
    }
    // This run's services: the shared model and caches, its progress posted to this bridge.
    auto run = std::make_shared<seg::SegmentServices>(*base);
    QPointer<ObjectMaskBridge> self(this);
    const bool synchronous = m_synchronous;
    run->progress = [self, synchronous](const seg::SegmentProgress& p) {
        const auto post = [self, p] {
            if (self) {
                self->onProgress(p);
            }
        };
        if (synchronous) {
            post();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), post, Qt::QueuedConnection);
        }
    };
    std::vector<std::shared_ptr<command::CommandServices>> parts{run};
    if (auto shots = shotsFor(file)) {
        parts.push_back(std::make_shared<ai::analysis::AnalysisServices>(shots));
    }
    auto services = std::make_shared<command::ServiceBundle>(std::move(parts));
    startRun(std::move(activity));
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const ProjectSnapshot snapshot = c->snapshot();
    const SequenceId sequence = c->currentSequenceId();
    params["sequence"] = sequence.value;
    auto work = [snapshot, services, params, command = std::string(command), cancel = m_cancel]() -> Outcome {
        EditSession scratch(*snapshot);
        command::EnginePolicy policy;
        policy.services = services;
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        command::CommandRequest request;
        request.command = command;
        request.params = params;
        CancelControl control(cancel);
        command::CommandResult result = engine.execute(request, &control);
        return Outcome{std::move(result), scratch.snapshot()};
    };
    auto finish = [self, snapshot, sequence, apply, cancel = m_cancel](Outcome outcome) {
        if (!self) {
            return;
        }
        const command::CommandResult& r = outcome.result;
        if (!r.ok()) {
            self->finishRun(r.error->code == command::ErrorCode::Cancelled ? tr("Cancelled: nothing changed")
                                                                           : QString::fromStdString(r.error->message));
            return;
        }
        ProjectController* live = self->controller();
        if (cancel->load() || !live || live->snapshot() != snapshot || live->currentSequenceId() != sequence) {
            self->finishRun(cancel->load() ? tr("Cancelled: nothing changed") : tr("The project changed meanwhile: try again"));
            return;
        }
        self->finishRun(apply(outcome));
    };
    if (synchronous) {
        finish(work());
        return true;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::jthread([work, finish] {
        Outcome outcome = work();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [finish, outcome = std::move(outcome)]() mutable { finish(std::move(outcome)); },
                                  Qt::QueuedConnection);
    });
    return true;
}

namespace {

/// "Tracked 120 frames on DirectML (2.9 frames/s), stopped at the cut at 00:00:04:12".
QString summary(const Json& data) {
    const int frames = data.value("frames", 0);
    const int lost = data.value("lost", 0);
    QString text = data.value("cached", false) ? ObjectMaskBridge::tr("%n frame(s), from the cache", nullptr, frames)
                                               : ObjectMaskBridge::tr("%n frame(s)", nullptr, frames);
    const std::string device = data.value("device", std::string());
    if (!device.empty() && !data.value("cached", false)) {
        text += ObjectMaskBridge::tr(" on %1 (%2 frames/s)").arg(QString::fromStdString(device)).arg(data.value("framesPerSecond", 0.0), 0, 'f', 1);
    }
    if (lost > 0) {
        text += ObjectMaskBridge::tr("; the object is out of sight on %n frame(s)", nullptr, lost);
    }
    for (const char* key : {"stoppedAtCutBefore", "stoppedAtCutAfter"}) {
        if (data.contains(key) && data[key].is_string()) {
            text += ObjectMaskBridge::tr("; stopped at the cut at %1").arg(QString::fromStdString(data[key].get<std::string>()));
        }
    }
    return text;
}

} // namespace

bool ObjectMaskBridge::prompt(std::vector<seg::PromptPoint> points, std::optional<seg::MatteBox> box) {
    ProjectController* c = controller();
    MaskBridge* m = masks();
    if (!c || !m || !m->clipId().isValid()) {
        setStatus(tr("Select a video clip first"));
        return false;
    }
    if (!available()) {
        setStatus(tr("Download SAM 2.1 first (the Object tool's model, on this computer)"));
        return false;
    }
    const SequencePtr seq = c->currentSequence();
    const auto loc = seq->findClip(m->clipId());
    if (!loc) {
        return false;
    }
    const Clip& clip = loc->clip();
    const Ticks at = std::clamp(c->playhead(), clip.start(), clip.end() - Ticks{1});
    Json pts = Json::array();
    for (const seg::PromptPoint& p : points) {
        pts.push_back(Json{{"x", p.x}, {"y", p.y}, {"positive", p.positive}});
    }
    Json params{{"clip", clip.id.value}, {"time", at.value}, {"propagate", "none"}};
    if (!pts.empty()) {
        params["points"] = pts;
    }
    if (box) {
        params["box"] = Json{{"x", box->x}, {"y", box->y}, {"width", box->width}, {"height", box->height}};
    }
    const ClipId clipId = clip.id;
    const auto parsed = parseMaskKey(m_mask);
    if (parsed) {
        // More clicks on the object mask: refined on this frame.
        params["mask"] = parsed->second;
        params["effect"] = parsed->first.value;
        return runCommand("mask.refine", params, tr("Refining the object…"), [this, clipId, parsed](const Outcome& o) -> QString {
            ProjectController* live = controller();
            const auto loc = o.state->findSequence(live->currentSequenceId())->findClip(clipId);
            const Mask* made = loc ? findMask(loc->clip(), parsed->first, parsed->second) : nullptr;
            if (!made) {
                return tr("The object mask is gone");
            }
            const Mask copy = *made;
            const SequenceId seqId = live->currentSequenceId();
            auto r = live->session().apply("Refine Object Mask",
                                           [&](const ProjectState& s) { return edit::updateMask(s, seqId, clipId, parsed->first, copy); });
            return r ? summary(o.result.data) : QString::fromStdString(r.error().message);
        });
    }
    // A new object mask on the target of Masks (an effect, or the clip's opacity: a cut-out).
    const auto target = m->target().isEmpty() ? std::optional<EffectInstanceId>(EffectInstanceId{}) : parseEffectKey(m->target());
    if (!target) {
        return false;
    }
    if (target->isValid()) {
        params["effect"] = target->value;
    }
    return runCommand("mask.objectSelect", params, tr("Finding the object…"), [this, clipId, target](const Outcome& o) -> QString {
        ProjectController* live = controller();
        const EffectInstanceId holder{o.result.data.value("effect", std::uint64_t{0})};
        const std::uint32_t id = o.result.data.value("mask", 0u);
        const auto loc = o.state->findSequence(live->currentSequenceId())->findClip(clipId);
        const Mask* made = loc ? findMask(loc->clip(), holder, id) : nullptr;
        if (!made) {
            return tr("The object mask could not be made");
        }
        const Mask copy = *made;
        const SequenceId seqId = live->currentSequenceId();
        auto r = live->session().apply("Select Object", [&](const ProjectState& s) { return edit::addMask(s, seqId, clipId, *target, copy); });
        if (!r) {
            return QString::fromStdString(r.error().message);
        }
        // Select it in Masks: the next prompts refine it, its values show in the section.
        EffectInstanceId effect = *target;
        if (!effect.isValid()) {
            if (const auto now = live->currentSequence()->findClip(clipId)) {
                for (const EffectInstance& e : now->clip().effects()) {
                    if (e.effect == "wabcut.mask") {
                        effect = e.id;
                    }
                }
            }
        }
        m_mask = maskKeyOf(effect, r->id);
        emit stateChanged();
        if (MaskBridge* mb = masks()) {
            mb->select(m_mask);
        }
        return summary(o.result.data);
    });
}

bool ObjectMaskBridge::click(double x, double y, bool positive) {
    const auto where = placement();
    if (!where) {
        setStatus(tr("Select a video clip first"));
        return false;
    }
    const auto [u, v] = where->toFrame(x, y);
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
        setStatus(tr("Click on the clip's picture"));
        return false;
    }
    return prompt({seg::PromptPoint{u, v, positive}}, std::nullopt);
}

bool ObjectMaskBridge::drawBox(double x0, double y0, double x1, double y1) {
    const auto where = placement();
    if (!where) {
        setStatus(tr("Select a video clip first"));
        return false;
    }
    const auto [u0, v0] = where->toFrame(x0, y0);
    const auto [u1, v1] = where->toFrame(x1, y1);
    const double ax = std::clamp(std::min(u0, u1), 0.0, 1.0), bx = std::clamp(std::max(u0, u1), 0.0, 1.0);
    const double ay = std::clamp(std::min(v0, v1), 0.0, 1.0), by = std::clamp(std::max(v0, v1), 0.0, 1.0);
    if (bx - ax < 0.005 || by - ay < 0.005) {
        setStatus(tr("Drag a box around the object"));
        return false;
    }
    return prompt({}, seg::MatteBox{ax, ay, bx - ax, by - ay});
}

bool ObjectMaskBridge::propagate(const QString& direction) {
    MaskBridge* m = masks();
    const auto parsed = parseMaskKey(m_mask);
    if (!parsed || !m || !m->clipId().isValid()) {
        setStatus(tr("Click on an object first"));
        return false;
    }
    const ClipId clipId = m->clipId();
    const QString d = direction == QLatin1String("backward") || direction == QLatin1String("forward") ? direction : QStringLiteral("both");
    Json params{{"clip", clipId.value}, {"mask", parsed->second}, {"effect", parsed->first.value}, {"direction", d.toStdString()},
                {"acrossShots", m_acrossShots}};
    return runCommand("mask.propagate", params, tr("Tracking the object…"), [this, clipId, parsed](const Outcome& o) -> QString {
        ProjectController* live = controller();
        const auto loc = o.state->findSequence(live->currentSequenceId())->findClip(clipId);
        const Mask* made = loc ? findMask(loc->clip(), parsed->first, parsed->second) : nullptr;
        if (!made) {
            return tr("The object mask is gone");
        }
        const Mask copy = *made;
        const SequenceId seqId = live->currentSequenceId();
        auto r = live->session().apply("Track Object Mask", [&](const ProjectState& s) { return edit::updateMask(s, seqId, clipId, parsed->first, copy); });
        if (!r) {
            return QString::fromStdString(r.error().message);
        }
        // The strip: every frame of the track, in time order.
        if (const auto track = trackOf(copy.matte->track)) {
            m_confidence.clear();
            for (const seg::MatteFrame& f : track->frames) {
                m_confidence.append(f.present ? static_cast<double>(f.confidence) : -1.0);
            }
            m_total = static_cast<int>(track->frames.size());
        }
        return summary(o.result.data);
    });
}

void ObjectMaskBridge::cancel() {
    if (m_cancel) {
        m_cancel->store(true);
    }
}

// ---- AI-tracked redactions (the Source Monitor) ------------------------------------------------------------------

bool ObjectMaskBridge::redactObject(double x0, double y0, double x1, double y1) {
    ProjectController* c = controller();
    SourceMonitorBridge* sm = sourceMonitor();
    const MediaId media = sm ? sm->mediaId() : MediaId{};
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const MediaItem* item = snap && media.isValid() ? snap->findMedia(media) : nullptr;
    if (m_busy || !item || !item->info.hasVideo()) {
        setStatus(tr("Load a video source first"));
        return false;
    }
    if (!available()) {
        setStatus(tr("Download SAM 2.1 first (the Object tool's model, on this computer)"));
        return false;
    }
    const auto shown = sourcePlacement();
    if (!shown) {
        setStatus(tr("The source picture is not ready yet"));
        return false;
    }
    const auto [u0, v0] = shown->toFrame(std::clamp(x0, 0.0, 1.0), std::clamp(y0, 0.0, 1.0));
    const auto [u1, v1] = shown->toFrame(std::clamp(x1, 0.0, 1.0), std::clamp(y1, 0.0, 1.0));
    const double ax = std::clamp(std::min(u0, u1), 0.0, 1.0), bx = std::clamp(std::max(u0, u1), 0.0, 1.0);
    const double ay = std::clamp(std::min(v0, v1), 0.0, 1.0), by = std::clamp(std::max(v0, v1), 0.0, 1.0);
    if (bx - ax < 0.005 || by - ay < 0.005) {
        setStatus(tr("Drag a box around what to redact"));
        return false;
    }
    const edit::SourceMarks marks = edit::storedSourceMarks(*item);
    const TimeRange span = TimeRange::fromStartEnd(marks.in.value_or(Ticks{}), marks.out.value_or(item->info.duration));
    if (span.duration.value <= 0) {
        setStatus(tr("The In and Out marks leave nothing to redact"));
        return false;
    }
    seg::ObjectRequest request;
    seg::Prompt p;
    p.time = std::clamp(sm->positionTicks(), span.start, span.end() - Ticks{1});
    p.box = seg::MatteBox{ax, ay, bx - ax, by - ay};
    request.prompts = {p};
    request.direction = seg::Direction::Both;
    request.span = span;
    request.acrossShots = m_acrossShots;
    const std::filesystem::path file = projectFile();
    auto base = servicesFor(file);
    auto run = std::make_shared<seg::SegmentServices>(*base);
    QPointer<ObjectMaskBridge> self(this);
    const bool synchronous = m_synchronous;
    run->progress = [self, synchronous](const seg::SegmentProgress& progress) {
        const auto post = [self, progress] {
            if (self) {
                self->onProgress(progress);
            }
        };
        if (synchronous) {
            post();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), post, Qt::QueuedConnection);
        }
    };
    auto shots = shotsFor(file);
    const MediaItem itemCopy = *item;
    const int stream = 0; // the media's first video stream, as the Source Monitor shows it
    startRun(tr("Tracking what to redact…"));
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancelFlag = m_cancel;
    auto work = [run, shots, itemCopy, request, cancelFlag]() -> std::expected<seg::ObjectResult, Error> {
        std::stop_source stop;
        return seg::segmentObject(*run, shots.get(), itemCopy, stream, request, stop.get_token(),
                                  [cancelFlag](double, const std::string&) { return !cancelFlag->load(); });
    };
    auto finish = [self, media](std::expected<seg::ObjectResult, Error> result) {
        if (!self) {
            return;
        }
        if (!result) {
            self->finishRun(result.error().code == ErrorCode::Cancelled ? tr("Cancelled: nothing changed")
                                                                        : QString::fromStdString(result.error().message));
            return;
        }
        ProjectController* live = self->controller();
        MaskBridge* mb = self->masks();
        if (!live) {
            self->finishRun({});
            return;
        }
        const seg::MatteTrack& track = *result->track;
        Redaction r;
        r.media = media;
        r.source = track.span();
        // A little grown: a redaction errs on covering more (feather 0.4 %, 1 % more all round).
        r.region = seg::objectMask(track, media, "Tracked object", seg::kDefaultMatteFeather, 0.01);
        if (mb) {
            r.style = redactionStyleFromString(mb->redactStyle().toStdString()).value_or(RedactionStyle::Blur);
            r.audio = redactionAudioFromString(mb->redactAudio().toStdString()).value_or(RedactionAudio::None);
        }
        r.name = tr("Tracked redaction").toStdString();
        auto added = live->session().apply("Add Tracked Redaction", [&](const ProjectState& s) { return edit::addRedaction(s, r); });
        if (!added) {
            self->finishRun(QString::fromStdString(added.error().message));
            return;
        }
        Json data{{"frames", track.frames.size()}, {"lost", result->lost}, {"cached", result->cached}, {"device", result->device},
                  {"framesPerSecond", result->framesPerSecond}};
        self->finishRun(tr("Redacted, tracked over %1").arg(summary(data)));
        if (mb) {
            mb->setRedactTool(false);
            mb->refresh();
        }
    };
    if (synchronous) {
        finish(work());
        return true;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::jthread([work, finish] {
        auto outcome = work();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [finish, outcome = std::move(outcome)]() mutable { finish(std::move(outcome)); },
                                  Qt::QueuedConnection);
    });
    return true;
}

bool ObjectMaskBridge::redactFaces() {
    ProjectController* c = controller();
    SourceMonitorBridge* sm = sourceMonitor();
    const MediaId media = sm ? sm->mediaId() : MediaId{};
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const MediaItem* item = snap && media.isValid() ? snap->findMedia(media) : nullptr;
    if (m_busy || !item || !item->info.hasVideo()) {
        setStatus(tr("Load a video source first"));
        return false;
    }
    const auto base = servicesFor(projectFile());
    if (!base || !base->detector) {
        setStatus(tr("Download the face detector first (Preferences > AI > Local models: the subject detectors)"));
        return false;
    }
    const edit::SourceMarks marks = edit::storedSourceMarks(*item);
    Json params{{"media", media.value},
                {"in", marks.in.value_or(Ticks{}).value},
                {"out", marks.out.value_or(item->info.duration).value}};
    if (MaskBridge* mb = masks()) {
        params["style"] = mb->redactStyle().toStdString();
    }
    return runCommand("redaction.detectFaces", params, tr("Finding faces…"), [this](const Outcome& o) -> QString {
        ProjectController* live = controller();
        std::vector<Redaction> made;
        for (const Json& r : o.result.data.value("redactions", Json::array())) {
            const RedactionId id{r.value("redaction", std::uint64_t{0})};
            const auto it = std::find_if(o.state->redactions.begin(), o.state->redactions.end(), [&](const Redaction& x) { return x.id == id; });
            if (it != o.state->redactions.end()) {
                made.push_back(*it);
            }
        }
        if (made.empty()) {
            return tr("No face found: nothing was redacted");
        }
        auto added = live->session().apply("Redact Faces", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            ProjectState state = s;
            for (const Redaction& r : made) {
                auto one = edit::addRedaction(state, r);
                if (!one) {
                    return std::unexpected(one.error());
                }
                state = std::move(one->state);
            }
            return state;
        });
        if (!added) {
            return QString::fromStdString(added.error().message);
        }
        if (MaskBridge* mb = masks()) {
            mb->refresh();
        }
        return tr("Redacted %n face(s)", nullptr, static_cast<int>(made.size()));
    });
}

// ---- The model -----------------------------------------------------------------------------------------------------

void ObjectMaskBridge::downloadModel() {
    if (m_downloading || available()) {
        return;
    }
    auto* ai = qmlEngine(this) ? qmlEngine(this)->singletonInstance<AiController*>("Wabcut", "AiController") : nullptr;
    if (!ai) {
        setStatus(tr("The model cannot be downloaded here."));
        return;
    }
    if (ai->offlineMode()) {
        setStatus(tr("Offline mode is on: turn it off in Preferences > AI to download SAM 2.1."));
        return;
    }
    m_downloading = true;
    m_downloadProgress = 0.0;
    emit modelChanged();
    ai::IHttpTransport* network = &ai->transport();
    // The download borrows the transport: stopped and joined before AiController destroys it (A5-P2-19).
    disconnect(ai, &AiController::shuttingDown, this, nullptr);
    connect(ai, &AiController::shuttingDown, this, [this] {
        m_download.request_stop();
        if (m_download.joinable()) {
            m_download.join();
        }
    });
    QPointer<ObjectMaskBridge> self(this);
    m_download = std::jthread([self, network](std::stop_token stop) {
        ai::HttpDownloadTransport transport(*network);
        ai::ModelStore store(ai::ModelStore::defaultRoot());
        const auto files = seg::segmentModelFiles();
        std::uint64_t total = 0, done = 0;
        for (const ai::ModelInfo* m : files) {
            total += m ? m->size : 0;
        }
        QString error;
        for (const ai::ModelInfo* m : files) {
            if (!m) {
                continue;
            }
            auto ensured = store.ensure(
                *m, &transport,
                [self, done, total](const ai::DownloadProgress& p) {
                    const double fraction = total ? static_cast<double>(done + p.received) / static_cast<double>(total) : 0.0;
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [self, fraction] {
                        if (self) {
                            self->m_downloadProgress = fraction;
                            emit self->modelChanged();
                        }
                    }, Qt::QueuedConnection);
                },
                stop);
            if (!ensured) {
                error = ensured.error().code == ErrorCode::Cancelled ? QString() : QString::fromStdString(ensured.error().message);
                break;
            }
            done += m->size;
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, error] {
            if (!self) {
                return;
            }
            self->m_downloading = false;
            if (!self->m_injected) {
                self->m_services.reset(); // made again: the model is there now
            }
            if (!error.isEmpty()) {
                self->setStatus(tr("SAM 2.1 could not be downloaded: %1").arg(error));
            }
            emit self->modelChanged();
        }, Qt::QueuedConnection);
    });
}

void ObjectMaskBridge::cancelDownload() { m_download.request_stop(); }

} // namespace wabcut::app
