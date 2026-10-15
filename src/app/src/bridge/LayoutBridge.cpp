#include "LayoutBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "ShortsBridge.h" // BridgeJob

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/EditSession.h>

#include <QColor>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <string>

namespace wabcut::app {

namespace {

using Json = command::Json;

QVariant toVariant(const Json& value) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(Json{{"v", value}}.dump())).toVariant().toMap().value(
        QStringLiteral("v"));
}

double clampTo(double v, double lo, double hi) {
    if (!std::isfinite(v))
        return lo;
    return std::clamp(v, lo, hi);
}

/// "#RRGGBB" (or any colour QML hands over) -> [r, g, b] in 0..1.
Json rgbOf(const QVariant& value) {
    const QColor c(value.toString());
    if (!c.isValid())
        return Json::array({1.0, 1.0, 1.0});
    return Json::array({c.redF(), c.greenF(), c.blueF()});
}

QString hexOf(const Json& rgb) {
    if (!rgb.is_array() || rgb.size() < 3)
        return QStringLiteral("#FFFFFF");
    QColor c;
    c.setRgbF(static_cast<float>(rgb[0].get<double>()), static_cast<float>(rgb[1].get<double>()),
              static_cast<float>(rgb[2].get<double>()));
    return c.name(QColor::HexRgb).toUpper();
}

bool isCorner(const QString& corner) {
    return corner == QLatin1String("topLeft") || corner == QLatin1String("topRight") ||
           corner == QLatin1String("bottomLeft") || corner == QLatin1String("bottomRight");
}

/// The picture of a clip (width / height), from its media's first video stream (or its nested sequence).
std::optional<double> pictureAspect(const ProjectState& state, const Clip& clip) {
    if (clip.graphic())
        return std::nullopt; // a callout, not footage
    if (clip.isNested()) {
        if (const Sequence* nested = state.findSequence(clip.sequence); nested && nested->height > 0)
            return static_cast<double>(nested->width) / nested->height;
        return std::nullopt;
    }
    const MediaItem* media = state.findMedia(clip.media);
    if (!media || media->isGenerator() || media->info.video.empty())
        return std::nullopt;
    const VideoStreamInfo& v = media->info.video.front();
    if (v.width <= 0 || v.height <= 0)
        return std::nullopt;
    const double sar = v.sampleAspect.num > 0 && v.sampleAspect.den > 0
                           ? static_cast<double>(v.sampleAspect.num) / v.sampleAspect.den
                           : 1.0;
    return static_cast<double>(v.width) * sar / v.height;
}

/// The rectangle of a picture of `pictureAspect` fitted into a frame of `frameAspect` (contain, centred), in fractions.
struct Fit {
    double x = 0, y = 0, width = 1, height = 1;
};
Fit containFit(double pictureAspect, double frameAspect) {
    Fit f;
    if (pictureAspect <= 0 || frameAspect <= 0)
        return f;
    if (pictureAspect > frameAspect) {
        f.height = frameAspect / pictureAspect;
        f.y = (1.0 - f.height) / 2.0;
    } else {
        f.width = pictureAspect / frameAspect;
        f.x = (1.0 - f.width) / 2.0;
    }
    return f;
}

QVariantMap box(const QString& role, double x, double y, double width, double height) {
    return {{QStringLiteral("role"), role},     {QStringLiteral("x"), x},
            {QStringLiteral("y"), y},           {QStringLiteral("width"), width},
            {QStringLiteral("height"), height}, {QStringLiteral("circle"), false},
            {QStringLiteral("radius"), 0.0},    {QStringLiteral("border"), false},
            {QStringLiteral("shadow"), false},  {QStringLiteral("blur"), false}};
}

/// The C.15 defaults (layout.list's when the catalog has it).
QVariantMap builtinDefaults() {
    return {{QStringLiteral("corner"), QStringLiteral("bottomRight")},
            {QStringLiteral("size"), 0.28},
            {QStringLiteral("margin"), 0.03},
            {QStringLiteral("circle"), false},
            {QStringLiteral("cornerRadius"), 0.12},
            {QStringLiteral("borderWidth"), 4},
            {QStringLiteral("borderColor"), QStringLiteral("#FFFFFF")},
            {QStringLiteral("shadow"), true},
            {QStringLiteral("split"), 0.6},
            {QStringLiteral("facecamOnTop"), false},
            {QStringLiteral("blur"), 40},
            {QStringLiteral("zoom"), 1.0}};
}

