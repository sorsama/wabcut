#include "MaskBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SourceMonitorBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/render/PreparedComposite.h>

#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace wabcut::app {

namespace {

constexpr std::string_view kOpacityMask = "wabcut.mask";

QString toQString(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString maskKeyOf(EffectInstanceId effect, std::uint32_t id) {
    return effectKey(effect) + QLatin1Char('/') + QString::number(id);
}

std::optional<std::pair<EffectInstanceId, std::uint32_t>> parseMaskKey(const QString& key) {
    const qsizetype slash = key.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return std::nullopt;
    }
    const auto effect = parseEffectKey(key.left(slash));
    bool ok = false;
    const uint id = key.mid(slash + 1).toUInt(&ok);
    if (!effect || !ok || id == 0) {
        return std::nullopt;
    }
    return std::pair{*effect, static_cast<std::uint32_t>(id)};
}

bool maskable(const EffectInstance& e) {
    if (e.effect == kOpacityMask) {
        return true;
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    return d && d->kind == EffectKind::Video && !d->intrinsic && e.effect != "wabcut.title.style";
}

const EffectInstance* findEffect(const Clip& clip, EffectInstanceId id) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

const EffectInstance* opacityEffect(const Clip& clip) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == kOpacityMask) {
            return &e;
        }
    }
    return nullptr;
}

const Mask* findMask(const EffectInstance& e, std::uint32_t id) {
    const auto it = std::find_if(e.masks.begin(), e.masks.end(), [&](const Mask& m) { return m.id == id; });
    return it == e.masks.end() ? nullptr : &*it;
}

/// A parameter at curve time `t`: a key there when it is animated, else the constant.
void setParamAt(Mask& mask, std::string_view key, Ticks t, double value) {
    if (const MaskParamInfo* info = maskParam(key)) {
        value = std::clamp(value, info->minValue, info->maxValue);
    }
    const ParamValue* old = mask.find(key);
    if (old && old->curve()) {
        Curve curve = *old->curve();
        if (const auto at = curve.keyAt(t)) {
            curve.keys[*at].value = value;
        } else {
            curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(t)), Keyframe{t, value});
        }
        mask.set(std::string(key), ParamValue(std::move(curve)));
        return;
    }
    mask.set(std::string(key), ParamValue(value));
}

/// The path vertices edited at curve time `t`: the only key, the key at `t`, or a new key there.
std::vector<MaskVertex>& pathAt(Mask& mask, Ticks t) {
    if (mask.path.size() == 1) {
        return mask.path.front().vertices;
    }
    for (MaskPathKey& k : mask.path) {
        if (k.time == t) {
            return k.vertices;
        }
    }
    MaskPathKey key{t, maskPathAt(mask, t), Interpolation::Linear};
    const auto at = std::find_if(mask.path.begin(), mask.path.end(), [&](const MaskPathKey& k) { return k.time > t; });
    return mask.path.insert(at, std::move(key))->vertices;
}

MaskAffine inverse(const MaskAffine& m) {
    const double det = m.a * m.e - m.b * m.d;
    if (std::abs(det) < 1e-12) {
        return {};
    }
    const double ia = m.e / det, ib = -m.b / det, id = -m.d / det, ie = m.a / det;
    return {ia, ib, -(ia * m.c + ib * m.f), id, ie, -(id * m.c + ie * m.f)};
}

std::pair<double, double> apply(const MaskAffine& m, double x, double y) { return {m.a * x + m.b * y + m.c, m.d * x + m.e * y + m.f}; }

/// `polygon` (normalised) moved out by `distance` frame heights along its vertex normals (a display approximation).
std::vector<std::pair<double, double>> offsetPolygon(const std::vector<masks::Point2>& polygon, double distance, double aspect) {
    std::vector<std::pair<double, double>> out;
    const std::size_t n = polygon.size();
    if (n < 3) {
        return out;
    }
    double area = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % n];
        area += p.x * aspect * q.y - q.x * aspect * p.y;
    }
    const double sign = area > 0.0 ? 1.0 : -1.0;
    const auto normal = [&](std::size_t i) {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % n];
        const double dx = (q.x - p.x) * aspect, dy = q.y - p.y;
        const double len = std::hypot(dx, dy);
        return len > 0 ? std::pair{sign * dy / len, -sign * dx / len} : std::pair{0.0, 0.0};
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto [ax, ay] = normal((i + n - 1) % n);
        const auto [bx, by] = normal(i);
        double nx = ax + bx, ny = ay + by;
        const double len = std::hypot(nx, ny);
        if (len > 1e-9) {
            nx /= len, ny /= len;
        }
        out.push_back({(polygon[i].x * aspect + nx * distance) / aspect, polygon[i].y + ny * distance});
    }
    return out;
}

/// Stops a track from the GUI thread.
class CancelControl final : public command::ExecutionControl {
public:
    explicit CancelControl(std::shared_ptr<std::atomic<bool>> cancelled) : m_cancelled(std::move(cancelled)) {}
    std::optional<command::CommandError> checkpoint(std::string_view) override {
        if (m_cancelled && m_cancelled->load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};

/// Forwards a TrackingServices' pictures and sends its progress to a callback.
class ProgressForwarder final : public masks::TrackingServices {
public:
    ProgressForwarder(std::shared_ptr<masks::TrackingServices> inner, std::function<void(const masks::TrackProgress&)> callback)
        : m_inner(std::move(inner)), m_callback(std::move(callback)) {}
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        return m_inner->frame(state, media, stream, time, maxWidth);
    }
    void progress(const masks::TrackProgress& progress) override {
        m_inner->progress(progress);
        m_callback(progress);
    }
    std::expected<std::vector<masks::DetectedRegion>, Error> detect(const masks::GrayImage& picture, std::string_view kind) override {
        return m_inner->detect(picture, kind);
    }

private:
    std::shared_ptr<masks::TrackingServices> m_inner;
    std::function<void(const masks::TrackProgress&)> m_callback;
};

QString secondsText(Ticks t) { return QString::number(static_cast<double>(t.value) / kTicksPerSecond, 'f', 2) + QStringLiteral(" s"); }

} // namespace

struct MaskBridge::Located {
    SequenceId sequence;
    ClipId clip;
    EffectInstanceId effect;
    Mask mask;
    Ticks curveTime;
    double aspect = 16.0 / 9.0;
};

