#include "GraphicToolsBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Graphics.h>

#include <QColor>
#include <QQmlEngine>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wabcut::app {

using Json = nlohmann::json;

namespace {

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }
std::int64_t ticksOf(double s) { return static_cast<std::int64_t>(std::llround(std::max(0.0, s) * static_cast<double>(kTicksPerSecond))); }

} // namespace

GraphicToolsBridge::GraphicToolsBridge(QObject* parent) : QObject(parent) {}

GraphicToolsBridge* GraphicToolsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new GraphicToolsBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void GraphicToolsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller)
        connect(m_controller, &ProjectController::revisionChanged, this, &GraphicToolsBridge::changed);
    emit changed();
}

QStringList GraphicToolsBridge::textStyles() const {
    QStringList out;
    if (const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr) {
        for (const ProjectTextStyle& s : snap->textStyles)
            out << QString::fromStdString(s.name);
    }
    return out;
}

QString GraphicToolsBridge::run(const std::string& name, Json params) {
    if (!m_controller) {
        m_message = tr("No project");
        emit changed();
        return m_message;
    }
    params["sequence"] = m_controller->currentSequenceId().value;
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), command::EnginePolicy{});
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    const command::CommandResult result = engine.execute(request);
    m_message = result.ok() ? QString() : QString::fromStdString(result.error->message);
    emit changed();
    return m_message;
}

QString GraphicToolsBridge::align(const QString& clip, const QVariantList& layers, const QString& how, bool toSelection) {
    const auto id = parseClipKey(clip);
    if (!id)
        return tr("Select a graphic clip");
    Json ids = Json::array();
    for (const QVariant& v : layers)
        ids.push_back(v.toLongLong());
    return run("graphic.align", Json{{"clip", id->value},
                                     {"layers", ids},
                                     {"align", how.toStdString()},
                                     {"to", toSelection ? "selection" : "frame"}});
}

QVariantList GraphicToolsBridge::topLayers(const QString& clip) const {
    QVariantList out;
    const auto id = parseClipKey(clip);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (const Graphic* g = loc ? loc->clip().graphic() : nullptr) {
        for (const GraphicLayer& l : g->layers) {
            if (l.visible)
                out.append(static_cast<int>(l.id));
        }
    }
    return out;
}

QVariantMap GraphicToolsBridge::roll(const QString& clip) const {
    const auto id = parseClipKey(clip);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    const Graphic* g = loc ? loc->clip().graphic() : nullptr;
    const GraphicRoll r = g && g->roll ? *g->roll : GraphicRoll{};
    return QVariantMap{{QStringLiteral("enabled"), g && g->roll.has_value()},
                       {QStringLiteral("kind"), QString::fromLatin1(toString(r.kind).data())},
                       {QStringLiteral("startOffScreen"), r.startOffScreen},
                       {QStringLiteral("endOffScreen"), r.endOffScreen},
                       {QStringLiteral("preroll"), seconds(r.preroll)},
                       {QStringLiteral("postroll"), seconds(r.postroll)},
                       {QStringLiteral("easeIn"), seconds(r.easeIn)},
                       {QStringLiteral("easeOut"), seconds(r.easeOut)}};
}

QString GraphicToolsBridge::setRoll(const QString& clip, const QVariantMap& roll) {
    const auto id = parseClipKey(clip);
    if (!id)
        return tr("Select a graphic clip");
    if (!roll.value(QStringLiteral("enabled")).toBool())
        return run("graphic.setRoll", Json{{"clip", id->value}, {"roll", nullptr}});
    return run("graphic.setRoll", Json{{"clip", id->value},
                                       {"roll", Json{{"kind", roll.value(QStringLiteral("kind"), QStringLiteral("roll")).toString().toStdString()},
                                                     {"startOffScreen", roll.value(QStringLiteral("startOffScreen"), true).toBool()},
                                                     {"endOffScreen", roll.value(QStringLiteral("endOffScreen"), true).toBool()},
                                                     {"preroll", ticksOf(roll.value(QStringLiteral("preroll")).toDouble())},
                                                     {"postroll", ticksOf(roll.value(QStringLiteral("postroll")).toDouble())},
                                                     {"easeIn", ticksOf(roll.value(QStringLiteral("easeIn")).toDouble())},
                                                     {"easeOut", ticksOf(roll.value(QStringLiteral("easeOut")).toDouble())}}}});
}

QString GraphicToolsBridge::layerTextStyle(const QString& clip, int layer) const {
    const auto id = parseClipKey(clip);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    const Graphic* g = loc ? loc->clip().graphic() : nullptr;
    const GraphicLayer* l = g ? g->findLayer(static_cast<std::uint32_t>(layer)) : nullptr;
    return l ? QString::fromStdString(l->textStyle) : QString();
}

QString GraphicToolsBridge::createTextStyle(const QString& clip, int layer, const QString& name) {
    const auto id = parseClipKey(clip);
    if (!id)
        return tr("Select a graphic clip");
    return run("textStyle.create", Json{{"clip", id->value}, {"layer", layer}, {"name", name.trimmed().toStdString()}});
}

QString GraphicToolsBridge::applyTextStyle(const QString& clip, int layer, const QString& name) {
    const auto id = parseClipKey(clip);
    if (!id)
        return tr("Select a graphic clip");
    return run("textStyle.apply", Json{{"clip", id->value}, {"layers", Json::array({layer})}, {"name", name.toStdString()}});
}

