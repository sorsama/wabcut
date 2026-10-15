#include "ExtensionsBridge.h"

#include "AgentController.h"
#include "CommandPalette.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ShortsBridge.h" // BridgeJob

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/script/Host.h>
#include <wabcut/script/Manager.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <string>

namespace wabcut::app {

namespace sc = wabcut::script;
using command::Capability;
using command::CapabilitySet;
using Json = command::Json;

namespace {

constexpr std::array kCapabilities{Capability::Read, Capability::EditSequence, Capability::EditProject,
                                   Capability::MediaAnalysis};
constexpr qsizetype kLogLines = 500;

QString text(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString pathText(const std::filesystem::path& p) { return QDir::toNativeSeparators(QString::fromStdU16String(p.u16string())); }

std::filesystem::path localPath(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return std::filesystem::path(path.toStdU16String());
}

QVariant toVariant(const Json& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(Json{{"v", value}}.dump())).object().value(QStringLiteral("v")).toVariant(); }

Json toJson(const QVariantMap& map) {
    const QByteArray bytes = QJsonDocument::fromVariant(map).toJson(QJsonDocument::Compact);
    Json out = Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    return out.is_discarded() ? Json::object() : out;
}

// The permission dialog's words (the same as wabcut::script::describe(), translatable).
QString capabilityTitle(Capability c) {
    switch (c) {
    case Capability::Read:
        return ExtensionsBridge::tr("Read your project");
    case Capability::EditSequence:
        return ExtensionsBridge::tr("Edit your sequences");
    case Capability::EditProject:
        return ExtensionsBridge::tr("Change the project's structure");
    case Capability::MediaAnalysis:
        return ExtensionsBridge::tr("Analyse media and use AI features");
    case Capability::Network:
        break;
    }
    return ExtensionsBridge::tr("Network");
}

QString capabilityExplanation(Capability c) {
    switch (c) {
    case Capability::Read:
        return ExtensionsBridge::tr("See the project's sequences, clips, markers, media names, transcripts and captions.");
    case Capability::EditSequence:
        return ExtensionsBridge::tr("Change clips, markers, captions, effects and audio inside sequences; each change is "
                                    "one undo step named after the extension.");
    case Capability::EditProject:
        return ExtensionsBridge::tr("Add, change and remove tracks, bins, media, subclips and sequences; every change can "
                                    "be undone.");
    case Capability::MediaAnalysis:
        return ExtensionsBridge::tr("Run analysis and AI commands (transcription, search, translation, dubbing). Cloud AI "
                                    "still asks for your consent and stays within your spend cap and no-cloud settings.");
    case Capability::Network:
        break;
    }
    return ExtensionsBridge::tr("Extensions are never given network access.");
}

QString badgeText(sc::ExtensionStatus status) {
    switch (status) {
    case sc::ExtensionStatus::NeedsReview:
        return ExtensionsBridge::tr("Review needed");
    case sc::ExtensionStatus::NeedsTrust:
        return ExtensionsBridge::tr("Not trusted");
    case sc::ExtensionStatus::Disabled:
        return ExtensionsBridge::tr("Off");
    case sc::ExtensionStatus::Idle:
    case sc::ExtensionStatus::Starting:
    case sc::ExtensionStatus::Running:
        return ExtensionsBridge::tr("On");
    case sc::ExtensionStatus::Failed:
        return ExtensionsBridge::tr("Error");
    case sc::ExtensionStatus::Invalid:
        break;
    }
    return ExtensionsBridge::tr("Broken");
}

QString badgeKind(sc::ExtensionStatus status) {
    switch (status) {
    case sc::ExtensionStatus::NeedsReview:
        return QStringLiteral("review");
    case sc::ExtensionStatus::NeedsTrust:
        return QStringLiteral("trust");
    case sc::ExtensionStatus::Disabled:
        return QStringLiteral("off");
    case sc::ExtensionStatus::Idle:
    case sc::ExtensionStatus::Starting:
    case sc::ExtensionStatus::Running:
        return QStringLiteral("on");
    case sc::ExtensionStatus::Failed:
        return QStringLiteral("error");
    case sc::ExtensionStatus::Invalid:
        break;
    }
    return QStringLiteral("broken");
}

QVariantList grantedChips(CapabilitySet set) {
    QVariantList out;
    for (const Capability c : kCapabilities) {
        if (set.has(c))
            out.append(QVariantMap{{QStringLiteral("capability"), text(command::toString(c))},
                                   {QStringLiteral("title"), capabilityTitle(c)}});
    }
    return out;
}

QString paletteIdOf(const std::string& extension, const std::string& command) {
    return text(sc::kPaletteIdPrefix) + text(extension) + QLatin1Char('/') + text(command);
}

QVariantMap reviewMap(const sc::ExtensionReview& r) {
    QVariantList requests;
    for (const sc::CapabilityRequest& request : r.requests) {
        requests.append(QVariantMap{{QStringLiteral("capability"), text(command::toString(request.info.capability))},
                                    {QStringLiteral("title"), capabilityTitle(request.info.capability)},
                                    {QStringLiteral("explanation"), capabilityExplanation(request.info.capability)},
                                    {QStringLiteral("granted"), request.granted}});
    }
    QStringList notes;
    for (const std::string& note : r.notes)
        notes << QString::fromStdString(note);
    const sc::Manifest& m = r.manifest;
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("error"), QString()},
            {QStringLiteral("id"), QString::fromStdString(m.id)},
            {QStringLiteral("name"), QString::fromStdString(m.name)},
            {QStringLiteral("version"), QString::fromStdString(sc::toString(m.version))},
            {QStringLiteral("author"), QString::fromStdString(m.author)},
            {QStringLiteral("licence"), QString::fromStdString(m.licence)},
            {QStringLiteral("description"), QString::fromStdString(m.description)},
            {QStringLiteral("scope"), text(sc::toString(r.scope))},
            {QStringLiteral("project"), r.scope == sc::ExtensionScope::Project},
            {QStringLiteral("folder"), pathText(r.folder)},
            {QStringLiteral("update"), r.update},
            {QStringLiteral("needsTrust"), r.needsTrust},
            {QStringLiteral("notes"), notes},
            {QStringLiteral("requests"), requests}};
}