struct MaskBridge::Placement {
    render::Affine2D frameToCanvas;
    render::Affine2D canvasToFrame;
    double frameWidth = 1.0;
    double frameHeight = 1.0;
    double canvasWidth = 1.0;
    double canvasHeight = 1.0;
    double aspect = 16.0 / 9.0;
    Ticks curveTime;

    QPointF toCanvas(double u, double v) const {
        const render::Vec2 p = frameToCanvas.map({u * frameWidth, v * frameHeight});
        return {p.x / canvasWidth, p.y / canvasHeight};
    }
    std::pair<double, double> toFrame(double fx, double fy) const {
        const render::Vec2 p = canvasToFrame.map({fx * canvasWidth, fy * canvasHeight});
        return {p.x / frameWidth, p.y / frameHeight};
    }
};

MaskBridge::MaskBridge(QObject* parent) : QObject(parent) {
    // A singleton cannot create the others while it is being made: attach to the project once the engine is done.
    QMetaObject::invokeMethod(this, &MaskBridge::refresh, Qt::QueuedConnection);
}

MaskBridge::~MaskBridge() {
    if (m_cancel) {
        m_cancel->store(true);
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

MaskBridge* MaskBridge::create(QQmlEngine*, QJSEngine*) { return new MaskBridge; }

namespace {

/// The app's transcripts (the Transcript singleton's), so a redaction edit here updates the stored captions like the
/// redaction.* commands do (review wave 5, S5-P1-2); null without them.
std::shared_ptr<ai::edit::TranscriptServices> appTranscripts(const QObject* bridge) {
    QQmlEngine* engine = qmlEngine(bridge);
    auto* transcripts = engine ? engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript") : nullptr;
    return transcripts ? transcripts->services() : nullptr;
}

/// `edited` (a redaction edit of `before`) with the captions over the speech it touches refreshed.
template <class Outcome>
Outcome withRedactedCues(const ProjectState& before, Outcome edited, const std::shared_ptr<ai::edit::TranscriptServices>& services) {
    if (edited && services && services->transcripts) {
        if constexpr (std::is_same_v<typename Outcome::value_type, ProjectState>) {
            *edited = ai::edit::refreshRedactedCues(before, std::move(*edited), *services->transcripts);
        } else {
            edited->state = ai::edit::refreshRedactedCues(before, std::move(edited->state), *services->transcripts);
        }
    }
    return edited;
}

} // namespace

ProjectController* MaskBridge::controller() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

SourceMonitorBridge* MaskBridge::sourceMonitor() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor") : nullptr;
}

void MaskBridge::attach() {
    if (m_attached) {
        return;
    }
    ProjectController* c = controller();
    if (!c) {
        return;
    }
    m_attached = true;
    connect(c, &ProjectController::historyChanged, this, &MaskBridge::onHistory);
    connect(c, &ProjectController::playheadChanged, this, &MaskBridge::refresh);
    connect(c, &ProjectController::selectionChanged, this, &MaskBridge::refresh);
    connect(c, &ProjectController::sequenceChanged, this, &MaskBridge::refresh);
    connect(c, &ProjectController::sessionClosing, this, [this] {
        cancelTrack();
        m_clip = {};
    });
    if (SourceMonitorBridge* sm = sourceMonitor()) {
        connect(sm, &SourceMonitorBridge::sourceChanged, this, &MaskBridge::refreshRedactions);
        connect(sm, &SourceMonitorBridge::positionChanged, this, &MaskBridge::refreshRedactions);
        connect(sm, &SourceMonitorBridge::marksChanged, this, &MaskBridge::refreshRedactions);
    }
}

QString MaskBridge::clip() const { return clipKey(m_clip); }

void MaskBridge::onHistory() {
    ProjectController* c = controller();
    if (!c)
        return;
    const TimelineChange& change = c->lastChange();
    bool relevant = change.projectWide() || change.touches(m_clip) || change.touches(c->primaryClip());
    if (!relevant && (!m_targetFromSelection || !m_clip.isValid()))
        relevant = c->lastChangeAtPlayhead(); // another clip may be under the playhead now
    if (relevant)
        refresh();
}

QVariantList MaskBridge::penPoints() const {
    QVariantList out;
    if (const auto p = placement()) {
        for (const MaskVertex& v : m_pen) {
            out.append(p->toCanvas(v.x, v.y));
        }
    }
    return out;
}

void MaskBridge::setStatus(const QString& status) {
    if (status == m_status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}

// ---- State ---------------------------------------------------------------------------------------------------------

void MaskBridge::refresh() {
    attach();
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    ClipId target;
    if (seq) {
        if (const auto t = c->propertyTarget(StreamKind::Video); t && seq->findClip(*t)) {
            target = *t;
        }
        m_targetFromSelection = target.isValid();
        if (!target.isValid()) {
            for (auto it = seq->videoTracks.rbegin(); it != seq->videoTracks.rend() && !target.isValid(); ++it) {
                if (const auto i = (*it)->clipIndexAt(c->playhead()); i && (*it)->clips[*i].enabled) {
                    target = (*it)->clips[*i].id;
                }
            }
        }
    }
    if (target != m_clip) {
        m_clip = target;
        m_target.clear();
        m_selected.clear();
        m_pen.clear();
        if (!m_tool.isEmpty()) {
            m_tool.clear();
            emit toolChanged();
        }
    }
    m_clipName.clear();
    m_targets.clear();
    m_masks.clear();
    m_maskCount = 0;
    m_current.clear();
    const Clip* clip = nullptr;
    if (m_clip.isValid()) {
        const auto loc = seq->findClip(m_clip);
        clip = loc && loc->clip().stream.kind == StreamKind::Video ? &loc->clip() : nullptr;
    }
    if (!clip) {
        m_clip = {};
    } else {
        m_clipName = toQString(clip->name());
        m_targets.append(QVariantMap{{QStringLiteral("effect"), QString()}, {QStringLiteral("name"), tr("Opacity mask")}});
        for (const EffectInstance& e : clip->effects()) {
            // 0.4: every target's masks (the opacity mask holder included) count for Effect Controls' section.
            if (maskable(e))
                m_maskCount += static_cast<int>(e.masks.size());
            if (maskable(e) && e.effect != kOpacityMask) {
                const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
                m_targets.append(QVariantMap{{QStringLiteral("effect"), effectKey(e.id)},
                                             {QStringLiteral("name"), d ? toQString(d->displayName) : toQString(e.effect)}});
            }
        }
        const EffectInstance* holder = nullptr;
        if (m_target.isEmpty()) {
            holder = opacityEffect(*clip);
        } else if (const auto id = parseEffectKey(m_target)) {
            holder = findEffect(*clip, *id);
            if (!holder || !maskable(*holder)) {
                m_target.clear();
                holder = opacityEffect(*clip);
            }
        }
        const Ticks t = c->curveTimeAtPlayhead(*clip);
        bool selectedFound = false;
        if (holder) {
            for (const Mask& m : holder->masks) {
                const QString key = maskKeyOf(holder->id, m.id);
                const bool tracked = std::any_of(m.params.begin(), m.params.end(), [](const MaskParam& p) {
                    return p.key.starts_with("transform.") && p.value.isCurve();
                });
                m_masks.append(QVariantMap{{QStringLiteral("key"), key},
                                           {QStringLiteral("name"), toQString(m.name)},
                                           {QStringLiteral("shape"), toQString(toString(m.shape))},
                                           {QStringLiteral("mode"), toQString(toString(m.mode))},
                                           {QStringLiteral("inverted"), m.inverted},
                                           {QStringLiteral("enabled"), m.enabled},
                                           {QStringLiteral("tracked"), tracked}});
                if (key == m_selected) {
                    selectedFound = true;
                    QStringList animated;
                    for (const MaskParam& p : m.params) {
                        if (p.value.isCurve()) {
                            animated << toQString(p.key);
                        }
                    }
                    m_current = {{QStringLiteral("name"), toQString(m.name)},
                                 {QStringLiteral("shape"), toQString(toString(m.shape))},
                                 {QStringLiteral("mode"), toQString(toString(m.mode))},
                                 {QStringLiteral("inverted"), m.inverted},
                                 {QStringLiteral("enabled"), m.enabled},
                                 {QStringLiteral("feather"), maskValue(m, "feather", t)},
                                 {QStringLiteral("expansion"), maskValue(m, "expansion", t)},
                                 {QStringLiteral("opacity"), maskValue(m, "opacity", t)},
                                 {QStringLiteral("animated"), animated}};
                }
            }
        }
        if (!selectedFound) {
            m_selected.clear();
        }
    }
    emit stateChanged();
    refreshGeometry();
    refreshRedactions();
}

void MaskBridge::setTarget(const QString& effect) {
    if (effect == m_target) {
        return;
    }
    m_target = effect;
    m_selected.clear();
    refresh();
}

void MaskBridge::select(const QString& key) {
    if (key == m_selected) {
        return;
    }
    m_selected = key;
    refresh();
}

void MaskBridge::setTool(const QString& tool) {
    if (tool == m_tool) {
        return;
    }
    m_tool = tool;
    m_pen.clear();
    emit toolChanged();
    emit geometryChanged();
}

std::optional<MaskBridge::Placement> MaskBridge::placement() const {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const auto loc = seq && m_clip.isValid() ? seq->findClip(m_clip) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    return placementOf(*c->snapshot(), *seq, loc->clip(), c->curveTimeAtPlayhead(loc->clip()));
}

std::optional<MaskBridge::Placement> MaskBridge::placementOf(const ProjectState& project, const Sequence& seq,
                                                             const Clip& clip, Ticks t) {
    const MediaItem* item = project.findMedia(clip.media);
    if (!item || clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size())) {
        return std::nullopt;
    }
    const VideoStreamInfo& v = item->info.video[static_cast<std::size_t>(clip.stream.index)];
    if (v.width <= 0 || v.height <= 0 || seq.width <= 0 || seq.height <= 0) {
        return std::nullopt;
    }
    const ClipProperties& p = clip.properties();
    render::LayerTransform transform;
    transform.position = {p.value(ParamId::PositionX, t), p.value(ParamId::PositionY, t)};
    transform.scale = p.value(ParamId::Scale, t) * scaleToFrameFactor(project, clip, seq.width, seq.height); // P.2
    transform.rotationDegrees = p.value(ParamId::Rotation, t);
    transform.anchor = {p.value(ParamId::AnchorX, t), p.value(ParamId::AnchorY, t)};
    // As playback places it (Evaluate.cpp): the stream's display rotation before the clip's own motion.
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
    out.aspect = v.width * par / v.height;
    out.curveTime = t;
    return out;
}

std::optional<MaskBridge::Placement> MaskBridge::sourcePlacement() const {
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
    return placementOf(*source, seq, clip, clip.curveTimeAt(sm->positionTicks()));
}

std::optional<MaskBridge::Located> MaskBridge::locate(const QString& key) const {
    const auto parsed = parseMaskKey(key);
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const auto loc = parsed && seq && m_clip.isValid() ? seq->findClip(m_clip) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    const EffectInstance* e = findEffect(loc->clip(), parsed->first);
    const Mask* m = e ? findMask(*e, parsed->second) : nullptr;
    if (!m) {
        return std::nullopt;
    }
    Located out{seq->id, m_clip, e->id, *m, c->curveTimeAtPlayhead(loc->clip()), 16.0 / 9.0};
    if (const auto p = placement()) {
        out.aspect = p->aspect;
    }
    return out;
}

// ---- Geometry ------------------------------------------------------------------------------------------------------

void MaskBridge::refreshGeometry() {
    m_outlines.clear();
    m_handles.clear();
    m_featherOuter.clear();
    m_featherInner.clear();
    const auto p = placement();
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const auto loc = p && seq ? seq->findClip(m_clip) : std::nullopt;
    if (!loc) {
        emit geometryChanged();
        return;
    }
    const Clip& clip = loc->clip();
    const EffectInstance* holder = nullptr;
    if (m_target.isEmpty()) {
        holder = opacityEffect(clip);
    } else if (const auto id = parseEffectKey(m_target)) {
        holder = findEffect(clip, *id);
    }
    const Ticks t = p->curveTime;
    const double aspect = p->aspect;
    const auto handle = [&](const char* kind, int index, double u, double v, double fu, double fv) {
        const QPointF at = p->toCanvas(u, v);
        const QPointF from = p->toCanvas(fu, fv);
        m_handles.append(QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)},
                                     {QStringLiteral("index"), index},
                                     {QStringLiteral("x"), at.x()},
                                     {QStringLiteral("y"), at.y()},
                                     {QStringLiteral("fromX"), from.x()},
                                     {QStringLiteral("fromY"), from.y()}});
    };
    for (const Mask& m : holder ? holder->masks : std::vector<Mask>{}) {
        const QString key = maskKeyOf(holder->id, m.id);
        const std::vector<masks::Point2> polygon = masks::maskPolygonAt(m, t, aspect);
        QVariantList points;
        for (const masks::Point2& q : polygon) {
            points.append(p->toCanvas(q.x, q.y));
        }
        const bool selected = key == m_selected;
        m_outlines.append(QVariantMap{{QStringLiteral("key"), key},
                                      {QStringLiteral("selected"), selected},
                                      {QStringLiteral("enabled"), m.enabled},
                                      {QStringLiteral("points"), points}});
        if (!selected || m.shape == MaskShape::Matte) { // AI-14: an object mask's box is a stand-in, not a handle
            continue;
        }
        const double e = maskValue(m, "expansion", t);
        const double f = maskValue(m, "feather", t);
        if (f > 0.0 || e != 0.0) {
            for (const auto& [list, distance] : {std::pair{&m_featherOuter, e + f / 2.0}, std::pair{&m_featherInner, e - f / 2.0}}) {
                for (const auto& [u, v] : offsetPolygon(polygon, distance, aspect)) {
                    list->append(p->toCanvas(u, v));
                }
            }
        }
        // Handles in shape space, through the mask's transform, then onto the canvas.
        const MaskAffine mt = maskTransformAt(m, t, aspect);
        const auto put = [&](const char* kind, int index, double u, double v, double fu, double fv) {
            const auto [x, y] = apply(mt, u, v);
            const auto [fx, fy] = apply(mt, fu, fv);
            handle(kind, index, x, y, fx, fy);
        };
        if (m.shape == MaskShape::Path) {
            const std::vector<MaskVertex> vs = maskPathAt(m, t);
            double cx = 0.0, cy = 0.0;
            for (const MaskVertex& v : vs) {
                cx += v.x / static_cast<double>(vs.size());
                cy += v.y / static_cast<double>(vs.size());
            }
            for (int i = 0; i < static_cast<int>(vs.size()); ++i) {
                const MaskVertex& v = vs[static_cast<std::size_t>(i)];
                if (v.inX != 0.0 || v.inY != 0.0) {
                    put("in", i, v.x + v.inX, v.y + v.inY, v.x, v.y);
                }
                if (v.outX != 0.0 || v.outY != 0.0) {
                    put("out", i, v.x + v.outX, v.y + v.outY, v.x, v.y);
                }
                put("vertex", i, v.x, v.y, v.x, v.y);
            }
            if (!vs.empty()) {
                double nx = (vs[0].x - cx) * aspect, ny = vs[0].y - cy;
                const double len = std::hypot(nx, ny);
                nx = len > 0 ? nx / len : 1.0, ny = len > 0 ? ny / len : 0.0;
                const double ex = vs[0].x + nx * e / aspect, ey = vs[0].y + ny * e;
                put("expansion", 0, ex, ey, vs[0].x, vs[0].y);
                put("feather", 0, ex + nx * (f / 2.0) / aspect, ey + ny * (f / 2.0), ex, ey);
            }
        } else {
            const double cx = maskValue(m, "center.x", t), cy = maskValue(m, "center.y", t);
            const double hw = maskValue(m, "size.x", t) * aspect / 2.0, hh = maskValue(m, "size.y", t) / 2.0;
            const double r = maskValue(m, "rotation", t) * std::numbers::pi / 180.0;
            const double ux = std::cos(r), uy = std::sin(r); // local x axis (aspect space)
            const double vx = -std::sin(r), vy = std::cos(r); // local y axis
            const auto local = [&](double lx, double ly) { return std::pair{cx + (ux * lx + vx * ly) / aspect, cy + uy * lx + vy * ly}; };
            const auto [kx, ky] = local(hw, hh);
            const auto [rx, ry] = local(0.0, -hh - 0.06);
            const auto [tx, ty] = local(0.0, -hh);
            const auto [ex, ey] = local(hw + e, 0.0);
            const auto [fx, fy] = local(hw + e + f / 2.0, 0.0);
            const auto [sx, sy] = local(hw, 0.0);
            put("rotate", 0, rx, ry, tx, ty);
            put("corner", 0, kx, ky, kx, ky);
            put("expansion", 0, ex, ey, sx, sy);
            put("feather", 0, fx, fy, ex, ey);
            put("center", 0, cx, cy, cx, cy);
        }
    }
    emit geometryChanged();
}

