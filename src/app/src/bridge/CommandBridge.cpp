#include "CommandBridge.h"

#include "CommandPalette.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/Parameters.h>

#include <QQmlEngine>
#include <QSet>

#include <algorithm>
#include <map>
#include <set>

namespace wabcut::app {

namespace {

using command::Json;

const std::set<std::string>& timeKeys() {
    static const std::set<std::string> keys{"time", "at", "delta", "duration", "sourceIn", "sourceOut"};
    return keys;
}

bool hasType(const Json& schema, const char* type) {
    if (!schema.contains("type"))
        return false;
    const Json& t = schema["type"];
    if (t.is_string())
        return t.get<std::string>() == type;
    if (t.is_array()) {
        for (const Json& item : t) {
            if (item.is_string() && item.get<std::string>() == type)
                return true;
        }
    }
    return false;
}

/// A time value: the known time keys, or any parameter the catalog describes as a timecode (its time and delta
/// parameters: integer ticks or a timecode string).
bool isTimeParam(const std::string& key, const Json& schema) {
    if (timeKeys().contains(key))
        return true;
    const std::string description = schema.value("description", std::string());
    return hasType(schema, "integer") && hasType(schema, "string") && description.find("imecode") != std::string::npos;
}

/// Parameters that name a model object by id (an effect instance, a transition). The palette has no picker for them
/// yet and never asks for a raw id (principle 9).
bool isIdParam(const std::string& key, const Json& schema) {
    return (key == "effect" || key == "transition") && !schema.contains("enum");
}

QString labelFor(const std::string& key) {
    static const std::map<std::string, const char*> kLabels{
        {"time", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Time")},
        {"at", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "At")},
        {"delta", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Offset")},
        {"duration", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Duration")},
        {"sourceIn", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Source In")},
        {"sourceOut", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Source Out")},
        {"name", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Name")},
        {"comment", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Comment")},
        {"colorIndex", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Color")},
        {"track", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Track")},
        {"kind", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Kind")},
        {"edge", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Edge")},
        {"mode", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Mode")},
        {"value", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Value")},
        {"param", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Parameter")},
        {"enabled", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Enabled")},
        {"linked", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Include linked clips")},
        {"media", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Media")},
        {"videoTracks", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Video tracks to move")},
        {"audioTracks", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Audio tracks to move")},
        {"effect", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Effect")},
        {"index", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Index")},
        {"interpolation", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Interpolation")},
        {"animated", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Animated")},
        {"alignment", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Alignment")},
        {"fit", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Fit")},
        {"speed", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Speed (%)")},
        {"reverse", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Reverse")},
        {"maintainPitch", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Keep pitch")},
        {"ripple", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Ripple")},
        {"videoDuration", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Video duration")},
        {"audioDuration", QT_TRANSLATE_NOOP("wabcut::app::CommandBridge", "Audio duration")},
    };
    const auto it = kLabels.find(key);
    return it != kLabels.end() ? CommandBridge::tr(it->second) : QString::fromStdString(key);
}

/// One prompt of a command, or nullopt for a parameter the palette fills in itself (sequence, clip, clips) or
/// cannot ask for (`unsupported` is set when such a parameter is required).
struct Prompt {
    QVariantMap descriptor;
    bool automatic = false;
    bool unsupported = false;
};

Prompt promptFor(const std::string& key, const Json& schema, bool required, const QStringList& trackLabels) {
    Prompt p;
    if (key == "sequence" || key == "clip" || key == "clips") {
        p.automatic = true;
        return p;
    }
    if (isIdParam(key, schema)) {
        p.unsupported = required;
        p.automatic = !required;
        return p;
    }
    QVariantMap d{{QStringLiteral("key"), QString::fromStdString(key)},
                  {QStringLiteral("label"), labelFor(key)},
                  {QStringLiteral("required"), required},
                  {QStringLiteral("description"),
                   QString::fromStdString(schema.value("description", std::string()))}};
    QString kind;
    if (schema.contains("enum") && schema["enum"].is_array()) {
        kind = QStringLiteral("choice");
        QStringList options;
        for (const Json& v : schema["enum"]) {
            if (v.is_string())
                options << QString::fromStdString(v.get<std::string>());
        }
        d.insert(QStringLiteral("options"), options);
    } else if (isTimeParam(key, schema)) {
        kind = QStringLiteral("timecode");
    } else if (key == "colorIndex") {
        kind = QStringLiteral("color");
    } else if (key == "track" && (hasType(schema, "string") || hasType(schema, "integer"))) {
        kind = QStringLiteral("track");
        d.insert(QStringLiteral("options"), trackLabels);
    } else if (key == "param" && hasType(schema, "string")) {
        // A clip parameter by its stable key (effect parameters need an effect instance, which is not asked for).
        kind = QStringLiteral("choice");
        QStringList options;
        for (const ParameterInfo& info : allParameters())
            options << QString::fromLatin1(info.key.data(), static_cast<qsizetype>(info.key.size()));
        d.insert(QStringLiteral("options"), options);
    } else if (hasType(schema, "boolean")) {
        kind = QStringLiteral("boolean");
    } else if (hasType(schema, "string")) {
        kind = QStringLiteral("text");
    } else if (hasType(schema, "integer")) {
        kind = QStringLiteral("integer");
    } else if (hasType(schema, "number")) {
        kind = QStringLiteral("number");
    }
    if (kind.isEmpty()) {
        p.unsupported = required;
        p.automatic = !required; // optional and too complex: left out
        return p;
    }
    if (schema.contains("minimum"))
        d.insert(QStringLiteral("minimum"), schema["minimum"].get<double>());
    if (schema.contains("maximum"))
        d.insert(QStringLiteral("maximum"), schema["maximum"].get<double>());
    d.insert(QStringLiteral("kind"), kind);
    p.descriptor = d;
    return p;
}

QStringList trackLabelsOf(const Sequence* seq) {
    QStringList labels;
    if (!seq)
        return labels;
    for (std::size_t i = seq->videoTracks.size(); i-- > 0;)
        labels << QStringLiteral("V%1").arg(i + 1);
    for (std::size_t i = 0; i < seq->audioTracks.size(); ++i)
        labels << QStringLiteral("A%1").arg(i + 1);
    return labels;
}

/// The newest version of each mutating catalog command the palette shows.
std::vector<const command::CommandSpec*> paletteSpecs() {
    std::map<std::string, const command::CommandSpec*> newest;
    for (const command::CommandSpec& spec : command::builtinCommands().all()) {
        if (!spec.mutates() || CommandBridge::covered(QString::fromStdString(spec.name)))
            continue;
        auto& slot = newest[spec.name];
        if (!slot || slot->version < spec.version)
            slot = &spec;
    }
    std::vector<const command::CommandSpec*> out;
    for (const auto& [name, spec] : newest)
        out.push_back(spec);
    return out;
}

bool needsSelection(const command::CommandSpec& spec) {
    const Json& required = spec.params.value("required", Json::array());
    for (const Json& r : required) {
        if (r.is_string() && (r.get<std::string>() == "clip" || r.get<std::string>() == "clips"))
            return true;
    }
    return false;
}

QString summaryOf(const ChangeSet& changes) {
    QStringList parts;
    std::size_t added = 0, removed = 0, modified = 0, tracks = 0;
    bool markers = false, marks = false;
    for (const SequenceChange& s : changes.sequences) {
        added += s.addedClips.size();
        removed += s.removedClips.size();
        modified += s.modifiedClips.size();
        markers = markers || s.markersChanged;
        marks = marks || s.marksChanged;
        for (const TrackChange& t : s.tracks)
            tracks += t.added ? 1 : 0;
    }
    if (modified)
        parts << CommandBridge::tr("changes %n clip(s)", nullptr, static_cast<int>(modified));
    if (added)
        parts << CommandBridge::tr("adds %n clip(s)", nullptr, static_cast<int>(added));
    if (removed)
        parts << CommandBridge::tr("removes %n clip(s)", nullptr, static_cast<int>(removed));
    if (tracks)
        parts << CommandBridge::tr("adds %n track(s)", nullptr, static_cast<int>(tracks));
    if (markers)
        parts << CommandBridge::tr("updates markers");
    if (marks)
        parts << CommandBridge::tr("updates In/Out");
    if (parts.isEmpty())
        return CommandBridge::tr("No change");
    QString text = parts.join(QStringLiteral(" · "));
    text[0] = text[0].toUpper();
    return text;
}

QString messageFor(const command::CommandError& error) {
    switch (error.code) {
    case command::ErrorCode::InvalidParams:
        return CommandBridge::tr("Check the values");
    case command::ErrorCode::NotFound:
        return CommandBridge::tr("Nothing matches these values");
    case command::ErrorCode::EditRejected:
        return CommandBridge::tr("This edit is not possible here");
    case command::ErrorCode::LockedTrack:
        return CommandBridge::tr("A locked track would change");
    case command::ErrorCode::PermissionDenied:
        return CommandBridge::tr("Not allowed");
    default:
        return CommandBridge::tr("The command failed");
    }
}

} // namespace

CommandBridge::CommandBridge(QObject* parent) : QObject(parent) {}

CommandBridge* CommandBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new CommandBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void CommandBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::selectionChanged, this, &CommandBridge::commandsChanged);
        connect(m_controller, &ProjectController::sequenceChanged, this, &CommandBridge::commandsChanged);
    }
    emit commandsChanged();
}

bool CommandBridge::covered(const QString& name) {
    if (ProjectController::handlesCommand(name) || KeymapCommandProvider::viewHandles(name))
        return true;
    // The Source Monitor and the track headers run these with the loaded source and its patching (M5h); the palette
    // lists Insert, Overwrite and Match Frame as keymap commands.
    static const QSet<QString> kSourceMonitor{
        QStringLiteral("edit.insertFromSource"), QStringLiteral("edit.overwriteFromSource"),
        QStringLiteral("source.setMarks"),       QStringLiteral("source.clearMarks"),
        QStringLiteral("sequence.setTargets"),
    };
    if (kSourceMonitor.contains(name))
        return true;
    const auto runs = [](const QString& id) {
        return ProjectController::handlesCommand(id) || KeymapCommandProvider::viewHandles(id);
    };
    if (const command::CommandSpec* spec = command::builtinCommands().find(name.toStdString())) {
        if (runs(QString::fromStdString(spec->name)))
            return true;
        for (const std::string& alias : spec->aliases) {
            if (runs(QString::fromStdString(alias)))
                return true;
        }
    }
    return false;
}

bool CommandBridge::offers(const QString& name) {
    const command::CommandSpec* spec = command::builtinCommands().find(name.toStdString());
    return spec && spec->mutates() && !covered(name);
}

QVariantList CommandBridge::commands() const {
    QVariantList list;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const QStringList tracks = trackLabelsOf(seq.get());
    const bool hasSelection = m_controller && !m_controller->selection().empty();
    for (const command::CommandSpec* spec : paletteSpecs()) {
        QVariantList params;
        bool unsupported = false;
        const Json& props = spec->params.value("properties", Json::object());
        const Json& required = spec->params.value("required", Json::array());
        for (auto it = props.begin(); it != props.end(); ++it) {
            const bool isRequired = std::find(required.begin(), required.end(), Json(it.key())) != required.end();
            const Prompt prompt = promptFor(it.key(), it.value(), isRequired, tracks);
            unsupported = unsupported || prompt.unsupported;
            if (!prompt.automatic && !prompt.unsupported)
                params << prompt.descriptor;
        }
        QString reason;
        if (unsupported)
            reason = tr("Needs values the palette cannot ask for yet");
        else if (needsSelection(*spec) && !hasSelection)
            reason = tr("Select a clip first");
        list << QVariantMap{
            {QStringLiteral("name"), QString::fromStdString(spec->name)},
            {QStringLiteral("title"), QString::fromStdString(spec->title)},
            {QStringLiteral("description"), QString::fromStdString(spec->description)},
            {QStringLiteral("group"), spec->capability == command::Capability::EditProject ? tr("Project edit")
                                                                                            : tr("Timeline edit")},
            {QStringLiteral("enabled"), reason.isEmpty()},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("params"), params},
        };
    }
    return list;
}

QVariantMap CommandBridge::defaults(const QString& name) const {
    QVariantMap values;
    const command::CommandSpec* spec = command::builtinCommands().find(name.toStdString());
    if (!spec || !m_controller)
        return values;
    const QVariantMap clip = m_controller->selectedClip();
    const Json& props = spec->params.value("properties", Json::object());
    const Json& required = spec->params.value("required", Json::array());
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QString key = QString::fromStdString(it.key());
        const Json& schema = it.value();
        const bool isRequired = std::find(required.begin(), required.end(), Json(it.key())) != required.end();
        if (schema.contains("default")) {
            const Json& d = schema["default"];
            if (d.is_boolean())
                values.insert(key, d.get<bool>());
            else if (d.is_number())
                values.insert(key, d.get<double>());
            else if (d.is_string())
                values.insert(key, QString::fromStdString(d.get<std::string>()));
        }
        if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty() && !values.contains(key))
            values.insert(key, QString::fromStdString(schema["enum"].front().get<std::string>()));
        if (key == QLatin1String("time") || key == QLatin1String("at"))
            values.insert(key, m_controller->playheadText());
        else if (isTimeParam(it.key(), schema) && isRequired && key != QLatin1String("sourceIn")
                 && key != QLatin1String("sourceOut"))
            values.insert(key, m_controller->formatTimecode({})); // optional lengths keep the command's default
        else if (key == QLatin1String("colorIndex"))
            values.insert(key, clip.value(QStringLiteral("colorIndex"), 0).toInt());
        else if (key == QLatin1String("name") && name.startsWith(QLatin1String("clip.")))
            values.insert(key, clip.value(QStringLiteral("name")).toString());
        else if (key == QLatin1String("track"))
            values.insert(key, QStringLiteral("V1"));
        else if (hasType(schema, "boolean") && !values.contains(key))
            values.insert(key, true);
    }
    return values;
}

std::expected<command::CommandRequest, QString> CommandBridge::request(const QString& name, const QVariantMap& params,
                                                                       bool dryRun) const {
    if (!m_controller)
        return std::unexpected(tr("No project"));
    const command::CommandSpec* spec = command::builtinCommands().find(name.toStdString());
    if (!spec)
        return std::unexpected(tr("Unknown command"));
    if (needsSelection(*spec) && m_controller->selection().empty())
        return std::unexpected(tr("Select a clip first"));
    command::CommandRequest request;
    request.command = spec->name;
    request.dryRun = dryRun;
    Json json = Json::object();
    const Json& props = spec->params.value("properties", Json::object());
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string& key = it.key();
        const QString qkey = QString::fromStdString(key);
        if (key == "sequence") {
            json[key] = m_controller->currentSequenceId().value;
        } else if (key == "clip") {
            if (!m_controller->primaryClip())
                continue;
            json[key] = m_controller->primaryClip().value;
        } else if (key == "clips") {
            Json ids = Json::array();
            for (ClipId id : m_controller->selection())
                ids.push_back(id.value);
            if (!ids.empty())
                json[key] = ids;
        } else if (params.contains(qkey)) {
            const QVariant v = params.value(qkey);
            if (key == "track" && v.typeId() == QMetaType::QString) {
                json[key] = v.toString().toStdString();
            } else {
                switch (v.typeId()) {
                case QMetaType::Bool:
                    json[key] = v.toBool();
                    break;
                case QMetaType::Int:
                case QMetaType::LongLong:
                    json[key] = v.toLongLong();
                    break;
                case QMetaType::Double:
                    if (hasType(it.value(), "integer"))
                        json[key] = static_cast<std::int64_t>(std::llround(v.toDouble()));
                    else
                        json[key] = v.toDouble();
                    break;
                default: {
                    const QString text = v.toString();
                    if (text.isEmpty())
                        continue;
                    json[key] = text.toStdString();
                }
                }
            }
        }
    }
    request.params = std::move(json);
    return request;
}

QVariantMap CommandBridge::run(const QString& name, const QVariantMap& params, bool dryRun) {
    const auto request = this->request(name, params, dryRun);
    if (!request) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), request.error()},
                {QStringLiteral("summary"), QString()}};
    }
    // A fresh engine per request: it holds only the idempotency cache, and the session belongs to the controller.
    command::CommandEngine engine(m_controller->session());
    const command::CommandResult result = engine.execute(*request);
    if (!result.ok()) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), messageFor(*result.error)},
                {QStringLiteral("detail"), QString::fromStdString(result.error->message)},
                {QStringLiteral("summary"), QString()}};
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("error"), QString()},
            {QStringLiteral("summary"), summaryOf(result.changes)}};
}

QVariantMap CommandBridge::preview(const QString& name, const QVariantMap& params) {
    return run(name, params, true);
}

QVariantMap CommandBridge::apply(const QString& name, const QVariantMap& params) {
    return run(name, params, false);
}

} // namespace wabcut::app
