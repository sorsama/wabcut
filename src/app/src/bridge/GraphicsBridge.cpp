#include "GraphicsBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "GlobalBridge.h" // qtLineBreaker()
#include "GraphicPainter.h"
#include "GraphicText.h"
#include "ProjectController.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <list>
#include <set>

namespace wabcut::app {

using Json = nlohmann::json;

namespace {

constexpr const char* kProviderId = "graphicthumb";

Ticks fromSeconds(double s) {
    return Ticks{static_cast<std::int64_t>(std::llround(std::max(0.0, s) * static_cast<double>(kTicksPerSecond)))};
}
double toSeconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

QVariant toVariant(const Json& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(Json{{"v", value}}.dump())).toVariant().toMap().value(QStringLiteral("v")); }

Json toJson(const QVariant& value) {
    const QByteArray bytes = QJsonDocument::fromVariant(QVariantMap{{QStringLiteral("v"), value}}).toJson(QJsonDocument::Compact);
    const Json parsed = Json::parse(bytes.toStdString(), nullptr, false);
    return parsed.is_object() && parsed.contains("v") ? parsed["v"] : Json();
}

QString hexOf(const std::array<double, 4>& rgba, bool withAlpha = false) {
    const auto channel = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    QString out = QStringLiteral("#%1%2%3")
                      .arg(channel(rgba[0]), 2, 16, QLatin1Char('0'))
                      .arg(channel(rgba[1]), 2, 16, QLatin1Char('0'))
                      .arg(channel(rgba[2]), 2, 16, QLatin1Char('0'));
    if (withAlpha)
        out += QStringLiteral("%1").arg(channel(rgba[3]), 2, 16, QLatin1Char('0'));
    return out.toUpper();
}

QVariantMap colorMap(const GraphicColor& c, const BrandKit* kit) {
    const auto rgba = resolveColor(c, kit);
    return {{QStringLiteral("color"), hexOf(rgba)}, {QStringLiteral("alpha"), rgba[3]}, {QStringLiteral("brand"), QString::fromStdString(c.brand)}};
}

/// "#RRGGBB" (keeps `current`'s alpha), "#RRGGBBAA", or "brand:<name>" (the kit's colour, `current`'s as the fallback).
std::optional<GraphicColor> parseColor(const QString& text, const GraphicColor& current, const BrandKit* kit) {
    GraphicColor out = current;
    if (text.startsWith(QLatin1String("brand:"))) {
        out.brand = text.mid(6).toStdString();
        if (kit) {
            if (const BrandColor* b = kit->color(out.brand))
                out.rgba = b->rgba;
        }
        return out;
    }
    QString hex = text.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex = hex.mid(1);
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    bool ok = false;
    const uint value = hex.toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    const uint rgb = hex.size() == 8 ? (value >> 8) : value;
    out.rgba[0] = ((rgb >> 16) & 0xFF) / 255.0;
    out.rgba[1] = ((rgb >> 8) & 0xFF) / 255.0;
    out.rgba[2] = (rgb & 0xFF) / 255.0;
    if (hex.size() == 8)
        out.rgba[3] = (value & 0xFF) / 255.0;
    out.brand.clear();
    return out;
}

QVariantMap paramMap(const ParamValue& p, Ticks t) {
    const Curve* curve = p.curve();
    return {{QStringLiteral("value"), p.valueAt(t)},
            {QStringLiteral("animated"), curve != nullptr},
            {QStringLiteral("keyed"), curve != nullptr && curve->keyAt(t).has_value()}};
}

ParamValue* paramAt(GraphicLayer& l, std::string_view path) {
    if (path == "transform.x")
        return &l.transform.x;
    if (path == "transform.y")
        return &l.transform.y;
    if (path == "transform.anchorX")
        return &l.transform.anchorX;
    if (path == "transform.anchorY")
        return &l.transform.anchorY;
    if (path == "transform.scaleX")
        return &l.transform.scaleX;
    if (path == "transform.scaleY")
        return &l.transform.scaleY;
    if (path == "transform.rotation")
        return &l.transform.rotation;
    if (path == "transform.opacity")
        return &l.transform.opacity;
    if (path == "shape.width")
        return &l.shape.width;
    if (path == "shape.height")
        return &l.shape.height;
    if (path == "shape.cornerRadius")
        return &l.shape.cornerRadius;
    if (path == "shape.outerRadius")
        return &l.shape.outerRadius;
    if (path == "shape.innerRadius")
        return &l.shape.innerRadius;
    if (path == "shape.trimStart")
        return &l.shape.trimStart;
    if (path == "shape.trimEnd")
        return &l.shape.trimEnd;
    if (path == "shape.trimOffset")
        return &l.shape.trimOffset;
    if (path == "stroke.width")
        return &l.shape.stroke.width;
    return nullptr;
}

/// The list holding layer `id` (the graphic's, or a group's children) and its index there.
std::pair<std::vector<GraphicLayer>*, std::size_t> containerOf(std::vector<GraphicLayer>& layers, std::uint32_t id) {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].id == id)
            return {&layers, i};
        if (auto found = containerOf(layers[i].children, id); found.first)
            return found;
    }
    return {nullptr, 0};
}

QString kindName(LayerKind kind) { return QString::fromLatin1(toString(kind).data(), static_cast<qsizetype>(toString(kind).size())); }

template <class E>
QString nameOf(E value) {
    const std::string_view s = toString(value);
    return QString::fromLatin1(s.data(), static_cast<qsizetype>(s.size()));
}

QString sluggish(const QString& text) {
    QString out;
    for (const QChar c : text.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            out += c;
        else if ((c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.')) && !out.isEmpty() &&
                 !out.endsWith(QLatin1Char('-')))
            out += QLatin1Char('-');
    }
    while (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    return out.left(96);
}

/// The time a thumbnail shows: the middle of the responsive design's stretch, else of the template's length.
Ticks thumbnailTime(const GraphicTemplate& t) {
    if (const auto& r = t.graphic.responsive; r && r->duration.value > 0) {
        const std::int64_t middle = r->intro.value + std::max<std::int64_t>(0, r->duration.value - r->intro.value - r->outro.value) / 2;
        return Ticks{std::min(middle, r->duration.value)};
    }
    const std::int64_t length = t.duration.value > 0 ? t.duration.value : 5 * kTicksPerSecond;
    return Ticks{length / 2};
}

/// The canvas box of `ids`' draw items (the layer and its children), or nullopt when nothing is drawn.
std::optional<QRectF> canvasBox(const DrawList& list, const std::set<std::uint32_t>& ids) {
    std::optional<QRectF> box;
    for (const DrawItem& item : list.items) {
        if (!ids.contains(item.layer))
            continue;
        const Box& b = item.bounds;
        const std::array<std::array<double, 2>, 4> corners{item.transform.map(b.x, b.y), item.transform.map(b.x + b.width, b.y),
                                                           item.transform.map(b.x, b.y + b.height),
                                                           item.transform.map(b.x + b.width, b.y + b.height)};
        double minX = corners[0][0], maxX = corners[0][0], minY = corners[0][1], maxY = corners[0][1];
        for (const auto& c : corners) {
            minX = std::min(minX, c[0]);
            maxX = std::max(maxX, c[0]);
            minY = std::min(minY, c[1]);
            maxY = std::max(maxY, c[1]);
        }
        const QRectF r(QPointF(minX, minY), QPointF(maxX, maxY));
        box = box ? box->united(r) : r;
    }
    return box;
}

void collectIds(const GraphicLayer& layer, std::set<std::uint32_t>& out) {
    out.insert(layer.id);
    for (const GraphicLayer& child : layer.children)
        collectIds(child, out);
}

// ---- Built-in templates (C.16) ---------------------------------------------------------------------------------------

/// A command of `registry` on `session` (a read, or an edit as one undo step).
command::CommandResult runCatalog(EditSession& session, const command::CommandRegistry& registry,
                                  std::shared_ptr<command::CommandServices> services, const std::string& name, Json params) {
    command::EnginePolicy policy;
    policy.services = std::move(services);
    command::CommandEngine engine(session, registry, policy);
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    return engine.execute(request);
}

/// A read command on a scratch session of `state` (any thread): its data, or nullopt when it failed.
std::optional<Json> readOn(const command::CommandRegistry& registry, const ProjectState& state, const std::string& name,
                           Json params, std::shared_ptr<command::CommandServices> services) {
    try {
        EditSession scratch(state);
        const command::CommandResult r = runCatalog(scratch, registry, std::move(services), name, std::move(params));
        if (!r.ok() || !r.data.is_object())
            return std::nullopt;
        return std::optional<Json>(r.data);
    } catch (...) {
        return std::nullopt;
    }
}

Rgba rgbaOf(const Json& j, Rgba fallback) {
    if (!j.is_array() || j.size() < 4)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()};
}

std::vector<MaskVertex> verticesOf(const Json& j) {
    std::vector<MaskVertex> out;
    if (!j.is_array())
        return out;
    for (const Json& v : j) {
        if (!v.is_array() || v.size() < 2)
            continue;
        MaskVertex vertex;
        vertex.x = v[0].get<double>();
        vertex.y = v[1].get<double>();
        if (v.size() >= 6) {
            vertex.inX = v[2].get<double>();
            vertex.inY = v[3].get<double>();
            vertex.outX = v[4].get<double>();
            vertex.outY = v[5].get<double>();
        }
        out.push_back(vertex);
    }
    return out;
}

template <class E>
E enumIn(const Json& j, const char* key, E fallback, std::optional<E> (*parse)(std::string_view)) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return fallback;
    return parse(it->get<std::string>()).value_or(fallback);
}