QString MaskBridge::maskAt(double x, double y) const {
    for (qsizetype i = m_outlines.size() - 1; i >= 0; --i) {
        const QVariantMap o = m_outlines[i].toMap();
        const QVariantList points = o.value(QStringLiteral("points")).toList();
        bool inside = false;
        for (qsizetype a = 0, b = points.size() - 1; a < points.size(); b = a++) {
            const QPointF p = points[a].toPointF();
            const QPointF q = points[b].toPointF();
            if ((p.y() > y) != (q.y() > y) && x < p.x() + (y - p.y()) * (q.x() - p.x()) / (q.y() - p.y())) {
                inside = !inside;
            }
        }
        if (inside) {
            return o.value(QStringLiteral("key")).toString();
        }
    }
    return {};
}

// ---- Edits ---------------------------------------------------------------------------------------------------------

bool MaskBridge::commitMask(EffectInstanceId effect, const Mask& mask, const char* label, const std::string& mergeKey) {
    ProjectController* c = controller();
    if (!c || !m_clip.isValid()) {
        return false;
    }
    const SequenceId seq = c->currentSequenceId();
    const ClipId clip = m_clip;
    auto r = c->session().apply(label, [&](const ProjectState& s) { return edit::updateMask(s, seq, clip, effect, mask); }, mergeKey);
    if (!r) {
        setStatus(QString::fromStdString(r.error().message));
        return false;
    }
    setStatus({});
    return true;
}

