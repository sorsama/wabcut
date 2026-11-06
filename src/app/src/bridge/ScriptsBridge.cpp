#include "ScriptsBridge.h"

#include "AgentController.h"
#include "CommandPalette.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ScriptRun.h"

#include <wabcut/command/Command.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/ExportServices.h>
#include <wabcut/model/EditSession.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>

namespace wabcut::app {

namespace sc = wabcut::app::scripts;

namespace {

constexpr qint64 kMaxScriptBytes = 2 * 1024 * 1024;

QString permissionTitle(const QString& key) {
    if (key == QLatin1String("Read")) {
        return ScriptsBridge::tr("Read your project");
    }
    if (key == QLatin1String("EditSequence")) {
        return ScriptsBridge::tr("Edit your sequences");
    }
    if (key == QLatin1String("EditProject")) {
        return ScriptsBridge::tr("Change the project's structure");
    }
    if (key == QLatin1String("MediaAnalysis")) {
        return ScriptsBridge::tr("Analyse media and use AI features");
    }
    return ScriptsBridge::tr("Use commands that read or write files");
}

QString permissionExplanation(const QString& key) {
    if (key == QLatin1String("Read")) {
        return ScriptsBridge::tr("See the project's sequences, clips, markers, media names, transcripts and captions.");
    }
    if (key == QLatin1String("EditSequence")) {
        return ScriptsBridge::tr("Change clips, markers, captions, effects and audio inside sequences. Everything the script "
                                 "changes becomes one undo step.");
    }
    if (key == QLatin1String("EditProject")) {
        return ScriptsBridge::tr("Add, change and remove tracks, bins, media, subclips and sequences; the changes to the project can be "
                                 "undone. A script has no access to the export queue, the export presets and watch folders or your "
                                 "effects library.");
    }
    if (key == QLatin1String("MediaAnalysis")) {
        return ScriptsBridge::tr("Run analysis and AI commands (transcription, search, translation, dubbing). Cloud AI still asks "
                                 "for your consent and stays within your spend cap and no-cloud settings.");
    }
    return ScriptsBridge::tr("Import, export and merge files in the project's folder with Wabcut's commands; Wabcut refuses other places "
                             "to them. A Python script is a program with your rights, so this cannot stop it from reading or "
                             "writing files itself, or from opening other projects with wabcut-cli.");
}

std::filesystem::path toPath(const QString& text) { return std::filesystem::path(text.toStdU16String()); }

/// The assistant's services without the ones that reach outside the project: the export queue, presets and watch folders
/// and the user's effects library. A script's undo and rollback cover the project only, so a script that may edit it
/// cannot cancel an export, delete a preset or a watch folder, or rewrite the effects library either (those commands
/// answer that the host has no such service).
class ScriptServices final : public command::CommandServices {
public:
    explicit ScriptServices(std::shared_ptr<command::CommandServices> all) : m_all(std::move(all)) {}
    std::vector<command::CommandServices*> parts() const override {
        std::vector<command::CommandServices*> out;
        if (!m_all) {
            return out;
        }
        for (command::CommandServices* part : m_all->parts()) {
            if (!command::findService<command::ExportServices>(part) && !command::findService<command::EffectLibraryServices>(part)) {
                out.push_back(part);
            }
        }
        return out;
    }

private:
    std::shared_ptr<command::CommandServices> m_all;
};

} // namespace

// ---- The output pane -----------------------------------------------------------------------------------------------

QVariant ScriptOutputModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row& row = m_rows[index.row()];
    return role == KindRole ? QVariant(row.kind) : role == TextRole ? QVariant(row.text) : QVariant();
}

QHash<int, QByteArray> ScriptOutputModel::roleNames() const {
    return {{KindRole, "kind"}, {TextRole, "line"}};
}

ScriptOutputModel::ScriptOutputModel(QObject* parent) : QAbstractListModel(parent) {
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(40);
    connect(&m_flushTimer, &QTimer::timeout, this, &ScriptOutputModel::flush);
}

void ScriptOutputModel::append(const QString& kind, const QString& text) {
    m_pending.append({kind, text});
    if (!m_flushTimer.isActive()) {
        m_flushTimer.start();
    }
}