/// graphic.resolve's (and template.resolve's) draw list { width, height, items } back into a DrawList for the painter.
DrawList drawListFromJson(const Json& data) {
    DrawList list;
    list.width = data.value("width", 0);
    list.height = data.value("height", 0);
    const auto items = data.find("items");
    if (items == data.end() || !items->is_array())
        return list;
    for (const Json& j : *items) {
        DrawItem item;
        item.layer = j.value("layer", 0u);
        const std::string kind = j.value("kind", std::string("shape"));
        item.kind = kind == "text" ? DrawKind::Text : kind == "image" ? DrawKind::Image : DrawKind::Shape;
        if (const auto m = j.find("transform"); m != j.end() && m->is_array() && m->size() == 6) {
            item.transform.a = (*m)[0].get<double>();
            item.transform.b = (*m)[1].get<double>();
            item.transform.c = (*m)[2].get<double>();
            item.transform.d = (*m)[3].get<double>();
            item.transform.e = (*m)[4].get<double>();
            item.transform.f = (*m)[5].get<double>();
        }
        item.opacity = j.value("opacity", 1.0);
        item.blend = enumIn(j, "blend", BlendMode::Normal, blendModeFromString);
        if (const auto b = j.find("bounds"); b != j.end() && b->is_array() && b->size() == 4)
            item.bounds = Box{(*b)[0].get<double>(), (*b)[1].get<double>(), (*b)[2].get<double>(), (*b)[3].get<double>()};
        if (const auto sh = j.find("shadow"); sh != j.end() && sh->is_object()) {
            item.shadow.enabled = true;
            item.shadow.color = rgbaOf(sh->value("color", Json()), item.shadow.color);
            item.shadow.opacity = sh->value("opacity", 0.0);
            item.shadow.angle = sh->value("angle", 0.0);
            item.shadow.distance = sh->value("distance", 0.0);
            item.shadow.softness = sh->value("softness", 0.0);
        }
        if (const auto masks = j.find("masks"); masks != j.end() && masks->is_array()) {
            for (const Json& m : *masks) {
                DrawMask mask;
                mask.owner = m.value("owner", 0u);
                mask.mode = enumIn(m, "mode", MaskMode::Add, maskModeFromString);
                mask.inverted = m.value("inverted", false);
                mask.feather = m.value("feather", 0.0);
                mask.expansion = m.value("expansion", 0.0);
                mask.opacity = m.value("opacity", 1.0);
                mask.outline = verticesOf(m.value("outline", Json()));
                item.masks.push_back(std::move(mask));
            }
        }
        switch (item.kind) {
        case DrawKind::Shape: {
            item.path = verticesOf(j.value("path", Json()));
            item.closed = j.value("closed", true);
            if (const auto fill = j.find("fill"); fill != j.end() && fill->is_object()) {
                item.fill.kind = enumIn(*fill, "kind", PaintKind::Solid, paintKindFromString);
                item.fill.color = rgbaOf(fill->value("color", Json()), item.fill.color);
                if (const auto stops = fill->find("stops"); stops != fill->end() && stops->is_array()) {
                    for (const Json& stop : *stops)
                        item.fill.stops.emplace_back(stop.value("offset", 0.0), rgbaOf(stop.value("color", Json()), Rgba{0.0, 0.0, 0.0, 1.0}));
                }
                if (const auto a = fill->find("start"); a != fill->end() && a->is_array() && a->size() == 2)
                    item.fill.start = {(*a)[0].get<double>(), (*a)[1].get<double>()};
                if (const auto b = fill->find("end"); b != fill->end() && b->is_array() && b->size() == 2)
                    item.fill.end = {(*b)[0].get<double>(), (*b)[1].get<double>()};
            }
            if (const auto stroke = j.find("stroke"); stroke != j.end() && stroke->is_object()) {
                item.stroke.enabled = true;
                item.stroke.color = rgbaOf(stroke->value("color", Json()), item.stroke.color);
                item.stroke.width = stroke->value("width", 0.0);
                if (const auto dashes = stroke->find("dashes"); dashes != stroke->end() && dashes->is_array()) {
                    for (const Json& d : *dashes)
                        item.stroke.dashes.push_back(d.get<double>());
                }
                item.stroke.join = enumIn(*stroke, "join", LineJoin::Miter, lineJoinFromString);
                item.stroke.cap = enumIn(*stroke, "cap", LineCap::Butt, lineCapFromString);
            }
            if (const auto trim = j.find("trim"); trim != j.end() && trim->is_object()) {
                item.trimStart = trim->value("start", 0.0);
                item.trimEnd = trim->value("end", 1.0);
                item.trimOffset = trim->value("offset", 0.0);
            }
            break;
        }
        case DrawKind::Text: {
            const auto text = j.find("text");
            if (text == j.end() || !text->is_object())
                break;
            DrawText& t = item.text;
            if (const auto runs = text->find("runs"); runs != text->end() && runs->is_array()) {
                for (const Json& r : *runs) {
                    DrawTextRun run;
                    run.byteStart = r.value("byteStart", std::size_t{0});
                    run.text = r.value("text", std::string());
                    run.fontFamily = r.value("fontFamily", std::string());
                    run.fontStyle = r.value("fontStyle", std::string());
                    run.fontWeight = r.value("fontWeight", 400);
                    run.fontSize = r.value("fontSize", 72.0);
                    run.fill = rgbaOf(r.value("fill", Json()), run.fill);
                    run.italic = r.value("italic", false);
                    run.underline = r.value("underline", false);
                    run.tracking = r.value("tracking", 0.0);
                    run.leading = r.value("leading", 0.0);
                    run.baselineShift = r.value("baselineShift", 0.0);
                    t.runs.push_back(std::move(run));
                }
            }
            t.align = enumIn(*text, "align", TextAlign::Left, textAlignFromString);
            t.boxWidth = text->value("boxWidth", 0.0);
            t.boxHeight = text->value("boxHeight", 0.0);
            t.language = text->value("language", std::string());
            if (const auto bg = text->find("background"); bg != text->end() && bg->is_object()) {
                t.background = true;
                t.backgroundColor = rgbaOf(bg->value("color", Json()), t.backgroundColor);
                t.backgroundPadding = bg->value("padding", 0.0);
                t.backgroundRadius = bg->value("radius", 0.0);
            }
            if (const auto clusters = text->find("clusters"); clusters != text->end() && clusters->is_array()) {
                for (const Json& c : *clusters) {
                    DrawCluster cluster;
                    cluster.byteStart = c.value("byteStart", std::size_t{0});
                    cluster.byteEnd = c.value("byteEnd", std::size_t{0});
                    cluster.opacity = c.value("opacity", 1.0);
                    cluster.offsetX = c.value("offsetX", 0.0);
                    cluster.offsetY = c.value("offsetY", 0.0);
                    cluster.scale = c.value("scale", 1.0);
                    cluster.rotation = c.value("rotation", 0.0);
                    t.clusters.push_back(cluster);
                }
            }
            break;
        }
        case DrawKind::Image:
            if (const auto image = j.find("image"); image != j.end() && image->is_object()) {
                item.image.media = MediaId{image->value("media", std::uint64_t{0})};
                item.image.width = image->value("width", 0.0);
                item.image.height = image->value("height", 0.0);
                item.image.fit = enumIn(*image, "fit", ImageFit::Contain, imageFitFromString);
            }
            break;
        }
        list.items.push_back(std::move(item));
    }
    return list;
}

/// A built-in template's draw list at design time `time` (none: its poster time): template.resolve's, else (no
/// template.resolve, or it drew nothing) its graphic from template.list with its id, resolved here. nullopt when the
/// catalog cannot draw it.
std::optional<DrawList> builtinDrawList(const command::CommandRegistry* registry, const ProjectSnapshot& snapshot,
                                        const std::shared_ptr<command::CommandServices>& services, const std::string& id,
                                        std::optional<Ticks> time, const ResolveOptions& options) {
    if (!registry)
        return std::nullopt;
    static const ProjectState kEmpty;
    const ProjectState& state = snapshot ? *snapshot : kEmpty;
    try {
        if (registry->knows("template.resolve")) {
            Json params{{"template", id}};
            if (time)
                params["time"] = time->value;
            if (const auto data = readOn(*registry, state, "template.resolve", std::move(params), services)) {
                DrawList list = drawListFromJson(*data);
                if (!list.items.empty())
                    return list;
            }
        }
        if (registry->knows("template.list")) {
            const auto data = readOn(*registry, state, "template.list", Json{{"library", "all"}, {"id", id}}, services);
            if (!data || !data->contains("builtin") || !(*data)["builtin"].is_array())
                return std::nullopt;
            for (const Json& entry : (*data)["builtin"]) {
                if (entry.value("id", std::string()) != id || !entry.contains("graphic"))
                    continue;
                const auto graphic = io::graphicFromJson(entry["graphic"].dump());
                if (!graphic)
                    return std::nullopt;
                const Ticks at = time ? *time
                                      : Ticks{entry.value("posterTime", static_cast<std::int64_t>(entry.value("duration", std::int64_t{0}) / 2))};
                return resolveGraphic(*graphic, at, options);
            }
        }
    } catch (const Json::exception&) {
    }
    return std::nullopt;
}

/// Paints templates' thumbnails: the graphic resolved with the project's brand kit at its thumbnail time and painted
/// (GraphicPainter), cached (64, least recently made first out). Thread-safe: runs on the provider's pool.
class ThumbnailRenderer {
public:
    explicit ThumbnailRenderer(std::weak_ptr<GraphicsBridge::Shared> shared) : m_shared(std::move(shared)) {}

    /// `id`: <library>/<id>/<version>/<kit revision>[/<milliseconds>].
    QImage render(const QString& id, QSize target) {
        const QStringList parts = id.split(QLatin1Char('/'));
        const auto shared = m_shared.lock();
        if (!shared || parts.size() < 3)
            return {};
        const QString cacheKey = id + QLatin1Char('@') + QString::number(target.width()) + QLatin1Char('x') + QString::number(target.height());
        std::shared_ptr<const GraphicTemplate> tmpl;
        std::optional<BrandKit> kit;
        const command::CommandRegistry* registry = nullptr;
        ProjectSnapshot snapshot;
        std::shared_ptr<command::CommandServices> services;
        {
            std::lock_guard lock(shared->mutex);
            if (const auto it = shared->templates.find((parts[0] + QLatin1Char('/') + parts[1]).toStdString()); it != shared->templates.end())
                tmpl = it->second;
            kit = shared->brandKit;
            registry = shared->registry;
            snapshot = shared->snapshot;
            services = shared->services;
        }
        // A built-in template (C.16) lives in the command catalog, not here.
        const bool builtin = !tmpl && parts[0] == QLatin1String("builtin");
        if (!tmpl && !builtin)
            return {};
        const QString kitKey = kit ? QString::fromStdString(io::brandKitToJson(*kit)) : QString();
        {
            std::lock_guard lock(m_cacheMutex);
            if (const auto it = m_cache.find(cacheKey + kitKey); it != m_cache.end())
                return it->second;
        }
        bool okTime = false;
        const qint64 ms = parts.size() >= 5 ? parts[4].toLongLong(&okTime) : 0;
        ResolveOptions options;
        options.brandKit = kit ? &*kit : nullptr;
        const auto measurer = qtTextMeasurer();
        const auto breaker = qtLineBreaker();
        options.measurer = measurer.get();
        options.lineBreaker = breaker.get();
        DrawList list;
        if (tmpl) {
            list = resolveGraphic(tmpl->graphic, okTime ? Ticks{ms * (kTicksPerSecond / 1000)} : thumbnailTime(*tmpl), options);
        } else {
            auto resolved = builtinDrawList(registry, snapshot, services, parts[1].toStdString(),
                                            okTime ? std::optional<Ticks>(Ticks{ms * (kTicksPerSecond / 1000)}) : std::nullopt, options);
            if (!resolved)
                return {};
            list = std::move(*resolved);
        }
        QImage image = paintGraphic(list, target);
        shared->renders.fetch_add(1);
        shared->lastThread.store(QThread::currentThread());
        std::lock_guard lock(m_cacheMutex);
        m_order.push_back(cacheKey + kitKey);
        m_cache[cacheKey + kitKey] = image;
        while (m_order.size() > 64) {
            m_cache.erase(m_order.front());
            m_order.pop_front();
        }
        return image;
    }

private:
    std::weak_ptr<GraphicsBridge::Shared> m_shared;
    std::mutex m_cacheMutex;
    std::map<QString, QImage> m_cache;
    std::list<QString> m_order;
};