bool MaskBridge::editMask(const QString& key, const char* label, const std::function<void(Mask&, Ticks)>& change,
                          const std::string& mergeKey) {
    const auto located = locate(key);
    if (!located) {
        return false;
    }
    Mask m = located->mask;
    change(m, located->curveTime);
    return commitMask(located->effect, m, label, mergeKey);
}

bool MaskBridge::addMaskWith(Mask mask, const char* label) {
    ProjectController* c = controller();
    if (!c || !m_clip.isValid()) {
        setStatus(tr("Select a video clip first"));
        return false;
    }
    const SequenceId seq = c->currentSequenceId();
    const ClipId clip = m_clip;
    const auto effect = m_target.isEmpty() ? std::optional<EffectInstanceId>(EffectInstanceId{}) : parseEffectKey(m_target);
    if (!effect) {
        return false;
    }
    auto r = c->session().apply(label, [&](const ProjectState& s) { return edit::addMask(s, seq, clip, *effect, mask); });
    if (!r) {
        setStatus(QString::fromStdString(r.error().message));
        return false;
    }
    setStatus({});
    EffectInstanceId holder = *effect;
    if (!holder.isValid()) {
        const auto loc = c->currentSequence()->findClip(clip);
        if (const EffectInstance* e = loc ? opacityEffect(loc->clip()) : nullptr) {
            holder = e->id;
        }
    }
    m_selected = maskKeyOf(holder, r->id);
    if (!m_tool.isEmpty()) {
        m_tool.clear();
        emit toolChanged();
    }
    refresh();
    return true;
}