QVariantMap failedReview(const QString& error) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
}

std::optional<CapabilitySet> capabilitiesOf(const QStringList& names) {
    CapabilitySet set;
    for (const QString& name : names) {
        const auto c = command::capabilityFromString(name.toStdString());
        if (!c)
            return std::nullopt;
        set.bits |= CapabilitySet::of({*c}).bits;
    }
    return set;
}

} // namespace

// ---- The manager's delegate (GUI thread, except wake()) ---------------------------------------------------------

class ExtensionsBridge::Delegate final : public sc::ManagerDelegate {
public:
    explicit Delegate(ExtensionsBridge* bridge) : m_bridge(bridge), m_queued(bridge->m_pumpQueued) {}

    /// Called by the manager's watcher threads: one queued pump at a time.
    void wake() override {
        if (m_queued->exchange(true))
            return;
        ExtensionsBridge* bridge = m_bridge;
        QMetaObject::invokeMethod(
            bridge,
            [bridge] {
                bridge->m_pumpQueued->store(false);
                bridge->pump();
            },
            Qt::QueuedConnection);
    }
    void changed() override {
        if (live)
            m_bridge->refresh();
    }
    void notify(const sc::ExtensionInfo& extension, const sc::Notification& n) override {
        if (live)
            emit m_bridge->notified(nameOf(extension), QString::fromStdString(n.level), QString::fromStdString(n.message));
    }
    void prompt(const sc::ExtensionInfo& extension, const sc::PromptRequest& p) override {
        if (live)
            m_bridge->addPrompt(QString::fromStdString(extension.id), p.prompt, toVariant(p.form).toMap());
    }
    void promptClosed(std::string_view extensionId, std::uint64_t prompt) override {
        if (live)
            m_bridge->removePrompt(text(extensionId) + QLatin1Char('#') + QString::number(prompt));
    }
    void log(std::string_view extensionId, const sc::LogLine& line) override {
        if (!live)
            return;
        QVariantList& lines = logs[text(extensionId)];
        lines.append(QVariantMap{{QStringLiteral("level"), QString::fromStdString(line.level)},
                                 {QStringLiteral("message"), QString::fromStdString(line.message)}});
        if (lines.size() > kLogLines)
            lines.remove(0, lines.size() - kLogLines);
        emit m_bridge->logged(text(extensionId));
    }
    /// C.14: an extension's AI-model command, run as a command job (never inside pump()).
    bool runLongCommand(const sc::ExtensionInfo& extension, sc::LongCommand job) override {
        return live && m_bridge->startLongCommand(nameOf(extension), std::move(job));
    }
    void faulted(const sc::ExtensionInfo& extension, const sc::StopReport& report, bool disabled) override {
        if (!live)
            return;
        m_bridge->refresh();
        emit m_bridge->faulted(QString::fromStdString(extension.id), nameOf(extension),
                               QString::fromStdString(report.reason), disabled);
    }

    static QString nameOf(const sc::ExtensionInfo& e) {
        return QString::fromStdString(e.manifest ? e.manifest->name : e.id);
    }

    bool live = true; ///< false while the manager is destroyed (its callbacks must not reach the bridge then)
    std::map<QString, QVariantList> logs;

private:
    ExtensionsBridge* m_bridge;
    std::shared_ptr<std::atomic<bool>> m_queued;
};

// ---- The palette's "Extensions" group --------------------------------------------------------------------------