/// One thumbnail: painted on the provider's pool, handed back on the response's thread. The engine keeps a response
/// (cancelled or not) until it has said finished(), so the job's pointer stays good.
class ThumbnailResponse final : public QQuickImageResponse {
public:
    ThumbnailResponse(const std::shared_ptr<ThumbnailRenderer>& renderer, const QString& id, QSize size, QThreadPool* pool) {
        pool->start(QRunnable::create([this, renderer, id, size] {
            QImage image = renderer->render(id, size);
            if (image.isNull()) {
                image = QImage(size, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
            }
            QMetaObject::invokeMethod(
                this,
                [this, image] {
                    m_image = image;
                    emit finished();
                },
                Qt::QueuedConnection);
        }));
    }

    QQuickTextureFactory* textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }

private:
    QImage m_image;
};

/// image://graphicthumb/<library>/<id>/<version>/<kit revision>[/<milliseconds>]: a template's thumbnail, painted on a
/// small thread pool of its own (never the GUI thread), cached.
class GraphicThumbnailProvider final : public QQuickAsyncImageProvider {
public:
    explicit GraphicThumbnailProvider(std::weak_ptr<GraphicsBridge::Shared> shared)
        : m_renderer(std::make_shared<ThumbnailRenderer>(std::move(shared))) {
        m_pool.setMaxThreadCount(2); // low memory: a couple of thumbnails at a time
    }

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override {
        const QSize target = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(320, 180);
        return new ThumbnailResponse(m_renderer, id, target, &m_pool);
    }

private:
    std::shared_ptr<ThumbnailRenderer> m_renderer;
    QThreadPool m_pool; ///< last: waits for its jobs first
};

/// The top text layer of `layers` (top first, a group's children right after the group), 0 for none.
std::uint32_t topTextLayer(const std::vector<GraphicLayer>& layers) {
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        if (it->kind == LayerKind::Text)
            return it->id;
        if (const std::uint32_t inner = topTextLayer(it->children))
            return inner;
    }
    return 0;
}

} // namespace

struct GraphicsBridge::Target {
    SequenceId sequence;
    ClipId clip;
    Clip placed;
    Graphic graphic;
};

GraphicsBridge::GraphicsBridge(QObject* parent) : QObject(parent), m_shared(std::make_shared<Shared>()) {
    const QString env = qEnvironmentVariable("WABCUT_TEMPLATE_LIBRARY");
    const QString folder = !env.isEmpty() ? env
                                          : QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                                .filePath(QStringLiteral("templates"));
    m_userLibrary = std::filesystem::path(folder.toStdU16String());
}

GraphicsBridge::~GraphicsBridge() = default;

GraphicsBridge* GraphicsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new GraphicsBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        if (!qmlEngine->imageProvider(QString::fromLatin1(kProviderId)))
            qmlEngine->addImageProvider(QString::fromLatin1(kProviderId), new GraphicThumbnailProvider(bridge->m_shared));
    }
    return bridge;
}

void GraphicsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, [this] {
            const TimelineChange& change = m_controller->lastChange();
            if (change.projectWide() || change.structural)
                rebuildBrand();
            const ProjectSnapshot snap = m_controller->snapshot();
            if (snap && snap->templates.size() != m_projectTemplatesSeen)
                refreshTemplates();
            else if (snap) {
                // A template saved again under its id keeps the count: compare the versions.
                for (const QVariant& t : m_templates) {
                    const QVariantMap m = t.toMap();
                    if (m.value(QStringLiteral("library")).toString() != QLatin1String("project"))
                        continue;
                    const GraphicTemplate* current = snap->findTemplate(m.value(QStringLiteral("id")).toString().toStdString());
                    if (!current || current->version != m.value(QStringLiteral("version")).toInt()) {
                        refreshTemplates();
                        break;
                    }
                }
            }
            if (change.structural || !m_clip.isValid() || change.touches(m_clip) || change.projectWide())
                rebuild();
        });
        connect(m_controller, &ProjectController::sequenceChanged, this, &GraphicsBridge::rebuild);
        connect(m_controller, &ProjectController::selectionChanged, this, &GraphicsBridge::rebuild);
        connect(m_controller, &ProjectController::selectedClipChanged, this, &GraphicsBridge::rebuild);
        connect(m_controller, &ProjectController::playheadChanged, this, [this] {
            if (m_clip.isValid() && m_selectedLayer != 0)
                rebuildLayer();
        });
    }
    rebuildBrand();
    refreshTemplates();
    rebuild();
}

void GraphicsBridge::setUserLibrary(const std::filesystem::path& folder) {
    m_userLibrary = folder;
    refreshTemplates();
}

void GraphicsBridge::setRegistry(const command::CommandRegistry* registry) {
    m_registry = registry;
    refreshTemplates();
}

const command::CommandRegistry& GraphicsBridge::registry() const {
    return m_registry ? *m_registry : ai::edit::editingCommands();
}

std::shared_ptr<command::GraphicsServices> GraphicsBridge::graphicsServices() const {
    return std::make_shared<command::GraphicsServices>(m_userLibrary, qtTextMeasurer());
}

QString GraphicsBridge::userLibrary() const { return QString::fromStdU16String(m_userLibrary.u16string()); }

QString GraphicsBridge::clip() const { return m_clip.isValid() ? clipKey(m_clip) : QString(); }

std::shared_ptr<const GraphicTemplate> GraphicsBridge::findTemplate(const QString& key) const {
    std::lock_guard lock(m_shared->mutex);
    const auto it = m_shared->templates.find(key.toStdString());
    return it == m_shared->templates.end() ? nullptr : it->second;
}

int GraphicsBridge::thumbnailRenders() const { return m_shared->renders.load(); }

bool GraphicsBridge::lastThumbnailOnGuiThread() const {
    QThread* thread = m_shared->lastThread.load();
    return thread != nullptr && thread == QCoreApplication::instance()->thread();
}

void GraphicsBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QString GraphicsBridge::setError(const QString& error) {
    setMessage(error);
    return error;
}

// ---- Templates ------------------------------------------------------------------------------------------------------

void GraphicsBridge::refreshTemplates() {
    QVariantList list;
    QVariantList errors;
    std::map<std::string, std::shared_ptr<const GraphicTemplate>> store;
    const auto add = [&](const GraphicTemplate& t, const QString& library) {
        const QString id = QString::fromStdString(t.id);
        const QString key = library + QLatin1Char('/') + id;
        auto shared = std::make_shared<const GraphicTemplate>(t);
        store[key.toStdString()] = shared;
        QVariantList fields;
        for (const TemplateField& f : t.fields) {
            fields.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(f.id)},
                                      {QStringLiteral("label"), QString::fromStdString(f.label)},
                                      {QStringLiteral("kind"), nameOf(f.kind)}});
        }
        const Ticks length = t.duration.value > 0 ? t.duration : t.graphic.responsive ? t.graphic.responsive->duration : Ticks{5 * kTicksPerSecond};
        list.append(QVariantMap{{QStringLiteral("key"), key},
                                {QStringLiteral("id"), id},
                                {QStringLiteral("library"), library},
                                {QStringLiteral("name"), QString::fromStdString(t.name)},
                                {QStringLiteral("description"), QString::fromStdString(t.description)},
                                {QStringLiteral("version"), t.version},
                                {QStringLiteral("width"), t.graphic.width},
                                {QStringLiteral("height"), t.graphic.height},
                                {QStringLiteral("seconds"), toSeconds(length)},
                                {QStringLiteral("fields"), fields},
                                {QStringLiteral("thumbnail"), QStringLiteral("image://%1/%2/%3/%4").arg(QString::fromLatin1(kProviderId), key).arg(t.version).arg(m_kitRevision)}});
    };
    const auto addFolder = [&](const std::filesystem::path& folder, const QString& library) {
        for (const io::LibraryTemplate& entry : io::readTemplateLibrary(folder)) {
            if (entry.tmpl)
                add(*entry.tmpl, library);
            else
                errors.append(QVariantMap{{QStringLiteral("file"), QString::fromStdU16String(entry.file.u16string())},
                                          {QStringLiteral("message"), QString::fromStdString(entry.tmpl.error().message)}});
        }
    };
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    // The built-in templates (C.16): template.list's "builtin" entries, first. Their thumbnails show the poster time.
    if (m_controller && registry().knows("template.list")) {
        const command::CommandResult r =
            runCatalog(m_controller->session(), registry(), graphicsServices(), "template.list", Json{{"library", "all"}});
        const auto builtins = r.ok() && r.data.is_object() ? r.data.find("builtin") : r.data.end();
        if (r.ok() && r.data.is_object() && builtins != r.data.end() && builtins->is_array()) {
            for (const Json& e : *builtins) {
                try {
                    const QString id = QString::fromStdString(e.value("id", std::string()));
                    if (id.isEmpty())
                        continue;
                    QVariantList fields;
                    for (const Json& f : e.value("fields", Json::array())) {
                        fields.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(f.value("id", std::string()))},
                                                  {QStringLiteral("label"), QString::fromStdString(f.value("label", std::string()))},
                                                  {QStringLiteral("kind"), QString::fromStdString(f.value("kind", std::string()))}});
                    }
                    const std::int64_t duration = e.value("duration", std::int64_t{0});
                    const Ticks length{duration > 0 ? duration : 5 * kTicksPerSecond};
                    const std::int64_t poster = e.value("posterTime", length.value / 2);
                    const int version = e.value("version", 1);
                    const QString key = QStringLiteral("builtin/") + id;
                    list.append(QVariantMap{{QStringLiteral("key"), key},
                                            {QStringLiteral("id"), id},
                                            {QStringLiteral("library"), QStringLiteral("builtin")},
                                            {QStringLiteral("category"), QString::fromStdString(e.value("category", std::string()))},
                                            {QStringLiteral("name"), QString::fromStdString(e.value("name", std::string()))},
                                            {QStringLiteral("description"), QString::fromStdString(e.value("description", std::string()))},
                                            {QStringLiteral("version"), version},
                                            {QStringLiteral("width"), e.value("width", 1920)},
                                            {QStringLiteral("height"), e.value("height", 1080)},
                                            {QStringLiteral("seconds"), toSeconds(length)},
                                            {QStringLiteral("fields"), fields},
                                            {QStringLiteral("thumbnail"),
                                             QStringLiteral("image://%1/%2/%3/%4/%5")
                                                 .arg(QString::fromLatin1(kProviderId), key)
                                                 .arg(version)
                                                 .arg(m_kitRevision)
                                                 .arg(poster * 1000 / kTicksPerSecond)}});
                } catch (const Json::exception&) {
                    // an entry of another shape: left out
                }
            }
        }
    }
    m_projectTemplatesSeen = snap ? snap->templates.size() : 0;
    if (snap) {
        for (const GraphicTemplate& t : snap->templates)
            add(t, QStringLiteral("project"));
    }
    if (!m_userLibrary.empty())
        addFolder(m_userLibrary, QStringLiteral("user"));
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->templates = std::move(store);
        m_shared->registry = &registry();
        m_shared->snapshot = snap;
        m_shared->services = graphicsServices();
    }
    m_templates = list;
    m_libraryErrors = errors;
    emit templatesChanged();
}

