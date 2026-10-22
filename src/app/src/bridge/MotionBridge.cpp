#include "MotionBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/Synthetic.h>

#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace wabcut::app {

namespace {

constexpr double kDegrees = 180.0 / std::numbers::pi;
/// Samples of the motion path between two keyframes.
constexpr int kPathSamples = 24;
/// The rotation zone: from the corner's grab distance out to this many times it.
constexpr double kRotateReach = 3.0;

/// A model anchor (displayed pixels) in the transform's space of a stream turned by `quarter` quarter turns, as
/// playback::layerOf turns it, and back.
render::Vec2 toTransformAnchor(double x, double y, int quarter) {
    switch (quarter) {
    case 1:
        return {y, -x};
    case 2:
        return {-x, -y};
    case 3:
        return {-y, x};
    default:
        return {x, y};
    }
}
render::Vec2 fromTransformAnchor(render::Vec2 a, int quarter) {
    switch (quarter) {
    case 1:
        return {-a.y, a.x};
    case 2:
        return {-a.x, -a.y};
    case 3:
        return {a.y, -a.x};
    default:
        return a;
    }
}

/// The linear part (rotation and scale) of the Motion placement: anchor space -> canvas vectors.
struct Linear {
    double a = 1, b = 0, c = 0, d = 1;
    render::Vec2 map(render::Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
};
std::optional<Linear> invertedOf(const Linear& l) {
    const double det = l.a * l.d - l.b * l.c;
    if (std::abs(det) < 1e-12)
        return std::nullopt;
    return Linear{l.d / det, -l.b / det, -l.c / det, l.a / det};
}
Linear linearOf(double rotationDegrees, double scale) {
    const double r = rotationDegrees / kDegrees;
    return {std::cos(r) * scale, std::sin(r) * scale, -std::sin(r) * scale, std::cos(r) * scale};
}

render::Vec2 sub(render::Vec2 a, render::Vec2 b) { return {a.x - b.x, a.y - b.y}; }

bool insideQuad(const std::array<render::Vec2, 4>& q, render::Vec2 p) {
    // Convex quad (a placed rectangle): p is on the same side of every edge.
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const render::Vec2 a = q[static_cast<std::size_t>(i)];
        const render::Vec2 b = q[static_cast<std::size_t>((i + 1) % 4)];
        const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::abs(cross) < 1e-12)
            continue;
        const int s = cross > 0 ? 1 : -1;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return true;
}

/// The frame corners (top-left, top-right, bottom-right, bottom-left) on the canvas.
std::array<render::Vec2, 4> cornersOf(const MotionBridge::Geometry& g, const render::Affine2D& m) {
    return {m.map({0.0, 0.0}), m.map({g.frameWidth, 0.0}), m.map({g.frameWidth, g.frameHeight}),
            m.map({0.0, g.frameHeight})};
}

/// The handles: corners and edge middles, in hitTest's order (`prefix` "scale-", or "crop-" for the crop handles).
std::vector<std::pair<QString, render::Vec2>> handlesOf(const std::array<render::Vec2, 4>& c,
                                                        const QString& prefix = QStringLiteral("scale-")) {
    const auto mid = [](render::Vec2 a, render::Vec2 b) { return render::Vec2{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0}; };
    return {{prefix + QStringLiteral("tl"), c[0]},         {prefix + QStringLiteral("tr"), c[1]},
            {prefix + QStringLiteral("br"), c[2]},         {prefix + QStringLiteral("bl"), c[3]},
            {prefix + QStringLiteral("t"), mid(c[0], c[1])}, {prefix + QStringLiteral("r"), mid(c[1], c[2])},
            {prefix + QStringLiteral("b"), mid(c[2], c[3])}, {prefix + QStringLiteral("l"), mid(c[3], c[0])}};
}

constexpr std::string_view kCropEffect = "wabcut.crop";
/// The narrowest picture a crop leaves (a fraction of the frame).
constexpr double kMinCropSpan = 0.01;

double effectValue(const EffectInstance& e, std::string_view key, Ticks t) {
    const ParamValue* v = e.find(key);
    return v ? v->valueAt(t) : 0.0;
}

} // namespace