void ScriptOutputModel::flush() {
    m_flushTimer.stop();
    if (m_pending.isEmpty()) {
        return;
    }
    // Only the newest kMaxRows lines can matter.
    if (m_pending.size() > kMaxRows) {
        m_pending = m_pending.mid(m_pending.size() - kMaxRows);
    }
    const qsizetype overflow = m_rows.size() + m_pending.size() - kMaxRows;
    if (overflow > 0) {
        beginRemoveRows({}, 0, static_cast<int>(overflow) - 1);
        m_rows.remove(0, overflow);
        endRemoveRows();
    }
    beginInsertRows({}, static_cast<int>(m_rows.size()), static_cast<int>(m_rows.size() + m_pending.size()) - 1);
    m_rows.append(m_pending);
    endInsertRows();
    m_pending.clear();
}

void ScriptOutputModel::clear() {
    m_flushTimer.stop();
    m_pending.clear();
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

QString ScriptOutputModel::plainText() const {
    QStringList lines;
    for (const Row& row : m_rows) {
        lines << row.text;
    }
    for (const Row& row : m_pending) {
        lines << row.text;
    }
    return lines.join(QLatin1Char('\n'));
}

// ---- The palette ---------------------------------------------------------------------------------------------------

class ScriptsBridge::Provider final : public PaletteProvider {
public:
    explicit Provider(ScriptsBridge* bridge) : m_bridge(bridge) {}
    QString id() const override { return QStringLiteral("scripts"); }

    std::vector<PaletteItem> items(const QString&) const override {
        std::vector<PaletteItem> out;
        if (!m_bridge) {
            return out;
        }
        PaletteItem run;
        run.key = id() + QStringLiteral(":run");
        run.title = ScriptsBridge::tr("Run Python Script…");
        run.group = ScriptsBridge::tr("Scripts");
        run.detail = ScriptsBridge::tr("Automate the open project with a Python script");
        run.section = QStringLiteral("commands");
        run.keywords = {QStringLiteral("python"), QStringLiteral("script"), QStringLiteral("automation"), QStringLiteral("batch"),
                        QStringLiteral("macro")};
        out.push_back(std::move(run));
        int index = 0;
        for (const QVariant& v : m_bridge->recent().mid(0, 5)) {
            const QVariantMap recent = v.toMap();
            PaletteItem item;
            item.key = id() + QStringLiteral(":recent:") + QString::number(index++);
            item.title = ScriptsBridge::tr("Run %1").arg(recent.value(QStringLiteral("name")).toString());
            item.group = ScriptsBridge::tr("Scripts");
            item.detail = recent.value(QStringLiteral("folder")).toString();
            item.section = QStringLiteral("commands");
            item.keywords = {QStringLiteral("python"), QStringLiteral("script"), recent.value(QStringLiteral("name")).toString()};
            out.push_back(std::move(item));
        }
        return out;
    }

    QString activate(const PaletteItem& item, const QVariantMap&) override {
        if (!m_bridge) {
            return ScriptsBridge::tr("Scripts are not available");
        }
        if (item.key == id() + QStringLiteral(":run")) {
            m_bridge->openDialog();
            return {};
        }
        const int index = item.key.mid(id().size() + QStringLiteral(":recent:").size() + 0).toInt();
        const QVariantList recent = m_bridge->recent();
        if (index < 0 || index >= recent.size()) {
            return ScriptsBridge::tr("That script is no longer in the list");
        }
        const QString problem = m_bridge->chooseRecent(recent[index].toMap().value(QStringLiteral("path")).toString());
        m_bridge->openDialog();
        return problem;
    }

private:
    QPointer<ScriptsBridge> m_bridge;
};

std::shared_ptr<PaletteProvider> ScriptsBridge::paletteProvider() {
    if (!m_provider) {
        m_provider = std::make_shared<Provider>(this);
    }
    return m_provider;
}

// ---- The bridge ----------------------------------------------------------------------------------------------------

ScriptsBridge::ScriptsBridge(QObject* parent) : QObject(parent), m_output(this) {}

ScriptsBridge::~ScriptsBridge() = default;

ScriptsBridge* ScriptsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ScriptsBridge;
    const QString dataDir = qEnvironmentVariableIsEmpty("WABCUT_DATA_DIR")
                                ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                : qEnvironmentVariable("WABCUT_DATA_DIR");
#ifdef Q_OS_WIN
    const QString cli = QCoreApplication::applicationDirPath() + QStringLiteral("/wabcut-cli.exe");
#else
    const QString cli = QCoreApplication::applicationDirPath() + QStringLiteral("/wabcut-cli");
#endif
    bridge->setPaths(dataDir + QStringLiteral("/script-permissions.json"), QString(), QFileInfo::exists(cli) ? cli : QString());
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setLifecycle(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        // The assistant's services: a script is a remote caller like the assistant (consent, spend cap, no-cloud).
        QPointer<QQmlEngine> engine(qmlEngine);
        bridge->setServicesFactory([engine]() -> std::shared_ptr<command::CommandServices> {
            auto* agent = engine ? engine->singletonInstance<AgentController*>("Wabcut", "AgentController") : nullptr;
            return agent ? std::make_shared<ScriptServices>(agent->commandServices()) : nullptr;
        });
    }
    return bridge;
}