QString GraphicsBridge::runCommandNow(const std::string& name, const std::string& params, QVariantMap* data) {
    if (!m_controller)
        return setError(tr("No project"));
    Json request = Json::parse(params, nullptr, false);
    if (!request.is_object())
        request = Json::object();
    if (!request.contains("sequence"))
        request["sequence"] = m_controller->currentSequenceId().value;
    const command::CommandResult result = runCatalog(m_controller->session(), registry(), graphicsServices(), name, std::move(request));
    const QString error = commandErrorText(result);
    if (!error.isEmpty())
        return setError(error);
    if (data)
        *data = toVariant(result.data).toMap();
    setMessage({});
    return {};
}

QString GraphicsBridge::instantiate(const QString& templateKey, const QString& trackKey, const QVariant& at) {
    const int slash = static_cast<int>(templateKey.indexOf(QLatin1Char('/')));
    if (slash <= 0)
        return setError(tr("Unknown template"));
    // A built-in (C.16) is found after the project's and the user's libraries: a user copy with its id wins.
    Json params{{"template", templateKey.mid(slash + 1).toStdString()}};
    if (templateKey.left(slash) != QLatin1String("builtin"))
        params["library"] = templateKey.left(slash).toStdString();
    if (at.typeId() == QMetaType::QString)
        params["at"] = at.toString().toStdString();
    else
        params["at"] = static_cast<std::int64_t>(std::llround(std::max(0.0, at.toDouble())));
    if (const auto track = parseTrackKey(trackKey)) {
        params["track"] = track->value;
        params["placement"] = "targeted";
    } else {
        params["placement"] = "freeTrackAtOrAbove";
    }
    QVariantMap data;
    const QString error = runCommandNow("template.instantiate", params.dump(), &data);
    if (!error.isEmpty())
        return error;
    const qint64 clip = data.value(QStringLiteral("clip")).toLongLong();
    if (clip > 0 && m_controller) {
        m_controller->selectClip(clipKey(ClipId{static_cast<std::uint64_t>(clip)}));
        selectFirstTextLayer(); // ready to type: in Edit, and double-click / Enter on the Program Monitor
    }
    return {};
}

void GraphicsBridge::selectFirstTextLayer() {
    const auto t = target();
    if (!t)
        return;
    std::uint32_t id = 0;
    if (t->graphic.instance) {
        for (const TemplateField& f : t->graphic.instance->fields) {
            const GraphicLayer* l = f.kind == FieldKind::Text ? t->graphic.findLayer(f.layer) : nullptr;
            if (l && l->kind == LayerKind::Text) {
                id = l->id;
                break;
            }
        }
    }
    if (id == 0)
        id = topTextLayer(t->graphic.layers);
    if (id != 0)
        setSelectedLayer(static_cast<int>(id));
}

QString GraphicsBridge::instantiateAtPlayhead(const QString& templateKey) {
    return instantiate(templateKey, {}, QVariant::fromValue(m_controller ? static_cast<double>(m_controller->playhead().value) : 0.0));
}

QVariantList GraphicsBridge::exposeCandidates() const {
    const auto t = target();
    QVariantList out;
    if (!t)
        return out;
    std::set<QString> used;
    const auto idFor = [&](const GraphicLayer& l, const QString& property) {
        QString base = sluggish(QString::fromStdString(l.name));
        if (base.isEmpty() || !base.at(0).isLetter())
            base = QStringLiteral("layer%1").arg(l.id) + (base.isEmpty() ? QString() : QStringLiteral("-") + base);
        QString id = base + QLatin1Char('.') + property;
        for (int n = 2; used.contains(id); ++n)
            id = base + QLatin1Char('.') + property + QString::number(n);
        used.insert(id);
        return id;
    };
    const auto row = [&](const GraphicLayer& l, const QString& kind, const QString& property, const QString& label) {
        out.append(QVariantMap{{QStringLiteral("id"), idFor(l, property)},
                               {QStringLiteral("label"), label},
                               {QStringLiteral("kind"), kind},
                               {QStringLiteral("layer"), static_cast<int>(l.id)},
                               {QStringLiteral("property"), property},
                               {QStringLiteral("layerName"), QString::fromStdString(l.name)}});
    };
    std::function<void(const std::vector<GraphicLayer>&)> visit = [&](const std::vector<GraphicLayer>& layers) {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            const GraphicLayer& l = *it;
            const QString name = l.name.empty() ? kindName(l.kind) : QString::fromStdString(l.name);
            switch (l.kind) {
            case LayerKind::Text:
                row(l, QStringLiteral("text"), QStringLiteral("text"), name);
                row(l, QStringLiteral("font"), QStringLiteral("font"), tr("%1 font").arg(name));
                row(l, QStringLiteral("color"), QStringLiteral("fill"), tr("%1 colour").arg(name));
                if (l.text.background.enabled)
                    row(l, QStringLiteral("color"), QStringLiteral("background"), tr("%1 background").arg(name));
                break;
            case LayerKind::Shape:
                if (l.shape.fill.kind == PaintKind::Solid)
                    row(l, QStringLiteral("color"), QStringLiteral("fill"), tr("%1 fill").arg(name));
                if (l.shape.stroke.enabled)
                    row(l, QStringLiteral("color"), QStringLiteral("stroke"), tr("%1 stroke").arg(name));
                break;
            case LayerKind::Image:
                row(l, QStringLiteral("image"), QStringLiteral("media"), tr("%1 picture").arg(name));
                break;
            case LayerKind::Group:
                break;
            }
            if (l.shadow.enabled)
                row(l, QStringLiteral("color"), QStringLiteral("shadow"), tr("%1 shadow").arg(name));
            row(l, QStringLiteral("checkbox"), QStringLiteral("visible"), tr("Show %1").arg(name));
            row(l, QStringLiteral("duration"), QStringLiteral("out"), tr("%1 out point").arg(name));
            visit(l.children);
        }
    };
    visit(t->graphic.layers);
    out.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("duration")},
                           {QStringLiteral("label"), tr("Duration")},
                           {QStringLiteral("kind"), QStringLiteral("duration")},
                           {QStringLiteral("layer"), 0},
                           {QStringLiteral("property"), QStringLiteral("clip")},
                           {QStringLiteral("layerName"), QString()}});
    return out;
}

QString GraphicsBridge::saveTemplate(const QString& id, const QString& name, const QString& description, const QString& library,
                                     const QVariantList& fields) {
    const auto t = target();
    if (!t)
        return setError(tr("Select a graphic clip to save as a template"));
    QString templateId = id.trimmed().isEmpty() ? sluggish(name) : id.trimmed();
    if (templateId.isEmpty())
        templateId = QStringLiteral("template");
    if (!isValidTemplateId(templateId.toStdString()))
        return setError(tr("A template id has letters, digits, dots, dashes and underscores"));
    Json list = Json::array();
    for (const QVariant& v : fields) {
        const QVariantMap f = v.toMap();
        Json field{{"id", f.value(QStringLiteral("id")).toString().toStdString()},
                   {"label", f.value(QStringLiteral("label")).toString().toStdString()},
                   {"kind", f.value(QStringLiteral("kind")).toString().toStdString()},
                   {"property", f.value(QStringLiteral("property")).toString().toStdString()}};
        const int layer = f.value(QStringLiteral("layer")).toInt();
        if (layer > 0)
            field["layer"] = layer;
        list.push_back(std::move(field));
    }
    // The clip's length is the template's (Premiere keeps a template's duration): an instance is as long as the original.
    const Json params{{"clip", t->clip.value},
                      {"id", templateId.toStdString()},
                      {"name", name.toStdString()},
                      {"description", description.toStdString()},
                      {"duration", t->placed.duration().value},
                      {"library", library == QLatin1String("user") ? "user" : "project"},
                      {"fields", std::move(list)}};
    const QString error = runCommandNow("template.save", params.dump());
    if (error.isEmpty())
        refreshTemplates();
    return error;
}

QString GraphicsBridge::removeTemplate(const QString& templateKey) {
    if (!m_controller)
        return setError(tr("No project"));
    const int slash = static_cast<int>(templateKey.indexOf(QLatin1Char('/')));
    const QString library = templateKey.left(slash);
    const std::string id = templateKey.mid(slash + 1).toStdString();
    if (library == QLatin1String("builtin"))
        return setError(tr("A built-in template cannot be deleted"));
    if (library == QLatin1String("project")) {
        const bool ok = m_controller->applyEditFn("Delete Template", [&](const ProjectState& s) { return edit::removeProjectTemplate(s, id); });
        if (!ok)
            return setError(m_controller->lastError());
    } else {
        for (const io::LibraryTemplate& entry : io::readTemplateLibrary(m_userLibrary)) {
            if (entry.tmpl && entry.tmpl->id == id) {
                std::error_code ec;
                std::filesystem::remove(entry.file, ec);
                if (ec)
                    return setError(tr("Could not delete the template file"));
            }
        }
    }
    refreshTemplates();
    setMessage({});
    return {};
}

// ---- The selected graphic -------------------------------------------------------------------------------------------

std::optional<GraphicsBridge::Target> GraphicsBridge::target() const {
    if (!m_controller)
        return std::nullopt;
    const SequencePtr seq = m_controller->currentSequence();
    const ClipId primary = m_controller->primaryClip();
    const auto loc = seq && primary.isValid() ? seq->findClip(primary) : std::nullopt;
    if (!loc || !loc->clip().graphic())
        return std::nullopt;
    return Target{seq->id, primary, loc->clip(), *loc->clip().graphic()};
}

Ticks GraphicsBridge::graphicTime() const {
    const auto t = target();
    if (!t)
        return Ticks{};
    return graphicTimeAt(t->placed, t->graphic, m_controller->playhead());
}

