#include "GraphicCanvasBridge.h"

#include "BridgeIds.h"
#include "GlobalBridge.h"
#include "GraphicText.h"
#include "ProjectController.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/TitleOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/PreparedComposite.h>
#include <wabcut/render/Title.h>

#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QStyleHints>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace wabcut::app {

namespace {

constexpr double kDegrees = 180.0 / std::numbers::pi;

Affine2D translation(double x, double y) { return {1.0, 0.0, 0.0, 1.0, x, y}; }
Affine2D scaling(double x, double y) { return {x, 0.0, 0.0, y, 0.0, 0.0}; }
Affine2D rotationMatrix(double degrees) {
    const double r = degrees / kDegrees;
    return {std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0.0, 0.0}; // clockwise on screen (y down)
}

Affine2D inverted(const Affine2D& m) {
    const double det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-12)
        return {};
    return {m.d / det, -m.b / det, -m.c / det, m.a / det, (m.c * m.f - m.d * m.e) / det, (m.b * m.e - m.a * m.f) / det};
}

/// The linear part of `m` applied to a vector.
std::array<double, 2> mapVector(const Affine2D& m, double x, double y) { return {m.a * x + m.c * y, m.b * x + m.d * y}; }

/// The layer's matrix from its transform values, as the resolver builds it: position, rotation, scale, anchor.
Affine2D layerMatrix(const GraphicCanvasBridge::Layer& l) {
    return translation(l.x, l.y) * rotationMatrix(l.rotation) * scaling(l.scaleX / 100.0, l.scaleY / 100.0)
           * translation(-l.anchorX, -l.anchorY);
}

bool shownAt(const GraphicLayer& l, Ticks time) { return l.visible && time >= l.in && (!l.out || time < *l.out); }

/// Sets `value` at graphic time `t`: the constant, or for an animated value the key there (updated, else added with
/// its neighbour's interpolation).
void setAt(ParamValue& value, Ticks t, double v) {
    if (const Curve* curve = value.curve()) {
        Curve copy = *curve;
        if (const auto key = copy.keyAt(t)) {
            copy.keys[*key].value = v;
        } else {
            Keyframe k;
            k.time = t;
            k.value = v;
            const std::size_t index = copy.insertionIndex(t);
            if (index > 0)
                k.interpolation = copy.keys[index - 1].interpolation;
            copy.keys.insert(copy.keys.begin() + static_cast<std::ptrdiff_t>(index), k);
        }
        value = ParamValue(std::move(copy));
    } else {
        value = ParamValue(v);
    }
}

bool differs(double a, double b) { return std::abs(a - b) > 1e-9; }

QString kindName(LayerKind kind) {
    switch (kind) {
    case LayerKind::Text:
        return QStringLiteral("text");
    case LayerKind::Shape:
        return QStringLiteral("shape");
    case LayerKind::Image:
        return QStringLiteral("image");
    case LayerKind::Group:
        return QStringLiteral("group");
    }
    return {};
}

QString alignName(TextAlign align) {
    switch (align) {
    case TextAlign::Left:
        return QStringLiteral("left");
    case TextAlign::Center:
        return QStringLiteral("center");
    case TextAlign::Right:
        return QStringLiteral("right");
    case TextAlign::Justify:
        return QStringLiteral("justify");
    }
    return QStringLiteral("left");
}

/// A text layer's content box for a box of `width` x `height` (the block hangs from its origin by alignment).
Box textBox(const GraphicCanvasBridge::Layer& l, double width, double height) {
    const double x = l.align == TextAlign::Center ? -width / 2.0 : l.align == TextAlign::Right ? -width : 0.0;
    return {x, 0.0, width, height};
}

void walk(const std::vector<GraphicLayer>& layers, std::uint32_t parent, const Affine2D& parentToDesign, Ticks t,
          const std::unordered_map<std::uint32_t, const DrawItem*>& items, const ResolveOptions& options,
          std::vector<GraphicCanvasBridge::Layer>& out) {
    for (const GraphicLayer& l : layers) {
        if (!shownAt(l, t))
            continue;
        GraphicCanvasBridge::Layer c;
        c.id = l.id;
        c.parent = parent;
        c.kind = l.kind;
        c.name = l.name;
        c.pinned = l.pin.has_value();
        c.x = l.transform.x.valueAt(t);
        c.y = l.transform.y.valueAt(t);
        c.anchorX = l.transform.anchorX.valueAt(t);
        c.anchorY = l.transform.anchorY.valueAt(t);
        c.scaleX = l.transform.scaleX.valueAt(t);
        c.scaleY = l.transform.scaleY.valueAt(t);
        c.rotation = l.transform.rotation.valueAt(t);
        c.parentToDesign = parentToDesign;
        c.toDesign = parentToDesign * layerMatrix(c);
        const auto item = items.find(l.id);
        if (l.kind != LayerKind::Group && item != items.end()) {
            c.toDesign = item->second->transform; // a pinned layer is where its pin puts it
            c.bounds = item->second->bounds;
        } else {
            c.bounds = layerBounds(l, t, options);
        }
        if (l.kind == LayerKind::Text) {
            c.paragraph = l.text.boxWidth > 0.0;
            c.align = l.text.align;
            c.boxWidth = l.text.boxWidth;
            c.boxHeight = l.text.boxHeight;
            c.text = l.text.text();
            c.language = l.text.language;
            if (!l.text.runs.empty()) {
                const TextStyle& s = l.text.runs.front().style;
                c.fontFamily = s.fontFamily;
                c.fontWeight = s.fontWeight;
                c.fontSize = s.fontSize;
                c.italic = s.italic;
                c.fill = s.fill.rgba;
            }
            if (item != items.end() && !item->second->text.runs.empty()) {
                const DrawTextRun& r = item->second->text.runs.front(); // the brand kit applied
                c.fontFamily = r.fontFamily;
                c.fontWeight = r.fontWeight;
                c.fill = r.fill;
            }
            if (item != items.end()) {
                c.textPath = item->second->text.path;
                c.textPathClosed = item->second->text.pathClosed;
                if (!c.textPath.empty()) {
                    c.paragraph = false; // the path is the line: no box to size
                }
            }
        }
        out.push_back(c);
        if (l.kind == LayerKind::Group)
            walk(l.children, l.id, c.toDesign, t, items, options, out);
    }
}

/// The graphic clip resolved at the playhead (a worker thread: it only reads the snapshot).
std::shared_ptr<const GraphicCanvasBridge::Resolved> computeResolved(const ProjectSnapshot& snap, SequenceId seqId, ClipId clipId,
                                                                     Ticks playhead, const TextMeasurer* measurer,
                                                                     const LineBreaker* breaker) {
    auto out = std::make_shared<GraphicCanvasBridge::Resolved>();
    out->clip = clipId;
    out->sequence = seqId;
    const Sequence* seq = snap ? snap->findSequence(seqId) : nullptr;
    const auto located = seq ? seq->findClip(clipId) : std::nullopt;
    const Graphic* graphic = located ? located->clip().graphic() : nullptr;
    if (!graphic)
        return out;
    const Clip& clip = located->clip();
    out->sequenceWidth = std::max(1, seq->width);
    out->sequenceHeight = std::max(1, seq->height);
    out->designWidth = std::max(1, graphic->width);
    out->designHeight = std::max(1, graphic->height);
    out->shown = playhead >= clip.start() && playhead < clip.end();
    const Ticks t = std::clamp(playhead, clip.start(), std::max(clip.start(), clip.end() - Ticks{1}));
    out->graphicTime = graphicTimeAt(clip, *graphic, t);

    // The design canvas in the clip's frame (contain, centred), the frame on the sequence through the clip's Motion.
    double fw = out->sequenceWidth;
    double fh = out->sequenceHeight;
    if (const MediaItem* item = snap->findMedia(clip.media); item && !item->info.video.empty()) {
        const VideoStreamInfo& v = item->info.video.front();
        if (v.width > 0 && v.height > 0) {
            fw = v.width;
            fh = v.height;
        }
    }
    const double s = std::min(fw / out->designWidth, fh / out->designHeight);
    const Affine2D fit = translation((fw - out->designWidth * s) / 2.0, (fh - out->designHeight * s) / 2.0) * scaling(s, s);
    const ClipProperties& p = clip.properties();
    const Ticks ct = clip.curveTimeAt(t);
    render::LayerTransform motion;
    motion.position = {p.value(ParamId::PositionX, ct), p.value(ParamId::PositionY, ct)};
    motion.scale = p.value(ParamId::Scale, ct);
    motion.rotationDegrees = p.value(ParamId::Rotation, ct);
    motion.anchor = {p.value(ParamId::AnchorX, ct), p.value(ParamId::AnchorY, ct)};
    const render::Affine2D frame = render::layerToCanvas(motion, fw, fh, 1.0, {seq->width, seq->height});
    out->designToSequence = Affine2D{frame.a, frame.b, frame.c, frame.d, frame.e, frame.f} * fit;

    ResolveOptions options;
    options.measurer = measurer;
    options.lineBreaker = breaker;
    const DrawList list = resolveGraphicClip(*snap, clip, t, options);
    std::unordered_map<std::uint32_t, const DrawItem*> items;
    for (const DrawItem& item : list.items)
        items[item.layer] = &item;
    ResolveOptions bounds = options;
    bounds.brandKit = snap->brandKit ? &*snap->brandKit : nullptr;
    walk(graphic->layers, 0, Affine2D{}, out->graphicTime, items, bounds, out->layers);
    return out;
}