QString GraphicToolsBridge::pushTextStyle(const QString& clip, int layer) {
    const auto id = parseClipKey(clip);
    const QString style = layerTextStyle(clip, layer);
    if (!id || style.isEmpty())
        return tr("The layer follows no text style");
    return run("textStyle.update", Json{{"name", style.toStdString()}, {"clip", id->value}, {"layer", layer}});
}

QString GraphicToolsBridge::renameTextStyle(const QString& from, const QString& to) {
    return run("textStyle.rename", Json{{"name", from.toStdString()}, {"to", to.trimmed().toStdString()}});
}

QString GraphicToolsBridge::removeTextStyle(const QString& name) {
    return run("textStyle.remove", Json{{"name", name.toStdString()}});
}


// ---- Text Styles as project items ---------------------------------------------------------------------------------------

namespace {

/// Text style names compare case-insensitively (ASCII), as the model's do.
bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

/// Text layers of `layers` (groups included) that follow `name`, and every text layer's id.
void textLayers(const std::vector<GraphicLayer>& layers, std::string_view name, int& uses, std::vector<std::uint32_t>* ids,
                std::size_t depth = 0) {
    if (depth > kMaxGraphicDepth)
        return;
    for (const GraphicLayer& l : layers) {
        if (l.kind == LayerKind::Text) {
            if (!name.empty() && sameName(l.textStyle, name))
                ++uses;
            if (ids)
                ids->push_back(l.id);
        } else if (l.kind == LayerKind::Group) {
            textLayers(l.children, name, uses, ids, depth + 1);
        }
    }
}

} // namespace

QVariantList GraphicToolsBridge::textStyleItems() const {
    QVariantList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    for (const ProjectTextStyle& s : snap->textStyles) {
        int uses = 0;
        for (const auto& seq : snap->sequences) {
            for (const auto* tracks : {&seq->videoTracks}) {
                for (const auto& track : *tracks) {
                    for (const Clip& clip : track->clips) {
                        if (const Graphic* g = clip.graphic())
                            textLayers(g->layers, s.name, uses, nullptr);
                    }
                }
            }
        }
        const auto& rgba = s.style.fill.rgba;
        const QColor color = QColor::fromRgbF(static_cast<float>(std::clamp(rgba[0], 0.0, 1.0)),
                                              static_cast<float>(std::clamp(rgba[1], 0.0, 1.0)),
                                              static_cast<float>(std::clamp(rgba[2], 0.0, 1.0)));
        out.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(s.name)},
                                  {QStringLiteral("fontFamily"), QString::fromStdString(s.style.fontFamily)},
                                  {QStringLiteral("fontSize"), s.style.fontSize},
                                  {QStringLiteral("fontWeight"), s.style.fontWeight},
                                  {QStringLiteral("italic"), s.style.italic},
                                  {QStringLiteral("color"), color.name()},
                                  {QStringLiteral("uses"), uses}});
    }
    return out;
}

QString GraphicToolsBridge::duplicateTextStyle(const QString& name) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return {};
    const ProjectTextStyle* source = nullptr;
    for (const ProjectTextStyle& s : snap->textStyles) {
        if (sameName(s.name, name.toStdString()))
            source = &s;
    }
    if (!source)
        return {};
    const auto taken = [&](const std::string& candidate) {
        return std::any_of(snap->textStyles.begin(), snap->textStyles.end(),
                           [&](const ProjectTextStyle& s) { return sameName(s.name, candidate); });
    };
    std::string copyName = source->name + " copy";
    for (int n = 2; taken(copyName) && n < 1000; ++n)
        copyName = source->name + " copy " + std::to_string(n);
    ProjectTextStyle copy = *source;
    copy.name = copyName;
    const bool ok = m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::GraphicToolsBridge", "Duplicate Text Style"),
                                              [&](const ProjectState& state) { return edit::saveTextStyle(state, copy); });
    m_message = ok ? QString() : m_controller->lastError();
    emit changed();
    return ok ? QString::fromStdString(copyName) : QString();
}

QString GraphicToolsBridge::applyTextStyleToSelection(const QString& name) {
    if (!m_controller)
        return tr("No project");
    const ProjectSnapshot snap = m_controller->snapshot();
    const SequenceId seqId = m_controller->currentSequenceId();
    const Sequence* seq = snap ? snap->findSequence(seqId) : nullptr;
    if (!seq)
        return tr("No sequence");
    std::vector<std::pair<ClipId, std::vector<std::uint32_t>>> targets;
    for (const QString& key : m_controller->selectedClips()) {
        const auto id = parseClipKey(key);
        const auto found = id ? seq->findClip(*id) : std::nullopt;
        const Graphic* g = found ? found->clip().graphic() : nullptr;
        if (!g)
            continue;
        int uses = 0;
        std::vector<std::uint32_t> layers;
        textLayers(g->layers, {}, uses, &layers);
        if (!layers.empty())
            targets.emplace_back(*id, std::move(layers));
    }
    if (targets.empty()) {
        m_message = tr("Select a graphic with text on the timeline first");
        emit changed();
        return m_message;
    }
    const std::string style = name.toStdString();
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::GraphicToolsBridge", "Apply Text Style"), [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState current = state;
            for (const auto& [clip, layers] : targets) {
                auto r = edit::applyTextStyle(current, seqId, clip, layers, style);
                if (!r)
                    return std::unexpected(r.error());
                current = std::move(r->state);
            }
            edit::EditResult result;
            result.state = std::move(current);
            result.changes = diff(state, result.state);
            return result;
        });
    m_message = ok ? QString() : m_controller->lastError();
    emit changed();
    return m_message;
}

} // namespace wabcut::app