void GraphicsBridge::rebuild() {
    const auto t = target();
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const BrandKit* kit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
    const ClipId showing = t ? t->clip : ClipId{};
    if (showing != m_clip && m_selectedLayer != 0) {
        m_selectedLayer = 0; // another graphic: its own layers
        emit selectedLayerChanged();
    }
    m_clip = showing;
    m_graphic.clear();
    m_fields.clear();
    m_layers.clear();
    if (t) {
        const Graphic& g = t->graphic;
        QVariant responsive;
        if (g.responsive) {
            responsive = QVariantMap{{QStringLiteral("duration"), toSeconds(g.responsive->duration)},
                                     {QStringLiteral("intro"), toSeconds(g.responsive->intro)},
                                     {QStringLiteral("outro"), toSeconds(g.responsive->outro)}};
        }
        QVariant instance;
        if (g.instance) {
            instance = QVariantMap{{QStringLiteral("id"), QString::fromStdString(g.instance->templateId)},
                                   {QStringLiteral("version"), g.instance->version}};
        }
        m_graphic = {{QStringLiteral("name"), QString::fromStdString(g.name)},
                     {QStringLiteral("width"), g.width},
                     {QStringLiteral("height"), g.height},
                     {QStringLiteral("responsive"), responsive},
                     {QStringLiteral("template"), instance},
                     {QStringLiteral("seconds"), toSeconds(t->placed.duration())}};
        if (g.instance) {
            for (const TemplateField& f : g.instance->fields) {
                const FieldValue value = edit::currentFieldValue(g, f, t->placed.duration());
                QVariant shown;
                switch (f.kind) {
                case FieldKind::Text:
                    shown = QString::fromStdString(value.text);
                    break;
                case FieldKind::Color:
                    shown = value.color.brand.empty() ? hexOf(resolveColor(value.color, kit)) : QStringLiteral("brand:") + QString::fromStdString(value.color.brand);
                    break;
                case FieldKind::Font:
                    shown = QString::fromStdString(value.text);
                    break;
                case FieldKind::Image:
                    shown = value.media.isValid() ? mediaKey(value.media) : QString();
                    break;
                case FieldKind::Duration:
                    shown = toSeconds(value.duration);
                    break;
                case FieldKind::Checkbox:
                    shown = value.flag;
                    break;
                }
                m_fields.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(f.id)},
                                            {QStringLiteral("label"), QString::fromStdString(f.label)},
                                            {QStringLiteral("kind"), nameOf(f.kind)},
                                            {QStringLiteral("layer"), static_cast<int>(f.layer)},
                                            {QStringLiteral("property"), QString::fromStdString(f.property)},
                                            {QStringLiteral("value"), shown}});
            }
        }
        // The tree, top first, children after their group.
        std::function<void(const std::vector<GraphicLayer>&, std::uint32_t, int)> flatten = [&](const std::vector<GraphicLayer>& layers,
                                                                                                 std::uint32_t parent, int depth) {
            for (std::size_t k = layers.size(); k-- > 0;) {
                const GraphicLayer& l = layers[k];
                QVariantMap row{{QStringLiteral("id"), static_cast<int>(l.id)},
                                {QStringLiteral("parent"), static_cast<int>(parent)},
                                {QStringLiteral("depth"), depth},
                                {QStringLiteral("kind"), kindName(l.kind)},
                                {QStringLiteral("name"), l.name.empty() ? kindName(l.kind) : QString::fromStdString(l.name)},
                                {QStringLiteral("visible"), l.visible},
                                {QStringLiteral("children"), static_cast<int>(l.children.size())},
                                {QStringLiteral("index"), static_cast<int>(k)},
                                {QStringLiteral("siblings"), static_cast<int>(layers.size())},
                                {QStringLiteral("pinned"), l.pin.has_value()}};
                if (l.kind == LayerKind::Text)
                    row.insert(QStringLiteral("text"), QString::fromStdString(l.text.text()));
                m_layers.append(row);
                flatten(l.children, l.id, depth + 1);
            }
        };
        flatten(g.layers, 0, 0);
        if (m_selectedLayer != 0 && !g.findLayer(m_selectedLayer)) {
            m_selectedLayer = 0;
            emit selectedLayerChanged();
        }
    } else if (m_selectedLayer != 0) {
        m_selectedLayer = 0;
        emit selectedLayerChanged();
    }
    emit graphicChanged();
    rebuildLayer();
}

void GraphicsBridge::setSelectedLayer(int id) {
    const auto value = static_cast<std::uint32_t>(std::max(0, id));
    if (value == m_selectedLayer)
        return;
    m_selectedLayer = value;
    emit selectedLayerChanged();
    rebuildLayer();
}

void GraphicsBridge::rebuildLayer() {
    QVariantMap out;
    const auto t = target();
    const GraphicLayer* l = t && m_selectedLayer != 0 ? t->graphic.findLayer(m_selectedLayer) : nullptr;
    if (l) {
        const ProjectSnapshot snap = m_controller->snapshot();
        const BrandKit* kit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
        const Ticks time = graphicTimeAt(t->placed, t->graphic, m_controller->playhead());
        const LayerTransform& xf = l->transform;
        out = {{QStringLiteral("id"), static_cast<int>(l->id)},
               {QStringLiteral("kind"), kindName(l->kind)},
               {QStringLiteral("name"), QString::fromStdString(l->name)},
               {QStringLiteral("visible"), l->visible},
               {QStringLiteral("blend"), nameOf(l->blend)},
               {QStringLiteral("inSeconds"), toSeconds(l->in)},
               {QStringLiteral("outSeconds"), l->out ? toSeconds(*l->out) : -1.0},
               {QStringLiteral("transform"), QVariantMap{{QStringLiteral("x"), paramMap(xf.x, time)},
                                                         {QStringLiteral("y"), paramMap(xf.y, time)},
                                                         {QStringLiteral("anchorX"), paramMap(xf.anchorX, time)},
                                                         {QStringLiteral("anchorY"), paramMap(xf.anchorY, time)},
                                                         {QStringLiteral("scaleX"), paramMap(xf.scaleX, time)},
                                                         {QStringLiteral("scaleY"), paramMap(xf.scaleY, time)},
                                                         {QStringLiteral("rotation"), paramMap(xf.rotation, time)},
                                                         {QStringLiteral("opacity"), paramMap(xf.opacity, time)}}},
               {QStringLiteral("shadow"), QVariantMap{{QStringLiteral("enabled"), l->shadow.enabled},
                                                      {QStringLiteral("color"), colorMap(l->shadow.color, kit)},
                                                      {QStringLiteral("opacity"), l->shadow.opacity},
                                                      {QStringLiteral("angle"), l->shadow.angle},
                                                      {QStringLiteral("distance"), l->shadow.distance},
                                                      {QStringLiteral("softness"), l->shadow.softness}}}};
        const Json json = Json::parse(io::layerToJson(*l), nullptr, false);
        out.insert(QStringLiteral("masks"), json.is_object() && json.contains("masks") ? toVariant(json["masks"]) : QVariantList());
        if (l->pin) {
            QStringList edges;
            if (l->pin->edges & LayerPin::kLeft)
                edges << QStringLiteral("left");
            if (l->pin->edges & LayerPin::kTop)
                edges << QStringLiteral("top");
            if (l->pin->edges & LayerPin::kRight)
                edges << QStringLiteral("right");
            if (l->pin->edges & LayerPin::kBottom)
                edges << QStringLiteral("bottom");
            out.insert(QStringLiteral("pin"), QVariantMap{{QStringLiteral("target"), static_cast<int>(l->pin->target)}, {QStringLiteral("edges"), edges}});
        }
        if (l->kind == LayerKind::Text) {
            const TextContent& text = l->text;
            QVariantList runs;
            for (const TextRun& r : text.runs) {
                runs.append(QVariantMap{{QStringLiteral("text"), QString::fromStdString(r.text)},
                                        {QStringLiteral("fontFamily"), QString::fromStdString(r.style.fontFamily)},
                                        {QStringLiteral("fontStyle"), QString::fromStdString(r.style.fontStyle)},
                                        {QStringLiteral("brandFont"), QString::fromStdString(r.style.brandFont)},
                                        {QStringLiteral("fontWeight"), r.style.fontWeight},
                                        {QStringLiteral("fontSize"), r.style.fontSize},
                                        {QStringLiteral("fill"), colorMap(r.style.fill, kit)},
                                        {QStringLiteral("italic"), r.style.italic},
                                        {QStringLiteral("underline"), r.style.underline},
                                        {QStringLiteral("tracking"), r.style.tracking},
                                        {QStringLiteral("leading"), r.style.leading},
                                        {QStringLiteral("baselineShift"), r.style.baselineShift}});
            }
            QStringList animators;
            for (const TextAnimator& a : text.animators)
                animators << QString::fromStdString(a.name);
            out.insert(QStringLiteral("text"),
                       QVariantMap{{QStringLiteral("text"), QString::fromStdString(text.text())},
                                   {QStringLiteral("align"), nameOf(text.align)},
                                   {QStringLiteral("boxWidth"), text.boxWidth},
                                   {QStringLiteral("boxHeight"), text.boxHeight},
                                   {QStringLiteral("language"), QString::fromStdString(text.language)},
                                   {QStringLiteral("background"), QVariantMap{{QStringLiteral("enabled"), text.background.enabled},
                                                                              {QStringLiteral("color"), colorMap(text.background.color, kit)},
                                                                              {QStringLiteral("padding"), text.background.padding},
                                                                              {QStringLiteral("radius"), text.background.radius}}},
                                   {QStringLiteral("runs"), runs},
                                   {QStringLiteral("animators"), animators}});
        } else if (l->kind == LayerKind::Shape) {
            const ShapeContent& s = l->shape;
            QVariantList stops;
            for (const GradientStop& stop : s.fill.stops)
                stops.append(QVariantMap{{QStringLiteral("offset"), stop.offset}, {QStringLiteral("color"), colorMap(stop.color, kit)}});
            QStringList dashes;
            for (const double d : s.stroke.dashes)
                dashes << QString::number(d);
            out.insert(QStringLiteral("shape"),
                       QVariantMap{{QStringLiteral("type"), nameOf(s.kind)},
                                   {QStringLiteral("width"), paramMap(s.width, time)},
                                   {QStringLiteral("height"), paramMap(s.height, time)},
                                   {QStringLiteral("cornerRadius"), paramMap(s.cornerRadius, time)},
                                   {QStringLiteral("outerRadius"), paramMap(s.outerRadius, time)},
                                   {QStringLiteral("innerRadius"), paramMap(s.innerRadius, time)},
                                   {QStringLiteral("trimStart"), paramMap(s.trimStart, time)},
                                   {QStringLiteral("trimEnd"), paramMap(s.trimEnd, time)},
                                   {QStringLiteral("trimOffset"), paramMap(s.trimOffset, time)},
                                   {QStringLiteral("points"), static_cast<int>(s.points)},
                                   {QStringLiteral("closed"), s.closed},
                                   {QStringLiteral("fill"), QVariantMap{{QStringLiteral("type"), nameOf(s.fill.kind)},
                                                                        {QStringLiteral("color"), colorMap(s.fill.color, kit)},
                                                                        {QStringLiteral("stops"), stops}}},
                                   {QStringLiteral("stroke"), QVariantMap{{QStringLiteral("enabled"), s.stroke.enabled},
                                                                          {QStringLiteral("color"), colorMap(s.stroke.color, kit)},
                                                                          {QStringLiteral("width"), paramMap(s.stroke.width, time)},
                                                                          {QStringLiteral("dashes"), dashes.join(QLatin1Char(' '))},
                                                                          {QStringLiteral("join"), nameOf(s.stroke.join)},
                                                                          {QStringLiteral("cap"), nameOf(s.stroke.cap)}}}});
        } else if (l->kind == LayerKind::Image) {
            const ImageContent& im = l->image;
            out.insert(QStringLiteral("image"), QVariantMap{{QStringLiteral("media"), im.media.isValid() ? mediaKey(im.media) : QString()},
                                                            {QStringLiteral("brandLogo"), QString::fromStdString(im.brandLogo)},
                                                            {QStringLiteral("width"), im.width},
                                                            {QStringLiteral("height"), im.height},
                                                            {QStringLiteral("fit"), nameOf(im.fit)}});
        }
    }
    m_layer = out;
    emit layerChanged();
}