QString pickKeyOf(ClipId clip, std::uint32_t layer) {
    QString key = clipKey(clip);
    if (layer != 0)
        key += QLatin1Char('/') + QString::number(layer);
    return key;
}

QString pickKeyOf(const GraphicCanvasBridge::PickObject& object) { return pickKeyOf(object.clip, object.layer); }

/// The corners of the box (x0, y0)-(x1, y1) through `m`, as fractions of a w x h canvas.
std::array<double, 8> quadThrough(const render::Affine2D& m, double x0, double y0, double x1, double y1, double w, double h) {
    std::array<double, 8> out{};
    std::size_t i = 0;
    for (const auto& [x, y] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x1, y1}, std::pair{x0, y1}}) {
        const render::Vec2 p = m.map({x, y});
        out[i++] = p.x / w;
        out[i++] = p.y / h;
    }
    return out;
}

/// What the Crop effects leave of a w x h frame: { x0, y0, x1, y1 } in frame pixels (x1 <= x0 when cropped away).
std::array<double, 4> croppedFrame(const std::vector<render::LayerEffect>& effects, double w, double h) {
    double left = 0, top = 0, right = 0, bottom = 0;
    for (const render::LayerEffect& e : effects) {
        if (e.type != render::EffectType::Crop)
            continue;
        left = std::max(left, std::clamp(e.params[0], 0.0, 1.0));
        top = std::max(top, std::clamp(e.params[1], 0.0, 1.0));
        right = std::max(right, std::clamp(e.params[2], 0.0, 1.0));
        bottom = std::max(bottom, std::clamp(e.params[3], 0.0, 1.0));
    }
    return {left * w, top * h, (1.0 - right) * w, (1.0 - bottom) * h};
}

/// A title's inline editor (as GraphicCanvasBridge::selection.editor): its text's box, its look at the canvas's scale.
QVariantMap titleEditor(const render::TitleStyle& title, const render::LayerTransform& motion, const std::array<double, 8>& quad,
                        double canvasHeight) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (std::size_t i = 0; i + 1 < quad.size(); i += 2) {
        x0 = std::min(x0, quad[i]);
        x1 = std::max(x1, quad[i]);
        y0 = std::min(y0, quad[i + 1]);
        y1 = std::max(y1, quad[i + 1]);
    }
    const QString style = QString::fromStdString(title.fontStyle);
    const QColor color = QColor::fromRgbF(static_cast<float>(std::clamp(title.fill[0], 0.0, 1.0)),
                                          static_cast<float>(std::clamp(title.fill[1], 0.0, 1.0)),
                                          static_cast<float>(std::clamp(title.fill[2], 0.0, 1.0)),
                                          static_cast<float>(std::clamp(title.fillOpacity, 0.0, 1.0)));
    return {{QStringLiteral("x"), x0},
            {QStringLiteral("y"), y0},
            {QStringLiteral("width"), x1 - x0},
            {QStringLiteral("height"), y1 - y0},
            {QStringLiteral("fontFamily"), QString::fromStdString(title.fontFamily)},
            {QStringLiteral("fontSize"), title.sizePx * std::abs(motion.scale) / std::max(1.0, canvasHeight)},
            {QStringLiteral("fontWeight"), style.contains(QLatin1String("Black")) ? 900
                                           : style.contains(QLatin1String("Bold")) ? 700 : 400},
            {QStringLiteral("italic"), style.contains(QLatin1String("Italic")) || style.contains(QLatin1String("Oblique"))},
            {QStringLiteral("color"), color.name(QColor::HexArgb)},
            {QStringLiteral("align"), title.align == render::TitleAlign::Left    ? QStringLiteral("left")
                                      : title.align == render::TitleAlign::Right ? QStringLiteral("right")
                                                                                 : QStringLiteral("center")},
            {QStringLiteral("text"), QString::fromStdString(title.text)},
            {QStringLiteral("language"), QString()},
            {QStringLiteral("rotation"), motion.rotationDegrees}};
}