MotionBridge::MotionBridge(QObject* parent) : QObject(parent) {}

MotionBridge* MotionBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new MotionBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void MotionBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &MotionBridge::refresh);
        connect(controller, &ProjectController::playheadChanged, this, &MotionBridge::refresh);
        connect(controller, &ProjectController::selectionChanged, this, &MotionBridge::onSelectionChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &MotionBridge::onSelectionChanged);
    }
    onSelectionChanged();
}

QString MotionBridge::clip() const { return m_clip ? clipKey(*m_clip) : QString(); }

std::optional<ClipId> MotionBridge::targetClip() const {
    return m_controller ? m_controller->propertyTarget(StreamKind::Video) : std::nullopt;
}

void MotionBridge::setEngaged(bool engaged) {
    if (engaged) {
        const auto target = targetClip();
        if (!target) {
            setMessage(tr("Select a video clip to move, scale or rotate it in the Program Monitor"));
            if (m_engaged) {
                m_engaged = false;
                emit engagedChanged();
            }
            refresh();
            return;
        }
        m_clip = target;
    } else {
        if (m_drag)
            endDrag();
        m_clip.reset();
    }
    if (m_engaged != engaged) {
        m_engaged = engaged;
        emit engagedChanged();
    }
    setMessage({});
    refresh();
}

void MotionBridge::setSnapping(bool snapping) {
    if (m_snapping == snapping)
        return;
    m_snapping = snapping;
    emit snappingChanged();
}

void MotionBridge::setSnapLines(std::vector<double> vertical, std::vector<double> horizontal) {
    m_snapVertical = std::move(vertical);
    m_snapHorizontal = std::move(horizontal);
}

void MotionBridge::onSelectionChanged() {
    // Selecting another clip lets go of Motion (Effect Controls shows the new clip with nothing selected).
    if (!m_selecting && m_engaged && targetClip() != m_clip)
        setEngaged(false);
    else
        refresh();
}

std::optional<MotionBridge::Geometry> MotionBridge::geometryOf(const ProjectState& project, const Sequence& seq,
                                                               const Clip& clip, Ticks curveTime) {
    if (clip.stream.kind != StreamKind::Video || seq.width <= 0 || seq.height <= 0)
        return std::nullopt;
    Geometry g;
    g.clip = clip.id;
    g.sequence = seq.id;
    g.curveTime = curveTime;
    g.canvasWidth = seq.width;
    g.canvasHeight = seq.height;
    if (clip.sequence.isValid()) {
        // A nested or multicam clip: the nested sequence's composite at its canvas size.
        const Sequence* nested = project.findSequence(clip.sequence);
        if (!nested || nested->width <= 0 || nested->height <= 0)
            return std::nullopt;
        g.frameWidth = nested->width;
        g.frameHeight = nested->height;
    } else {
        const MediaItem* item = project.findMedia(clip.media);
        if (!item)
            return std::nullopt;
        const auto itemFrame = [&](double& w, double& h) {
            const VideoStreamInfo* v = item->info.video.empty() ? nullptr : &item->info.video.front();
            w = v && v->width > 0 ? v->width : seq.width;
            h = v && v->height > 0 ? v->height : seq.height;
        };
        if (item->isTitle()) {
            g.frameWidth = seq.width; // drawn on a transparent canvas of the sequence's size
            g.frameHeight = seq.height;
        } else if (item->isGraphic()) {
            itemFrame(g.frameWidth, g.frameHeight);
        } else if (item->isGenerator()) {
            if (!syntheticOf(*item) || item->info.video.empty())
                return std::nullopt; // adjustment layers have no picture of their own
            itemFrame(g.frameWidth, g.frameHeight);
        } else {
            if (clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size()))
                return std::nullopt;
            const VideoStreamInfo& v = item->info.video[static_cast<std::size_t>(clip.stream.index)];
            if (v.width <= 0 || v.height <= 0)
                return std::nullopt;
            g.frameWidth = v.width;
            g.frameHeight = v.height;
            g.pixelAspect = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
            g.quarter = ((v.rotationDegrees / 90) % 4 + 4) % 4;
        }
    }
    g.frameScale = scaleToFrameFactor(project, clip, seq.width, seq.height); // P.2: Scale to Frame Size
    g.values = edit::motionAt(clip, curveTime);
    g.frameToCanvas = placement(g, g.values);
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == kCropEffect && e.enabled) {
            g.crop = Geometry::Crop{e.id, effectValue(e, "left", curveTime), effectValue(e, "top", curveTime),
                                    effectValue(e, "right", curveTime), effectValue(e, "bottom", curveTime)};
            break;
        }
    }
    if (std::abs(g.frameToCanvas.determinant()) < 1e-12)
        return std::nullopt; // scaled to nothing
    return g;
}