void GraphicsBridge::rebuildBrand() {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    QVariantMap brand;
    std::optional<BrandKit> kit;
    if (snap && snap->brandKit) {
        kit = *snap->brandKit;
        QVariantList colors;
        for (const BrandColor& c : kit->colors)
            colors.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(c.name)}, {QStringLiteral("color"), hexOf(c.rgba)}});
        QVariantList fonts;
        for (const BrandFont& f : kit->fonts)
            fonts.append(QVariantMap{{QStringLiteral("role"), QString::fromStdString(f.role)},
                                     {QStringLiteral("family"), QString::fromStdString(f.family)},
                                     {QStringLiteral("weight"), f.weight}});
        QVariantList logos;
        for (const BrandLogo& l : kit->logos)
            logos.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(l.name)}, {QStringLiteral("media"), mediaKey(l.media)}});
        brand = {{QStringLiteral("name"), QString::fromStdString(kit->name)},
                 {QStringLiteral("colors"), colors},
                 {QStringLiteral("fonts"), fonts},
                 {QStringLiteral("logos"), logos}};
    }
    QVariantList media;
    if (snap) {
        for (const auto& item : snap->media) {
            if (!item || item->isGenerator() || !item->info.hasVideo())
                continue;
            media.append(QVariantMap{{QStringLiteral("id"), mediaKey(item->id)}, {QStringLiteral("name"), QString::fromStdString(item->name)}});
        }
    }
    bool kitChanged = false;
    {
        std::lock_guard lock(m_shared->mutex);
        kitChanged = m_shared->brandKit != kit;
        m_shared->brandKit = std::move(kit);
    }
    if (brand == m_brand && media == m_media && !kitChanged)
        return;
    m_brand = brand;
    m_media = media;
    emit brandChanged();
    if (kitChanged) {
        ++m_kitRevision; // the thumbnails re-theme
        refreshTemplates();
    }
}

// ---- Edits ----------------------------------------------------------------------------------------------------------

QString GraphicsBridge::editGraphic(const char* label, const std::function<std::optional<QString>(Graphic&)>& fn, const std::string& merge) {
    const auto t = target();
    if (!t)
        return setError(tr("Select a graphic clip"));
    const SequenceId sequence = t->sequence;
    const ClipId clip = t->clip;
    std::optional<QString> failure;
    const bool ok = m_controller->applyEditFn(
        label,
        [&](const ProjectState& s) -> edit::EditOutcome {
            const Sequence* seq = s.findSequence(sequence);
            const auto loc = seq ? seq->findClip(clip) : std::nullopt;
            if (!loc || !loc->clip().graphic())
                return std::unexpected(Error(ErrorCode::NotFound, "the graphic clip is gone"));
            Graphic g = *loc->clip().graphic();
            if (auto error = fn(g)) {
                failure = *error;
                return std::unexpected(Error(ErrorCode::InvalidArgument, error->toStdString()));
            }
            return edit::setGraphic(s, sequence, clip, std::move(g));
        },
        merge);
    if (!ok)
        return setError(failure.value_or(m_controller->lastError()));
    setMessage({});
    return {};
}

QString GraphicsBridge::editLayer(int layerId, const char* label,
                                  const std::function<std::optional<QString>(GraphicLayer&, const Graphic&)>& fn, const std::string& merge) {
    return editGraphic(
        label,
        [&](Graphic& g) -> std::optional<QString> {
            GraphicLayer* l = g.findLayer(static_cast<std::uint32_t>(layerId));
            if (!l)
                return tr("The graphic has no such layer");
            return fn(*l, g);
        },
        merge);
}

void GraphicsBridge::endEdit() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

QString GraphicsBridge::setField(const QString& fieldId, const QVariant& value) {
    const auto t = target();
    if (!t || !t->graphic.instance)
        return setError(tr("Select a graphic made from a template"));
    const auto it = std::find_if(t->graphic.instance->fields.begin(), t->graphic.instance->fields.end(),
                                 [&](const TemplateField& f) { return f.id == fieldId.toStdString(); });
    if (it == t->graphic.instance->fields.end())
        return setError(tr("The template has no such field"));
    const ProjectSnapshot snap = m_controller->snapshot();
    const BrandKit* kit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
    FieldValue v = edit::currentFieldValue(t->graphic, *it, t->placed.duration());
    switch (it->kind) {
    case FieldKind::Text:
        v.text = value.toString().toStdString();
        break;
    case FieldKind::Font:
        v.text = value.toString().toStdString();
        break;
    case FieldKind::Color: {
        const auto color = parseColor(value.toString(), v.color, kit);
        if (!color)
            return setError(tr("Not a colour"));
        v.color = *color;
        break;
    }
    case FieldKind::Image: {
        const auto media = detail::parseId<MediaId>(QLatin1Char('m'), value.toString());
        if (!media)
            return setError(tr("Choose a picture"));
        v.media = *media;
        break;
    }
    case FieldKind::Duration:
        v.duration = fromSeconds(value.toDouble());
        break;
    case FieldKind::Checkbox:
        v.flag = value.toBool();
        break;
    }
    const SequenceId sequence = t->sequence;
    const ClipId clip = t->clip;
    const std::string id = it->id;
    const bool ok = m_controller->applyEditFn(
        "Edit Template Fields",
        [&](const ProjectState& s) { return edit::setTemplateFields(s, sequence, clip, {{id, v}}); },
        "graphicField:" + std::to_string(clip.value) + ":" + id);
    if (!ok)
        return setError(m_controller->lastError());
    setMessage({});
    return {};
}

QString GraphicsBridge::addLayer(const QString& kind) {
    const auto t = target();
    const int width = t ? t->graphic.width : (m_controller && m_controller->currentSequence() ? m_controller->currentSequence()->width : 1920);
    const int height = t ? t->graphic.height : (m_controller && m_controller->currentSequence() ? m_controller->currentSequence()->height : 1080);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const bool brandPrimary = snap && snap->brandKit && snap->brandKit->color("Primary");
    Json layer;
    if (kind == QLatin1String("text")) {
        layer = Json{{"kind", "text"},
                     {"name", tr("Title").toStdString()},
                     {"text", Json{{"align", "center"}, {"runs", Json::array({Json{{"text", tr("Title").toStdString()}, {"size", 96}, {"weight", 700}}})}}},
                     {"transform", Json{{"x", width / 2}, {"y", height / 2 - 58}}}};
    } else if (kind == QLatin1String("rectangle") || kind == QLatin1String("shape") || kind == QLatin1String("ellipse")) {
        const bool ellipse = kind == QLatin1String("ellipse");
        Json fill = brandPrimary ? Json{{"color", Json{{"brand", "Primary"}, {"rgba", Json::array({0.12, 0.1, 0.18, 0.85})}}}}
                                 : Json{{"color", Json::array({0.12, 0.1, 0.18, 0.85})}};
        layer = Json{{"kind", "shape"},
                     {"name", (ellipse ? tr("Ellipse") : tr("Rectangle")).toStdString()},
                     {"shape", Json{{"type", ellipse ? "ellipse" : "rectangle"},
                                    {"width", ellipse ? 300 : 600},
                                    {"height", ellipse ? 300 : 160},
                                    {"cornerRadius", ellipse ? 0 : 12},
                                    {"fill", fill}}},
                     {"transform", Json{{"x", width / 2}, {"y", height / 2}}}};
    } else if (kind == QLatin1String("image")) {
        layer = Json{{"kind", "image"},
                     {"name", tr("Image").toStdString()},
                     {"image", Json{{"width", 400}, {"height", 300}}},
                     {"transform", Json{{"x", width / 2}, {"y", height / 2}}}};
    } else if (kind == QLatin1String("group")) {
        layer = Json{{"kind", "group"}, {"name", tr("Group").toStdString()}};
    } else {
        return setError(tr("Unknown layer kind"));
    }
    if (!t) {
        // Premiere's New Layer: a new graphic clip at the playhead, on the first free video track from the targeted one.
        if (!m_controller || !m_controller->currentSequence())
            return setError(tr("Open a sequence first"));
        layer["id"] = 1;
        const Json params{{"at", m_controller->playhead().value},
                          {"placement", "freeTrackAtOrAbove"},
                          {"name", layer["name"]},
                          {"graphic", Json{{"width", width}, {"height", height}, {"layers", Json::array({layer})}}}};
        QVariantMap data;
        const QString error = runCommandNow("graphic.add", params.dump(), &data);
        if (!error.isEmpty())
            return error;
        const qint64 clip = data.value(QStringLiteral("clip")).toLongLong();
        if (clip > 0)
            m_controller->selectClip(clipKey(ClipId{static_cast<std::uint64_t>(clip)}));
        const QVariantList layers = data.value(QStringLiteral("layers")).toList();
        if (!layers.isEmpty())
            setSelectedLayer(layers.front().toMap().value(QStringLiteral("id")).toInt());
        return {};
    }
    auto parsed = io::layerFromJson(layer.dump(), width, height);
    if (!parsed)
        return setError(QString::fromStdString(parsed.error().message));
    // A new layer goes into the selected group, else on top.
    std::uint32_t parent = 0;
    if (const GraphicLayer* selected = m_selectedLayer ? t->graphic.findLayer(m_selectedLayer) : nullptr; selected && selected->kind == LayerKind::Group)
        parent = selected->id;
    std::uint32_t created = 0;
    const SequenceId sequence = t->sequence;
    const ClipId clip = t->clip;
    GraphicLayer newLayer = std::move(*parsed);
    const bool ok = m_controller->applyEditFn("Add Graphic Layer", [&](const ProjectState& s) {
        auto r = edit::addGraphicLayer(s, sequence, clip, newLayer, parent);
        if (r)
            created = r->createdLayer;
        return r;
    });
    if (!ok)
        return setError(m_controller->lastError());
    setMessage({});
    if (created)
        setSelectedLayer(static_cast<int>(created));
    return {};
}