/// Everything a click on the picture can pick at `playhead`, top first: the frame as the program monitor composites it
/// (playback::evaluateVideoLayers, nothing culled), a graphic clip as its layers, a title as its text's box, a nested
/// clip as one object. A worker thread may run it: it only reads the snapshot (titles are laid out by the shared
/// TitleRenderer, which is thread-safe).
std::shared_ptr<const GraphicCanvasBridge::PickStack> computePickStack(const ProjectSnapshot& snap, SequenceId seqId, Ticks playhead,
                                                                       int revision, const TextMeasurer* measurer,
                                                                       const LineBreaker* breaker) {
    using Bridge = GraphicCanvasBridge;
    auto out = std::make_shared<Bridge::PickStack>();
    out->sequence = seqId;
    out->playhead = playhead;
    out->revision = revision;
    const Sequence* seq = snap ? snap->findSequence(seqId) : nullptr;
    if (!seq || seq->width <= 0 || seq->height <= 0)
        return out;
    const double w = seq->width;
    const double h = seq->height;
    const render::CanvasSize canvas{seq->width, seq->height};
    playback::PreviewSettings preview;
    preview.useProxies = true; // what the monitor can show: an offline original's proxy too
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(*snap, *seq, playhead, specs, /*cullOccluded*/ false, &preview, nullptr);

    const auto clipName = [&](ClipId id) {
        const auto located = seq->findClip(id);
        return located ? QString::fromStdString(located->clip().name()) : QString();
    };
    std::vector<Bridge::PickObject> bottomUp;
    const auto addFrame = [&](const playback::LayerSpec& s, double fw, double fh, double pixelAspect) {
        const auto crop = croppedFrame(s.effects, fw, fh);
        if (crop[2] <= crop[0] || crop[3] <= crop[1])
            return;
        Bridge::PickObject o;
        o.clip = s.clip;
        o.kind = QStringLiteral("clip");
        o.name = clipName(s.clip);
        o.quad = quadThrough(render::layerToCanvas(s.transform, fw, fh, pixelAspect, canvas), crop[0], crop[1], crop[2], crop[3], w, h);
        bottomUp.push_back(std::move(o));
    };
    const std::function<void(std::size_t, std::size_t)> collect = [&](std::size_t from, std::size_t to) {
        for (std::size_t j = from; j < to;) {
            const playback::LayerSpec& s = specs[j];
            const std::size_t next = std::min(to, playback::nextSibling(specs, j));
            if (s.nested) {
                if (!s.matte && !s.clip.isValid())
                    collect(j + 1, next); // a track's or the sequence's effects: a group over clips in place
                else if (!s.matte) // a nested or multicam clip: its canvas, placed with its Motion
                    addFrame(s, std::max(1, s.nestedCanvas.width), std::max(1, s.nestedCanvas.height), 1.0);
                j = next;
                continue;
            }
            ++j;
            if (s.caption || !s.clip.isValid() || !s.item)
                continue;
            if (s.graphic) {
                const auto resolved = computeResolved(snap, seqId, s.clip, playhead, measurer, breaker);
                for (const Bridge::Layer& l : resolved->layers) {
                    if (l.kind == LayerKind::Group || l.bounds.width <= 0.0 || l.bounds.height <= 0.0)
                        continue;
                    const Affine2D m = resolved->designToSequence * l.toDesign;
                    const render::Affine2D toCanvas{m.a, m.b, m.c, m.d, m.e, m.f};
                    Bridge::PickObject o;
                    o.clip = s.clip;
                    o.layer = l.id;
                    o.kind = kindName(l.kind);
                    o.name = QString::fromStdString(l.name);
                    o.text = l.kind == LayerKind::Text && !l.pinned;
                    o.quad = quadThrough(toCanvas, l.bounds.x, l.bounds.y, l.bounds.x + l.bounds.width,
                                         l.bounds.y + l.bounds.height, resolved->sequenceWidth, resolved->sequenceHeight);
                    bottomUp.push_back(std::move(o));
                }
                continue;
            }
            if (s.title) {
                const std::shared_ptr<const render::TitleRaster> raster = render::TitleRenderer::shared().render(*s.title);
                const auto& b = raster->info().bounds;
                const double fw = std::max(1, s.title->frameWidth);
                const double fh = std::max(1, s.title->frameHeight);
                const auto crop = croppedFrame(s.effects, fw, fh);
                const double x0 = std::max(b[0], crop[0]);
                const double y0 = std::max(b[1], crop[1]);
                const double x1 = std::min(b[0] + b[2], crop[2]);
                const double y1 = std::min(b[1] + b[3], crop[3]);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                Bridge::PickObject o;
                o.clip = s.clip;
                o.kind = QStringLiteral("title");
                o.name = clipName(s.clip);
                o.text = true;
                o.quad = quadThrough(render::layerToCanvas(s.transform, fw, fh, 1.0, canvas), x0, y0, x1, y1, w, h);
                o.editor = titleEditor(*s.title, s.transform, o.quad, h);
                bottomUp.push_back(std::move(o));
                continue;
            }
            if (s.fill) {
                addFrame(s, s.fillSize.width, s.fillSize.height, 1.0);
                continue;
            }
            const auto stream = static_cast<std::size_t>(s.streamIndex);
            if (stream >= s.item->info.video.size())
                continue;
            const VideoStreamInfo& v = s.item->info.video[stream];
            if (v.width > 0 && v.height > 0)
                addFrame(s, v.width, v.height, v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0);
        }
    };
    collect(0, specs.size());
    out->objects.assign(std::make_move_iterator(bottomUp.rbegin()), std::make_move_iterator(bottomUp.rend()));
    return out;
}

} // namespace

/// Results of the worker come back through this while the bridge is alive (the destructor clears it under the lock).
struct GraphicCanvasBridge::Gate {
    std::mutex mutex;
    GraphicCanvasBridge* bridge = nullptr;
};

GraphicCanvasBridge::GraphicCanvasBridge(QObject* parent) : QObject(parent), m_gate(std::make_shared<Gate>()) {
    m_gate->bridge = this;
    // The frame is read again once edits and playhead moves settle: during playback (a move every frame) it never is.
    m_stackSettle = new QTimer(this);
    m_stackSettle->setSingleShot(true);
    m_stackSettle->setInterval(120);
    connect(m_stackSettle, &QTimer::timeout, this, &GraphicCanvasBridge::startPickStack);
}

GraphicCanvasBridge::~GraphicCanvasBridge() {
    std::lock_guard lock(m_gate->mutex);
    m_gate->bridge = nullptr;
}

GraphicCanvasBridge* GraphicCanvasBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new GraphicCanvasBridge;
    bridge->setMeasurer(qtTextMeasurer()); // the same text layout as Essential Graphics and its thumbnails
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void GraphicCanvasBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &GraphicCanvasBridge::scheduleResolve);
        connect(controller, &ProjectController::playheadChanged, this, &GraphicCanvasBridge::scheduleResolve);
        connect(controller, &ProjectController::selectionChanged, this, &GraphicCanvasBridge::onSelectionChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &GraphicCanvasBridge::onSelectionChanged);
        connect(controller, &ProjectController::revisionChanged, this, &GraphicCanvasBridge::schedulePickStack);
        connect(controller, &ProjectController::playheadChanged, this, &GraphicCanvasBridge::schedulePickStack);
        connect(controller, &ProjectController::sequenceChanged, this, &GraphicCanvasBridge::schedulePickStack);
    }
    m_stack.reset();
    m_pickedKey.clear();
    onSelectionChanged();
    schedulePickStack();
}

void GraphicCanvasBridge::setMeasurer(std::shared_ptr<const TextMeasurer> measurer) {
    m_measurer = std::move(measurer);
    scheduleResolve();
}

namespace {

/// The graphic clip the handles are for: the explicit one, else the primary selection when it is a graphic.
std::optional<ClipId> effectiveClip(ProjectController* controller, const std::optional<ClipId>& explicitClip) {
    if (!controller)
        return std::nullopt;
    const SequencePtr seq = controller->currentSequence();
    const auto isGraphic = [&](ClipId id) {
        const auto located = seq ? seq->findClip(id) : std::nullopt;
        return located && located->clip().graphic();
    };
    if (explicitClip && isGraphic(*explicitClip))
        return explicitClip;
    const ClipId primary = controller->primaryClip();
    if (primary.isValid() && isGraphic(primary))
        return primary;
    return std::nullopt;
}

} // namespace

QString GraphicCanvasBridge::selectedClip() const {
    const auto clip = effectiveClip(m_controller, m_explicitClip);
    return clip ? clipKey(*clip) : QString();
}

void GraphicCanvasBridge::setSelectedClip(const QString& clip) {
    const QString before = selectedClip();
    m_explicitClip = parseClipKey(clip);
    if (selectedClip() != before) {
        m_selectedLayer = 0;
        emit selectedLayerChanged();
        emit selectedClipChanged();
    }
    scheduleResolve();
}

void GraphicCanvasBridge::onSelectionChanged() {
    const QString before = selectedClip();
    m_explicitClip.reset(); // the timeline's selection wins over the panel's choice
    if (selectedClip() != before) {
        if (m_drag)
            endDrag();
        m_selectedLayer = 0;
        emit selectedLayerChanged();
        emit selectedClipChanged();
    }
    scheduleResolve();
    refreshPick();
}

void GraphicCanvasBridge::setSelectedLayer(int layer) {
    const auto id = static_cast<std::uint32_t>(std::max(0, layer));
    if (id == m_selectedLayer)
        return;
    m_selectedLayer = id;
    emit selectedLayerChanged();
    publish();
    refreshPick();
}

void GraphicCanvasBridge::setActionSafe(double inset) {
    if (!differs(inset, m_actionSafe))
        return;
    m_actionSafe = std::clamp(inset, 0.0, 0.45);
    emit marginsChanged();
}

void GraphicCanvasBridge::setTitleSafe(double inset) {
    if (!differs(inset, m_titleSafe))
        return;
    m_titleSafe = std::clamp(inset, 0.0, 0.45);
    emit marginsChanged();
}

void GraphicCanvasBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void GraphicCanvasBridge::refresh() { scheduleResolve(); }