render::Affine2D MotionBridge::placement(const Geometry& g, const edit::MotionValues& v) {
    render::LayerTransform t;
    t.position = {v.positionX, v.positionY};
    t.scale = v.scale * g.frameScale; // Motion's Scale on top of Scale to Frame Size's fit, as playback draws it
    t.rotationDegrees = v.rotation + 90.0 * g.quarter;
    t.anchor = toTransformAnchor(v.anchorX, v.anchorY, g.quarter);
    return render::layerToCanvas(t, g.frameWidth, g.frameHeight, g.pixelAspect,
                                 {static_cast<int>(g.canvasWidth), static_cast<int>(g.canvasHeight)});
}

void MotionBridge::refresh() {
    std::optional<Geometry> next;
    std::vector<PathKey> keys;
    QVariantMap pathJs;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (m_engaged && m_clip && seq) {
        if (const auto located = seq->findClip(*m_clip)) {
            const Clip& c = located->clip();
            const Ticks playhead = m_controller->playhead();
            if (playhead >= c.start() && playhead < c.end()) {
                const ProjectSnapshot snap = m_controller->snapshot();
                next = geometryOf(*snap, *seq, c, c.curveTimeAt(playhead));
            }
            // The motion path: the anchor's place at every key of Position X or Y the clip shows, and between them.
            const ParamValue& px = c.properties()[ParamId::PositionX];
            const ParamValue& py = c.properties()[ParamId::PositionY];
            if (next && (px.isCurve() || py.isCurve())) {
                std::set<std::int64_t> times;
                for (const ParamValue* v : {&px, &py}) {
                    if (const Curve* curve = v->curve()) {
                        for (const Keyframe& k : curve->keys) {
                            if (c.timelineTimeOfCurveTime(k.time))
                                times.insert(k.time.value);
                        }
                    }
                }
                const double cx = next->canvasWidth / 2.0;
                const double cy = next->canvasHeight / 2.0;
                const auto at = [&](Ticks t) { return render::Vec2{cx + px.valueAt(t), cy + py.valueAt(t)}; };
                QVariantList points;
                QVariantList keysJs;
                const auto addPoint = [&](render::Vec2 p) {
                    points << p.x / next->canvasWidth << p.y / next->canvasHeight;
                };
                std::optional<std::int64_t> previous;
                int index = 0;
                for (const std::int64_t t : times) {
                    if (previous) {
                        for (int i = 1; i < kPathSamples; ++i) {
                            const auto s = Ticks{*previous + (t - *previous) / kPathSamples * i}; // no overflow
                            addPoint(at(s));
                        }
                    }
                    const render::Vec2 p = at(Ticks{t});
                    addPoint(p);
                    PathKey key{Ticks{t}, p.x, p.y, Ticks{t} == next->curveTime};
                    keysJs << QVariantMap{{QStringLiteral("index"), index++},
                                          {QStringLiteral("x"), p.x / next->canvasWidth},
                                          {QStringLiteral("y"), p.y / next->canvasHeight},
                                          {QStringLiteral("current"), key.current}};
                    keys.push_back(key);
                    previous = t;
                }
                if (!keys.empty())
                    pathJs = QVariantMap{{QStringLiteral("points"), points}, {QStringLiteral("keys"), keysJs}};
            }
        }
    }
    m_geometry = std::move(next);
    m_pathKeys = std::move(keys);
    m_pathJs = std::move(pathJs);
    emit stateChanged();
}