bool MaskBridge::addMask(const QString& shape) {
    Mask m;
    m.shape = shape == QLatin1String("rectangle") ? MaskShape::Rectangle : MaskShape::Ellipse;
    m.set("size.x", 0.3);
    m.set("size.y", 0.4);
    return addMaskWith(std::move(m), "Add Mask");
}

bool MaskBridge::drawShape(const QString& shape, double x0, double y0, double x1, double y1) {
    const auto p = placement();
    if (!p) {
        setStatus(tr("Select a video clip first"));
        return false;
    }
    const auto [u0, v0] = p->toFrame(x0, y0);
    const auto [u1, v1] = p->toFrame(x1, y1);
    Mask m;
    m.shape = shape == QLatin1String("rectangle") ? MaskShape::Rectangle : MaskShape::Ellipse;
    double w = std::abs(u1 - u0), h = std::abs(v1 - v0);
    double cx = (u0 + u1) / 2.0, cy = (v0 + v1) / 2.0;
    if (w < 0.01 && h < 0.01) { // a click: a default size there
        w = 0.3, h = 0.4, cx = u0, cy = v0;
    }
    m.set("center.x", cx);
    m.set("center.y", cy);
    m.set("size.x", std::max(w, 0.005));
    m.set("size.y", std::max(h, 0.005));
    return addMaskWith(std::move(m), m.shape == MaskShape::Rectangle ? "Draw Rectangle Mask" : "Draw Ellipse Mask");
}

void MaskBridge::penPoint(double x, double y) {
    const auto p = placement();
    if (!p) {
        return;
    }
    const auto [u, v] = p->toFrame(x, y);
    m_pen.push_back(MaskVertex{u, v, 0.0, 0.0, 0.0, 0.0});
    emit geometryChanged();
}

void MaskBridge::penTangent(double x, double y) {
    const auto p = placement();
    if (!p || m_pen.empty()) {
        return;
    }
    const auto [u, v] = p->toFrame(x, y);
    MaskVertex& last = m_pen.back();
    last.outX = u - last.x;
    last.outY = v - last.y;
    last.inX = -last.outX;
    last.inY = -last.outY;
    emit geometryChanged();
}

bool MaskBridge::penClose() {
    if (m_pen.size() < 3) {
        setStatus(tr("A path needs at least three points"));
        return false;
    }
    Mask m;
    m.shape = MaskShape::Path;
    m.path = {MaskPathKey{Ticks{}, m_pen, Interpolation::Linear}};
    m_pen.clear();
    return addMaskWith(std::move(m), "Draw Path Mask");
}

void MaskBridge::penCancel() {
    m_pen.clear();
    emit geometryChanged();
}

bool MaskBridge::removeMask(const QString& key) {
    const auto located = locate(key);
    ProjectController* c = controller();
    if (!located || !c) {
        return false;
    }
    auto r = c->session().apply("Remove Mask", [&](const ProjectState& s) {
        return edit::removeMask(s, located->sequence, located->clip, located->effect, located->mask.id);
    });
    if (!r) {
        setStatus(QString::fromStdString(r.error().message));
        return false;
    }
    if (key == m_selected) {
        m_selected.clear();
    }
    refresh();
    return true;
}

bool MaskBridge::setMode(const QString& key, const QString& mode) {
    const auto parsed = maskModeFromString(mode.toStdString());
    return parsed && editMask(key, "Mask Mode", [&](Mask& m, Ticks) { m.mode = *parsed; });
}

bool MaskBridge::setInverted(const QString& key, bool inverted) {
    return editMask(key, "Invert Mask", [&](Mask& m, Ticks) { m.inverted = inverted; });
}

bool MaskBridge::setEnabled(const QString& key, bool enabled) {
    return editMask(key, enabled ? "Show Mask" : "Hide Mask", [&](Mask& m, Ticks) { m.enabled = enabled; });
}

bool MaskBridge::rename(const QString& key, const QString& name) {
    return editMask(key, "Rename Mask", [&](Mask& m, Ticks) { m.name = name.left(kMaxMaskName / 4).toStdString(); });
}

bool MaskBridge::setValue(const QString& param, double value) {
    const std::string key = param.toStdString();
    if (!maskParam(key)) {
        return false;
    }
    return editMask(m_selected, "Mask Parameter", [&](Mask& m, Ticks t) { setParamAt(m, key, t, value); },
                    "maskvalue:" + m_selected.toStdString() + ":" + key);
}

void MaskBridge::endEdit() {
    if (ProjectController* c = controller()) {
        c->endPropertyGesture();
    }
}

void MaskBridge::beginDrag(const QString& kind, int index, double x, double y) {
    m_dragKind = kind;
    m_dragIndex = index;
    m_dragOffsetX = 0.0;
    m_dragOffsetY = 0.0;
    ++m_dragSerial;
    for (const QVariant& h : m_handles) {
        const QVariantMap map = h.toMap();
        if (map.value(QStringLiteral("kind")).toString() == kind && map.value(QStringLiteral("index")).toInt() == index) {
            m_dragOffsetX = map.value(QStringLiteral("x")).toDouble() - x;
            m_dragOffsetY = map.value(QStringLiteral("y")).toDouble() - y;
        }
    }
}