command::CommandResult run(EditSession& session, const command::CommandRegistry& registry, const std::string& name,
                           Json params, bool dryRun) {
    command::CommandEngine engine(session, registry, command::EnginePolicy{});
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    request.dryRun = dryRun;
    return engine.execute(request);
}

QString errorText(const command::CommandResult& result) {
    return result.ok() ? QString() : QString::fromStdString(result.error->message);
}

} // namespace

LayoutBridge::LayoutBridge(QObject* parent) : QObject(parent), m_job(std::make_unique<BridgeJob>()) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, &LayoutBridge::runPreview);
    m_previewResult = {{QStringLiteral("busy"), false}, {QStringLiteral("ok"), false}};
}

LayoutBridge::~LayoutBridge() = default;

LayoutBridge* LayoutBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new LayoutBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void LayoutBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::selectionChanged, this, &LayoutBridge::refreshTargets);
        connect(controller, &ProjectController::revisionChanged, this, &LayoutBridge::refreshTargets);
        connect(controller, &ProjectController::sequenceChanged, this, &LayoutBridge::refreshTargets);
        // The playhead moves often: follow it only while the picker is open.
        connect(controller, &ProjectController::playheadChanged, this, [this] {
            if (m_previewShown)
                refreshTargets();
        });
    }
    refreshTargets();
}

void LayoutBridge::setRegistry(const command::CommandRegistry* registry) {
    m_registry = registry;
    emit catalogChanged();
}

const command::CommandRegistry& LayoutBridge::registry() const {
    return m_registry ? *m_registry : ai::edit::editingCommands();
}

bool LayoutBridge::available() const {
    return registry().knows("layout.apply");
}

bool LayoutBridge::sequencePresetsAvailable() const {
    return registry().knows("sequence.presets") && registry().knows("sequence.create");
}

QVariantList LayoutBridge::layouts() const {
    const auto entry = [](const QString& preset, const QString& name, bool needsFacecam, bool vertical,
                          const QString& description) {
        return QVariantMap{{QStringLiteral("preset"), preset},       {QStringLiteral("name"), name},
                           {QStringLiteral("needsFacecam"), needsFacecam}, {QStringLiteral("vertical"), vertical},
                           {QStringLiteral("description"), description}};
    };
    QVariantList builtin{
        entry(QStringLiteral("pip"), tr("Facecam corner"), true, false, tr("Your webcam in a corner over the gameplay.")),
        entry(QStringLiteral("verticalSplit"), tr("Vertical split"), true, true,
              tr("Gameplay and facecam stacked for a 9:16 short.")),
        entry(QStringLiteral("verticalBlurFill"), tr("Vertical blur fill"), false, true,
              tr("Gameplay across the middle over a blurred copy of itself, for a 9:16 short.")),
    };
    if (!m_controller || !registry().knows("layout.list"))
        return builtin;
    const command::CommandResult r = run(m_controller->session(), registry(), "layout.list", Json::object(), false);
    if (!r.ok() || !r.data.contains("layouts") || !r.data["layouts"].is_array())
        return builtin;
    // The catalog's list, with the app's names (and its descriptions when the catalog has none).
    QVariantList out;
    for (const Json& l : r.data["layouts"]) {
        const QString preset = QString::fromStdString(l.value("preset", std::string()));
        auto known = std::find_if(builtin.begin(), builtin.end(), [&](const QVariant& b) {
            return b.toMap().value(QStringLiteral("preset")).toString() == preset;
        });
        QVariantMap row = known != builtin.end() ? known->toMap() : entry(preset, preset, false, false, QString());
        row[QStringLiteral("needsFacecam")] = l.value("needsFacecam", row.value(QStringLiteral("needsFacecam")).toBool());
        out.append(row);
    }
    return out.isEmpty() ? builtin : out;
}

QVariantMap LayoutBridge::defaults() const {
    QVariantMap out = builtinDefaults();
    if (!m_controller || !registry().knows("layout.list"))
        return out;
    const command::CommandResult r = run(m_controller->session(), registry(), "layout.list", Json::object(), false);
    if (!r.ok() || !r.data.contains("defaults") || !r.data["defaults"].is_object())
        return out;
    for (auto it = r.data["defaults"].begin(); it != r.data["defaults"].end(); ++it) {
        const QString key = QString::fromStdString(it.key());
        out[key] = key == QLatin1String("borderColor") ? QVariant(hexOf(it.value())) : toVariant(it.value());
    }
    return out;
}