class ExtensionsBridge::Provider final : public PaletteProvider {
public:
    explicit Provider(ExtensionsBridge* bridge) : m_bridge(bridge) {}
    QString id() const override { return QStringLiteral("extension"); }

    std::vector<PaletteItem> items(const QString&) const override {
        std::vector<PaletteItem> out;
        if (!m_bridge)
            return out;
        PaletteItem manage;
        manage.key = id() + QStringLiteral(":manage");
        manage.title = ExtensionsBridge::tr("Manage Extensions…");
        manage.group = ExtensionsBridge::tr("Extensions");
        manage.detail = ExtensionsBridge::tr("Install, review, turn on and off");
        manage.section = QStringLiteral("commands");
        manage.keywords = {QStringLiteral("extensions"), QStringLiteral("scripts"), QStringLiteral("permissions"),
                           QStringLiteral("install")};
        out.push_back(std::move(manage));
        for (const QVariant& v : m_bridge->m_palette) {
            const QVariantMap c = v.toMap();
            const QString paletteId = c.value(QStringLiteral("paletteId")).toString();
            PaletteItem item;
            item.key = id() + QLatin1Char(':') + paletteId;
            item.title = c.value(QStringLiteral("title")).toString();
            item.group = ExtensionsBridge::tr("Extensions");
            item.shortcut = m_bridge->m_dispatcher ? m_bridge->m_dispatcher->shortcutText(paletteId) : QString();
            item.detail = c.value(QStringLiteral("extensionName")).toString();
            item.section = QStringLiteral("commands");
            item.keywords = {paletteId, c.value(QStringLiteral("extensionName")).toString(),
                             c.value(QStringLiteral("commandId")).toString(), QStringLiteral("extension")};
            out.push_back(std::move(item));
        }
        return out;
    }

    QString activate(const PaletteItem& item, const QVariantMap&) override {
        if (!m_bridge)
            return ExtensionsBridge::tr("Extensions are not available");
        if (item.key == id() + QStringLiteral(":manage")) {
            m_bridge->openManager();
            return {};
        }
        return m_bridge->run(item.key.mid(id().size() + 1));
    }

    void changed() const { notifyChanged(); }

private:
    QPointer<ExtensionsBridge> m_bridge;
};

// ---- Services: the assistant's, built when a command first needs one ----------------------------------------------

class ExtensionsBridge::LazyServices final : public command::CommandServices {
public:
    explicit LazyServices(std::function<std::shared_ptr<command::CommandServices>()> make) : m_make(std::move(make)) {}
    std::vector<command::CommandServices*> parts() const override {
        if (!m_made && m_make)
            m_made = m_make();
        if (!m_made)
            return {};
        return {m_made.get()};
    }

private:
    std::function<std::shared_ptr<command::CommandServices>()> m_make;
    mutable std::shared_ptr<command::CommandServices> m_made;
};

// ---- The bridge --------------------------------------------------------------------------------------------------

ExtensionsBridge::ExtensionsBridge(QObject* parent) : QObject(parent), m_longJob(std::make_unique<BridgeJob>()) {
    m_delegate = std::make_unique<Delegate>(this); // after m_pumpQueued, which it shares
    m_longState = QVariantMap{{QStringLiteral("running"), false}};
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ExtensionsBridge::pump);
}

ExtensionsBridge::~ExtensionsBridge() {
    m_timer.stop();
    m_delegate->live = false;
    m_manager.reset(); // stops the helpers before the delegate goes
}

ExtensionsBridge* ExtensionsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ExtensionsBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setLifecycle(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        bridge->setDispatcher(qmlEngine->singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher"));
        // The assistant's services: an extension is a remote caller like the assistant (consent, spend cap, no-cloud).
        QPointer<QQmlEngine> engine(qmlEngine);
        bridge->setServicesFactory([engine]() -> std::shared_ptr<command::CommandServices> {
            auto* agent = engine ? engine->singletonInstance<AgentController*>("Wabcut", "AgentController") : nullptr;
            return agent ? agent->commandServices() : nullptr;
        });
        // Not during start-up: the extensions start once the window is up.
        QTimer::singleShot(0, bridge, [bridge] { bridge->attach(); });
    }
    return bridge;
}

void ExtensionsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    detach();
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::selectionChanged, this, &ExtensionsBridge::publishSelection);
        connect(controller, &ProjectController::playheadChanged, this, &ExtensionsBridge::publishPlayhead);
        // The session is about to go: the manager holds it.
        connect(controller, &ProjectController::sessionClosing, this, &ExtensionsBridge::detach);
    }
}

void ExtensionsBridge::setLifecycle(ProjectLifecycle* lifecycle) {
    if (m_lifecycle == lifecycle)
        return;
    if (m_lifecycle)
        disconnect(m_lifecycle, nullptr, this, nullptr);
    m_lifecycle = lifecycle;
    if (lifecycle) {
        m_wasDirty = lifecycle->dirty();
        connect(lifecycle, &ProjectLifecycle::documentChanged, this, &ExtensionsBridge::onDocumentChanged);
    }
}

