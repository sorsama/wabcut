#include "RightsBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Rights.h>

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QUrl>

#include <filesystem>
#include <string_view>

namespace wabcut::app {

namespace {

using Json = command::Json;

QVariant variantFrom(const Json& json) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(json.dump())).toVariant();
}

QString say(const char* text) { return QCoreApplication::translate("wabcut::app::RightsBridge", text); }

/// The fields a caller may set, in the order the command names them.
constexpr const char* kFields[] = {"status", "source",    "licence",    "reference", "territory",
                                   "expiry", "cost",      "currency",   "creditLine", "notes"};

/// The text a failed request shows: the command's own message, with the few the dialog can say better.
QString errorOf(const command::CommandResult& result) {
    if (result.ok())
        return {};
    const std::string& message = result.error->message;
    if (message == "the sequence has no In and Out marks")
        return say("Set an In and an Out point first.");
    return QString::fromStdString(message);
}

} // namespace

RightsBridge::RightsBridge(QObject* parent) : QObject(parent) {}

RightsBridge* RightsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new RightsBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void RightsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, [this] {
            ++m_revision;
            emit revisionChanged();
        });
    }
}

void RightsBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QString RightsBridge::today() const { return QDate::currentDate().toString(Qt::ISODate); }

QString RightsBridge::normalisedCost(const QString& text) const {
    const auto minor = parseCostMinor(text.toStdString());
    return minor ? QString::fromStdString(formatCostMinor(*minor)) : QString();
}

void RightsBridge::openFor(const QStringList& mediaKeys) { emit openRequested(mediaKeys); }

QVariantMap RightsBridge::log(const QVariantMap& options) const {
    QVariantMap out{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QString()}, {QStringLiteral("rows"), QVariantList()}};
    if (!m_controller) {
        out[QStringLiteral("error")] = tr("There is no project.");
        return out;
    }
    const QString scope = options.value(QStringLiteral("scope"), QStringLiteral("sequence")).toString();
    if (scope == QLatin1String("sequence") && !m_controller->currentSequenceId().isValid()) {
        out[QStringLiteral("error")] = tr("There is no sequence to report on: choose the whole project instead.");
        return out;
    }
    command::EnginePolicy policy;
    policy.allowed = command::CapabilitySet::of({command::Capability::Read});
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
    command::CommandRequest request;
    request.command = "project.rightsLog";
    request.params = Json{{"format", "json"}, {"scope", scope.toStdString()}};
    if (m_controller->currentSequenceId().isValid())
        request.params["sequence"] = m_controller->currentSequenceId().value;
    if (const QString asOf = options.value(QStringLiteral("asOf")).toString(); !asOf.isEmpty())
        request.params["asOf"] = asOf.toStdString();
    if (options.contains(QStringLiteral("soonDays")))
        request.params["soonDays"] = options.value(QStringLiteral("soonDays")).toInt();
    if (options.contains(QStringLiteral("flaggedOnly")))
        request.params["flaggedOnly"] = options.value(QStringLiteral("flaggedOnly")).toBool();
    if (options.contains(QStringLiteral("includeUnrecorded")))
        request.params["includeUnrecorded"] = options.value(QStringLiteral("includeUnrecorded")).toBool();
    if (const QString range = options.value(QStringLiteral("range")).toString(); !range.isEmpty())
        request.params["range"] = range.toStdString();
    const command::CommandResult result = engine.execute(request);
    if (!result.ok()) {
        out[QStringLiteral("error")] = errorOf(result);
        return out;
    }
    const Json& report = result.data["report"];
    QVariantList rows;
    for (const Json& row : report["rows"]) {
        QVariantMap map = variantFrom(row).toMap();
        map.insert(QStringLiteral("key"), mediaKey(MediaId{row.value("mediaId", std::uint64_t{0})}));
        rows.append(map);
    }
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("rows")] = rows;
    out[QStringLiteral("totals")] = variantFrom(report["totals"]);
    out[QStringLiteral("asOf")] = QString::fromStdString(report.value("asOf", std::string()));
    out[QStringLiteral("text")] = QString::fromStdString(result.data.value("text", std::string()));
    return out;
}

QString RightsBridge::setRights(const QStringList& mediaKeys, const QVariantMap& fields) {
    if (!m_controller)
        return tr("There is no project.");
    Json media = Json::array();
    for (const QString& key : mediaKeys) {
        if (const auto id = detail::parseId<MediaId>(QLatin1Char('m'), key))
            media.push_back(id->value);
    }
    if (media.empty())
        return tr("Choose the clips in the list first.");
    Json params{{"media", media}};
    for (const char* name : kFields) {
        const QString key = QString::fromLatin1(name);
        if (fields.contains(key))
            params[name] = fields.value(key).toString().trimmed().toStdString();
    }
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "media.setRights", std::move(params));
    const QString error = errorOf(result);
    setMessage(error);
    return error;
}