void MaskBridge::drag(double x, double y) {
    const auto p = placement();
    const auto located = locate(m_selected);
    if (!p || !located || m_dragKind.isEmpty()) {
        return;
    }
    const double aspect = p->aspect;
    const auto [fu, fv] = p->toFrame(x + m_dragOffsetX, y + m_dragOffsetY);
    const MaskAffine mt = maskTransformAt(located->mask, located->curveTime, aspect);
    const auto [qx, qy] = apply(inverse(mt), fu, fv); // shape space
    const QString kind = m_dragKind;
    const int index = m_dragIndex;
    const std::string merge = "maskdrag:" + std::to_string(m_dragSerial);
    editMask(
        m_selected, "Edit Mask",
        [&](Mask& m, Ticks t) {
            if (m.shape == MaskShape::Path) {
                std::vector<MaskVertex>& vs = pathAt(m, t);
                if (index < 0 || index >= static_cast<int>(vs.size())) {
                    if (kind != QLatin1String("feather") && kind != QLatin1String("expansion")) {
                        return;
                    }
                }
                if (kind == QLatin1String("vertex")) {
                    vs[static_cast<std::size_t>(index)].x = qx;
                    vs[static_cast<std::size_t>(index)].y = qy;
                } else if (kind == QLatin1String("in") || kind == QLatin1String("out")) {
                    MaskVertex& v = vs[static_cast<std::size_t>(index)];
                    (kind == QLatin1String("in") ? v.inX : v.outX) = qx - v.x;
                    (kind == QLatin1String("in") ? v.inY : v.outY) = qy - v.y;
                } else if (!vs.empty()) {
                    double cx = 0.0, cy = 0.0;
                    for (const MaskVertex& v : vs) {
                        cx += v.x / static_cast<double>(vs.size());
                        cy += v.y / static_cast<double>(vs.size());
                    }
                    double nx = (vs[0].x - cx) * aspect, ny = vs[0].y - cy;
                    const double len = std::hypot(nx, ny);
                    nx = len > 0 ? nx / len : 1.0, ny = len > 0 ? ny / len : 0.0;
                    const double along = (qx - vs[0].x) * aspect * nx + (qy - vs[0].y) * ny; // frame heights
                    if (kind == QLatin1String("expansion")) {
                        setParamAt(m, "expansion", t, along);
                    } else {
                        setParamAt(m, "feather", t, std::max(0.0, 2.0 * (along - maskValue(m, "expansion", t))));
                    }
                }
                return;
            }
            const double cx = maskValue(m, "center.x", t), cy = maskValue(m, "center.y", t);
            const double r = maskValue(m, "rotation", t) * std::numbers::pi / 180.0;
            const double dx = (qx - cx) * aspect, dy = qy - cy;
            const double lx = std::cos(r) * dx + std::sin(r) * dy; // un-rotated local coordinates (aspect space)
            const double ly = -std::sin(r) * dx + std::cos(r) * dy;
            const double hw = maskValue(m, "size.x", t) * aspect / 2.0;
            if (kind == QLatin1String("center")) {
                setParamAt(m, "center.x", t, qx);
                setParamAt(m, "center.y", t, qy);
            } else if (kind == QLatin1String("corner")) {
                setParamAt(m, "size.x", t, 2.0 * std::abs(lx) / aspect);
                setParamAt(m, "size.y", t, 2.0 * std::abs(ly));
            } else if (kind == QLatin1String("rotate")) {
                setParamAt(m, "rotation", t, std::atan2(dy, dx) * 180.0 / std::numbers::pi + 90.0);
            } else if (kind == QLatin1String("expansion")) {
                setParamAt(m, "expansion", t, lx - hw);
            } else if (kind == QLatin1String("feather")) {
                setParamAt(m, "feather", t, std::max(0.0, 2.0 * (lx - hw - maskValue(m, "expansion", t))));
            }
        },
        merge);
}

void MaskBridge::endDrag() {
    m_dragKind.clear();
    m_dragIndex = -1;
    endEdit();
}

// ---- Tracking ------------------------------------------------------------------------------------------------------