QString LayoutBridge::gameplay() const {
    return m_gameplay ? clipKey(m_gameplay->clip) : QString();
}

QString LayoutBridge::facecam() const {
    return m_facecam ? clipKey(m_facecam->clip) : QString();
}

void LayoutBridge::setFacecam(const QString& clip) {
    const auto id = parseClipKey(clip);
    if (!id) {
        if (m_facecam) {
            m_facecam.reset();
            emit targetsChanged();
        }
        return;
    }
    if (m_facecam && m_facecam->clip == *id)
        return;
    // Only a candidate (a video clip above the gameplay that overlaps it).
    refreshTargets();
    const auto it = std::find_if(m_candidates.begin(), m_candidates.end(), [&](const QVariant& c) {
        return c.toMap().value(QStringLiteral("key")).toString() == clip;
    });
    if (it == m_candidates.end())
        return;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!snap || !seq)
        return;
    const auto loc = seq->findClip(*id);
    const auto pos = loc ? seq->trackPosition(loc->track->id) : std::nullopt;
    if (!loc || !pos)
        return;
    m_facecam = Target{*id, loc->track->id, pos->index, pictureAspect(*snap, loc->clip()).value_or(16.0 / 9.0)};
    emit targetsChanged();
}

void LayoutBridge::refreshTargets() {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const std::optional<ClipId> chosenFacecam = m_facecam ? std::optional<ClipId>(m_facecam->clip) : std::nullopt;
    m_gameplay.reset();
    m_facecam.reset();
    m_candidates.clear();
    m_gameplayName.clear();
    if (snap && seq) {
        const auto targetOf = [&](ClipId id) -> std::optional<Target> {
            const auto loc = seq->findClip(id);
            const auto pos = loc ? seq->trackPosition(loc->track->id) : std::nullopt;
            if (!loc || !pos || pos->kind != TrackKind::Video)
                return std::nullopt;
            const auto aspect = pictureAspect(*snap, loc->clip());
            if (!aspect)
                return std::nullopt;
            return Target{id, loc->track->id, pos->index, *aspect};
        };
        // The gameplay: the selected video clip (the primary first), else the lowest one under the playhead.
        std::vector<ClipId> selected{m_controller->primaryClip()};
        for (const ClipId id : m_controller->selection())
            selected.push_back(id);
        for (const ClipId id : selected) {
            if (!id.isValid())
                continue;
            if ((m_gameplay = targetOf(id)))
                break;
        }
        const Ticks playhead = m_controller->playhead();
        if (!m_gameplay) {
            for (const TrackPtr& track : seq->videoTracks) {
                if (const auto index = track->clipIndexAt(playhead)) {
                    if ((m_gameplay = targetOf(track->clips[*index].id)))
                        break;
                }
            }
        }
        if (m_gameplay) {
            const auto gameplayLoc = seq->findClip(m_gameplay->clip);
            const Clip& g = gameplayLoc->clip();
            m_gameplayName = QString::fromStdString(g.name());
            // The facecam: picture clips on video tracks above that overlap the gameplay; the one under the playhead
            // first, then the longest overlap.
            struct Candidate {
                Target target;
                QString name;
                bool atPlayhead = false;
                Ticks overlap;
            };
            std::vector<Candidate> found;
            for (std::size_t t = m_gameplay->trackIndex + 1; t < seq->videoTracks.size(); ++t) {
                const Track& track = *seq->videoTracks[t];
                for (const Clip& c : track.clips) {
                    const Ticks from = std::max(c.start(), g.start());
                    const Ticks to = std::min(c.end(), g.end());
                    if (!(from < to))
                        continue;
                    const auto aspect = pictureAspect(*snap, c);
                    if (!aspect)
                        continue;
                    found.push_back(Candidate{Target{c.id, track.id, t, *aspect}, QString::fromStdString(c.name()),
                                              c.start() <= playhead && playhead < c.end(), to - from});
                }
            }
            std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
                if (a.atPlayhead != b.atPlayhead)
                    return a.atPlayhead;
                return b.overlap < a.overlap;
            });
            for (const Candidate& c : found) {
                m_candidates.append(QVariantMap{{QStringLiteral("key"), clipKey(c.target.clip)},
                                                {QStringLiteral("name"), c.name},
                                                {QStringLiteral("track"), QStringLiteral("V%1").arg(c.target.trackIndex + 1)}});
            }
            // Keep the user's choice while it is still a candidate, else the best one.
            const auto kept = std::find_if(found.begin(), found.end(), [&](const Candidate& c) {
                return chosenFacecam && c.target.clip == *chosenFacecam;
            });
            if (kept != found.end())
                m_facecam = kept->target;
            else if (!found.empty())
                m_facecam = found.front().target;
        }
    }
    emit targetsChanged();
}