std::array<render::Vec2, 4> MotionBridge::cropCorners(const Geometry& g) {
    const Geometry::Crop c = g.crop.value_or(Geometry::Crop{});
    const double x0 = c.left * g.frameWidth;
    const double x1 = (1.0 - c.right) * g.frameWidth;
    const double y0 = c.top * g.frameHeight;
    const double y1 = (1.0 - c.bottom) * g.frameHeight;
    const render::Affine2D& m = g.frameToCanvas;
    return {m.map({x0, y0}), m.map({x1, y0}), m.map({x1, y1}), m.map({x0, y1})};
}

void MotionBridge::setMode(const QString& mode) {
    const bool crop = mode == QLatin1String("crop");
    if (crop == m_crop)
        return;
    if (m_drag)
        endDrag();
    m_crop = crop;
    refresh();
}

bool MotionBridge::cropAvailable() const { return m_geometry && m_geometry->crop.has_value(); }

bool MotionBridge::isCropSection(const QString& section) const {
    const auto target = targetClip();
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto located = seq && target ? seq->findClip(*target) : std::nullopt;
    if (!located || !section.startsWith(QLatin1Char('e')))
        return false;
    bool ok = false;
    const auto id = section.mid(1).toULongLong(&ok);
    if (!ok)
        return false;
    for (const EffectInstance& e : located->clip().effects()) {
        if (e.id.value == id)
            return e.effect == kCropEffect;
    }
    return false;
}

void MotionBridge::toggleCrop() {
    if (m_engaged && m_crop) {
        setEngaged(false);
        return;
    }
    m_crop = true;
    setEngaged(true);
    if (m_engaged && !cropAvailable() && m_geometry)
        setMessage(tr("Apply the Crop effect to the clip to crop it in the Program Monitor"));
}

QVariantList MotionBridge::quad() const {
    QVariantList out;
    if (!active())
        return out;
    const auto corners = m_crop ? cropCorners(*m_geometry) : cornersOf(*m_geometry, m_geometry->frameToCanvas);
    for (const render::Vec2 p : corners)
        out << p.x / m_geometry->canvasWidth << p.y / m_geometry->canvasHeight;
    return out;
}

QVariantList MotionBridge::handles() const {
    QVariantList out;
    if (!active())
        return out;
    const auto corners = m_crop ? cropCorners(*m_geometry) : cornersOf(*m_geometry, m_geometry->frameToCanvas);
    for (const auto& [kind, p] : handlesOf(corners, m_crop ? QStringLiteral("crop-") : QStringLiteral("scale-"))) {
        out << QVariantMap{{QStringLiteral("kind"), kind},
                           {QStringLiteral("x"), p.x / m_geometry->canvasWidth},
                           {QStringLiteral("y"), p.y / m_geometry->canvasHeight}};
    }
    return out;
}

QVariantMap MotionBridge::anchor() const {
    if (!active() || m_crop)
        return {};
    const Geometry& g = *m_geometry;
    return {{QStringLiteral("x"), (g.canvasWidth / 2.0 + g.values.positionX) / g.canvasWidth},
            {QStringLiteral("y"), (g.canvasHeight / 2.0 + g.values.positionY) / g.canvasHeight}};
}