void GraphicCanvasBridge::scheduleResolve() {
    if (!m_controller)
        return;
    if (m_running) {
        m_pending = true; // the newest state is resolved once this one is back
        return;
    }
    const auto clip = effectiveClip(m_controller, m_explicitClip);
    if (!clip) {
        const bool had = m_resolved != nullptr;
        m_resolved.reset();
        if (had || m_active)
            publish();
        return;
    }
    const ProjectSnapshot snap = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    const Ticks playhead = m_controller->playhead();
    std::shared_ptr<const TextMeasurer> measurer = m_measurer;
    std::shared_ptr<const LineBreaker> breaker = qtLineBreaker();
    const quint64 generation = ++m_generation;
    m_running = true;
    auto work = [snap, sequence, clip = *clip, playhead, measurer, breaker] {
        return computeResolved(snap, sequence, clip, playhead, measurer.get(), breaker.get());
    };
    if (m_synchronous) {
        finishResolve(work(), generation, false);
        return;
    }
    QThreadPool::globalInstance()->start([gate = m_gate, work = std::move(work), generation] {
        std::shared_ptr<const Resolved> resolved;
        try { // QThreadPool does not catch: an exception would end the process
            resolved = work();
        } catch (...) {
            resolved = nullptr;
        }
        const bool offGui = QThread::currentThread() != QCoreApplication::instance()->thread();
        std::lock_guard lock(gate->mutex);
        if (GraphicCanvasBridge* self = gate->bridge) {
            QMetaObject::invokeMethod(
                self, [self, resolved, generation, offGui] { self->finishResolve(resolved, generation, offGui); },
                Qt::QueuedConnection);
        }
    });
}

void GraphicCanvasBridge::finishResolve(std::shared_ptr<const Resolved> resolved, quint64 generation, bool offGuiThread) {
    m_running = false;
    ++m_resolveCount;
    m_lastOffGuiThread = offGuiThread;
    const bool newest = generation == m_generation;
    if (!m_drag && newest) // during a drag the handles follow the gesture's own values
        m_resolved = std::move(resolved);
    if (m_pending) {
        m_pending = false;
        scheduleResolve();
    }
    publish();
}

const GraphicCanvasBridge::Layer* GraphicCanvasBridge::layer(std::uint32_t id) const {
    if (!m_resolved || id == 0)
        return nullptr;
    for (const Layer& l : m_resolved->layers) {
        if (l.id == id)
            return &l;
    }
    return nullptr;
}

std::array<double, 2> GraphicCanvasBridge::toDesign(double fx, double fy) const {
    if (!m_resolved)
        return {0.0, 0.0};
    return inverted(m_resolved->designToSequence).map(fx * m_resolved->sequenceWidth, fy * m_resolved->sequenceHeight);
}

QVariantMap GraphicCanvasBridge::pointJs(double dx, double dy) const {
    const auto s = m_resolved->designToSequence.map(dx, dy);
    return {{QStringLiteral("x"), s[0] / m_resolved->sequenceWidth}, {QStringLiteral("y"), s[1] / m_resolved->sequenceHeight}};
}

QVariantList GraphicCanvasBridge::quadJs(const Affine2D& toDesign, const Box& b) const {
    QVariantList out;
    const Affine2D m = m_resolved->designToSequence * toDesign;
    for (const auto& [x, y] : {std::pair{b.x, b.y}, std::pair{b.x + b.width, b.y}, std::pair{b.x + b.width, b.y + b.height},
                               std::pair{b.x, b.y + b.height}}) {
        const auto p = m.map(x, y);
        out << p[0] / m_resolved->sequenceWidth << p[1] / m_resolved->sequenceHeight;
    }
    return out;
}

QVariantMap GraphicCanvasBridge::canvasRect() const {
    if (!m_resolved)
        return {};
    const auto a = m_resolved->designToSequence.map(0, 0);
    const auto b = m_resolved->designToSequence.map(m_resolved->designWidth, m_resolved->designHeight);
    return {{QStringLiteral("x"), std::min(a[0], b[0]) / m_resolved->sequenceWidth},
            {QStringLiteral("y"), std::min(a[1], b[1]) / m_resolved->sequenceHeight},
            {QStringLiteral("width"), std::abs(b[0] - a[0]) / m_resolved->sequenceWidth},
            {QStringLiteral("height"), std::abs(b[1] - a[1]) / m_resolved->sequenceHeight}};
}

void GraphicCanvasBridge::publish() {
    const bool wasActive = m_active;
    m_active = m_resolved && m_resolved->shown;
    m_layersJs.clear();
    m_selectionJs.clear();
    if (m_resolved) {
        for (const Layer& l : m_resolved->layers) {
            m_layersJs.append(QVariantMap{{QStringLiteral("id"), static_cast<int>(l.id)},
                                          {QStringLiteral("parent"), static_cast<int>(l.parent)},
                                          {QStringLiteral("kind"), kindName(l.kind)},
                                          {QStringLiteral("name"), QString::fromStdString(l.name)},
                                          {QStringLiteral("quad"), quadJs(l.toDesign, l.bounds)},
                                          {QStringLiteral("pinned"), l.pinned},
                                          {QStringLiteral("text"), l.kind == LayerKind::Text}});
        }
        if (const Layer* s = layer(m_selectedLayer)) {
            const Box& b = s->bounds;
            const Affine2D m = s->toDesign;
            const auto at = [&](double lx, double ly) {
                const auto d = m.map(lx, ly);
                return pointJs(d[0], d[1]);
            };
            const auto handle = [&](const QString& kind, double lx, double ly) {
                QVariantMap h = at(lx, ly);
                h.insert(QStringLiteral("kind"), kind);
                return h;
            };
            QVariantList handles;
            if (!s->pinned) {
                if (s->paragraph) {
                    const bool right = s->align != TextAlign::Right;
                    const double edgeX = right ? b.x + b.width : b.x;
                    handles << handle(right ? QStringLiteral("box-right") : QStringLiteral("box-left"), edgeX, b.y + b.height / 2)
                            << handle(QStringLiteral("box-bottom"), b.x + b.width / 2, b.y + b.height)
                            << handle(QStringLiteral("box-corner"), edgeX, b.y + b.height);
                } else {
                    handles << handle(QStringLiteral("scale-tl"), b.x, b.y) << handle(QStringLiteral("scale-tr"), b.x + b.width, b.y)
                            << handle(QStringLiteral("scale-br"), b.x + b.width, b.y + b.height)
                            << handle(QStringLiteral("scale-bl"), b.x, b.y + b.height);
                }
                // Rotation: beyond the top edge's middle, away from the centre (in sequence pixels).
                const Affine2D toSeq = m_resolved->designToSequence * m;
                const auto top = toSeq.map(b.x + b.width / 2, b.y);
                const auto centre = toSeq.map(b.x + b.width / 2, b.y + b.height / 2);
                double ux = top[0] - centre[0];
                double uy = top[1] - centre[1];
                const double len = std::hypot(ux, uy);
                if (len < 1e-6) {
                    ux = 0;
                    uy = -1;
                } else {
                    ux /= len;
                    uy /= len;
                }
                const double reach = 0.025 * m_resolved->sequenceWidth;
                handles << QVariantMap{{QStringLiteral("kind"), QStringLiteral("rotate")},
                                       {QStringLiteral("x"), (top[0] + ux * reach) / m_resolved->sequenceWidth},
                                       {QStringLiteral("y"), (top[1] + uy * reach) / m_resolved->sequenceHeight},
                                       {QStringLiteral("fromX"), top[0] / m_resolved->sequenceWidth},
                                       {QStringLiteral("fromY"), top[1] / m_resolved->sequenceHeight}};
            }
            // 0.2, type on a path: a handle on every point of the path, and the path itself to draw.
            QVariantList pathLine;
            if (!s->textPath.empty()) {
                if (!s->pinned) {
                    for (std::size_t i = 0; i < s->textPath.size(); ++i) {
                        handles << handle(QStringLiteral("pathVertex-%1").arg(i), s->textPath[i].x, s->textPath[i].y);
                    }
                }
                const TextPathWalker walker(s->textPath, s->textPathClosed, false);
                const int steps = 96;
                for (int i = 0; i <= steps; ++i) {
                    const auto p = walker.at(walker.length() * i / steps);
                    const QVariantMap point = at(p[0], p[1]);
                    pathLine << point.value(QStringLiteral("x")) << point.value(QStringLiteral("y"));
                }
            }
            const auto anchor = s->parentToDesign.map(s->x, s->y);
            QVariantMap selection{{QStringLiteral("id"), static_cast<int>(s->id)},
                                  {QStringLiteral("textPath"), pathLine},
                                  {QStringLiteral("kind"), kindName(s->kind)},
                                  {QStringLiteral("name"), QString::fromStdString(s->name)},
                                  {QStringLiteral("quad"), quadJs(m, b)},
                                  {QStringLiteral("anchor"), pointJs(anchor[0], anchor[1])},
                                  {QStringLiteral("handles"), handles},
                                  {QStringLiteral("pinned"), s->pinned},
                                  {QStringLiteral("paragraph"), s->paragraph}};
            if (s->kind == LayerKind::Text) {
                // The inline editor: the box's bounding rectangle, the first run's look at the monitor's scale.
                const QVariantList quad = quadJs(m, b);
                double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
                for (int i = 0; i + 1 < quad.size(); i += 2) {
                    x0 = std::min(x0, quad[i].toDouble());
                    x1 = std::max(x1, quad[i].toDouble());
                    y0 = std::min(y0, quad[i + 1].toDouble());
                    y1 = std::max(y1, quad[i + 1].toDouble());
                }
                const Affine2D toSeq = m_resolved->designToSequence * m;
                const double verticalScale = std::hypot(toSeq.c, toSeq.d);
                const QColor color = QColor::fromRgbF(static_cast<float>(std::clamp(s->fill[0], 0.0, 1.0)),
                                                      static_cast<float>(std::clamp(s->fill[1], 0.0, 1.0)),
                                                      static_cast<float>(std::clamp(s->fill[2], 0.0, 1.0)),
                                                      static_cast<float>(std::clamp(s->fill[3], 0.0, 1.0)));
                selection.insert(QStringLiteral("editor"),
                                 QVariantMap{{QStringLiteral("x"), x0},
                                             {QStringLiteral("y"), y0},
                                             {QStringLiteral("width"), x1 - x0},
                                             {QStringLiteral("height"), y1 - y0},
                                             {QStringLiteral("fontFamily"), QString::fromStdString(s->fontFamily)},
                                             {QStringLiteral("fontSize"), s->fontSize * verticalScale / m_resolved->sequenceHeight},
                                             {QStringLiteral("fontWeight"), s->fontWeight},
                                             {QStringLiteral("italic"), s->italic},
                                             {QStringLiteral("color"), color.name(QColor::HexArgb)},
                                             {QStringLiteral("align"), alignName(s->align)},
                                             {QStringLiteral("text"), QString::fromStdString(s->text)},
                                             {QStringLiteral("language"), QString::fromStdString(s->language)},
                                             {QStringLiteral("rotation"), std::atan2(toSeq.b, toSeq.a) * kDegrees}});
            }
            m_selectionJs = selection;
        }
    }
    Q_UNUSED(wasActive)
    emit stateChanged();
}