double LayoutBridge::frameAspect() const {
    if (!m_controller || m_controller->frameHeight() <= 0)
        return 16.0 / 9.0;
    return static_cast<double>(m_controller->frameWidth()) / m_controller->frameHeight();
}

bool LayoutBridge::sequenceIsVertical() const {
    return frameAspect() < 1.0;
}

void LayoutBridge::setFacecamRegion(const QVariantMap& region) {
    QVariantMap next;
    if (!region.isEmpty()) {
        const double x = clampTo(region.value(QStringLiteral("x")).toDouble(), 0, 1);
        const double y = clampTo(region.value(QStringLiteral("y")).toDouble(), 0, 1);
        const double w = clampTo(region.value(QStringLiteral("width")).toDouble(), 0, 1 - x);
        const double h = clampTo(region.value(QStringLiteral("height")).toDouble(), 0, 1 - y);
        if (w >= 0.02 && h >= 0.02)
            next = {{QStringLiteral("x"), x}, {QStringLiteral("y"), y}, {QStringLiteral("width"), w}, {QStringLiteral("height"), h}};
    }
    if (next == m_region)
        return;
    m_region = next;
    emit targetsChanged();
}

QVariantMap LayoutBridge::setRegionFromFrame(double x0, double y0, double x1, double y1) {
    // The two corners in the sequence frame, normalised.
    const double fx0 = clampTo(std::min(x0, x1), 0, 1);
    const double fx1 = clampTo(std::max(x0, x1), 0, 1);
    const double fy0 = clampTo(std::min(y0, y1), 0, 1);
    const double fy1 = clampTo(std::max(y0, y1), 0, 1);
    // Into the gameplay's frame: it is fitted into the sequence frame, centred (its own Motion is not followed).
    const Fit fit = containFit(m_gameplay ? m_gameplay->aspect : frameAspect(), frameAspect());
    const auto gx = [&](double fx) { return clampTo((fx - fit.x) / fit.width, 0, 1); };
    const auto gy = [&](double fy) { return clampTo((fy - fit.y) / fit.height, 0, 1); };
    setFacecamRegion({{QStringLiteral("x"), gx(fx0)},
                      {QStringLiteral("y"), gy(fy0)},
                      {QStringLiteral("width"), gx(fx1) - gx(fx0)},
                      {QStringLiteral("height"), gy(fy1) - gy(fy0)}});
    if (m_region.isEmpty())
        setMessage(tr("Drag a larger box around the webcam"));
    return m_region;
}

void LayoutBridge::clearRegion() {
    setFacecamRegion({});
}

QVariantMap LayoutBridge::facecamRegionInFrame() const {
    if (m_region.isEmpty())
        return {};
    const Fit fit = containFit(m_gameplay ? m_gameplay->aspect : frameAspect(), frameAspect());
    return {{QStringLiteral("x"), fit.x + m_region.value(QStringLiteral("x")).toDouble() * fit.width},
            {QStringLiteral("y"), fit.y + m_region.value(QStringLiteral("y")).toDouble() * fit.height},
            {QStringLiteral("width"), m_region.value(QStringLiteral("width")).toDouble() * fit.width},
            {QStringLiteral("height"), m_region.value(QStringLiteral("height")).toDouble() * fit.height}};
}

void LayoutBridge::setDrawingRegion(bool drawing) {
    if (m_drawing == drawing)
        return;
    m_drawing = drawing;
    emit drawingChanged();
}