QString MotionBridge::clipAt(double fx, double fy) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || !std::isfinite(fx) || !std::isfinite(fy))
        return {};
    const ProjectSnapshot snap = m_controller->snapshot();
    const Ticks playhead = m_controller->playhead();
    const render::Vec2 p{fx * seq->width, fy * seq->height};
    // Top track first: the picture drawn last is the one under the pointer.
    for (auto it = seq->videoTracks.rbegin(); it != seq->videoTracks.rend(); ++it) {
        const Track* track = it->get();
        if (!track || !track->enabled)
            continue;
        for (const Clip& c : track->clips) {
            if (!c.enabled || playhead < c.start() || playhead >= c.end())
                continue;
            const auto g = geometryOf(*snap, *seq, c, c.curveTimeAt(playhead));
            if (g && insideQuad(cornersOf(*g, g->frameToCanvas), p))
                return clipKey(c.id);
        }
    }
    return {};
}

bool MotionBridge::engageAt(double fx, double fy) {
    const auto id = parseClipKey(clipAt(fx, fy));
    if (!id || !m_controller)
        return false;
    m_selecting = true;
    m_controller->selectClipById(*id, ProjectController::Replace);
    m_selecting = false;
    m_clip = id;
    m_crop = false; // a double-click edits Motion
    if (!m_engaged) {
        m_engaged = true;
        emit engagedChanged();
    }
    setMessage({});
    refresh();
    return true;
}

QString MotionBridge::hitTest(double fx, double fy, double reachX, double reachY) const {
    if (!active() || !std::isfinite(fx) || !std::isfinite(fy))
        return {};
    const Geometry& g = *m_geometry;
    const render::Vec2 p{fx * g.canvasWidth, fy * g.canvasHeight};
    const double rx = std::max(1e-9, reachX * g.canvasWidth);
    const double ry = std::max(1e-9, reachY * g.canvasHeight);
    const auto within = [&](render::Vec2 q, double times) {
        const double dx = (p.x - q.x) / (rx * times);
        const double dy = (p.y - q.y) / (ry * times);
        return dx * dx + dy * dy <= 1.0;
    };
    if (m_crop) {
        // The crop handles only; a press anywhere else in the box keeps the handles (nothing to drag there).
        for (const auto& [kind, h] : handlesOf(cropCorners(g), QStringLiteral("crop-"))) {
            if (within(h, 1.0))
                return kind;
        }
        return insideQuad(cornersOf(g, g.frameToCanvas), p) ? QStringLiteral("none") : QString();
    }
    const render::Vec2 anchorPoint{g.canvasWidth / 2.0 + g.values.positionX, g.canvasHeight / 2.0 + g.values.positionY};
    if (within(anchorPoint, 1.0))
        return QStringLiteral("anchor");
    for (std::size_t i = 0; i < m_pathKeys.size(); ++i) {
        if (within({m_pathKeys[i].x, m_pathKeys[i].y}, 1.0))
            return QStringLiteral("key:%1").arg(i);
    }
    const auto corners = cornersOf(g, g.frameToCanvas);
    for (const auto& [kind, h] : handlesOf(corners)) {
        if (within(h, 1.0))
            return kind;
    }
    const bool inside = insideQuad(corners, p);
    if (!inside) {
        for (const render::Vec2 c : corners) {
            if (within(c, kRotateReach))
                return QStringLiteral("rotate");
        }
        return {};
    }
    return QStringLiteral("move");
}

bool MotionBridge::beginDrag(const QString& kind, double fx, double fy) {
    if (!m_geometry || !m_controller || kind.isEmpty())
        return false;
    if (m_drag)
        endDrag();
    Drag d;
    d.kind = kind;
    d.start = *m_geometry;
    d.px = fx * d.start.canvasWidth;
    d.py = fy * d.start.canvasHeight;
    if (kind.startsWith(QLatin1String("key:"))) {
        bool ok = false;
        const int index = kind.mid(4).toInt(&ok);
        if (!ok || index < 0 || index >= static_cast<int>(m_pathKeys.size()))
            return false;
        d.key = m_pathKeys[static_cast<std::size_t>(index)];
    } else if (kind.startsWith(QLatin1String("crop-"))) {
        if (!m_crop || !d.start.crop)
            return false;
    } else if (kind != QLatin1String("move") && kind != QLatin1String("rotate") && kind != QLatin1String("anchor") &&
               !kind.startsWith(QLatin1String("scale-"))) {
        return false;
    }
    d.mergeKey = QStringLiteral("programMotion/%1/%2/%3").arg(d.start.clip.value).arg(kind).arg(++m_gestures).toStdString();
    m_drag = std::move(d);
    setMessage({});
    emit draggingChanged();
    return true;
}