int GraphicCanvasBridge::layerAt(double fx, double fy) const {
    if (!m_resolved || !m_resolved->shown)
        return 0;
    const auto p = toDesign(fx, fy);
    for (auto it = m_resolved->layers.rbegin(); it != m_resolved->layers.rend(); ++it) {
        if (it->kind == LayerKind::Group || it->bounds.width <= 0.0 || it->bounds.height <= 0.0)
            continue;
        const auto local = inverted(it->toDesign).map(p[0], p[1]);
        const Box& b = it->bounds;
        if (local[0] >= b.x && local[0] <= b.x + b.width && local[1] >= b.y && local[1] <= b.y + b.height)
            return static_cast<int>(it->id);
    }
    return 0;
}

bool GraphicCanvasBridge::beginDrag(const QString& kind, double fx, double fy) {
    const Layer* s = layer(m_selectedLayer);
    if (!s || !m_resolved || !m_resolved->shown || !m_controller)
        return false;
    if (s->pinned) {
        setMessage(tr("This layer is pinned to another one: it follows that layer"));
        return false;
    }
    if (kind.startsWith(QLatin1String("box-")) && !s->paragraph)
        return false;
    if (kind.startsWith(QLatin1String("pathVertex-")) &&
        (s->textPath.empty() || kind.mid(11).toULongLong() >= s->textPath.size()))
        return false;
    if (m_drag)
        endDrag();
    const auto p = toDesign(fx, fy);
    Drag d;
    d.kind = kind;
    d.layer = s->id;
    d.start = *s;
    d.px = p[0];
    d.py = p[1];
    d.mergeKey = QStringLiteral("graphicCanvas/%1/%2/%3/%4")
                     .arg(m_resolved->clip.value)
                     .arg(s->id)
                     .arg(kind)
                     .arg(++m_gestures)
                     .toStdString();
    m_drag = std::move(d);
    setMessage({});
    emit draggingChanged();
    return true;
}