void LayoutBridge::setPreviewShown(bool shown) {
    if (m_previewShown == shown)
        return;
    m_previewShown = shown;
    if (!shown)
        setDrawingRegion(false);
    emit previewChanged();
}

namespace {

/// The facecam source an options map asks for: "clip", "region" or "auto" (the clip when there is one).
QString sourceOf(const QVariantMap& options) {
    const QString source = options.value(QStringLiteral("facecamSource"), QStringLiteral("auto")).toString();
    return source == QLatin1String("clip") || source == QLatin1String("region") || source == QLatin1String("none")
               ? source
               : QStringLiteral("auto");
}

} // namespace

QVariantMap LayoutBridge::params(const QVariantMap& options) const {
    const QVariantMap d = defaults();
    const auto num = [&](const char* key) {
        const QString k = QString::fromLatin1(key);
        return options.contains(k) ? options.value(k).toDouble() : d.value(k).toDouble();
    };
    const auto flag = [&](const char* key) {
        const QString k = QString::fromLatin1(key);
        return options.contains(k) ? options.value(k).toBool() : d.value(k).toBool();
    };
    QString preset = options.value(QStringLiteral("preset"), QStringLiteral("pip")).toString();
    if (preset != QLatin1String("pip") && preset != QLatin1String("verticalSplit") && preset != QLatin1String("verticalBlurFill"))
        preset = QStringLiteral("pip");

    Json p = Json::object();
    p["preset"] = preset.toStdString();
    if (m_controller)
        p["sequence"] = m_controller->currentSequenceId().value;
    if (m_gameplay)
        p["gameplay"] = m_gameplay->clip.value;

    // The facecam: the clip, or the region of the gameplay's frame.
    const QString source = sourceOf(options);
    bool hasFacecam = false;
    if ((source == QLatin1String("clip") || source == QLatin1String("auto")) && m_facecam) {
        p["facecam"] = m_facecam->clip.value;
        hasFacecam = true;
    } else if ((source == QLatin1String("region") || source == QLatin1String("auto")) && !m_region.isEmpty()) {
        p["facecamRegion"] = Json{{"x", m_region.value(QStringLiteral("x")).toDouble()},
                                  {"y", m_region.value(QStringLiteral("y")).toDouble()},
                                  {"width", m_region.value(QStringLiteral("width")).toDouble()},
                                  {"height", m_region.value(QStringLiteral("height")).toDouble()}};
        hasFacecam = true;
    }

    // The facecam's look: picture-in-picture, and the blur fill's facecam when it has one.
    if (preset == QLatin1String("pip") || (preset == QLatin1String("verticalBlurFill") && hasFacecam)) {
        QString corner = options.value(QStringLiteral("corner"), d.value(QStringLiteral("corner"))).toString();
        if (!isCorner(corner))
            corner = QStringLiteral("bottomRight");
        p["corner"] = corner.toStdString();
        p["size"] = clampTo(num("size"), 0.05, 1.0);
        p["margin"] = clampTo(num("margin"), 0.0, 0.4);
        p["circle"] = flag("circle");
        p["cornerRadius"] = clampTo(num("cornerRadius"), 0.0, 0.5);
        p["borderWidth"] = clampTo(num("borderWidth"), 0.0, 100.0);
        p["borderColor"] = rgbOf(options.value(QStringLiteral("borderColor"), d.value(QStringLiteral("borderColor"))));
        p["shadow"] = flag("shadow");
    }
    if (preset == QLatin1String("verticalSplit")) {
        p["split"] = clampTo(num("split"), 0.2, 0.8);
        p["facecamOnTop"] = flag("facecamOnTop");
    }
    if (preset == QLatin1String("verticalBlurFill")) {
        p["blur"] = clampTo(num("blur"), 0.0, 500.0);
        p["zoom"] = clampTo(num("zoom"), 1.0, 4.0);
    }
    return toVariant(p).toMap();
}

double LayoutBridge::facecamAspect() const {
    if (m_facecam)
        return m_facecam->aspect;
    if (!m_region.isEmpty() && m_region.value(QStringLiteral("height")).toDouble() > 0) {
        const double ga = m_gameplay ? m_gameplay->aspect : frameAspect();
        return m_region.value(QStringLiteral("width")).toDouble() / m_region.value(QStringLiteral("height")).toDouble() * ga;
    }
    return 16.0 / 9.0;
}