void MotionBridge::dragTo(double fx, double fy, bool shift, bool ctrl, bool alt, double snapReach) {
    if (!m_drag || !std::isfinite(fx) || !std::isfinite(fy))
        return;
    const Drag& d = *m_drag;
    const Geometry& s = d.start;
    const render::Vec2 p{fx * s.canvasWidth, fy * s.canvasHeight};
    render::Vec2 delta{p.x - d.px, p.y - d.py};
    const render::Vec2 anchorCanvas{s.canvasWidth / 2.0 + s.values.positionX, s.canvasHeight / 2.0 + s.values.positionY};
    edit::MotionChange change;
    Ticks at = s.curveTime;
    const char* label = QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Position");
    QVariantList guides;

    if (d.kind.startsWith(QLatin1String("crop-")) && s.crop) {
        // The pointer in the clip's frame: the dragged edges follow it (fractions of the frame), leaving some picture.
        const render::Vec2 local = s.frameToCanvas.inverted().map(p);
        const double x = local.x / std::max(1.0, s.frameWidth);
        const double y = local.y / std::max(1.0, s.frameHeight);
        Geometry::Crop crop = *s.crop;
        const QString edges = d.kind.mid(5);
        if (edges.contains(QLatin1Char('l')))
            crop.left = std::clamp(x, 0.0, 1.0 - crop.right - kMinCropSpan);
        if (edges.contains(QLatin1Char('r')))
            crop.right = std::clamp(1.0 - x, 0.0, 1.0 - crop.left - kMinCropSpan);
        if (edges.contains(QLatin1Char('t')))
            crop.top = std::clamp(y, 0.0, 1.0 - crop.bottom - kMinCropSpan);
        if (edges.contains(QLatin1Char('b')))
            crop.bottom = std::clamp(1.0 - y, 0.0, 1.0 - crop.top - kMinCropSpan);
        if (applyCrop(s, crop, d.mergeKey))
            m_drag->committed = true;
        return;
    }
    if (d.kind == QLatin1String("move")) {
        if (shift) { // horizontal or vertical only
            if (std::abs(delta.x) >= std::abs(delta.y))
                delta.y = 0.0;
            else
                delta.x = 0.0;
        }
        if (m_snapping && !ctrl && snapReach > 0.0) {
            // The box's edges and centre, moved, against the frame's edges and centre and the guides.
            const auto corners = cornersOf(s, s.frameToCanvas);
            double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
            for (const render::Vec2 c : corners) {
                x0 = std::min(x0, c.x);
                x1 = std::max(x1, c.x);
                y0 = std::min(y0, c.y);
                y1 = std::max(y1, c.y);
            }
            std::vector<double> xs{0.0, s.canvasWidth / 2.0, s.canvasWidth};
            std::vector<double> ys{0.0, s.canvasHeight / 2.0, s.canvasHeight};
            for (const double v : m_snapVertical)
                xs.push_back(v * s.canvasWidth);
            for (const double h : m_snapHorizontal)
                ys.push_back(h * s.canvasHeight);
            const double reach = snapReach * s.canvasWidth;
            const auto best = [&](const std::vector<double>& lines, std::initializer_list<double> edges,
                                  double& shiftBy) -> std::optional<double> {
                double nearest = reach;
                std::optional<double> found;
                for (const double e : edges) {
                    for (const double line : lines) {
                        if (std::abs(line - e) <= nearest) {
                            nearest = std::abs(line - e);
                            shiftBy = line - e;
                            found = line;
                        }
                    }
                }
                return found;
            };
            double sx = 0.0, sy = 0.0;
            if (const auto gx = shift && delta.x == 0.0 ? std::nullopt
                                                        : best(xs, {x0 + delta.x, (x0 + x1) / 2.0 + delta.x, x1 + delta.x}, sx)) {
                delta.x += sx;
                guides << QVariantMap{{QStringLiteral("orientation"), QStringLiteral("vertical")},
                                      {QStringLiteral("position"), *gx / s.canvasWidth}};
            }
            if (const auto gy = shift && delta.y == 0.0 ? std::nullopt
                                                        : best(ys, {y0 + delta.y, (y0 + y1) / 2.0 + delta.y, y1 + delta.y}, sy)) {
                delta.y += sy;
                guides << QVariantMap{{QStringLiteral("orientation"), QStringLiteral("horizontal")},
                                      {QStringLiteral("position"), *gy / s.canvasHeight}};
            }
        }
        change.positionX = s.values.positionX + delta.x;
        change.positionY = s.values.positionY + delta.y;
    } else if (d.kind.startsWith(QLatin1String("scale-"))) {
        label = QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Scale");
        // Around the anchor point, along the handle's direction from it.
        render::Vec2 handle{};
        for (const auto& [kind, h] : handlesOf(cornersOf(s, s.frameToCanvas))) {
            if (kind == d.kind)
                handle = h;
        }
        const render::Vec2 u = sub(handle, anchorCanvas);
        const render::Vec2 v = sub(p, anchorCanvas);
        const double den = u.x * u.x + u.y * u.y;
        if (den < 1e-9)
            return;
        const double k = std::max(0.0, (v.x * u.x + v.y * u.y) / den);
        change.scale = std::clamp(s.values.scale * k, 0.0, parameterInfo(ParamId::Scale).maxValue);
    } else if (d.kind == QLatin1String("rotate")) {
        label = QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Rotation");
        const double a0 = std::atan2(d.py - anchorCanvas.y, d.px - anchorCanvas.x);
        const double a1 = std::atan2(p.y - anchorCanvas.y, p.x - anchorCanvas.x);
        double turn = (a1 - a0) * kDegrees;
        // The shortest way round, so crossing the left of the anchor does not jump a full turn.
        turn = std::remainder(turn, 360.0);
        double rotation = s.values.rotation + turn;
        if (shift)
            rotation = std::round(rotation / 45.0) * 45.0;
        change.rotation = rotation;
    } else if (d.kind == QLatin1String("anchor")) {
        label = QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Anchor Point");
        const Linear l = linearOf(s.values.rotation + 90.0 * s.quarter, s.values.scale * s.frameScale);
        const auto inverse = invertedOf(l);
        if (!inverse)
            return;
        const render::Vec2 a = toTransformAnchor(s.values.anchorX, s.values.anchorY, s.quarter);
        render::Vec2 next{};
        if (alt) {
            // Anchor Point only: the picture moves with the pointer, the anchor stays where it is on the canvas.
            const render::Vec2 m = inverse->map(delta);
            next = {a.x - m.x, a.y - m.y};
        } else {
            // The anchor goes under the pointer; Position follows, so the picture stays put.
            const render::Vec2 m = inverse->map(sub(p, anchorCanvas));
            next = {a.x + m.x, a.y + m.y};
            change.positionX = p.x - s.canvasWidth / 2.0;
            change.positionY = p.y - s.canvasHeight / 2.0;
        }
        const render::Vec2 model = fromTransformAnchor(next, s.quarter);
        change.anchorX = model.x;
        change.anchorY = model.y;
    } else if (d.key) {
        label = QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Move Keyframe");
        render::Vec2 moved{d.key->x + delta.x, d.key->y + delta.y};
        if (shift) {
            if (std::abs(delta.x) >= std::abs(delta.y))
                moved.y = d.key->y;
            else
                moved.x = d.key->x;
        }
        change.positionX = moved.x - s.canvasWidth / 2.0;
        change.positionY = moved.y - s.canvasHeight / 2.0;
        at = d.key->curveTime;
    } else {
        return;
    }

    if (apply(s, at, change, label, d.mergeKey))
        m_drag->committed = true;
    setGuides(std::move(guides));
}