void GraphicCanvasBridge::dragTo(double fx, double fy, bool shift, bool ctrl, double snap) {
    if (!m_drag || !m_resolved)
        return;
    Drag& d = *m_drag;
    const Layer& s = d.start;
    const auto p = toDesign(fx, fy);
    const Affine2D parentInverse = inverted(s.parentToDesign);
    Layer next = s;
    const char* label = QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Move Layer");
    QVariantList guides;

    if (d.kind == QLatin1String("move")) {
        double dx = p[0] - d.px;
        double dy = p[1] - d.py;
        if (!ctrl && snap > 0.0) {
            // The layer's box on the design canvas, moved; snap its edges and centre to the guides within reach.
            const Affine2D& m = s.toDesign;
            const Box& b = s.bounds;
            double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
            for (const auto& [x, y] : {std::pair{b.x, b.y}, std::pair{b.x + b.width, b.y}, std::pair{b.x + b.width, b.y + b.height},
                                       std::pair{b.x, b.y + b.height}}) {
                const auto q = m.map(x, y);
                bx0 = std::min(bx0, q[0]);
                bx1 = std::max(bx1, q[0]);
                by0 = std::min(by0, q[1]);
                by1 = std::max(by1, q[1]);
            }
            const double unit = std::hypot(m_resolved->designToSequence.a, m_resolved->designToSequence.b); // seq px per design px
            const double reach = snap * m_resolved->sequenceWidth / std::max(unit, 1e-9);
            std::vector<double> xs{0.0, m_resolved->designWidth / 2.0, m_resolved->designWidth};
            std::vector<double> ys{0.0, m_resolved->designHeight / 2.0, m_resolved->designHeight};
            // The safe margins are the sequence frame's: back to the design canvas.
            const Affine2D seqToDesign = inverted(m_resolved->designToSequence);
            for (const double inset : {m_actionSafe, m_titleSafe}) {
                const auto a = seqToDesign.map(m_resolved->sequenceWidth * inset, m_resolved->sequenceHeight * inset);
                const auto z = seqToDesign.map(m_resolved->sequenceWidth * (1 - inset), m_resolved->sequenceHeight * (1 - inset));
                xs.push_back(a[0]);
                xs.push_back(z[0]);
                ys.push_back(a[1]);
                ys.push_back(z[1]);
            }
            for (const Layer& other : m_resolved->layers) {
                if (other.id == s.id || other.kind == LayerKind::Group || other.parent == s.id)
                    continue;
                double ox0 = 1e18, oy0 = 1e18, ox1 = -1e18, oy1 = -1e18;
                const Box& ob = other.bounds;
                for (const auto& [x, y] : {std::pair{ob.x, ob.y}, std::pair{ob.x + ob.width, ob.y},
                                           std::pair{ob.x + ob.width, ob.y + ob.height}, std::pair{ob.x, ob.y + ob.height}}) {
                    const auto q = other.toDesign.map(x, y);
                    ox0 = std::min(ox0, q[0]);
                    ox1 = std::max(ox1, q[0]);
                    oy0 = std::min(oy0, q[1]);
                    oy1 = std::max(oy1, q[1]);
                }
                xs.insert(xs.end(), {ox0, (ox0 + ox1) / 2.0, ox1});
                ys.insert(ys.end(), {oy0, (oy0 + oy1) / 2.0, oy1});
            }
            const auto best = [&](const std::vector<double>& candidates, std::initializer_list<double> edges,
                                  double& shiftBy) -> std::optional<double> {
                double nearest = reach;
                std::optional<double> found;
                for (const double e : edges) {
                    for (const double c : candidates) {
                        const double gap = c - e;
                        if (std::abs(gap) <= nearest) {
                            nearest = std::abs(gap);
                            shiftBy = gap;
                            found = c;
                        }
                    }
                }
                return found;
            };
            double sx = 0.0, sy = 0.0;
            const auto gx = best(xs, {bx0 + dx, (bx0 + bx1) / 2.0 + dx, bx1 + dx}, sx);
            const auto gy = best(ys, {by0 + dy, (by0 + by1) / 2.0 + dy, by1 + dy}, sy);
            if (gx) {
                dx += sx;
                const auto at = m_resolved->designToSequence.map(*gx, m_resolved->designHeight / 2.0);
                guides << QVariantMap{{QStringLiteral("orientation"), QStringLiteral("vertical")},
                                      {QStringLiteral("position"), at[0] / m_resolved->sequenceWidth}};
            }
            if (gy) {
                dy += sy;
                const auto at = m_resolved->designToSequence.map(m_resolved->designWidth / 2.0, *gy);
                guides << QVariantMap{{QStringLiteral("orientation"), QStringLiteral("horizontal")},
                                      {QStringLiteral("position"), at[1] / m_resolved->sequenceHeight}};
            }
        }
        const auto v = mapVector(parentInverse, dx, dy);
        next.x = s.x + v[0];
        next.y = s.y + v[1];
        d.dx = dx;
        d.dy = dy;
    } else if (d.kind.startsWith(QLatin1String("scale-"))) {
        label = QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Scale Layer");
        const Box& b = s.bounds;
        const QString corner = d.kind.mid(6);
        const double cx = corner.endsWith(QLatin1Char('l')) ? b.x : b.x + b.width;
        const double cy = corner.startsWith(QLatin1Char('t')) ? b.y : b.y + b.height;
        const auto q = parentInverse.map(p[0], p[1]);
        const auto v = mapVector(rotationMatrix(-s.rotation), q[0] - s.x, q[1] - s.y);
        const double ex = cx - s.anchorX;
        const double ey = cy - s.anchorY;
        if (shift) { // free: each axis on its own
            if (std::abs(ex) > 1e-6)
                next.scaleX = 100.0 * v[0] / ex;
            if (std::abs(ey) > 1e-6)
                next.scaleY = 100.0 * v[1] / ey;
        } else { // uniform: along the corner's direction from the anchor
            const double ux = ex * s.scaleX / 100.0;
            const double uy = ey * s.scaleY / 100.0;
            const double den = ux * ux + uy * uy;
            if (den > 1e-9) {
                const double k = (v[0] * ux + v[1] * uy) / den;
                next.scaleX = s.scaleX * k;
                next.scaleY = s.scaleY * k;
            }
        }
        next.scaleX = std::clamp(next.scaleX, -100000.0, 100000.0);
        next.scaleY = std::clamp(next.scaleY, -100000.0, 100000.0);
    } else if (d.kind == QLatin1String("rotate")) {
        label = QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Rotate Layer");
        const auto q0 = parentInverse.map(d.px, d.py);
        const auto q = parentInverse.map(p[0], p[1]);
        const double a0 = std::atan2(q0[1] - s.y, q0[0] - s.x);
        const double a1 = std::atan2(q[1] - s.y, q[0] - s.x);
        double rotation = s.rotation + (a1 - a0) * kDegrees;
        if (shift)
            rotation = std::round(rotation / 15.0) * 15.0;
        next.rotation = rotation;
    } else if (d.kind == QLatin1String("anchor")) {
        label = QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Move Anchor Point");
        // The anchor goes under the pointer; the position moves with it so the content stays where it is.
        const auto q = parentInverse.map(p[0], p[1]);
        const auto local = inverted(layerMatrix(s)).map(q[0], q[1]);
        next.anchorX = local[0];
        next.anchorY = local[1];
        next.x = q[0];
        next.y = q[1];
    } else if (d.kind.startsWith(QLatin1String("pathVertex-"))) {
        // 0.2, type on a path: the point goes under the pointer (its tangents with it).
        label = QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Move Path Point");
        const std::size_t index = d.kind.mid(11).toULongLong();
        if (index >= next.textPath.size())
            return;
        const auto local = inverted(s.toDesign).map(p[0], p[1]);
        next.textPath[index].x = std::clamp(local[0], -1e6, 1e6);
        next.textPath[index].y = std::clamp(local[1], -1e6, 1e6);
    } else if (d.kind.startsWith(QLatin1String("box-"))) {
        label = QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Resize Text Box");
        const auto local = inverted(s.toDesign).map(p[0], p[1]);
        const bool width = d.kind != QLatin1String("box-bottom");
        const bool height = d.kind == QLatin1String("box-bottom") || d.kind == QLatin1String("box-corner");
        if (width) {
            const double w = s.align == TextAlign::Center ? 2.0 * std::abs(local[0])
                             : s.align == TextAlign::Right ? -local[0]
                                                           : local[0];
            next.boxWidth = std::clamp(w, 8.0, 100000.0);
        }
        if (height)
            next.boxHeight = std::clamp(local[1], 8.0, 100000.0);
        next.bounds = textBox(next, next.boxWidth, next.boxHeight > 0.0 ? next.boxHeight : s.bounds.height);
    } else {
        return;
    }

    if (applyLayer(next, label, d.mergeKey)) {
        d.committed = true;
        // The handles follow the gesture's own values at once; the resolve after the drag makes them exact.
        next.toDesign = next.parentToDesign * layerMatrix(next);
        if (next.pinned)
            next.toDesign = s.toDesign;
        auto updated = std::make_shared<Resolved>(*m_resolved);
        for (Layer& l : updated->layers) {
            if (l.id == next.id)
                l = next;
        }
        m_resolved = std::move(updated);
        publish();
    }
    if (guides != m_guidesJs) {
        m_guidesJs = guides;
        emit guidesChanged();
    }
}

void GraphicCanvasBridge::endDrag() {
    if (!m_drag)
        return;
    if (m_controller)
        m_controller->endPropertyGesture();
    m_drag.reset();
    if (!m_guidesJs.isEmpty()) {
        m_guidesJs.clear();
        emit guidesChanged();
    }
    emit draggingChanged();
    scheduleResolve();
}

void GraphicCanvasBridge::cancelDrag() {
    if (!m_drag)
        return;
    const bool committed = m_drag->committed;
    if (m_controller) {
        m_controller->endPropertyGesture();
        if (committed)
            m_controller->undo();
    }
    m_drag.reset();
    if (!m_guidesJs.isEmpty()) {
        m_guidesJs.clear();
        emit guidesChanged();
    }
    emit draggingChanged();
    scheduleResolve();
}

bool GraphicCanvasBridge::applyLayer(const Layer& next, const char* label, const std::string& mergeKey) {
    if (!m_controller || !m_resolved)
        return false;
    const std::shared_ptr<const Resolved> r = m_resolved;
    const Layer* currentPtr = layer(next.id);
    if (!currentPtr)
        return false;
    const Layer base = m_drag ? m_drag->start : *currentPtr;
    const bool ok = m_controller->applyEditFn(
        label,
        [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* seq = state.findSequence(r->sequence);
            const auto located = seq ? seq->findClip(r->clip) : std::nullopt;
            const Graphic* graphic = located ? located->clip().graphic() : nullptr;
            const GraphicLayer* current = graphic ? graphic->findLayer(next.id) : nullptr;
            if (!current)
                return std::unexpected(Error{ErrorCode::NotFound, "the graphic layer is gone"});
            GraphicLayer changed = *current;
            const Ticks t = r->graphicTime;
            // Only what the gesture changes: an animated value elsewhere keeps its keys.
            if (differs(next.x, base.x))
                setAt(changed.transform.x, t, next.x);
            if (differs(next.y, base.y))
                setAt(changed.transform.y, t, next.y);
            if (differs(next.anchorX, base.anchorX))
                setAt(changed.transform.anchorX, t, next.anchorX);
            if (differs(next.anchorY, base.anchorY))
                setAt(changed.transform.anchorY, t, next.anchorY);
            if (differs(next.scaleX, base.scaleX))
                setAt(changed.transform.scaleX, t, next.scaleX);
            if (differs(next.scaleY, base.scaleY))
                setAt(changed.transform.scaleY, t, next.scaleY);
            if (differs(next.rotation, base.rotation))
                setAt(changed.transform.rotation, t, next.rotation);
            if (differs(next.boxWidth, base.boxWidth))
                changed.text.boxWidth = next.boxWidth;
            if (differs(next.boxHeight, base.boxHeight))
                changed.text.boxHeight = next.boxHeight;
            if (next.textPath != base.textPath && changed.text.path && !changed.text.path->path.empty()) {
                // A still path takes the new points; an animated one gets (or changes) its key at the playhead.
                std::vector<MaskPathKey>& keys = changed.text.path->path;
                if (keys.size() == 1) {
                    keys.front().vertices = next.textPath;
                } else {
                    const auto key = std::find_if(keys.begin(), keys.end(), [&](const MaskPathKey& k) { return k.time == t; });
                    if (key != keys.end()) {
                        key->vertices = next.textPath;
                    } else {
                        const auto after = std::find_if(keys.begin(), keys.end(), [&](const MaskPathKey& k) { return k.time > t; });
                        keys.insert(after, MaskPathKey{t, next.textPath, keys.front().interpolation});
                    }
                }
            }
            return edit::updateGraphicLayer(state, r->sequence, r->clip, next.id, std::move(changed));
        },
        mergeKey);
    if (!ok)
        setMessage(m_controller->lastError());
    return ok;
}