QVariantMap RightsBridge::writeLog(const QString& file, const QVariantMap& options) {
    QVariantMap result = log(options);
    if (!result.value(QStringLiteral("ok")).toBool())
        return result;
    const QUrl url(file);
    const QString path = QDir::toNativeSeparators(url.isLocalFile() ? url.toLocalFile() : file);
    if (path.isEmpty() || !QDir::isAbsolutePath(QDir::fromNativeSeparators(path))) {
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("error")] = tr("Choose where to save the log.");
        return result;
    }
    const bool json = path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive);
    // log() read the rows as JSON for the dialog; a CSV file asks the command for its own text below.
    QString text = result.value(QStringLiteral("text")).toString();
    if (!json) {
        // The CSV: the same command, format csv.
        command::EnginePolicy policy;
        policy.allowed = command::CapabilitySet::of({command::Capability::Read});
        command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
        command::CommandRequest request;
        request.command = "project.rightsLog";
        request.params = Json{{"format", "csv"}, {"scope", options.value(QStringLiteral("scope"), QStringLiteral("sequence")).toString().toStdString()}};
        if (m_controller->currentSequenceId().isValid())
            request.params["sequence"] = m_controller->currentSequenceId().value;
        if (const QString asOf = options.value(QStringLiteral("asOf")).toString(); !asOf.isEmpty())
            request.params["asOf"] = asOf.toStdString();
        if (options.contains(QStringLiteral("flaggedOnly")))
            request.params["flaggedOnly"] = options.value(QStringLiteral("flaggedOnly")).toBool();
        if (options.contains(QStringLiteral("includeUnrecorded")))
            request.params["includeUnrecorded"] = options.value(QStringLiteral("includeUnrecorded")).toBool();
        const command::CommandResult csv = engine.execute(request);
        if (!csv.ok()) {
            result[QStringLiteral("ok")] = false;
            result[QStringLiteral("error")] = errorOf(csv);
            return result;
        }
        text = QString::fromStdString(csv.data.value("text", std::string()));
    }
    // Spreadsheets (Excel) read a CSV as UTF-8 only with a byte order mark: names with accents or Thai stay readable.
    QByteArray bytes = text.toUtf8();
    if (!json)
        bytes.prepend("\xEF\xBB\xBF");
    const auto wrote = io::writeFileAtomically(std::filesystem::path(path.toStdU16String()),
                                               std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (!wrote) {
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("error")] = tr("The log could not be written to %1: %2").arg(path, QString::fromStdString(wrote.error().message));
        return result;
    }
    result.remove(QStringLiteral("text"));
    result[QStringLiteral("path")] = path;
    result[QStringLiteral("count")] = result.value(QStringLiteral("rows")).toList().size();
    return result;
}

QVariantMap RightsBridge::makeCreditsRoll(const QVariantMap& options) {
    QVariantMap out{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QString()}};
    if (!m_controller || !m_controller->currentSequenceId().isValid()) {
        out[QStringLiteral("error")] = tr("There is no sequence to put the credits in.");
        return out;
    }
    Json params = Json::object();
    if (options.contains(QStringLiteral("heading")))
        params["heading"] = options.value(QStringLiteral("heading")).toString().toStdString();
    if (const QString order = options.value(QStringLiteral("order")).toString(); !order.isEmpty())
        params["order"] = order.toStdString();
    if (const QString range = options.value(QStringLiteral("range")).toString(); !range.isEmpty())
        params["range"] = range.toStdString();
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "sequence.creditsRoll", std::move(params));
    if (!result.ok()) {
        out[QStringLiteral("error")] = errorOf(result);
        setMessage(out.value(QStringLiteral("error")).toString());
        return out;
    }
    QStringList warnings;
    for (const std::string& w : result.warnings)
        warnings << QString::fromStdString(w);
    QStringList credits;
    for (const Json& c : result.data["credits"])
        credits << QString::fromStdString(c.get<std::string>());
    QStringList missing;
    for (const Json& m : result.data["missingCreditLine"])
        missing << QString::fromStdString(m.value("media", std::string()));
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("clip")] = clipKey(ClipId{result.data.value("clip", std::uint64_t{0})});
    out[QStringLiteral("credits")] = credits;
    out[QStringLiteral("missingCreditLine")] = missing;
    out[QStringLiteral("warnings")] = warnings;
    setMessage({});
    return out;
}

} // namespace wabcut::app