void ExtensionsBridge::setDispatcher(KeymapDispatcher* dispatcher) {
    if (m_dispatcher == dispatcher)
        return;
    if (m_dispatcher)
        disconnect(m_dispatcher, nullptr, this, nullptr);
    m_dispatcher = dispatcher;
    if (dispatcher) {
        // An extension command's key (or KeymapDispatcher.trigger) runs it.
        connect(dispatcher, &KeymapDispatcher::commandTriggered, this, &ExtensionsBridge::onCommandTriggered);
        connect(dispatcher, &KeymapDispatcher::keymapChanged, this, [this] {
            if (m_provider)
                m_provider->changed(); // the rows show the shortcuts
        });
    }
}

void ExtensionsBridge::setServicesFactory(std::function<std::shared_ptr<command::CommandServices>()> factory) {
    m_servicesFactory = std::move(factory);
    m_services.reset();
}

void ExtensionsBridge::setPaths(const sc::ExtensionPaths& paths) {
    const bool attached = m_attached;
    detach();
    m_timer.stop();
    m_delegate->live = false;
    m_manager.reset();
    m_delegate->live = true;
    m_delegate->logs.clear();
    m_paths = std::make_unique<sc::ExtensionPaths>(paths);
    m_prompts.clear();
    emit promptsChanged();
    if (attached)
        attach();
    else
        refresh();
}

void ExtensionsBridge::ensureManager() {
    if (m_manager)
        return;
    if (!m_paths) {
        // Like wabcut-cli: WABCUT_DATA_DIR moves <app data> (automation), else the app's data folder.
        const QString dataDir = qEnvironmentVariableIsEmpty("WABCUT_DATA_DIR")
                                    ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                    : qEnvironmentVariable("WABCUT_DATA_DIR");
        m_paths = std::make_unique<sc::ExtensionPaths>(sc::extensionPathsUnder(std::filesystem::path(dataDir.toStdU16String())));
    }
    m_manager = std::make_unique<sc::ExtensionManager>(*m_paths, QCoreApplication::applicationVersion().toStdString(),
                                                       *m_delegate);
}

sc::ExtensionManager* ExtensionsBridge::manager() {
    ensureManager();
    return m_manager.get();
}

void ExtensionsBridge::attach() {
    ensureManager();
    if (!m_controller) {
        refresh();
        return;
    }
    if (m_attached && m_attachedFile == m_controller->projectFile()) {
        refresh();
        return;
    }
    detach();
    if (!m_services)
        m_services = std::make_shared<LazyServices>(m_servicesFactory);
    m_attachedFile = m_controller->projectFile();
    m_manager->attach(m_controller->session(), ai::edit::editingCommands(), m_services, m_attachedFile);
    m_attached = true;
    refresh();
}

void ExtensionsBridge::detach() {
    if (!m_attached || !m_manager)
        return;
    m_attached = false;
    m_manager->detach();
    // Forms of stopped extensions close (promptClosed); drop any left.
    if (!m_prompts.isEmpty()) {
        m_prompts.clear();
        emit promptsChanged();
    }
    refresh();
}

void ExtensionsBridge::pump() {
    if (!m_manager)
        return;
    m_manager->pump();
    // A helper that became ready (or stopped) changes a status without a changed() from the manager.
    if (statusSignature() != m_signature)
        refresh();
    else
        schedule();
}

std::string ExtensionsBridge::statusSignature() const {
    std::string out;
    if (!m_manager)
        return out;
    for (const sc::ExtensionInfo& e : m_manager->list()) {
        out += e.id;
        out += ':';
        out += sc::toString(e.status);
        out += ':' + std::to_string(e.crashes) + ':' + std::to_string(e.commands.size()) + ':' +
               (e.lastStop ? std::string(sc::toString(e.lastStop->state)) : std::string()) + ';';
    }
    return out;
}