bool MotionBridge::apply(const Geometry& g, Ticks curveTime, const edit::MotionChange& change, const char* label,
                         const std::string& mergeKey) {
    if (!m_controller || change.empty())
        return false;
    const SequenceId seq = g.sequence;
    const ClipId clip = g.clip;
    const bool ok = m_controller->applyEditFn(
        label, [&](const ProjectState& state) { return edit::setMotion(state, seq, clip, curveTime, change); }, mergeKey);
    if (!ok)
        setMessage(m_controller->lastError());
    return ok;
}

bool MotionBridge::applyCrop(const Geometry& g, const Geometry::Crop& crop, const std::string& mergeKey) {
    if (!m_controller || !g.crop)
        return false;
    const SequenceId seq = g.sequence;
    const ClipId clip = g.clip;
    const Ticks t = g.curveTime;
    const std::array<std::pair<const char*, double>, 4> edges{
        {{"left", crop.left}, {"top", crop.top}, {"right", crop.right}, {"bottom", crop.bottom}}};
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Crop"),
        [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState working = state;
            for (const auto& [key, value] : edges) {
                const Sequence* s = working.findSequence(seq);
                const auto located = s ? s->findClip(clip) : std::nullopt;
                if (!located)
                    return std::unexpected(Error{ErrorCode::NotFound, "the clip is gone"});
                const EffectInstance* instance = nullptr;
                for (const EffectInstance& e : located->clip().effects()) {
                    if (e.id == g.crop->effect)
                        instance = &e;
                }
                if (!instance)
                    return std::unexpected(Error{ErrorCode::NotFound, "the Crop effect is gone"});
                const ParamValue* current = instance->find(key);
                const ParamValue next = edit::valueSetAt(current ? *current : ParamValue(0.0), t, value);
                if (current && next == *current)
                    continue;
                auto step = edit::setParameter(working, seq, clip, edit::ParamRef::ofEffect(g.crop->effect, key), next);
                if (!step)
                    return std::unexpected(step.error());
                working = std::move(step->state);
            }
            edit::EditResult result;
            result.changes = diff(state, working);
            result.state = std::move(working);
            return result;
        },
        mergeKey);
    if (!ok)
        setMessage(m_controller->lastError());
    return ok;
}