bool GraphicCanvasBridge::nudge(double dx, double dy) {
    const Layer* s = layer(m_selectedLayer);
    if (!s || !m_resolved || !m_resolved->shown || m_drag)
        return false;
    if (s->pinned) {
        setMessage(tr("This layer is pinned to another one: it follows that layer"));
        return false;
    }
    const Layer start = *s;
    Layer next = start;
    const auto v = mapVector(inverted(start.parentToDesign), dx, dy);
    next.x += v[0];
    next.y += v[1];
    if (!applyLayer(next, QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Nudge Layer"), {}))
        return false;
    next.toDesign = next.parentToDesign * layerMatrix(next);
    auto updated = std::make_shared<Resolved>(*m_resolved);
    for (Layer& l : updated->layers) {
        if (l.id == next.id)
            l = next;
    }
    m_resolved = std::move(updated);
    publish();
    scheduleResolve();
    return true;
}

void GraphicCanvasBridge::selectAdjacent(int direction) {
    if (!m_resolved || m_resolved->layers.empty())
        return;
    const auto& layers = m_resolved->layers;
    const auto n = static_cast<int>(layers.size());
    int index = -1;
    for (int i = 0; i < n; ++i) {
        if (layers[static_cast<std::size_t>(i)].id == m_selectedLayer)
            index = i;
    }
    const int step = direction < 0 ? -1 : 1;
    index = index < 0 ? (step > 0 ? 0 : n - 1) : ((index + step) % n + n) % n;
    setSelectedLayer(static_cast<int>(layers[static_cast<std::size_t>(index)].id));
}

QString GraphicCanvasBridge::setLayerText(int layerId, const QString& text) {
    if (!m_controller || !m_resolved)
        return tr("No graphic is selected");
    const std::shared_ptr<const Resolved> r = m_resolved;
    QString plain = text;
    plain.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    plain.replace(QChar(0x2029), QLatin1Char('\n')); // paragraph separators of the text editor
    plain.replace(QChar(0x2028), QLatin1Char('\n'));
    const std::string value = plain.toStdString();
    const auto id = static_cast<std::uint32_t>(std::max(0, layerId));
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Edit Text"), [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* seq = state.findSequence(r->sequence);
            const auto located = seq ? seq->findClip(r->clip) : std::nullopt;
            const Graphic* graphic = located ? located->clip().graphic() : nullptr;
            const GraphicLayer* current = graphic ? graphic->findLayer(id) : nullptr;
            if (!current || current->kind != LayerKind::Text)
                return std::unexpected(Error{ErrorCode::NotFound, "the text layer is gone"});
            if (current->text.text() == value)
                return std::unexpected(Error{ErrorCode::InvalidArgument, "the text is unchanged"});
            GraphicLayer changed = *current;
            // One run in the style of the first (as a template's text field does).
            TextRun run;
            if (!changed.text.runs.empty())
                run.style = changed.text.runs.front().style;
            run.text = value;
            changed.text.runs = {std::move(run)};
            return edit::updateGraphicLayer(state, r->sequence, r->clip, id, std::move(changed));
        });
    if (!ok) {
        const QString error = m_controller->lastError();
        return error.contains(QLatin1String("unchanged")) ? QString() : error;
    }
    scheduleResolve();
    return {};
}

// ---- Picking ----

namespace {

/// Whether a point lies in a convex quad (fractions; boundary included).
bool quadContains(const std::array<double, 8>& q, double x, double y) {
    int sign = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const double ax = q[2 * i], ay = q[2 * i + 1];
        const double bx = q[(2 * i + 2) % 8], by = q[(2 * i + 3) % 8];
        const double cross = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        if (std::abs(cross) < 1e-12)
            continue;
        const int s = cross > 0 ? 1 : -1;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return sign != 0;
}

QVariantList quadList(const std::array<double, 8>& quad) {
    QVariantList out;
    for (const double v : quad)
        out << v;
    return out;
}

} // namespace

QString GraphicCanvasBridge::pickKey() const {
    if (!m_controller)
        return {};
    const ClipId primary = m_controller->primaryClip();
    if (!primary.isValid())
        return {};
    const auto graphic = effectiveClip(m_controller, m_explicitClip);
    return pickKeyOf(primary, graphic && *graphic == primary ? m_selectedLayer : 0);
}

std::shared_ptr<const GraphicCanvasBridge::PickStack> GraphicCanvasBridge::stackNow() {
    if (!m_controller)
        return std::make_shared<PickStack>();
    const SequenceId sequence = m_controller->currentSequenceId();
    const Ticks playhead = m_controller->playhead();
    const int revision = m_controller->revision();
    if (m_stack && m_stack->sequence == sequence && m_stack->playhead == playhead && m_stack->revision == revision)
        return m_stack;
    const std::shared_ptr<const LineBreaker> breaker = qtLineBreaker();
    m_stack = computePickStack(m_controller->snapshot(), sequence, playhead, revision, m_measurer.get(), breaker.get());
    return m_stack;
}

void GraphicCanvasBridge::schedulePickStack() {
    if (!m_controller)
        return;
    if (m_synchronous)
        startPickStack();
    else
        m_stackSettle->start();
}

void GraphicCanvasBridge::startPickStack() {
    if (!m_controller)
        return;
    if (m_stackRunning) {
        m_stackPending = true; // read again once this one is back
        return;
    }
    const ProjectSnapshot snap = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    const Ticks playhead = m_controller->playhead();
    const int revision = m_controller->revision();
    if (m_stack && m_stack->sequence == sequence && m_stack->playhead == playhead && m_stack->revision == revision) {
        refreshPick();
        return;
    }
    std::shared_ptr<const TextMeasurer> measurer = m_measurer;
    std::shared_ptr<const LineBreaker> breaker = qtLineBreaker();
    const quint64 generation = ++m_stackGeneration;
    auto work = [snap, sequence, playhead, revision, measurer, breaker] {
        return computePickStack(snap, sequence, playhead, revision, measurer.get(), breaker.get());
    };
    if (m_synchronous) {
        finishPickStack(work(), generation);
        return;
    }
    m_stackRunning = true;
    QThreadPool::globalInstance()->start([gate = m_gate, work = std::move(work), generation] {
        std::shared_ptr<const PickStack> stack;
        try { // QThreadPool does not catch: an exception would end the process
            stack = work();
        } catch (...) {
            stack = nullptr;
        }
        std::lock_guard lock(gate->mutex);
        if (GraphicCanvasBridge* self = gate->bridge) {
            QMetaObject::invokeMethod(
                self, [self, stack, generation] { self->finishPickStack(stack, generation); }, Qt::QueuedConnection);
        }
    });
}