void ExtensionsBridge::schedule() {
    const auto next = m_manager ? m_manager->nextDeadline() : std::nullopt;
    if (!next) {
        m_timer.stop();
        return;
    }
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(*next - std::chrono::steady_clock::now());
    m_timer.start(static_cast<int>(std::clamp<long long>(wait.count() + 1, 0, 60'000)));
}

void ExtensionsBridge::refresh() {
    QVariantList rows;
    QVariantList palette;
    QList<KeymapDispatcher::ExtraCommand> extra;
    bool anyRunning = false;
    if (m_manager) {
        std::vector<sc::ExtensionInfo> list = m_manager->list();
        std::stable_sort(list.begin(), list.end(), [](const sc::ExtensionInfo& a, const sc::ExtensionInfo& b) {
            return a.scope == sc::ExtensionScope::User && b.scope == sc::ExtensionScope::Project;
        });
        std::map<std::string, QString> names;
        for (const sc::ExtensionInfo& e : list) {
            // An extension that does not load is listed by its folder's name (its id is "invalid:<folder>").
            const QString name = e.manifest ? Delegate::nameOf(e) : QString::fromStdU16String(e.folder.filename().u16string());
            // A project extension the user has not trusted (for this content): the manager reports it as needing review
            // until it was granted, then as needing trust; either way the row asks for trust (one dialog does both).
            bool needsTrust = e.status == sc::ExtensionStatus::NeedsTrust;
            if (!needsTrust && e.scope == sc::ExtensionScope::Project && e.status == sc::ExtensionStatus::NeedsReview) {
                const auto review = m_manager->review(e.id);
                needsTrust = review && review->needsTrust;
            }
            names[e.id] = name;
            const bool running = e.status == sc::ExtensionStatus::Running || e.status == sc::ExtensionStatus::Starting;
            anyRunning = anyRunning || running;
            QVariantList commands;
            for (const sc::RegisteredCommand& c : e.commands) {
                const QString paletteId = paletteIdOf(e.id, c.id);
                commands.append(QVariantMap{
                    {QStringLiteral("id"), QString::fromStdString(c.id)},
                    {QStringLiteral("title"), QString::fromStdString(c.title)},
                    {QStringLiteral("paletteId"), paletteId},
                    {QStringLiteral("shortcut"), m_dispatcher ? m_dispatcher->shortcutText(paletteId) : QString()}});
            }
            QString reason = QString::fromStdString(e.disabledReason);
            if (reason.isEmpty())
                reason = QString::fromStdString(e.loadError);
            if (reason.isEmpty() && e.status == sc::ExtensionStatus::Failed && e.lastStop)
                reason = QString::fromStdString(e.lastStop->reason);
            const bool crashed = e.lastStop && (e.lastStop->state == sc::HostState::Crashed ||
                                                e.lastStop->state == sc::HostState::Failed);
            rows.append(QVariantMap{
                {QStringLiteral("id"), QString::fromStdString(e.id)},
                {QStringLiteral("name"), name},
                {QStringLiteral("version"), e.manifest ? QString::fromStdString(sc::toString(e.manifest->version)) : QString()},
                {QStringLiteral("author"), e.manifest ? QString::fromStdString(e.manifest->author) : QString()},
                {QStringLiteral("licence"), e.manifest ? QString::fromStdString(e.manifest->licence) : QString()},
                {QStringLiteral("description"), e.manifest ? QString::fromStdString(e.manifest->description) : QString()},
                {QStringLiteral("scope"), text(sc::toString(e.scope))},
                {QStringLiteral("project"), e.scope == sc::ExtensionScope::Project},
                {QStringLiteral("folder"), pathText(e.folder)},
                {QStringLiteral("status"), text(sc::toString(e.status))},
                {QStringLiteral("badge"), badgeText(needsTrust ? sc::ExtensionStatus::NeedsTrust : e.status)},
                {QStringLiteral("badgeKind"), badgeKind(needsTrust ? sc::ExtensionStatus::NeedsTrust : e.status)},
                {QStringLiteral("needsTrust"), needsTrust},
                {QStringLiteral("enabled"), e.enabled},
                {QStringLiteral("running"), running},
                {QStringLiteral("needsReview"), e.needsReview},
                {QStringLiteral("granted"), grantedChips(e.granted)},
                {QStringLiteral("commands"), commands},
                {QStringLiteral("reason"), reason},
                {QStringLiteral("crashes"), e.crashes},
                {QStringLiteral("crashed"), crashed},
            });
        }
        for (const sc::PaletteCommand& c : m_manager->paletteCommands()) {
            const QString paletteId = QString::fromStdString(c.paletteId);
            palette.append(QVariantMap{{QStringLiteral("paletteId"), paletteId},
                                       {QStringLiteral("title"), QString::fromStdString(c.title)},
                                       {QStringLiteral("extensionId"), QString::fromStdString(c.extensionId)},
                                       {QStringLiteral("extensionName"), names[c.extensionId]},
                                       {QStringLiteral("commandId"), QString::fromStdString(c.commandId)}});
            extra.append(KeymapDispatcher::ExtraCommand{paletteId, QString::fromStdString(c.title)});
        }
    }
    m_anyRunning = anyRunning;
    m_signature = statusSignature();
    const bool paletteChanged = palette != m_palette;
    m_rows = std::move(rows);
    m_palette = std::move(palette);
    if (m_dispatcher)
        m_dispatcher->setExtraCommands(extra);
    if (paletteChanged && m_provider)
        m_provider->changed();
    emit extensionsChanged();
    schedule();
}

QString ExtensionsBridge::userFolder() const {
    if (m_paths)
        return pathText(m_paths->userExtensions);
    return QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                    QStringLiteral("/extensions"));
}

void ExtensionsBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

QString ExtensionsBridge::result(const QString& error) {
    setMessage(error);
    refresh();
    return error;
}

// ---- The permission flow -----------------------------------------------------------------------------------------

QVariantMap ExtensionsBridge::review(const QString& id) const {
    if (!m_manager)
        return failedReview(tr("No extension %1").arg(id));
    const auto r = m_manager->review(id.toStdString());
    return r ? reviewMap(*r) : failedReview(QString::fromStdString(r.error().message));
}

QVariantMap ExtensionsBridge::reviewFolder(const QUrl& folder) const {
    if (!m_manager)
        return failedReview(tr("Extensions are not available"));
    const auto r = m_manager->reviewFolder(localPath(folder));
    return r ? reviewMap(*r) : failedReview(QString::fromStdString(r.error().message));
}

QString ExtensionsBridge::install(const QUrl& folder, const QStringList& capabilities) {
    ensureManager();
    const auto chosen = capabilitiesOf(capabilities);
    if (!chosen)
        return result(tr("Unknown permission in %1").arg(capabilities.join(QStringLiteral(", "))));
    const auto id = m_manager->install(localPath(folder));
    if (!id)
        return result(QString::fromStdString(id.error().message));
    if (const auto enabled = m_manager->enable(*id, *chosen); !enabled)
        return result(QString::fromStdString(enabled.error().message));
    return result({});
}

QString ExtensionsBridge::enable(const QString& id, const QStringList& capabilities) {
    ensureManager();
    const auto chosen = capabilitiesOf(capabilities);
    if (!chosen)
        return result(tr("Unknown permission in %1").arg(capabilities.join(QStringLiteral(", "))));
    const auto enabled = m_manager->enable(id.toStdString(), *chosen);
    return result(enabled ? QString() : QString::fromStdString(enabled.error().message));
}

QString ExtensionsBridge::trustAndEnable(const QString& id, const QStringList& capabilities) {
    ensureManager();
    const auto chosen = capabilitiesOf(capabilities);
    if (!chosen)
        return result(tr("Unknown permission in %1").arg(capabilities.join(QStringLiteral(", "))));
    if (const auto enabled = m_manager->enable(id.toStdString(), *chosen); !enabled)
        return result(QString::fromStdString(enabled.error().message));
    const auto trusted = m_manager->trust(id.toStdString());
    return result(trusted ? QString() : QString::fromStdString(trusted.error().message));
}

QString ExtensionsBridge::disable(const QString& id) {
    ensureManager();
    m_manager->disable(id.toStdString());
    return result({});
}

QString ExtensionsBridge::revoke(const QString& id) {
    ensureManager();
    m_manager->revoke(id.toStdString());
    return result({});
}

QString ExtensionsBridge::distrust(const QString& id) {
    ensureManager();
    m_manager->distrust(id.toStdString());
    return result({});
}

QString ExtensionsBridge::uninstall(const QString& id) {
    ensureManager();
    const auto removed = m_manager->uninstall(id.toStdString());
    if (removed)
        m_delegate->logs.erase(id);
    return result(removed ? QString() : QString::fromStdString(removed.error().message));
}

void ExtensionsBridge::rescan() {
    ensureManager();
    m_manager->rescan();
    refresh();
}

QVariantList ExtensionsBridge::log(const QString& id) const {
    const auto it = m_delegate->logs.find(id);
    return it == m_delegate->logs.end() ? QVariantList{} : it->second;
}

// ---- Commands -----------------------------------------------------------------------------------------------------

QString ExtensionsBridge::run(const QString& paletteId) {
    ensureManager();
    if (!m_attached)
        attach();
    QString name;
    for (const QVariant& v : std::as_const(m_palette)) {
        const QVariantMap c = v.toMap();
        if (c.value(QStringLiteral("paletteId")).toString() == paletteId)
            name = c.value(QStringLiteral("extensionName")).toString();
    }
    if (name.isEmpty())
        //: A keyboard shortcut or palette row of an extension that is off, not installed or not reviewed
        return result(tr("%1 is not available: turn its extension on in Extensions").arg(paletteId));
    QPointer<ExtensionsBridge> self(this);
    const auto started = m_manager->run(paletteId.toStdString(), Json::object(), [self, name](const sc::CallResult& r) {
        if (!self || r.ok)
            return;
        QString why;
        if (r.errorKind == "timeLimit")
            why = tr("it took too long and was stopped");
        else if (r.errorKind == "cancelled")
            why = tr("it was cancelled");
        else if (r.errorKind == "stopped")
            why = tr("the extension stopped");
        else if (r.errorKind == "unknownCommand")
            why = tr("the extension no longer has this command");
        else
            why = QString::fromStdString(r.error);
        emit self->notified(name, QStringLiteral("error"), tr("Failed: %1").arg(why));
    });
    schedule();
    return result(started ? QString() : QString::fromStdString(started.error().message));
}

void ExtensionsBridge::onCommandTriggered(const QString& id) {
    if (!id.startsWith(QLatin1String("ext:")))
        return;
    if (const QString error = run(id); !error.isEmpty())
        emit notified(tr("Extensions"), QStringLiteral("error"), error);
}

std::shared_ptr<PaletteProvider> ExtensionsBridge::paletteProvider() {
    if (!m_provider)
        m_provider = std::make_shared<Provider>(this);
    return m_provider;
}

// ---- Forms --------------------------------------------------------------------------------------------------------

void ExtensionsBridge::addPrompt(const QString& extensionId, std::uint64_t prompt, const QVariantMap& form) {
    QString name = extensionId;
    for (const QVariant& v : std::as_const(m_rows)) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("id")).toString() == extensionId)
            name = row.value(QStringLiteral("name")).toString();
    }
    QVariantMap entry = form;
    entry.insert(QStringLiteral("key"), extensionId + QLatin1Char('#') + QString::number(prompt));
    entry.insert(QStringLiteral("extensionId"), extensionId);
    entry.insert(QStringLiteral("extensionName"), name);
    entry.insert(QStringLiteral("prompt"), QString::number(prompt));
    m_prompts.append(entry);
    emit promptsChanged();
}