void MotionBridge::endDrag() {
    if (!m_drag)
        return;
    if (m_controller)
        m_controller->endPropertyGesture();
    m_drag.reset();
    setGuides({});
    emit draggingChanged();
    refresh();
}

void MotionBridge::cancelDrag() {
    if (!m_drag)
        return;
    const bool committed = m_drag->committed;
    if (m_controller) {
        m_controller->endPropertyGesture();
        if (committed)
            m_controller->undo();
    }
    m_drag.reset();
    setGuides({});
    emit draggingChanged();
    refresh();
}

bool MotionBridge::nudge(double dx, double dy) {
    if (!m_geometry || m_drag || !std::isfinite(dx) || !std::isfinite(dy))
        return false;
    edit::MotionChange change;
    change.positionX = m_geometry->values.positionX + dx;
    change.positionY = m_geometry->values.positionY + dy;
    return apply(*m_geometry, m_geometry->curveTime, change,
                 QT_TRANSLATE_NOOP("wabcut::app::MotionBridge", "Nudge Selected Object"), {});
}

void MotionBridge::setGuides(QVariantList guides) {
    if (guides == m_guidesJs)
        return;
    m_guidesJs = std::move(guides);
    emit guidesChanged();
}

void MotionBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

} // namespace wabcut::app