void ScriptsBridge::setController(ProjectController* controller) { m_controller = controller; }

void ScriptsBridge::setLifecycle(ProjectLifecycle* lifecycle) { m_lifecycle = lifecycle; }

void ScriptsBridge::setServicesFactory(std::function<std::shared_ptr<command::CommandServices>()> factory) {
    m_servicesFactory = std::move(factory);
}

void ScriptsBridge::setPaths(const QString& grantFile, const QString& packageFolder, const QString& cliPath) {
    m_grantFile = grantFile;
    m_packageFolder = packageFolder;
    m_cliPath = cliPath;
    m_store = std::make_unique<sc::ScriptGrantStore>(grantFile);
    emit recentChanged();
}

void ScriptsBridge::setPythonOverride(const QString& path) {
    m_pythonOverride = path;
    ++m_pythonSearch;
    m_python = sc::locatePython(path);
    m_pythonChecked = true;
    m_pythonSearching = false;
    emit pythonChanged();
    emit stateChanged();
}

QString ScriptsBridge::packageFolder() const {
    QStringList candidates;
    if (!m_packageFolder.isEmpty()) {
        candidates << m_packageFolder;
    }
    if (const QString env = qEnvironmentVariable("WABCUT_PYTHON_PACKAGE_DIR"); !env.isEmpty()) {
        candidates << env;
    }
    const QString app = QCoreApplication::applicationDirPath();
    candidates << app + QStringLiteral("/python") << app + QStringLiteral("/../Resources/python");
    for (const QString& folder : candidates) {
        if (QFileInfo::exists(folder + QStringLiteral("/wabcut/__init__.py"))) {
            return QDir::cleanPath(folder);
        }
    }
    return {};
}

QString ScriptsBridge::projectFolder() const {
    if (!m_lifecycle || m_lifecycle->untitled() || m_lifecycle->demo()) {
        return {};
    }
    const QString file = m_lifecycle->filePath();
    return file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
}

QString ScriptsBridge::pythonText() const {
    if (m_pythonSearching || !m_pythonChecked) {
        return tr("Looking for Python…");
    }
    return m_python.ok() ? m_python.display() : m_python.error;
}