void ExtensionsBridge::removePrompt(const QString& key) {
    const auto before = m_prompts.size();
    m_prompts.removeIf([&](const QVariant& v) { return v.toMap().value(QStringLiteral("key")).toString() == key; });
    if (m_prompts.size() != before)
        emit promptsChanged();
}

bool ExtensionsBridge::answerPrompt(const QString& key, const QVariantMap& values) {
    const qsizetype hash = key.lastIndexOf(QLatin1Char('#'));
    if (!m_manager || hash < 0)
        return false;
    bool ok = false;
    const std::uint64_t prompt = key.mid(hash + 1).toULongLong(&ok);
    if (!ok || !m_manager->answerPrompt(key.left(hash).toStdString(), prompt, toJson(values)))
        return false;
    removePrompt(key);
    schedule();
    return true;
}

void ExtensionsBridge::cancelPrompt(const QString& key) {
    const qsizetype hash = key.lastIndexOf(QLatin1Char('#'));
    if (m_manager && hash >= 0) {
        bool ok = false;
        const std::uint64_t prompt = key.mid(hash + 1).toULongLong(&ok);
        if (ok)
            m_manager->answerPrompt(key.left(hash).toStdString(), prompt, std::nullopt);
    }
    removePrompt(key);
    schedule();
}