QVariantList LayoutBridge::schematic(const QVariantMap& options, double aspect) const {
    if (!(aspect > 0))
        aspect = frameAspect();
    const QVariantMap p = params(options);
    const QString preset = p.value(QStringLiteral("preset")).toString();
    const double gameplayAspect = m_gameplay ? m_gameplay->aspect : 16.0 / 9.0;
    const bool hasFacecam = p.contains(QStringLiteral("facecam")) || p.contains(QStringLiteral("facecamRegion")) ||
                            preset == QLatin1String("pip") || preset == QLatin1String("verticalSplit");
    QVariantList out;

    // The facecam in a corner (picture-in-picture, and the blur fill's facecam).
    const auto cornerBox = [&]() {
        const bool circle = p.value(QStringLiteral("circle")).toBool();
        const double fa = circle ? 1.0 : facecamAspect();
        const double w = p.value(QStringLiteral("size")).toDouble();
        const double h = w * aspect / fa;
        const double margin = p.value(QStringLiteral("margin")).toDouble();
        const double mx = margin / aspect;
        const double my = margin;
        const QString corner = p.value(QStringLiteral("corner")).toString();
        const double x = corner.endsWith(QLatin1String("Left")) ? mx : 1.0 - mx - w;
        const double y = corner.startsWith(QLatin1String("top")) ? my : 1.0 - my - h;
        QVariantMap b = box(QStringLiteral("facecam"), x, y, w, h);
        b[QStringLiteral("circle")] = circle;
        b[QStringLiteral("radius")] = circle ? 0.5 : p.value(QStringLiteral("cornerRadius")).toDouble();
        b[QStringLiteral("border")] = p.value(QStringLiteral("borderWidth")).toDouble() > 0;
        b[QStringLiteral("borderColor")] = hexOf(Json::array({p.value(QStringLiteral("borderColor")).toList().value(0).toDouble(),
                                                              p.value(QStringLiteral("borderColor")).toList().value(1).toDouble(),
                                                              p.value(QStringLiteral("borderColor")).toList().value(2).toDouble()}));
        b[QStringLiteral("shadow")] = p.value(QStringLiteral("shadow")).toBool();
        return b;
    };

    if (preset == QLatin1String("verticalSplit")) {
        const double split = p.value(QStringLiteral("split")).toDouble();
        const bool onTop = p.value(QStringLiteral("facecamOnTop")).toBool();
        out.append(box(QStringLiteral("gameplay"), 0, onTop ? 1.0 - split : 0.0, 1, split));
        out.append(box(QStringLiteral("facecam"), 0, onTop ? 0.0 : split, 1, 1.0 - split));
    } else if (preset == QLatin1String("verticalBlurFill")) {
        QVariantMap background = box(QStringLiteral("background"), 0, 0, 1, 1);
        background[QStringLiteral("blur")] = true;
        out.append(background);
        const double zoom = p.value(QStringLiteral("zoom")).toDouble();
        const double w = zoom;
        const double h = zoom * aspect / gameplayAspect;
        out.append(box(QStringLiteral("gameplay"), (1.0 - w) / 2.0, (1.0 - h) / 2.0, w, h));
        if (hasFacecam && p.contains(QStringLiteral("corner")))
            out.append(cornerBox());
    } else {
        const Fit fit = containFit(gameplayAspect, aspect);
        out.append(box(QStringLiteral("gameplay"), fit.x, fit.y, fit.width, fit.height));
        out.append(cornerBox());
    }
    return out;
}

void LayoutBridge::preview(const QVariantMap& options) {
    m_previewOptions = options;
    m_previewShown = true;
    m_pendingPreview = options;
    emit previewChanged();
    if (m_synchronous)
        runPreview();
    else
        m_debounce.start();
}