QString GraphicsBridge::removeLayer(int layerId) {
    const auto t = target();
    if (!t)
        return setError(tr("Select a graphic clip"));
    const SequenceId sequence = t->sequence;
    const ClipId clip = t->clip;
    const bool ok = m_controller->applyEditFn("Delete Graphic Layer", [&](const ProjectState& s) {
        return edit::removeGraphicLayer(s, sequence, clip, static_cast<std::uint32_t>(layerId));
    });
    if (!ok)
        return setError(m_controller->lastError());
    setMessage({});
    return {};
}

QString GraphicsBridge::moveLayer(int layerId, const QString& where) {
    return editGraphic("Move Graphic Layer", [&](Graphic& g) -> std::optional<QString> {
        auto [list, index] = containerOf(g.layers, static_cast<std::uint32_t>(layerId));
        if (!list)
            return tr("The graphic has no such layer");
        std::size_t to = index;
        if (where == QLatin1String("forward"))
            to = std::min(index + 1, list->size() - 1);
        else if (where == QLatin1String("backward"))
            to = index == 0 ? 0 : index - 1;
        else if (where == QLatin1String("front"))
            to = list->size() - 1;
        else if (where == QLatin1String("back"))
            to = 0;
        if (to == index)
            return std::nullopt;
        GraphicLayer moving = std::move((*list)[index]);
        list->erase(list->begin() + static_cast<std::ptrdiff_t>(index));
        list->insert(list->begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));
        return std::nullopt;
    });
}

QString GraphicsBridge::renameLayer(int layerId, const QString& name) {
    return editLayer(layerId, "Rename Graphic Layer", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        l.name = name.trimmed().toStdString();
        return std::nullopt;
    });
}

QString GraphicsBridge::setLayerVisible(int layerId, bool visible) {
    return editLayer(layerId, visible ? "Show Graphic Layer" : "Hide Graphic Layer", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        l.visible = visible;
        return std::nullopt;
    });
}

QString GraphicsBridge::updateLayer(int layerId, const QVariantMap& patch, const QString& merge) {
    const Json p = toJson(patch);
    const std::string mergeKey = merge.isEmpty() ? std::string() : "graphicLayer:" + merge.toStdString();
    return editLayer(
        layerId, "Edit Graphic Layer",
        [&](GraphicLayer& l, const Graphic& g) -> std::optional<QString> {
            Json current = Json::parse(io::layerToJson(l), nullptr, false);
            if (!current.is_object() || !p.is_object())
                return tr("Not a layer change");
            current.merge_patch(p);
            current["id"] = l.id;
            auto changed = io::layerFromJson(current.dump(), g.width, g.height);
            if (!changed)
                return QString::fromStdString(changed.error().message);
            l = std::move(*changed);
            return std::nullopt;
        },
        mergeKey);
}

QString GraphicsBridge::setValue(int layerId, const QString& path, double value) {
    const Ticks time = graphicTime();
    const std::string key = path.toStdString();
    return editLayer(
        layerId, "Edit Graphic Layer",
        [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
            ParamValue* p = paramAt(l, key);
            if (!p)
                return tr("Not an animatable property");
            if (const Curve* curve = p->curve()) {
                Curve next = *curve;
                if (const auto k = next.keyAt(time)) {
                    next.keys[*k].value = value;
                } else {
                    Keyframe kf;
                    kf.time = time;
                    kf.value = value;
                    next.keys.insert(next.keys.begin() + static_cast<std::ptrdiff_t>(next.insertionIndex(time)), kf);
                }
                *p = ParamValue(std::move(next));
            } else {
                *p = ParamValue(value);
            }
            return std::nullopt;
        },
        "graphicValue:" + std::to_string(layerId) + ":" + key);
}

QString GraphicsBridge::toggleKey(int layerId, const QString& path) {
    const Ticks time = graphicTime();
    const std::string key = path.toStdString();
    return editLayer(layerId, "Graphic Keyframe", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        ParamValue* p = paramAt(l, key);
        if (!p)
            return tr("Not an animatable property");
        const double current = p->valueAt(time);
        Curve next;
        if (const Curve* curve = p->curve())
            next = *curve;
        if (const auto k = p->curve() ? next.keyAt(time) : std::nullopt) {
            next.keys.erase(next.keys.begin() + static_cast<std::ptrdiff_t>(*k));
            *p = next.keys.empty() ? ParamValue(current) : ParamValue(std::move(next));
            return std::nullopt;
        }
        Keyframe kf;
        kf.time = time;
        kf.value = current;
        next.keys.insert(next.keys.begin() + static_cast<std::ptrdiff_t>(next.insertionIndex(time)), kf);
        *p = ParamValue(std::move(next));
        return std::nullopt;
    });
}

QString GraphicsBridge::setAnimated(int layerId, const QString& path, bool animated) {
    const Ticks time = graphicTime();
    const std::string key = path.toStdString();
    return editLayer(layerId, animated ? "Animate Graphic Property" : "Stop Animating", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        ParamValue* p = paramAt(l, key);
        if (!p)
            return tr("Not an animatable property");
        const double current = p->valueAt(time);
        if (animated == p->isCurve())
            return std::nullopt;
        if (animated) {
            Curve curve;
            Keyframe kf;
            kf.time = time;
            kf.value = current;
            curve.keys.push_back(kf);
            *p = ParamValue(std::move(curve));
        } else {
            *p = ParamValue(current);
        }
        return std::nullopt;
    });
}

QString GraphicsBridge::setText(int layerId, const QString& text) {
    return editLayer(
        layerId, "Edit Graphic Text",
        [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
            if (l.kind != LayerKind::Text)
                return tr("Not a text layer");
            TextRun run;
            if (!l.text.runs.empty())
                run.style = l.text.runs.front().style;
            run.text = text.toStdString();
            l.text.runs = {std::move(run)};
            return std::nullopt;
        },
        "graphicText:" + std::to_string(layerId));
}

QString GraphicsBridge::setRun(int layerId, int run, const QString& key, const QVariant& value) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const BrandKit* kit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
    return editLayer(
        layerId, "Edit Graphic Text",
        [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
            if (l.kind != LayerKind::Text || run < 0 || run >= static_cast<int>(l.text.runs.size()))
                return tr("No such text run");
            TextRun& r = l.text.runs[static_cast<std::size_t>(run)];
            TextStyle& s = r.style;
            if (key == QLatin1String("text"))
                r.text = value.toString().toStdString();
            else if (key == QLatin1String("fontFamily"))
                s.fontFamily = value.toString().toStdString();
            else if (key == QLatin1String("fontStyle"))
                s.fontStyle = value.toString().toStdString();
            else if (key == QLatin1String("brandFont"))
                s.brandFont = value.toString().toStdString();
            else if (key == QLatin1String("fontWeight"))
                s.fontWeight = value.toInt();
            else if (key == QLatin1String("fontSize"))
                s.fontSize = value.toDouble();
            else if (key == QLatin1String("italic"))
                s.italic = value.toBool();
            else if (key == QLatin1String("underline"))
                s.underline = value.toBool();
            else if (key == QLatin1String("tracking"))
                s.tracking = value.toDouble();
            else if (key == QLatin1String("leading"))
                s.leading = value.toDouble();
            else if (key == QLatin1String("baselineShift"))
                s.baselineShift = value.toDouble();
            else if (key == QLatin1String("fill")) {
                const auto color = parseColor(value.toString(), s.fill, kit);
                if (!color)
                    return tr("Not a colour");
                s.fill = *color;
            } else
                return tr("Unknown text property");
            return std::nullopt;
        },
        "graphicRun:" + std::to_string(layerId) + ":" + std::to_string(run) + ":" + key.toStdString());
}

QString GraphicsBridge::setColor(int layerId, const QString& what, const QString& color) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const BrandKit* kit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
    return editLayer(
        layerId, "Edit Graphic Colour",
        [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
            const auto apply = [&](GraphicColor& c) -> bool {
                const auto parsed = parseColor(color, c, kit);
                if (parsed)
                    c = *parsed;
                return parsed.has_value();
            };
            bool ok = true;
            if (what == QLatin1String("fill")) {
                if (l.kind == LayerKind::Text) {
                    for (TextRun& r : l.text.runs)
                        ok = apply(r.style.fill) && ok;
                } else {
                    ok = apply(l.shape.fill.color);
                    if (l.shape.fill.kind == PaintKind::None)
                        l.shape.fill.kind = PaintKind::Solid;
                }
            } else if (what == QLatin1String("stroke")) {
                ok = apply(l.shape.stroke.color);
                l.shape.stroke.enabled = true;
            } else if (what == QLatin1String("background")) {
                ok = apply(l.text.background.color);
            } else if (what == QLatin1String("shadow")) {
                ok = apply(l.shadow.color);
            } else if (what.startsWith(QLatin1String("stop:"))) {
                const int stop = what.mid(5).toInt();
                if (stop < 0 || stop >= static_cast<int>(l.shape.fill.stops.size()))
                    return tr("No such gradient stop");
                ok = apply(l.shape.fill.stops[static_cast<std::size_t>(stop)].color);
            } else {
                return tr("Unknown colour");
            }
            return ok ? std::nullopt : std::optional<QString>(tr("Not a colour"));
        },
        "graphicColor:" + std::to_string(layerId) + ":" + what.toStdString());
}

QString GraphicsBridge::setFillKind(int layerId, const QString& kind) {
    const auto parsed = paintKindFromString(kind.toStdString());
    if (!parsed)
        return setError(tr("Unknown fill"));
    const Ticks time = graphicTime();
    return editLayer(layerId, "Edit Graphic Fill", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        if (l.kind != LayerKind::Shape)
            return tr("Not a shape layer");
        Paint& fill = l.shape.fill;
        fill.kind = *parsed;
        if ((*parsed == PaintKind::Linear || *parsed == PaintKind::Radial) && fill.stops.size() < 2) {
            GraphicColor clear = fill.color;
            clear.rgba[3] = 0.0;
            fill.stops = {GradientStop{0.0, fill.color}, GradientStop{1.0, clear}};
        }
        const double w = std::max(1.0, l.shape.width.valueAt(time));
        const double h = std::max(1.0, l.shape.height.valueAt(time));
        if (*parsed == PaintKind::Linear) {
            fill.start = {-w / 2.0, 0.0};
            fill.end = {w / 2.0, 0.0};
        } else if (*parsed == PaintKind::Radial) {
            fill.start = {0.0, 0.0};
            fill.end = {std::max(w, h) / 2.0, 0.0};
        }
        return std::nullopt;
    });
}

QString GraphicsBridge::setStopOffset(int layerId, int stop, double offset) {
    return editLayer(
        layerId, "Edit Gradient",
        [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
            auto& stops = l.shape.fill.stops;
            if (stop < 0 || stop >= static_cast<int>(stops.size()))
                return tr("No such gradient stop");
            // Stops stay in order: an offset is kept between its neighbours.
            const double low = stop > 0 ? stops[static_cast<std::size_t>(stop - 1)].offset : 0.0;
            const double high = stop + 1 < static_cast<int>(stops.size()) ? stops[static_cast<std::size_t>(stop + 1)].offset : 1.0;
            stops[static_cast<std::size_t>(stop)].offset = std::clamp(offset, low, high);
            return std::nullopt;
        },
        "graphicStop:" + std::to_string(layerId) + ":" + std::to_string(stop));
}