void ExtensionsBridge::copyText(const QString& value) const {
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(value);
}

// ---- Editor events -------------------------------------------------------------------------------------------------

void ExtensionsBridge::onDocumentChanged() {
    const bool dirty = m_lifecycle && m_lifecycle->dirty();
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path{};
    if (m_attached && file != m_attachedFile) {
        // Another project (or "save as" into another folder): its own per-project extensions.
        detach();
        attach();
    }
    if (m_manager && m_anyRunning && m_wasDirty && !dirty && !file.empty())
        m_manager->publish(sc::Event{sc::EventKind::ProjectSaved,
                                     Json{{"file", QString::fromStdU16String(file.u16string()).toStdString()}}});
    m_wasDirty = dirty;
    schedule();
}

void ExtensionsBridge::publishSelection() {
    if (!m_manager || !m_anyRunning || !m_controller)
        return;
    Json clips = Json::array();
    for (const ClipId id : m_controller->selection())
        clips.push_back(id.value);
    m_manager->publish(sc::Event{sc::EventKind::SelectionChanged,
                                 Json{{"sequence", m_controller->currentSequenceId().value},
                                      {"clips", std::move(clips)},
                                      {"markers", Json::array()}}});
    schedule();
}

void ExtensionsBridge::publishPlayhead() {
    if (!m_manager || !m_anyRunning || !m_controller)
        return;
    const Ticks t = m_controller->playhead();
    m_manager->publish(sc::Event{sc::EventKind::PlayheadMoved,
                                 Json{{"sequence", m_controller->currentSequenceId().value},
                                      {"time", t.value},
                                      {"timecode", m_controller->formatTimecode(t).toStdString()}}});
    schedule();
}

// ---- AI-model commands of extensions (C.14 runLongCommand) ------------------------------------------------------

struct ExtensionsBridge::PendingLong {
    QString extension;
    sc::LongCommand job;
};