void LayoutBridge::runPreview() {
    if (!m_pendingPreview)
        return;
    if (m_job->running())
        return; // runs again when the current one ends
    const QVariantMap options = *m_pendingPreview;
    m_pendingPreview.reset();
    const unsigned generation = ++m_previewGeneration;
    if (!available() || !m_controller) {
        m_previewResult = {{QStringLiteral("busy"), false},
                           {QStringLiteral("ok"), false},
                           {QStringLiteral("unavailable"), true},
                           {QStringLiteral("error"), tr("Layouts arrive with the next engine update")}};
        emit previewChanged();
        return;
    }
    if (!m_gameplay) {
        m_previewResult = {{QStringLiteral("busy"), false},
                           {QStringLiteral("ok"), false},
                           {QStringLiteral("error"), tr("Select the gameplay clip")}};
        emit previewChanged();
        return;
    }
    const Json params = Json::parse(QJsonDocument::fromVariant(this->params(options)).toJson(QJsonDocument::Compact).toStdString());
    const ProjectSnapshot base = m_controller->snapshot();
    const command::CommandRegistry* catalog = &registry();
    m_previewResult[QStringLiteral("busy")] = true;
    emit previewChanged();
    QPointer<LayoutBridge> self(this);
    // A dry run on a copy of the project, off the GUI thread.
    m_job->start(this, m_synchronous, [self, base, catalog, params, generation](const std::atomic<bool>&) -> BridgeJob::Finish {
        EditSession scratch(*base);
        command::CommandResult result = run(scratch, *catalog, "layout.apply", params, true);
        return [self, generation, result = std::move(result)] {
            if (!self)
                return;
            if (generation == self->m_previewGeneration) {
                self->m_previewResult = {{QStringLiteral("busy"), false},
                                         {QStringLiteral("ok"), result.ok()},
                                         {QStringLiteral("error"), errorText(result)},
                                         {QStringLiteral("data"), toVariant(result.data)}};
                emit self->previewChanged();
            }
            if (self->m_pendingPreview)
                QMetaObject::invokeMethod(self, &LayoutBridge::runPreview, Qt::QueuedConnection);
        };
    }, [self, generation](const QString& message) -> BridgeJob::Finish {
        // The dry run threw: the picker says so instead of staying busy.
        return [self, generation, message] {
            if (!self)
                return;
            if (generation == self->m_previewGeneration) {
                self->m_previewResult = {{QStringLiteral("busy"), false},
                                         {QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), message}};
                emit self->previewChanged();
            }
            if (self->m_pendingPreview)
                QMetaObject::invokeMethod(self, &LayoutBridge::runPreview, Qt::QueuedConnection);
        };
    });
}

QString LayoutBridge::apply(const QVariantMap& options) {
    if (!m_controller)
        return tr("No project is open");
    if (!available())
        return tr("Layouts arrive with the next engine update");
    refreshTargets();
    if (!m_gameplay)
        return tr("Select the gameplay clip");
    const Json params = Json::parse(QJsonDocument::fromVariant(this->params(options)).toJson(QJsonDocument::Compact).toStdString());
    const command::CommandResult result = run(m_controller->session(), registry(), "layout.apply", params, false);
    if (!result.ok()) {
        const QString error = errorText(result);
        setMessage(error);
        return error;
    }
    setPreviewShown(false);
    setMessage(tr("Layout applied"));
    emit applied();
    return {};
}

QVariantList LayoutBridge::sequencePresets() const {
    if (!m_controller || !registry().knows("sequence.presets"))
        return {};
    const command::CommandResult r = run(m_controller->session(), registry(), "sequence.presets", Json::object(), false);
    if (!r.ok() || !r.data.contains("presets"))
        return {};
    return toVariant(r.data["presets"]).toList();
}

QString LayoutBridge::createShortsSequence() {
    if (!m_controller)
        return tr("No project is open");
    if (!sequencePresetsAvailable())
        return tr("Shorts sequences arrive with the next engine update");
    const QString name = tr("%1 (Short)").arg(m_controller->sequenceName());
    const command::CommandResult r =
        run(m_controller->session(), registry(), "sequence.create", Json{{"preset", "shorts"}, {"name", name.toStdString()}}, false);
    if (!r.ok()) {
        const QString error = errorText(r);
        setMessage(error);
        return error;
    }
    // Show it.
    const std::uint64_t created = r.data.value("sequence", std::uint64_t{0});
    const ProjectSnapshot snap = m_controller->snapshot();
    for (std::size_t i = 0; snap && i < snap->sequences.size(); ++i) {
        if (snap->sequences[i]->id.value == created) {
            m_controller->setCurrentSequenceIndex(static_cast<int>(i));
            break;
        }
    }
    setMessage(tr("Shorts sequence created: put the gameplay on it, then pick a vertical layout"));
    return {};
}

void LayoutBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

} // namespace wabcut::app