void ScriptsBridge::startPythonSearch() {
    const int token = ++m_pythonSearch;
    m_pythonChecked = false;
    m_pythonSearching = true;
    emit pythonChanged();
    const QString chosen = !m_pythonOverride.isEmpty() ? m_pythonOverride : (m_store ? m_store->pythonPath() : QString());
    QPointer<ScriptsBridge> self(this);
    QThread* thread = QThread::create([self, token, chosen] {
        const sc::PythonRuntime runtime = sc::locatePython(chosen);
        QMetaObject::invokeMethod(
            qApp,
            [self, token, runtime] {
                if (!self || token != self->m_pythonSearch) {
                    return; // a newer search replaced this one
                }
                self->m_python = runtime;
                self->m_pythonChecked = true;
                self->m_pythonSearching = false;
                emit self->pythonChanged();
                emit self->stateChanged();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ScriptsBridge::findPython() {
    m_pythonOverride.clear();
    startPythonSearch();
}

void ScriptsBridge::choosePython(const QUrl& file) {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (path.isEmpty()) {
        return;
    }
    if (m_store) {
        m_store->setPythonPath(QDir::toNativeSeparators(path));
        m_store->save();
    }
    m_pythonOverride.clear();
    startPythonSearch();
}

bool ScriptsBridge::canRun() const {
    return m_state == QLatin1String("ready") && pythonOk() && m_problem.isEmpty() && m_controller != nullptr;
}

void ScriptsBridge::setState(const QString& state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

void ScriptsBridge::resetRunData() {
    m_summary.clear();
    m_succeeded = false;
    m_commands = 0;
    m_edits = 0;
    m_output.clear();
    emit outcomeChanged();
    emit progressChanged();
}

QString ScriptsBridge::choose(const QUrl& file) {
    if (running()) {
        return tr("A script is running.");
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const QFileInfo info(path);
    resetRunData();
    m_path = info.absoluteFilePath();
    m_name = info.completeBaseName();
    m_header = {};
    m_permissions.clear();
    m_problem.clear();
    m_hash.clear();
    m_hadRemembered = false;
    if (!info.isFile()) {
        m_problem = tr("That file does not exist.");
    } else if (info.suffix().compare(QLatin1String("py"), Qt::CaseInsensitive) != 0 &&
               info.suffix().compare(QLatin1String("pyw"), Qt::CaseInsensitive) != 0) {
        m_problem = tr("Choose a Python script (a .py file).");
    } else if (info.size() > kMaxScriptBytes) {
        m_problem = tr("That script is over 2 MB; Wabcut will not run it.");
    } else {
        QFile in(m_path);
        if (!in.open(QIODevice::ReadOnly)) {
            m_problem = tr("That file cannot be read: %1").arg(in.errorString());
        } else {
            const QByteArray bytes = in.readAll();
            m_hash = sc::contentHash(bytes);
            m_header = sc::parseScriptHeader(QString::fromUtf8(bytes));
            if (!m_header.name.isEmpty()) {
                m_name = m_header.name;
            }
            const QStringList remembered = m_store ? m_store->grantsFor(m_hash) : QStringList();
            m_hadRemembered = !remembered.isEmpty();
            for (const QString& key : sc::permissionNames()) {
                Permission p;
                p.key = key;
                p.requested = key == QLatin1String("Read") || m_header.requested.contains(key);
                // On by default when asked for; what was allowed for this exact script before wins.
                p.chosen = key == QLatin1String("Read") || (p.requested && (m_hadRemembered ? remembered.contains(key) : true));
                m_permissions.append(p);
            }
        }
    }
    emit scriptChanged();
    emit permissionsChanged();
    if (!m_problem.isEmpty()) {
        setState(QStringLiteral("idle"));
        return m_problem;
    }
    setState(QStringLiteral("ready"));
    if (!m_pythonChecked && !m_pythonSearching) {
        startPythonSearch();
    }
    return {};
}

QString ScriptsBridge::chooseRecent(const QString& path) { return choose(QUrl::fromLocalFile(path)); }

QVariantList ScriptsBridge::permissions() const {
    QVariantList out;
    const bool folderKnown = !projectFolder().isEmpty();
    for (const Permission& p : m_permissions) {
        const bool fixed = p.key == QLatin1String("Read");
        QString note;
        bool available = fixed || p.requested;
        if (!fixed && !p.requested) {
            note = tr("The script does not ask for this.");
        } else if (p.key == QLatin1String("Files") && !folderKnown) {
            available = false;
            note = tr("Save the project first: file commands work inside the project's folder.");
        } else if (!fixed && m_hadRemembered && p.chosen) {
            note = tr("Allowed before");
        }
        out.append(QVariantMap{{QStringLiteral("key"), p.key},
                               {QStringLiteral("title"), permissionTitle(p.key)},
                               {QStringLiteral("explanation"), permissionExplanation(p.key)},
                               {QStringLiteral("requested"), p.requested},
                               {QStringLiteral("chosen"), p.chosen && available},
                               {QStringLiteral("fixed"), fixed},
                               {QStringLiteral("available"), available},
                               {QStringLiteral("note"), note}});
    }
    return out;
}

void ScriptsBridge::setRemember(bool remember) {
    if (m_remember == remember) {
        return;
    }
    m_remember = remember;
    emit permissionsChanged();
}

void ScriptsBridge::setChosen(const QString& key, bool on) {
    if (running() || key == QLatin1String("Read")) {
        return;
    }
    for (Permission& p : m_permissions) {
        if (p.key == key && p.requested) {
            p.chosen = on;
            emit permissionsChanged();
            return;
        }
    }
}

QStringList ScriptsBridge::chosenKeys() const {
    QStringList keys;
    const bool folderKnown = !projectFolder().isEmpty();
    for (const Permission& p : m_permissions) {
        if (p.chosen && (p.key != QLatin1String("Files") || folderKnown)) {
            keys << p.key;
        }
    }
    return keys;
}

QVariantList ScriptsBridge::recent() const {
    QVariantList out;
    if (!m_store) {
        return out;
    }
    for (const QString& path : m_store->recent()) {
        const QFileInfo info(path);
        out.append(QVariantMap{{QStringLiteral("path"), path},
                               {QStringLiteral("name"), info.fileName()},
                               {QStringLiteral("folder"), QDir::toNativeSeparators(info.absolutePath())}});
    }
    return out;
}

void ScriptsBridge::forgetChoice() {
    if (m_store && !m_hash.isEmpty()) {
        m_store->forget(m_hash);
        m_store->save();
    }
    m_hadRemembered = false;
    emit permissionsChanged();
}

QString ScriptsBridge::run() {
    if (m_state != QLatin1String("ready")) {
        return tr("Choose a script first.");
    }
    if (!m_controller) {
        return tr("No project is open.");
    }
    if (!pythonOk()) {
        return m_pythonChecked ? m_python.error : tr("Wabcut is still looking for Python.");
    }
    const QStringList keys = chosenKeys();
    ScriptRunOptions options;
    options.scriptPath = m_path;
    options.name = m_name;
    options.base = m_controller->snapshot();
    options.allowed = command::CapabilitySet::of({command::Capability::Read});
    if (keys.contains(QLatin1String("EditSequence"))) {
        options.allowed.bits |= command::CapabilitySet::of({command::Capability::EditSequence}).bits;
    }
    if (keys.contains(QLatin1String("EditProject"))) {
        options.allowed.bits |= command::CapabilitySet::of({command::Capability::EditProject}).bits;
    }
    if (keys.contains(QLatin1String("MediaAnalysis"))) {
        options.allowed.bits |= command::CapabilitySet::of({command::Capability::MediaAnalysis}).bits;
    }
    if (keys.contains(QLatin1String("Files"))) {
        options.fileRoots.push_back(toPath(projectFolder()));
    }
    options.services = m_servicesFactory ? m_servicesFactory() : nullptr;
    options.python = m_python;
    options.packageFolder = packageFolder();
    options.cliPath = m_cliPath;
    options.appVersion = QCoreApplication::applicationVersion();
    options.callTimeout = m_callTimeout;
    options.sequence = m_controller->currentSequenceId().value;
    if (!m_grantFile.isEmpty()) {
        options.auditFile = QFileInfo(m_grantFile).absolutePath() + QStringLiteral("/script-audit.jsonl");
    }
    if (options.packageFolder.isEmpty()) {
        return tr("The Wabcut Python package was not found next to the app. Reinstall Wabcut.");
    }

    if (m_store) {
        if (m_remember) {
            m_store->remember(m_hash, m_path, keys);
        }
        m_store->addRecent(m_path);
        m_store->save();
        emit recentChanged();
    }
    resetRunData();
    setState(QStringLiteral("running"));
    m_run = std::make_unique<ScriptRun>(
        std::move(options), [this](const QString& kind, const QString& text) { m_output.append(kind, text); },
        [this](const ScriptOutcome& outcome) { onFinished(outcome); },
        [this](int commands, int edits) {
            m_commands = commands;
            m_edits = edits;
            emit progressChanged();
        });
    m_run->start();
    return {};
}

void ScriptsBridge::cancel() {
    if (m_run && running()) {
        m_output.append(QStringLiteral("info"), tr("Stopping…"));
        m_run->cancel();
    }
}

void ScriptsBridge::onFinished(const ScriptOutcome& outcome) {
    m_output.flush();
    m_commands = outcome.commands;
    m_edits = outcome.edits;
    bool ok = outcome.ok;
    QString message = outcome.message;
    if (outcome.ok) {
        if (!outcome.changed()) {
            message = tr("Done. The script made no changes to the project.");
        } else if (!m_controller || m_controller->snapshot().get() != outcome.base.get()) {
            ok = false;
            message = tr("The project changed while the script ran, so its edits were not applied. Run the script again.");
        } else {
            const std::string label = tr("Script: %1").arg(m_name).toStdString();
            if (auto committed = m_controller->session().commit(label, *outcome.result); !committed) {
                ok = false;
                message = tr("The project did not accept the script's result (%1). Nothing was changed.")
                              .arg(QString::fromStdString(committed.error().message));
            } else {
                message = tr("Done. %n edit(s) applied as one undo step: \"%1\".", "", outcome.edits)
                              .arg(QString::fromStdString(label));
            }
        }
    }
    m_summary = message;
    m_succeeded = ok;
    m_output.append(QStringLiteral("info"), message);
    m_output.flush();
    setState(QStringLiteral("finished"));
    emit progressChanged();
    emit outcomeChanged();
    emit finished(ok, message);
}

void ScriptsBridge::reset() {
    if (running()) {
        return;
    }
    m_run.reset();
    resetRunData();
    m_path.clear();
    m_name.clear();
    m_hash.clear();
    m_problem.clear();
    m_header = {};
    m_permissions.clear();
    emit scriptChanged();
    emit permissionsChanged();
    setState(QStringLiteral("idle"));
}

void ScriptsBridge::copyOutput() const {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(m_output.plainText());
    }
}

} // namespace wabcut::app