namespace {

/// Cancels through `cancelled`; reports the command's "... (NN%)" checkpoints as progress (as CommandJob does).
class LongControl final : public command::ExecutionControl {
public:
    LongControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        static const QRegularExpression percent(QStringLiteral("\\((\\d+)%\\)\\s*$"));
        QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        double progress = -1.0;
        if (const auto match = percent.match(text); match.hasMatch()) {
            progress = match.captured(1).toDouble() / 100.0;
            text = text.left(match.capturedStart()).trimmed();
        }
        m_report(text, progress);
        if (m_cancelled.load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }

private:
    const std::atomic<bool>& m_cancelled;
    std::function<void(QString, double)> m_report;
};

command::CommandResult refused(command::ErrorCode code, std::string message) {
    command::CommandResult result;
    result.error = command::CommandError{};
    result.error->code = code;
    result.error->message = std::move(message);
    return result;
}

} // namespace

bool ExtensionsBridge::startLongCommand(const QString& extension, sc::LongCommand job) {
    if (!m_controller || !job.finish)
        return false;
    auto pending = std::make_unique<PendingLong>();
    pending->extension = extension;
    pending->job = std::move(job);
    m_longQueue.push_back(std::move(pending));
    if (m_longJob->running()) {
        m_longState[QStringLiteral("queued")] = static_cast<int>(m_longQueue.size());
        emit longCommandChanged();
    } else {
        runNextLong();
    }
    return true;
}

void ExtensionsBridge::cancelLongCommand() {
    if (m_longJob->running())
        m_longJob->cancel();
}

void ExtensionsBridge::setLongState(const QString& extension, const QString& text, double progress) {
    m_longState = QVariantMap{{QStringLiteral("running"), !extension.isEmpty()},
                              {QStringLiteral("extension"), extension},
                              {QStringLiteral("text"), text},
                              {QStringLiteral("progress"), progress},
                              {QStringLiteral("queued"), static_cast<int>(m_longQueue.size())}};
    emit longCommandChanged();
}

void ExtensionsBridge::runNextLong() {
    if (m_longJob->running() || m_longQueue.empty())
        return;
    std::unique_ptr<PendingLong> next = std::move(m_longQueue.front());
    m_longQueue.pop_front();
    if (!m_controller) {
        next->job.finish(command::toJson(refused(command::ErrorCode::InvalidRequest, "no project is open")));
        runNextLong();
        return;
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const command::CommandRegistry* registry = m_longRegistry ? m_longRegistry : &ai::edit::editingCommands();
    const QString extension = next->extension;
    setLongState(extension, tr("Starting"), -1.0);

    QPointer<ExtensionsBridge> self(this);
    QPointer<ProjectController> live(m_controller);
    // Progress reaches the GUI thread queued.
    auto report = [self, extension](QString text, double progress) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, extension, text = std::move(text), progress] {
                if (self && self->m_longJob->running())
                    self->setLongState(extension, text, progress);
            },
            Qt::QueuedConnection);
    };
    auto finish = std::move(next->job.finish);
    const Json document = std::move(next->job.document);
    const command::EnginePolicy policy = std::move(next->job.policy);
    const bool started = m_longJob->start(
        this, false,
        [base, registry, document, policy, report, self, live, finish](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
            // On the worker: a copy of the project, the extension's own policy (never the app's).
            EditSession scratch(*base);
            command::CommandEngine engine(scratch, *registry, policy);
            LongControl control(cancelled, report);
            command::CommandResult result = engine.executeJson(document, &control);
            const ProjectSnapshot after = scratch.snapshot();
            const std::string label = scratch.undoLabel().value_or(std::string());
            return [self, live, base, after, label, finish, result = std::move(result)]() mutable {
                // Back on the GUI thread: the copy's result is one undo step when nothing changed meanwhile. A
                // project that changed is not edited behind the user's edits, and the command is not run again here
                // (that would hold the GUI thread): the extension is told to run it again.
                if (result.ok() && after.get() != base.get()) {
                    if (!live) {
                        result = refused(command::ErrorCode::InvalidRequest, "the project was closed");
                    } else if (live->snapshot().get() != base.get()) {
                        result = refused(command::ErrorCode::StaleRevision,
                                         "the project changed while the command ran; run it again");
                    } else if (auto committed = live->session().commit(label, *after); !committed) {
                        result = refused(command::ErrorCode::EditRejected, committed.error().message);
                    }
                }
                finish(command::toJson(result));
                if (self) {
                    self->setLongState(QString(), QString(), -1.0);
                    self->runNextLong();
                    self->schedule(); // the extension's reply goes out with the next pump
                }
            };
        },
        // A command that throws ends as an Internal error for the extension; the queue goes on.
        [self, finish](const QString& message) -> BridgeJob::Finish {
            return [self, finish, message] {
                finish(command::toJson(refused(command::ErrorCode::Internal, message.toStdString())));
                if (self) {
                    self->setLongState(QString(), QString(), -1.0);
                    self->runNextLong();
                    self->schedule();
                }
            };
        });
    if (!started) {
        finish(command::toJson(refused(command::ErrorCode::InvalidRequest, "another command job is running")));
        setLongState(QString(), QString(), -1.0);
    }
}

} // namespace wabcut::app