QString GraphicsBridge::addStop(int layerId) {
    return editLayer(layerId, "Edit Gradient", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        auto& stops = l.shape.fill.stops;
        if (stops.size() >= kMaxGradientStops)
            return tr("A gradient has at most %1 stops").arg(kMaxGradientStops);
        // In the widest gap, the colour of its left side.
        std::size_t at = 0;
        double widest = -1.0;
        for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
            if (stops[i + 1].offset - stops[i].offset > widest) {
                widest = stops[i + 1].offset - stops[i].offset;
                at = i;
            }
        }
        GradientStop added = stops.empty() ? GradientStop{} : stops[at];
        added.offset = stops.size() < 2 ? 0.5 : (stops[at].offset + stops[at + 1].offset) / 2.0;
        stops.insert(stops.begin() + static_cast<std::ptrdiff_t>(stops.empty() ? 0 : at + 1), added);
        return std::nullopt;
    });
}

QString GraphicsBridge::removeStop(int layerId, int stop) {
    return editLayer(layerId, "Edit Gradient", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        auto& stops = l.shape.fill.stops;
        if (stops.size() <= 2)
            return tr("A gradient needs two stops");
        if (stop < 0 || stop >= static_cast<int>(stops.size()))
            return tr("No such gradient stop");
        stops.erase(stops.begin() + stop);
        return std::nullopt;
    });
}

QString GraphicsBridge::setStroke(int layerId, const QString& key, const QVariant& value) {
    return editLayer(layerId, "Edit Graphic Stroke", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        Stroke& s = l.shape.stroke;
        if (key == QLatin1String("enabled")) {
            s.enabled = value.toBool();
        } else if (key == QLatin1String("dashes")) {
            std::vector<double> dashes;
            for (const QString& part : value.toString().split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts)) {
                bool ok = false;
                const double d = part.toDouble(&ok);
                if (!ok || d < 0.0)
                    return tr("Dashes are pixel lengths, e.g. 8 4");
                dashes.push_back(d);
            }
            if (dashes.size() > kMaxDashes)
                return tr("At most %1 dash lengths").arg(kMaxDashes);
            s.dashes = std::move(dashes);
        } else if (key == QLatin1String("join")) {
            const auto join = lineJoinFromString(value.toString().toStdString());
            if (!join)
                return tr("Unknown line join");
            s.join = *join;
        } else if (key == QLatin1String("cap")) {
            const auto cap = lineCapFromString(value.toString().toStdString());
            if (!cap)
                return tr("Unknown line cap");
            s.cap = *cap;
        } else {
            return tr("Unknown stroke property");
        }
        return std::nullopt;
    });
}

QString GraphicsBridge::setMask(int layerId, int mask, const QString& key, const QVariant& value) {
    const Json v = toJson(value);
    return editLayer(
        layerId, "Edit Graphic Mask",
        [&](GraphicLayer& l, const Graphic& g) -> std::optional<QString> {
            Json json = Json::parse(io::layerToJson(l), nullptr, false);
            if (!json.is_object() || !json.contains("masks") || mask < 0 || mask >= static_cast<int>(json["masks"].size()))
                return tr("No such mask");
            json["masks"][static_cast<std::size_t>(mask)][key.toStdString()] = v;
            auto changed = io::layerFromJson(json.dump(), g.width, g.height);
            if (!changed)
                return QString::fromStdString(changed.error().message);
            l = std::move(*changed);
            return std::nullopt;
        },
        "graphicMask:" + std::to_string(layerId) + ":" + std::to_string(mask) + ":" + key.toStdString());
}

QString GraphicsBridge::removeMask(int layerId, int mask) {
    return editLayer(layerId, "Delete Graphic Mask", [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
        if (mask < 0 || mask >= static_cast<int>(l.masks.size()))
            return tr("No such mask");
        l.masks.erase(l.masks.begin() + mask);
        return std::nullopt;
    });
}

QString GraphicsBridge::applyPreset(int layerId, const QString& preset, const QString& phase, double seconds, const QString& easing,
                                    const QString& direction) {
    const auto t = target();
    if (!t)
        return setError(tr("Select a graphic clip"));
    edit::PresetOptions o;
    const auto kind = edit::presetKindFromString(preset.toStdString());
    if (!kind)
        return setError(tr("Unknown animation preset"));
    o.kind = *kind;
    o.phase = edit::presetPhaseFromString(phase.toStdString()).value_or(edit::PresetPhase::In);
    o.easing = edit::presetEasingFromString(easing.toStdString()).value_or(edit::PresetEasing::EaseOut);
    o.direction = edit::presetDirectionFromString(direction.toStdString()).value_or(edit::PresetDirection::Left);
    if (seconds > 0.0)
        o.duration = fromSeconds(seconds);
    const SequenceId sequence = t->sequence;
    const ClipId clip = t->clip;
    const bool ok = m_controller->applyEditFn("Apply Animation Preset", [&](const ProjectState& s) {
        return edit::applyGraphicPreset(s, sequence, clip, static_cast<std::uint32_t>(layerId), o);
    });
    if (!ok)
        return setError(m_controller->lastError());
    setMessage({});
    return {};
}

QVariantList GraphicsBridge::pinTargets(int layerId) const {
    QVariantList out;
    auto t = target();
    if (!t)
        return out;
    auto [list, index] = containerOf(t->graphic.layers, static_cast<std::uint32_t>(layerId));
    if (!list)
        return out;
    for (std::size_t i = list->size(); i-- > 0;) {
        if (i == index)
            continue;
        const GraphicLayer& l = (*list)[i];
        out.append(QVariantMap{{QStringLiteral("id"), static_cast<int>(l.id)},
                               {QStringLiteral("name"), l.name.empty() ? kindName(l.kind) : QString::fromStdString(l.name)}});
    }
    return out;
}

QString GraphicsBridge::setPin(int layerId, int pinTo, const QStringList& edges) {
    const auto t = target();
    if (!t)
        return setError(tr("Select a graphic clip"));
    std::uint8_t mask = 0;
    for (const QString& e : edges) {
        mask |= e == QLatin1String("left")     ? LayerPin::kLeft
                : e == QLatin1String("top")    ? LayerPin::kTop
                : e == QLatin1String("right")  ? LayerPin::kRight
                : e == QLatin1String("bottom") ? LayerPin::kBottom
                                               : 0;
    }
    std::array<double, 4> offsets{0.0, 0.0, 0.0, 0.0};
    if (pinTo > 0 && mask != 0) {
        // Where both are now (canvas pixels at the playhead): the pin keeps the layer there.
        const ProjectSnapshot snap = m_controller->snapshot();
        ResolveOptions options;
        options.brandKit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
        const auto measurer = qtTextMeasurer();
        const auto breaker = qtLineBreaker();
        options.measurer = measurer.get();
        options.lineBreaker = breaker.get();
        const DrawList list = resolveGraphic(t->graphic, graphicTime(), options);
        std::set<std::uint32_t> mine;
        std::set<std::uint32_t> theirs;
        if (const GraphicLayer* l = t->graphic.findLayer(static_cast<std::uint32_t>(layerId)))
            collectIds(*l, mine);
        if (const GraphicLayer* l = t->graphic.findLayer(static_cast<std::uint32_t>(pinTo)))
            collectIds(*l, theirs);
        const auto a = canvasBox(list, mine);
        const auto b = canvasBox(list, theirs);
        if (a && b)
            offsets = {a->left() - b->left(), a->top() - b->top(), a->right() - b->right(), a->bottom() - b->bottom()};
    }
    return editLayer(layerId, pinTo > 0 && mask != 0 ? "Pin Graphic Layer" : "Unpin Graphic Layer",
                     [&](GraphicLayer& l, const Graphic&) -> std::optional<QString> {
                         if (pinTo <= 0 || mask == 0) {
                             l.pin.reset();
                         } else {
                             LayerPin pin;
                             pin.target = static_cast<std::uint32_t>(pinTo);
                             pin.edges = mask;
                             pin.offsets = offsets;
                             l.pin = pin;
                         }
                         return std::nullopt;
                     });
}

QString GraphicsBridge::setResponsive(bool enabled, double duration, double intro, double outro) {
    return editGraphic("Responsive Design", [&](Graphic& g) -> std::optional<QString> {
        if (!enabled) {
            g.responsive.reset();
            return std::nullopt;
        }
        if (duration <= 0.0)
            return tr("A responsive graphic needs a length");
        ResponsiveTime r;
        r.duration = fromSeconds(duration);
        r.intro = fromSeconds(std::min(intro, duration));
        r.outro = fromSeconds(std::min(outro, std::max(0.0, duration - intro)));
        g.responsive = r;
        return std::nullopt;
    });
}

bool GraphicsBridge::runCommand(const QString& commandId) {
    if (!commandId.startsWith(QLatin1String("graphics.")))
        return false;
    const QString id = commandId.mid(9);
    if (id == QLatin1String("newTextLayer")) {
        addLayer(QStringLiteral("text"));
    } else if (id == QLatin1String("newRectangleLayer")) {
        addLayer(QStringLiteral("rectangle"));
    } else if (id == QLatin1String("newEllipseLayer")) {
        addLayer(QStringLiteral("ellipse"));
    } else if (id == QLatin1String("bringToFront") || id == QLatin1String("bringForward") || id == QLatin1String("sendBackward") ||
               id == QLatin1String("sendToBack")) {
        if (m_selectedLayer == 0)
            return true;
        moveLayer(static_cast<int>(m_selectedLayer), id == QLatin1String("bringToFront")   ? QStringLiteral("front")
                                                     : id == QLatin1String("bringForward") ? QStringLiteral("forward")
                                                     : id == QLatin1String("sendBackward") ? QStringLiteral("backward")
                                                                                           : QStringLiteral("back"));
    } else if (id == QLatin1String("selectNextLayer") || id == QLatin1String("selectPreviousLayer")) {
        if (m_layers.isEmpty())
            return true;
        int at = -1;
        for (int i = 0; i < m_layers.size(); ++i) {
            if (m_layers[i].toMap().value(QStringLiteral("id")).toUInt() == m_selectedLayer)
                at = i;
        }
        const int step = id == QLatin1String("selectNextLayer") ? 1 : -1;
        const int next = at < 0 ? 0 : (at + step + static_cast<int>(m_layers.size())) % static_cast<int>(m_layers.size());
        setSelectedLayer(m_layers[next].toMap().value(QStringLiteral("id")).toInt());
    } else {
        return false;
    }
    return true;
}

} // namespace wabcut::app