bool MaskBridge::track(const QString& direction, int frames) {
    ProjectController* c = controller();
    const auto located = locate(m_selected);
    if (m_tracking || !c || !located) {
        if (!located) {
            setStatus(tr("Select a mask to track"));
        }
        return false;
    }
    const SequencePtr seq = c->currentSequence();
    const Clip& clip = seq->findClip(located->clip)->clip();
    const Ticks at = std::clamp(c->playhead(), clip.start(), clip.end() - Ticks{1});
    command::Json params{{"clip", located->clip.value},
                         {"effect", located->effect.value},
                         {"mask", located->mask.id},
                         {"direction", direction == QLatin1String("backward") ? "backward" : "forward"},
                         {"time", at.value},
                         {"sequence", seq->id.value}};
    if (frames > 0) {
        params["frames"] = frames;
    }
    std::shared_ptr<masks::TrackingServices> pictures = m_trackingServices;
    if (!pictures) {
        std::filesystem::path projectFile;
        if (QQmlEngine* engine = qmlEngine(this)) {
            if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
                projectFile = file->document().file();
            }
        }
        pictures = std::make_shared<editor::MediaTrackingService>(projectFile);
    }
    m_tracking = true;
    m_trackProgress = 0.0;
    m_trackTotal = frames > 0 ? frames : 0;
    m_confidence.clear();
    m_trackStatus = tr("Tracking…");
    emit trackingChanged();

    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const ProjectSnapshot base = c->snapshot();
    const ClipId clipId = located->clip;
    const EffectInstanceId effect = located->effect;
    const std::uint32_t maskId = located->mask.id;
    QPointer<MaskBridge> self(this);
    const bool synchronous = m_synchronous;
    const auto onProgress = [self, synchronous](const masks::TrackProgress& progress) {
        const auto post = [self, progress] {
            if (!self) {
                return;
            }
            self->m_trackProgress = progress.total > 0 ? static_cast<double>(progress.frame) / progress.total : 0.0;
            self->m_trackTotal = progress.total;
            self->m_confidence.append(progress.lost ? -1.0 : progress.confidence);
            emit self->trackingChanged();
        };
        if (synchronous) {
            post();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), post, Qt::QueuedConnection);
        }
    };
    auto work = [base, pictures, onProgress, params, cancel = m_cancel]() -> std::pair<command::CommandResult, ProjectSnapshot> {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = std::make_shared<ProgressForwarder>(pictures, onProgress);
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        command::CommandRequest request;
        request.command = "track.mask";
        request.params = params;
        CancelControl control(cancel);
        command::CommandResult result = engine.execute(request, &control);
        return {std::move(result), scratch.snapshot()};
    };
    const auto finish = [self, clipId, effect, maskId, base, sequence = seq->id, cancel = m_cancel](
                            std::pair<command::CommandResult, ProjectSnapshot> outcome) {
        if (!self) {
            return;
        }
        const command::CommandResult& r = outcome.first;
        if (!r.ok()) {
            self->finishTrack(false,
                              r.error->code == command::ErrorCode::Cancelled ? tr("Tracking cancelled")
                                                                             : tr("Could not track: %1").arg(QString::fromStdString(r.error->message)),
                              std::nullopt, clipId, effect, maskId, Ticks{});
            return;
        }
        ProjectController* live = self->controller();
        if (cancel->load() || !live || live->snapshot() != base || live->currentSequenceId() != sequence) {
            self->finishTrack(false, cancel->load() ? tr("Tracking cancelled") : tr("Project changed while tracking; track again"),
                              std::nullopt, clipId, effect, maskId, Ticks{});
            return;
        }
        const int done = r.data.value("frames", 0);
        const std::string stopped = r.data.value("stopped", std::string());
        const Ticks last{r.data.value("lastMediaTime", std::int64_t{0})};
        QString message = stopped == "lost" ? tr("Lost after %n frame(s): the picture changed too much", nullptr, done)
                                            : tr("Tracked %n frame(s)", nullptr, done);
        self->finishTrack(true, message, *outcome.second, clipId, effect, maskId, last);
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

void MaskBridge::finishTrack(bool ok, const QString& message, std::optional<ProjectState> result, ClipId clip, EffectInstanceId effect,
                             std::uint32_t mask, Ticks lastMedia) {
    m_tracking = false;
    m_trackStatus = message;
    ProjectController* c = controller();
    if (ok && result && c) {
        // The tracked mask from the worker's copy, applied to the project as it is now (one undo step).
        const SequenceId seq = c->currentSequenceId();
        const Sequence* tracked = result->findSequence(seq);
        const auto loc = tracked ? tracked->findClip(clip) : std::nullopt;
        const EffectInstance* e = loc ? findEffect(loc->clip(), effect) : nullptr;
        const Mask* m = e ? findMask(*e, mask) : nullptr;
        if (m) {
            const Mask copy = *m;
            auto r = c->session().apply("Track Mask", [&](const ProjectState& s) { return edit::updateMask(s, seq, clip, effect, copy); });
            if (!r) {
                m_trackStatus = tr("Could not apply the track: %1").arg(QString::fromStdString(r.error().message));
            } else if (const auto now = c->currentSequence()->findClip(clip)) {
                // The playhead follows to the last frame tracked (as a step through the clip does).
                const Clip& cl = now->clip();
                const Ticks offset = cl.reversed ? cl.sourceOut() - Ticks{1} - lastMedia : lastMedia - cl.sourceIn;
                const Ticks timeline = cl.start() + toTimelineTicks(offset, cl.speed);
                if (timeline >= cl.start() && timeline < cl.end()) {
                    c->setPlayhead(snapToFrame(timeline, c->currentSequence()->frameRate));
                }
            }
        }
    }
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
    emit trackingChanged();
    refresh();
}

void MaskBridge::cancelTrack() {
    if (m_cancel) {
        m_cancel->store(true);
    }
}

// ---- Redactions ----------------------------------------------------------------------------------------------------

void MaskBridge::refreshRedactions() {
    m_redactions.clear();
    m_activeRegions.clear();
    ProjectController* c = controller();
    SourceMonitorBridge* sm = sourceMonitor();
    const MediaId media = sm ? sm->mediaId() : MediaId{};
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const MediaItem* item = snap && media.isValid() ? snap->findMedia(media) : nullptr;
    if (item) {
        const double duration = std::max<double>(1.0, static_cast<double>(item->info.duration.value));
        double aspect = 16.0 / 9.0;
        if (!item->info.video.empty() && item->info.video.front().height > 0) {
            const VideoStreamInfo& v = item->info.video.front();
            aspect = v.width * (v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0) / v.height;
        }
        const Ticks position = sm->positionTicks();
        // Outlines are drawn over the displayed picture: stored-frame fractions through the private clip's placement
        // (rotation, pixel aspect), exactly where playback renders the redaction (Codex wave-4 P1-2).
        const std::optional<Placement> shown = sourcePlacement();
        for (const Redaction* covering : redactionsCovering(*snap, media)) { // a subclip's master's too (review wave 5)
            const Redaction& r = *covering;
            const QString key = QStringLiteral("r") + QString::number(r.id.value);
            m_redactions.append(QVariantMap{{QStringLiteral("key"), key},
                                            {QStringLiteral("name"), r.name.empty() ? tr("Redaction") : toQString(r.name)},
                                            {QStringLiteral("style"), toQString(toString(r.style))},
                                            {QStringLiteral("audio"), toQString(toString(r.audio))},
                                            {QStringLiteral("enabled"), r.enabled},
                                            {QStringLiteral("start"), static_cast<double>(r.source.start.value) / duration},
                                            {QStringLiteral("end"), static_cast<double>(r.source.end().value) / duration},
                                            {QStringLiteral("range"), secondsText(r.source.start) + QStringLiteral(" – ") + secondsText(r.source.end())}});
            if (r.enabled && r.source.contains(position) && shown) {
                QVariantList points;
                for (const masks::Point2& p : masks::maskPolygonAt(r.region, position, aspect)) {
                    points.append(shown->toCanvas(p.x, p.y));
                }
                m_activeRegions.append(QVariantMap{{QStringLiteral("key"), key},
                                                   {QStringLiteral("style"), toQString(toString(r.style))},
                                                   {QStringLiteral("points"), points}});
            }
        }
    }
    emit redactionsChanged();
}

void MaskBridge::setRedactTool(bool on) {
    if (on == m_redactTool) {
        return;
    }
    m_redactTool = on;
    emit redactionsChanged();
}

void MaskBridge::setRedactStyle(const QString& style) {
    if (style == m_redactStyle || !redactionStyleFromString(style.toStdString())) {
        return;
    }
    m_redactStyle = style;
    emit redactionsChanged();
}

void MaskBridge::setRedactAudio(const QString& audio) {
    if (audio == m_redactAudio || !redactionAudioFromString(audio.toStdString())) {
        return;
    }
    m_redactAudio = audio;
    emit redactionsChanged();
}

bool MaskBridge::addRedaction(double x0, double y0, double x1, double y1) {
    ProjectController* c = controller();
    SourceMonitorBridge* sm = sourceMonitor();
    const MediaId media = sm ? sm->mediaId() : MediaId{};
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const MediaItem* item = snap && media.isValid() ? snap->findMedia(media) : nullptr;
    if (!item || !item->info.hasVideo()) {
        setStatus(tr("Load a video source first"));
        return false;
    }
    Redaction r;
    r.media = media;
    const edit::SourceMarks marks = edit::storedSourceMarks(*item);
    const Ticks start = marks.in.value_or(Ticks{});
    const Ticks end = marks.out.value_or(item->info.duration);
    if (end <= start) {
        setStatus(tr("The In and Out marks leave nothing to redact"));
        return false;
    }
    r.source = TimeRange::fromStartEnd(start, end);
    // The drag is in fractions of the displayed picture (the private sequence's canvas); a redaction is stored in
    // fractions of the stored frame, which playback then rotates and stretches like the clip (Codex wave-4 P1-2).
    // Quarter rotations and pixel aspect keep a box axis-aligned, so its two corners give the stored box.
    const std::optional<Placement> shown = sourcePlacement();
    if (!shown) {
        setStatus(tr("The source picture is not ready yet"));
        return false;
    }
    const auto [u0, v0] = shown->toFrame(std::clamp(x0, 0.0, 1.0), std::clamp(y0, 0.0, 1.0));
    const auto [u1, v1] = shown->toFrame(std::clamp(x1, 0.0, 1.0), std::clamp(y1, 0.0, 1.0));
    const double ax = std::clamp(std::min(u0, u1), 0.0, 1.0), bx = std::clamp(std::max(u0, u1), 0.0, 1.0);
    const double ay = std::clamp(std::min(v0, v1), 0.0, 1.0), by = std::clamp(std::max(v0, v1), 0.0, 1.0);
    if (bx <= ax || by <= ay) {
        setStatus(tr("Drag over the picture to mark what to redact"));
        return false;
    }
    r.region.shape = MaskShape::Rectangle;
    r.region.set("center.x", (ax + bx) / 2.0);
    r.region.set("center.y", (ay + by) / 2.0);
    r.region.set("size.x", std::max(bx - ax, 0.02));
    r.region.set("size.y", std::max(by - ay, 0.02));
    r.style = *redactionStyleFromString(m_redactStyle.toStdString());
    r.audio = *redactionAudioFromString(m_redactAudio.toStdString());
    r.name = tr("Redaction %1").arg(m_redactions.size() + 1).toStdString();
    const auto transcripts = appTranscripts(this);
    auto added = c->session().apply("Add Redaction",
                                    [&](const ProjectState& s) { return withRedactedCues(s, edit::addRedaction(s, r), transcripts); });
    if (!added) {
        setStatus(QString::fromStdString(added.error().message));
        return false;
    }
    setStatus(tr("Redacted %1 of %2 in every sequence").arg(secondsText(r.source.duration), toQString(item->name)));
    setRedactTool(false);
    refreshRedactions();
    return true;
}

bool MaskBridge::editRedaction(const QString& key, const char* label, const std::function<void(Redaction&)>& change) {
    ProjectController* c = controller();
    bool ok = false;
    const quint64 id = key.startsWith(QLatin1Char('r')) ? key.mid(1).toULongLong(&ok) : 0;
    if (!c || !ok || id == 0) {
        return false;
    }
    const ProjectSnapshot snap = c->snapshot();
    const auto it = std::find_if(snap->redactions.begin(), snap->redactions.end(), [&](const Redaction& r) { return r.id.value == id; });
    if (it == snap->redactions.end()) {
        return false;
    }
    Redaction r = *it;
    change(r);
    const auto transcripts = appTranscripts(this);
    auto done = c->session().apply(label, [&](const ProjectState& s) { return withRedactedCues(s, edit::updateRedaction(s, r), transcripts); });
    if (!done) {
        setStatus(QString::fromStdString(done.error().message));
        return false;
    }
    refreshRedactions();
    return true;
}

bool MaskBridge::removeRedaction(const QString& key) {
    ProjectController* c = controller();
    bool ok = false;
    const quint64 id = key.startsWith(QLatin1Char('r')) ? key.mid(1).toULongLong(&ok) : 0;
    if (!c || !ok || id == 0) {
        return false;
    }
    const auto transcripts = appTranscripts(this);
    auto done = c->session().apply("Remove Redaction", [&](const ProjectState& s) {
        return withRedactedCues(s, edit::removeRedaction(s, RedactionId{id}), transcripts);
    });
    if (!done) {
        setStatus(QString::fromStdString(done.error().message));
        return false;
    }
    refreshRedactions();
    return true;
}

bool MaskBridge::setRedactionStyle(const QString& key, const QString& style) {
    const auto s = redactionStyleFromString(style.toStdString());
    return s && editRedaction(key, "Redaction Style", [&](Redaction& r) { r.style = *s; });
}

bool MaskBridge::setRedactionAudio(const QString& key, const QString& audio) {
    const auto a = redactionAudioFromString(audio.toStdString());
    return a && editRedaction(key, "Redaction Sound", [&](Redaction& r) { r.audio = *a; });
}

bool MaskBridge::setRedactionEnabled(const QString& key, bool enabled) {
    return editRedaction(key, enabled ? "Redaction On" : "Redaction Off", [&](Redaction& r) { r.enabled = enabled; });
}

bool MaskBridge::setRedactionOverride(const QString& key, bool redact) {
    ProjectController* c = controller();
    bool ok = false;
    const quint64 id = key.startsWith(QLatin1Char('r')) ? key.mid(1).toULongLong(&ok) : 0;
    if (!c || !ok || id == 0 || !c->primaryClip().isValid()) {
        setStatus(tr("Select the clip on the timeline first"));
        return false;
    }
    command::EnginePolicy policy;
    policy.services = appTranscripts(this); // redaction.override refreshes the captions it touches (S5-P1-2)
    command::CommandEngine engine(c->session(), ai::edit::editingCommands(), policy);
    command::CommandRequest request;
    request.command = "redaction.override";
    request.params = {{"redaction", id}, {"clip", c->primaryClip().value}, {"redact", redact}, {"sequence", c->currentSequenceId().value}};
    const command::CommandResult result = engine.execute(request);
    if (!result.ok()) {
        setStatus(QString::fromStdString(result.error->message));
        return false;
    }
    setStatus(redact ? tr("Redacted again on the selected clip") : tr("Not redacted on the selected clip"));
    refreshRedactions();
    return true;
}

} // namespace wabcut::app