void GraphicCanvasBridge::finishPickStack(std::shared_ptr<const PickStack> stack, quint64 generation) {
    m_stackRunning = false;
    // Kept when it is still the frame shown (a click may have read a newer one meanwhile).
    if (stack && generation == m_stackGeneration && m_controller && stack->sequence == m_controller->currentSequenceId()
        && stack->playhead == m_controller->playhead() && stack->revision == m_controller->revision())
        m_stack = std::move(stack);
    if (m_stackPending) {
        m_stackPending = false;
        startPickStack();
    }
    refreshPick();
}

std::vector<const GraphicCanvasBridge::PickObject*> GraphicCanvasBridge::objectsAt(const PickStack& stack, double fx, double fy) {
    std::vector<const PickObject*> out;
    for (const PickObject& o : stack.objects) {
        if (quadContains(o.quad, fx, fy))
            out.push_back(&o);
    }
    return out;
}

const GraphicCanvasBridge::PickObject* GraphicCanvasBridge::clickTarget(const std::vector<const PickObject*>& under) const {
    if (under.empty())
        return nullptr;
    if (!m_pickedKey.isEmpty() && pickKey() == m_pickedKey) {
        for (const PickObject* o : under) {
            if (pickKeyOf(*o) == m_pickedKey)
                return o; // picked before and under the pointer: it stays (it can be dragged from behind)
        }
    }
    return under.front();
}

void GraphicCanvasBridge::pickObject(const PickObject& object) {
    if (!m_controller)
        return;
    m_picking = true;
    if (m_controller->primaryClip() != object.clip)
        m_controller->selectClipById(object.clip, ProjectController::Replace);
    setSelectedLayer(static_cast<int>(object.layer));
    m_picking = false;
    m_pickedKey = pickKeyOf(object);
    const QVariantList outline = object.layer == 0 ? quadList(object.quad) : QVariantList();
    if (outline != m_pickOutlineJs) {
        m_pickOutlineJs = outline;
        emit pickChanged();
    }
}

void GraphicCanvasBridge::refreshPick() {
    if (m_picking)
        return;
    if (!m_pickedKey.isEmpty() && pickKey() != m_pickedKey) { // picked elsewhere since: a new cycle
        m_pickedKey.clear();
        m_editedKey.clear();
        m_gesturePrev.clear();
        m_pressValid = false;
    }
    QVariantList outline = m_pickOutlineJs;
    if (m_pickedKey.isEmpty()) {
        outline.clear();
    } else if (m_stack && m_controller && m_stack->sequence == m_controller->currentSequenceId()
               && m_stack->playhead == m_controller->playhead() && m_stack->revision == m_controller->revision()) {
        outline.clear(); // the frame read now: the picked clip's box there, if it shows
        for (const PickObject& o : m_stack->objects) {
            if (o.layer == 0 && pickKeyOf(o) == m_pickedKey)
                outline = quadList(o.quad);
        }
    }
    if (outline != m_pickOutlineJs) {
        m_pickOutlineJs = outline;
        emit pickChanged();
    }
}

QVariantMap GraphicCanvasBridge::objectJs(const PickObject& object) const {
    QVariantMap out{{QStringLiteral("key"), pickKeyOf(object)},
                    {QStringLiteral("clip"), clipKey(object.clip)},
                    {QStringLiteral("layer"), static_cast<int>(object.layer)},
                    {QStringLiteral("kind"), object.kind},
                    {QStringLiteral("name"), object.name},
                    {QStringLiteral("text"), object.text},
                    {QStringLiteral("quad"), quadList(object.quad)}};
    if (!object.editor.isEmpty())
        out.insert(QStringLiteral("editor"), object.editor);
    return out;
}

QVariantList GraphicCanvasBridge::stackAt(double fx, double fy) {
    QVariantList out;
    const auto stack = stackNow();
    for (const PickObject* o : objectsAt(*stack, fx, fy))
        out << objectJs(*o);
    return out;
}

QVariantMap GraphicCanvasBridge::pickTarget(double fx, double fy) {
    refreshPick();
    const auto stack = stackNow();
    const PickObject* target = clickTarget(objectsAt(*stack, fx, fy));
    return target ? objectJs(*target) : QVariantMap();
}

QVariantMap GraphicCanvasBridge::pickPress(double fx, double fy) {
    if (!m_controller)
        return {};
    refreshPick(); // a selection made elsewhere ends the cycle
    // The second press of a double-click belongs to the gesture the first one began.
    const QStyleHints* hints = QGuiApplication::styleHints();
    const qint64 interval = hints ? hints->mouseDoubleClickInterval() : 400;
    const bool repeat = m_pressValid && m_pressClock.isValid() && m_pressClock.elapsed() <= interval
                        && std::abs(fx - m_pressX) < 0.02 && std::abs(fy - m_pressY) < 0.02;
    if (!repeat)
        m_gesturePrev = m_pickedKey;
    m_pressValid = true;
    m_pressX = fx;
    m_pressY = fy;
    m_pressClock.restart();
    const auto stack = stackNow();
    const PickObject* target = clickTarget(objectsAt(*stack, fx, fy));
    if (!target) { // nothing there: no layer handles, no picked outline; the timeline keeps its selection
        m_pickedKey.clear();
        m_editedKey.clear();
        setSelectedLayer(0);
        refreshPick();
        return {};
    }
    pickObject(*target);
    return objectJs(*target);
}

QVariantMap GraphicCanvasBridge::pickDoubleClick(double fx, double fy) {
    if (!m_controller)
        return {};
    m_pressValid = false; // the next press begins a new gesture
    const auto stack = stackNow();
    const auto under = objectsAt(*stack, fx, fy);
    if (under.empty())
        return {};
    std::ptrdiff_t index = -1;
    if (!m_gesturePrev.isEmpty()) {
        for (std::size_t i = 0; i < under.size(); ++i) {
            if (pickKeyOf(*under[i]) == m_gesturePrev)
                index = static_cast<std::ptrdiff_t>(i);
        }
    }
    const PickObject* target = nullptr;
    bool edit = false;
    if (index < 0) { // nothing picked here yet: the topmost, edited when it is text
        target = under.front();
        edit = target->text;
    } else if (under[static_cast<std::size_t>(index)]->text && m_editedKey != m_gesturePrev) {
        target = under[static_cast<std::size_t>(index)];
        edit = true;
    } else { // one deeper, wrapping to the top
        target = under[static_cast<std::size_t>(index + 1) % under.size()];
    }
    pickObject(*target);
    m_editedKey = edit ? m_pickedKey : QString();
    m_gesturePrev = m_pickedKey;
    QVariantMap out = objectJs(*target);
    out.insert(QStringLiteral("action"), edit ? QStringLiteral("edit") : QStringLiteral("select"));
    return out;
}

QVariantList GraphicCanvasBridge::hoverAt(double fx, double fy) const {
    if (!m_stack || !m_controller || m_stack->sequence != m_controller->currentSequenceId()
        || m_stack->playhead != m_controller->playhead() || m_stack->revision != m_controller->revision())
        return {};
    const PickObject* target = clickTarget(objectsAt(*m_stack, fx, fy));
    return target ? quadList(target->quad) : QVariantList();
}

QString GraphicCanvasBridge::setTitleText(const QString& clip, const QString& text) {
    const auto id = parseClipKey(clip);
    if (!m_controller || !id)
        return tr("No title is selected");
    const ProjectSnapshot snap = m_controller->snapshot();
    const SequencePtr seq = m_controller->currentSequence();
    const auto located = seq ? seq->findClip(*id) : std::nullopt;
    const MediaItem* item = located && snap ? snap->findMedia(located->clip().media) : nullptr;
    if (!item || !item->isTitle())
        return tr("This clip is not a title");
    QString plain = text;
    plain.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    plain.replace(QChar(0x2029), QLatin1Char('\n')); // paragraph separators of the text editor
    plain.replace(QChar(0x2028), QLatin1Char('\n'));
    const std::string value = plain.toStdString();
    if (value == item->generator->text)
        return {};
    const MediaId media = item->id;
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::GraphicCanvasBridge", "Edit Title Text"),
        [&](const ProjectState& state) { return edit::setTitleText(state, media, value); });
    return ok ? QString() : m_controller->lastError();
}

} // namespace wabcut::app
