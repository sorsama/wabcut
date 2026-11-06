#include "CloudBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "CloudBridgeInternal.h"
#include "HistoryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/Http.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/cloud/HttpStores.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

namespace wabcut::app {

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace cloud_detail;

// ---- the worker ----

CloudWorker::~CloudWorker() { stop(); }

void CloudWorker::stop() {
    m_stop.request_stop();
    if (m_thread.joinable())
        m_thread.join();
}

bool CloudWorker::start(QObject* context, bool synchronous, Work work) {
    if (m_running)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_stop = std::stop_source();
    m_running = true;
    if (synchronous) {
        Finish finish;
        try {
            finish = work(m_stop.get_token());
        } catch (...) {
        }
        m_running = false;
        if (finish)
            finish();
        return true;
    }
    QPointer<QObject> guard(context);
    m_thread = std::thread([this, guard, work = std::move(work), token = m_stop.get_token()]() mutable {
        Finish finish;
        try {
            finish = work(token);
        } catch (...) {
            finish = {};
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, finish = std::move(finish)] {
                if (!guard)
                    return;
                m_running = false;
                if (finish)
                    finish();
            },
            Qt::QueuedConnection);
    });
    return true;
}

// ---- the bridge ----

CloudBridge::CloudBridge(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {
    m_random = cloud::systemRandom();
    m_clock = cloud::systemClock();
    d->who = cloud::currentIdentity(m_random);
    m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
    loadLocations();
    rebuildLocationRows();
    m_beat.setInterval(static_cast<int>(cloud::kLockRefreshSeconds * 1000));
    connect(&m_beat, &QTimer::timeout, this, &CloudBridge::heartbeat);
    m_beat.start();
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { releaseLockNow(); });
}

CloudBridge::~CloudBridge() {
    m_worker.stop();
    releaseLockNow();
}

CloudBridge* CloudBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new CloudBridge;
    if (qmlEngine) {
        bridge->setControllers(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"),
                               qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"),
                               qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController"));
        bridge->d->history = qmlEngine->singletonInstance<HistoryBridge*>("Wabcut", "History");
    }
    return bridge;
}

void CloudBridge::setControllers(ProjectController* controller, ProjectLifecycle* lifecycle, AiController* ai) {
    m_controller = controller;
    m_lifecycle = lifecycle;
    m_ai = ai;
    if (lifecycle)
        connect(lifecycle, &ProjectLifecycle::documentChanged, this, &CloudBridge::onDocumentChanged);
    if (controller) {
        // undo and redo of the sync step turn the "you undid the last sync" notice on and off
        connect(controller, &ProjectController::historyChanged, this, [this] {
            if (d->hasLink && d->guard.active && d->guardShown == syncStepIntact())
                rebuildCloudRow();
        });
    }
    if (ai) {
        connect(ai, &AiController::privacyChanged, this, [this] {
            emit blockedChanged();
            rebuildCloudRow();
        });
        connect(ai, &AiController::shuttingDown, this, [this] { m_worker.stop(); });
    }
    onDocumentChanged();
}

void CloudBridge::setHeartbeatEnabled(bool enabled) {
    if (enabled)
        m_beat.start();
    else
        m_beat.stop();
}

// ---- small things ----

fs::path CloudBridge::projectFile() const { return m_controller ? m_controller->projectFile() : fs::path(); }

bool CloudBridge::projectSaved() const {
    const fs::path file = projectFile();
    std::error_code ec;
    return !file.empty() && fs::is_regular_file(file, ec);
}

QString CloudBridge::projectName() const {
    const fs::path file = projectFile();
    return file.empty() ? tr("Untitled") : qs(pathToUtf8(file.stem()));
}

QString CloudBridge::blockedReason() const {
    if (m_ai && m_ai->offlineMode())
        return tr("Offline mode is on, so nothing is sent to the internet or to a synced folder. Turn it off under Preferences > AI.");
    if (m_ai && m_ai->projectLock())
        return tr("This project is locked to no-cloud work, so nothing leaves this computer. Unlock it under Project > Privacy.");
    return {};
}

fs::path CloudBridge::cacheFolder() const {
    if (!m_cacheOverride.empty())
        return m_cacheOverride;
    return pathFromUtf8(QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toStdString()) / "cloud";
}

cloud::Identity CloudBridge::identity() const { return d->who; }

void CloudBridge::setMessage(const QString& text, bool error) {
    m_message = text;
    m_messageIsError = error;
    emit messageChanged();
}

void CloudBridge::setPhase(const QString& phase, double progress) {
    if (phase == m_phase && qFuzzyCompare(progress + 1.0, m_progress + 1.0))
        return;
    m_phase = phase;
    m_progress = progress;
    emit jobChanged();
}

bool CloudBridge::start(const QString& phase, CloudWorker::Work work) {
    if (m_worker.running()) {
        setMessage(tr("Something is still running. Wait for it to finish or cancel it."), true);
        return false;
    }
    m_phase = phase;
    m_progress = -1.0;
    const bool started = m_worker.start(this, m_synchronous, [this, work = std::move(work)](std::stop_token stop) {
        CloudWorker::Finish finish = work(stop);
        return CloudWorker::Finish([this, finish = std::move(finish)] {
            m_phase.clear();
            m_progress = -1.0;
            if (finish)
                finish();
            emit jobChanged();
        });
    });
    emit jobChanged();
    return started;
}

void CloudBridge::cancelJob() { m_worker.cancel(); }

cloud::Progress CloudBridge::progressSink(const QString& phase) {
    auto last = std::make_shared<std::atomic<std::int64_t>>(0);
    QPointer<CloudBridge> self(this);
    return [self, phase, last](std::uint64_t done, std::uint64_t total) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - last->load() < 150 && done < total)
            return true;
        last->store(now);
        const double fraction = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
        if (self) {
            QMetaObject::invokeMethod(self.data(), [self, phase, fraction] {
                if (self && self->m_worker.running())
                    self->setPhase(phase, fraction);
            }, Qt::QueuedConnection);
        }
        return true;
    };
}

// ---- locations ----

void CloudBridge::loadLocations() {
    d->locations.clear();
    const QByteArray raw = QSettings().value(QStringLiteral("cloud/locations")).toString().toUtf8();
    if (raw.isEmpty())
        return;
    const json doc = json::parse(raw.constData(), nullptr, false);
    if (!doc.is_array())
        return;
    for (const json& j : doc) {
        if (!j.is_object() || d->locations.size() >= 50)
            continue;
        Location l;
        const auto str = [&](const char* key) {
            const auto it = j.find(key);
            return it != j.end() && it->is_string() ? qs(it->get<std::string>()) : QString();
        };
        l.id = str("id");
        l.name = str("name").left(80);
        l.kind = str("kind");
        l.purpose = str("purpose");
        l.folder = str("folder");
        l.url = str("url");
        l.user = str("user");
        l.endpoint = str("endpoint");
        l.region = str("region");
        l.bucket = str("bucket");
        l.prefix = str("prefix");
        l.pathStyle = j.value("pathStyle", true);
        l.allowInsecure = j.value("allowInsecure", false);
        if (j.contains("mediaFolders") && j["mediaFolders"].is_array()) {
            for (const json& f : j["mediaFolders"]) {
                if (f.is_string() && l.mediaFolders.size() < 16)
                    l.mediaFolders << qs(f.get<std::string>());
            }
        }
        const bool kindOk = l.kind == QLatin1String("folder") || l.kind == QLatin1String("webdav") || l.kind == QLatin1String("s3");
        const bool purposeOk = l.purpose == QLatin1String("projects") || l.purpose == QLatin1String("reviews");
        if (cloud::validSegment(l.id.toStdString()) && kindOk && purposeOk && !l.name.isEmpty())
            d->locations.push_back(std::move(l));
    }
}

void CloudBridge::saveLocations() const {
    json list = json::array();
    for (const Location& l : d->locations) {
        list.push_back(json{{"id", s(l.id)}, {"name", s(l.name)}, {"kind", s(l.kind)}, {"purpose", s(l.purpose)}, {"folder", s(l.folder)},
                            {"url", s(l.url)}, {"user", s(l.user)}, {"endpoint", s(l.endpoint)}, {"region", s(l.region)},
                            {"bucket", s(l.bucket)}, {"prefix", s(l.prefix)}, {"pathStyle", l.pathStyle},
                            {"allowInsecure", l.allowInsecure}, {"mediaFolders", [&] {
                                json folders = json::array();
                                for (const QString& f : l.mediaFolders)
                                    folders.push_back(s(f));
                                return folders;
                            }()}});
    }
    QSettings().setValue(QStringLiteral("cloud/locations"), qs(list.dump()));
}

CloudBridge::Location* CloudBridge::findLocation(const QString& id) {
    for (Location& l : d->locations) {
        if (l.id == id)
            return &l;
    }
    return nullptr;
}

const CloudBridge::Location* CloudBridge::findLocation(const QString& id) const {
    for (const Location& l : d->locations) {
        if (l.id == id)
            return &l;
    }
    return nullptr;
}

QString CloudBridge::kindName(const QString& kind) const {
    if (kind == QLatin1String("webdav"))
        return tr("WebDAV");
    if (kind == QLatin1String("s3"))
        return tr("S3-compatible bucket");
    return tr("Synced folder");
}

void CloudBridge::rebuildLocationRows() {
    QVariantList rows;
    for (const Location& l : d->locations) {
        QString summary;
        if (l.kind == QLatin1String("folder"))
            summary = QDir::toNativeSeparators(l.folder);
        else if (l.kind == QLatin1String("webdav"))
            summary = l.url;
        else
            summary = l.bucket + QStringLiteral(" · ") + l.endpoint + (l.prefix.isEmpty() ? QString() : QStringLiteral(" · ") + l.prefix);
        const bool needsSecret = l.kind != QLatin1String("folder");
        rows.append(QVariantMap{{QStringLiteral("id"), l.id},
                                {QStringLiteral("name"), l.name},
                                {QStringLiteral("kind"), l.kind},
                                {QStringLiteral("kindName"), kindName(l.kind)},
                                {QStringLiteral("purpose"), l.purpose},
                                {QStringLiteral("purposeName"), l.purpose == QLatin1String("reviews") ? tr("Review links") : tr("Cloud projects")},
                                {QStringLiteral("summary"), summary},
                                {QStringLiteral("needsSecret"), needsSecret},
                                {QStringLiteral("hasSecret"), !needsSecret || d->secretKnown.contains(l.id)},
                                {QStringLiteral("folder"), l.folder},
                                {QStringLiteral("url"), l.url},
                                {QStringLiteral("user"), l.user},
                                {QStringLiteral("endpoint"), l.endpoint},
                                {QStringLiteral("region"), l.region},
                                {QStringLiteral("bucket"), l.bucket},
                                {QStringLiteral("prefix"), l.prefix},
                                {QStringLiteral("pathStyle"), l.pathStyle},
                                {QStringLiteral("allowInsecure"), l.allowInsecure},
                                {QStringLiteral("mediaFolders"), l.mediaFolders},
                                {QStringLiteral("check"), l.check},
                                {QStringLiteral("checkText"), l.checkText}});
    }
    m_locationRows = rows;
    emit locationsChanged();
}

ai::SecretStore& CloudBridge::secretStore() {
    if (m_testSecrets)
        return *m_testSecrets;
    return m_ai->platform().keys().store();
}

ai::IHttpTransport& CloudBridge::transport() {
    if (m_testTransport)
        return *m_testTransport;
    return m_ai->transport();
}

std::optional<CloudBridge::Secrets> CloudBridge::readSecrets(const QString& id) {
    if (const auto it = d->secrets.find(id); it != d->secrets.end())
        return it->second;
    auto read = secretStore().read("cloud:" + s(id));
    if (!read || !*read)
        return std::nullopt;
    const json doc = json::parse((*read)->reveal(), nullptr, false);
    if (!doc.is_object())
        return std::nullopt;
    Secrets secrets;
    secrets.a = doc.value("a", std::string());
    secrets.b = doc.value("b", std::string());
    ai::Redactor::global().addSecret(secrets.a);
    ai::Redactor::global().addSecret(secrets.b);
    d->secrets[id] = secrets;
    d->secretKnown.insert(id);
    return secrets;
}

bool CloudBridge::writeSecrets(const QString& id, const Secrets& secrets) {
    const json doc{{"a", secrets.a}, {"b", secrets.b}};
    if (!secretStore().write("cloud:" + s(id), ai::Secret(doc.dump())))
        return false;
    ai::Redactor::global().addSecret(secrets.a);
    ai::Redactor::global().addSecret(secrets.b);
    d->secrets[id] = secrets;
    d->secretKnown.insert(id);
    return true;
}

std::expected<std::unique_ptr<cloud::RemoteStore>, QString> CloudBridge::openStore(const Location& l) {
    if (l.kind == QLatin1String("folder")) {
        if (l.folder.isEmpty())
            return std::unexpected(tr("Choose the folder."));
        return std::unique_ptr<cloud::RemoteStore>(std::make_unique<cloud::FolderStore>(pathFromUtf8(s(l.folder))));
    }
    const auto secrets = readSecrets(l.id);
    if (!secrets)
        return std::unexpected(l.kind == QLatin1String("webdav") ? tr("The password for \"%1\" is missing. Enter it again.").arg(l.name)
                                                                  : tr("The keys for \"%1\" are missing. Enter them again.").arg(l.name));
    if (!m_testTransport && !m_ai)
        return std::unexpected(tr("The network is not available."));
    if (l.kind == QLatin1String("webdav")) {
        cloud::WebDavConfig config;
        config.url = s(l.url);
        config.user = s(l.user);
        config.allowInsecure = l.allowInsecure;
        auto store = std::make_unique<cloud::WebDavStore>(transport(), config, ai::Secret(secrets->a));
        if (!store->problem().empty())
            return std::unexpected(qs(store->problem()));
        return std::unique_ptr<cloud::RemoteStore>(std::move(store));
    }
    cloud::S3Config config;
    config.endpoint = s(l.endpoint);
    config.region = s(l.region);
    config.bucket = s(l.bucket);
    config.prefix = s(l.prefix);
    config.pathStyle = l.pathStyle;
    config.allowInsecure = l.allowInsecure;
    auto store = std::make_unique<cloud::S3Store>(transport(), config, ai::Secret(secrets->a), ai::Secret(secrets->b), m_clock);
    if (!store->problem().empty())
        return std::unexpected(qs(store->problem()));
    return std::unique_ptr<cloud::RemoteStore>(std::move(store));
}

QVariantMap CloudBridge::saveLocation(const QVariantMap& spec) {
    const auto fail = [](const QString& error) { return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}}; };
    const QString existingId = spec.value(QStringLiteral("id")).toString();
    Location* existing = existingId.isEmpty() ? nullptr : findLocation(existingId);
    if (!existingId.isEmpty() && !existing)
        return fail(tr("That storage location no longer exists."));
    Location l = existing ? *existing : Location{};
    const auto text = [&](const char* key) { return spec.value(qs(key)).toString().trimmed(); };
    l.name = text("name").left(80);
    l.kind = text("kind");
    l.purpose = text("purpose");
    if (l.name.isEmpty())
        return fail(tr("Give the location a name."));
    if (l.kind != QLatin1String("folder") && l.kind != QLatin1String("webdav") && l.kind != QLatin1String("s3"))
        return fail(tr("Choose what kind of storage it is."));
    if (l.purpose != QLatin1String("projects") && l.purpose != QLatin1String("reviews"))
        return fail(tr("Choose what it is for: cloud projects or review links."));
    if (existing && (existing->kind != l.kind || existing->purpose != l.purpose) && (d->hasLink && d->link.location == existing->id))
        return fail(tr("The open project uses this location. Unlink the project first to change what it is for."));
    for (const Location& other : d->locations) {
        if (other.id != l.id && other.name.compare(l.name, Qt::CaseInsensitive) == 0)
            return fail(tr("Another location already has that name."));
    }
    Secrets secrets;
    const bool newLocation = !existing;
    if (l.kind == QLatin1String("folder")) {
        l.folder = text("folder");
        const QFileInfo info(l.folder);
        if (l.folder.isEmpty() || !info.isDir())
            return fail(tr("Choose a folder that exists. For OneDrive, Dropbox or Google Drive pick a folder inside the synced folder."));
        if (info.isRoot())
            return fail(tr("Choose a folder, not a whole drive."));
        l.folder = QDir::cleanPath(info.absoluteFilePath());
    } else if (l.kind == QLatin1String("webdav")) {
        l.url = text("url");
        l.user = text("user");
        l.allowInsecure = spec.value(QStringLiteral("allowInsecure")).toBool();
        if (const std::string problem = cloud::endpointProblem(s(l.url), l.allowInsecure); !problem.empty())
            return fail(qs(problem));
        if (l.user.isEmpty())
            return fail(tr("Enter the user name."));
        secrets.a = s(spec.value(QStringLiteral("secretA")).toString());
        if (newLocation && secrets.a.empty())
            return fail(tr("Enter the password."));
    } else {
        l.endpoint = text("endpoint");
        l.region = text("region");
        l.bucket = text("bucket");
        l.prefix = text("prefix");
        l.pathStyle = !spec.contains(QStringLiteral("pathStyle")) || spec.value(QStringLiteral("pathStyle")).toBool();
        l.allowInsecure = spec.value(QStringLiteral("allowInsecure")).toBool();
        if (const std::string problem = cloud::endpointProblem(s(l.endpoint), l.allowInsecure); !problem.empty())
            return fail(qs(problem));
        if (l.bucket.isEmpty() || l.bucket.contains(QLatin1Char('/')))
            return fail(tr("Enter the bucket name."));
        if (l.region.isEmpty())
            return fail(tr("Enter the region (use us-east-1 or auto when the service does not care)."));
        secrets.a = s(spec.value(QStringLiteral("secretA")).toString());
        secrets.b = s(spec.value(QStringLiteral("secretB")).toString());
        if (newLocation && (secrets.a.empty() || secrets.b.empty()))
            return fail(tr("Enter both the access key and the secret key."));
        // a prefix that cannot be used is reported now (the store checks the same rule)
        if (!l.prefix.isEmpty()) {
            std::string folder = s(l.prefix);
            std::size_t start = 0;
            while (start <= folder.size()) {
                const std::size_t slash = folder.find('/', start);
                const std::string part = folder.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (!part.empty() && !cloud::validSegment(part))
                    return fail(tr("The folder name in the bucket may only use letters, digits, '.', '_' and '-'."));
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }
    }
    if (newLocation) {
        l.id = qs(cloud::newToken(m_random, 8));
    } else if (!secrets.a.empty() || !secrets.b.empty()) {
        const auto old = readSecrets(l.id);
        if (secrets.a.empty() && old)
            secrets.a = old->a;
        if (secrets.b.empty() && old)
            secrets.b = old->b;
    }
    if (l.kind != QLatin1String("folder") && (!secrets.a.empty() || !secrets.b.empty() || newLocation)) {
        if (!writeSecrets(l.id, secrets))
            return fail(tr("The password or keys could not be saved in the system's key store."));
    }
    l.check.clear();
    l.checkText.clear();
    if (existing)
        *existing = l;
    else
        d->locations.push_back(l);
    saveLocations();
    rebuildLocationRows();
    rebuildCloudRow();
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("id"), l.id}, {QStringLiteral("error"), QString()}};
}

bool CloudBridge::removeLocation(const QString& id) {
    const Location* l = findLocation(id);
    if (!l)
        return false;
    if (d->hasLink && d->link.location == id) {
        setMessage(tr("The open project is stored on this location. Unlink the project first."), true);
        return false;
    }
    for (const cloud::ReviewLinkRecord& r : d->links) {
        if (r.location == id) {
            setMessage(tr("Review links of the open project were published here. Revoke them first."), true);
            return false;
        }
    }
    if (l->kind != QLatin1String("folder"))
        (void)secretStore().remove("cloud:" + s(id));
    d->secrets.erase(id);
    d->secretKnown.erase(id);
    d->locations.erase(std::remove_if(d->locations.begin(), d->locations.end(), [&](const Location& x) { return x.id == id; }), d->locations.end());
    saveLocations();
    rebuildLocationRows();
    rebuildCloudRow();
    return true;
}

void CloudBridge::setMediaFolders(const QString& id, const QStringList& folders) {
    Location* l = findLocation(id);
    if (!l)
        return;
    l->mediaFolders.clear();
    for (const QString& f : folders) {
        const QString clean = QDir::cleanPath(QUrl(f).isLocalFile() ? QUrl(f).toLocalFile() : f);
        if (!clean.isEmpty() && l->mediaFolders.size() < 16 && !l->mediaFolders.contains(clean))
            l->mediaFolders << clean;
    }
    saveLocations();
    rebuildLocationRows();
}

void CloudBridge::testLocation(const QString& id) {
    Location* l = findLocation(id);
    if (!l)
        return;
    auto opened = openStore(*l);
    if (!opened) {
        l->check = QStringLiteral("problem");
        l->checkText = opened.error();
        rebuildLocationRows();
        return;
    }
    const bool needsSwap = l->purpose == QLatin1String("projects");
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const cloud::RandomBytes random = m_random;
    l->check.clear();
    l->checkText = tr("Checking…");
    rebuildLocationRows();
    const bool started = start(tr("Checking \"%1\"").arg(l->name), [this, id, store, random, needsSwap](std::stop_token stop) -> CloudWorker::Finish {
        const cloud::StoreCheck check = cloud::checkStore(*store, random, stop);
        QString state = QStringLiteral("ok");
        QString text;
        if (!check.reachable) {
            state = QStringLiteral("problem");
            text = check.problems.empty() ? tr("The storage cannot be reached.") : qs(check.problems.front());
        } else if (needsSwap && !check.conditionalWrites) {
            state = QStringLiteral("problem");
            QStringList lines;
            for (const std::string& p : check.problems)
                lines << qs(p);
            text = tr("This storage cannot keep two editors from overwriting each other, so it cannot hold cloud projects. %1").arg(lines.join(QLatin1Char(' ')));
        } else {
            text = needsSwap ? tr("Works. It can hold cloud projects.") : tr("Works. Review links can be published here.");
        }
        return [this, id, state, text] {
            if (Location* loc = findLocation(id)) {
                loc->check = state;
                loc->checkText = text;
                rebuildLocationRows();
            }
        };
    });
    if (!started) {
        l->checkText.clear();
        rebuildLocationRows();
    }
}

// ---- the open project and its cloud copy ----

void CloudBridge::onDocumentChanged() {
    const fs::path file = projectFile();
    const bool dirty = m_lifecycle && m_lifecycle->dirty();
    if (file != d->linkedFile) {
        releaseLockNow();
        d->linkedFile = file;
        d->hasLink = false;
        d->link = {};
        d->guard = {};
        d->conflicts.clear();
        m_conflictRows.clear();
        m_revisionRows.clear();
        m_unmergedRows.clear();
        d->links.clear();
        std::error_code ec;
        if (!file.empty() && fs::is_regular_file(file, ec)) {
            if (auto link = cloud::readCloudLink(file)) {
                d->link = *link;
                d->hasLink = true;
            }
            d->links = cloud::readReviewLinks(file);
        }
        d->lockKind = LockKind::None;
        d->lockText.clear();
        rebuildCloudRow();
        rebuildLinkRows();
        emit historyChanged();
        emit projectChanged();
        if (d->hasLink)
            acquireLockAsync();
    } else {
        emit projectChanged();
        rebuildCloudRow();
        if (d->hasLink && d->link.autoSync && d->wasDirty && !dirty && !m_worker.running() && blockedReason().isEmpty() && !m_synchronous)
            syncNow(false);
    }
    d->wasDirty = dirty;
}

void CloudBridge::refreshCloud() {
    d->linkedFile.clear(); // read the link and the review links of the project again
    onDocumentChanged();
    rebuildCloudRow();
}

void CloudBridge::rebuildCloudRow() {
    QVariantMap row{{QStringLiteral("linked"), d->hasLink}};
    if (d->hasLink) {
        const Location* l = findLocation(qs(d->link.location));
        QString hint;
        const QString blocked = blockedReason();
        if (!l)
            hint = tr("The storage location of this project was removed. Add it again.");
        else if (d->lockKind == LockKind::Other)
            hint = tr("%1 is editing this project. You can read it here; saving and sending are off until they are done.").arg(d->lockText);
        else if (!blocked.isEmpty())
            hint = blocked;
        const bool guardFails = d->guard.active && !syncStepIntact();
        d->guardShown = guardFails;
        row.insert(QStringLiteral("name"), qs(d->link.name));
        row.insert(QStringLiteral("projectId"), qs(d->link.projectId));
        row.insert(QStringLiteral("locationId"), qs(d->link.location));
        row.insert(QStringLiteral("locationName"), l ? l->name : tr("(removed)"));
        row.insert(QStringLiteral("revision"), static_cast<int>(d->link.baseRevision));
        row.insert(QStringLiteral("syncedText"), d->link.syncedAt.empty() ? tr("never") : whenText(d->link.syncedAt));
        row.insert(QStringLiteral("lock"), d->lockKind == LockKind::Mine ? QStringLiteral("mine")
                                           : d->lockKind == LockKind::Other ? QStringLiteral("other")
                                           : d->lockKind == LockKind::Unknown ? QStringLiteral("unknown")
                                                                               : QStringLiteral("none"));
        row.insert(QStringLiteral("lockText"), d->lockText);
        row.insert(QStringLiteral("readOnly"), d->lockKind == LockKind::Other);
        row.insert(QStringLiteral("autoSync"), d->link.autoSync);
        row.insert(QStringLiteral("canSync"), l != nullptr && !m_worker.running());
        row.insert(QStringLiteral("hint"), hint);
        row.insert(QStringLiteral("revertGuard"), guardFails);
        row.insert(QStringLiteral("conflictCount"), static_cast<int>(m_conflictRows.size()));
    }
    m_cloudRow = row;
    emit cloudChanged();
    updateReadOnly();
}

void CloudBridge::updateReadOnly() {
    if (!m_lifecycle)
        return;
    m_lifecycle->setCloudReadOnly(d->hasLink && d->lockKind == LockKind::Other, d->lockText);
}

void CloudBridge::releaseLockNow() {
    if (d->lock && d->lock->held()) {
        // Given up on a thread of its own: leaving the program must not wait for a storage that does not answer.
        std::shared_ptr<cloud::CloudLock> lock(std::move(d->lock));
        std::shared_ptr<cloud::RemoteStore> store(std::move(d->lockStore));
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> finished = done->get_future();
        std::thread([lock, store, done] {
            (void)lock->release();
            done->set_value();
        }).detach();
        finished.wait_for(std::chrono::seconds(3));
    }
    d->lock.reset();
    d->lockStore.reset();
    if (m_lifecycle && d->lockKind == LockKind::Other)
        m_lifecycle->setCloudReadOnly(false, {});
}

void CloudBridge::acquireLockAsync() {
    if (!d->hasLink)
        return;
    const Location* l = findLocation(qs(d->link.location));
    if (!l)
        return;
    auto opened = openStore(*l);
    if (!opened) {
        d->lockKind = LockKind::Unknown;
        d->lockText = opened.error();
        rebuildCloudRow();
        return;
    }
    d->lockStore = std::move(*opened);
    d->lock = std::make_unique<cloud::CloudLock>(*d->lockStore, d->link.projectId, identity(), QCoreApplication::applicationVersion().toStdString(), m_clock);
    cloud::CloudLock* lock = d->lock.get();
    start(tr("Taking the editing lock"), [this, lock](std::stop_token stop) -> CloudWorker::Finish {
        auto outcome = lock->acquire(stop);
        return [this, outcome = std::move(outcome)] {
            if (!d->hasLink)
                return;
            if (!outcome) {
                d->lockKind = LockKind::Unknown;
                d->lockText = qs(outcome.error().message);
                rebuildCloudRow();
                return;
            }
            switch (outcome->kind) {
            case cloud::LockOutcome::Kind::Acquired:
                d->lockKind = LockKind::Mine;
                d->lockText = tr("You are editing");
                break;
            case cloud::LockOutcome::Kind::TookOver:
                d->lockKind = LockKind::Mine;
                d->lockText = tr("You are editing");
                setMessage(qs(outcome->warning));
                break;
            case cloud::LockOutcome::Kind::HeldByOther:
                d->lockKind = LockKind::Other;
                d->lockText = tr("%1 on %2, since %3").arg(qs(outcome->other.user), qs(outcome->other.host), whenText(outcome->other.since));
                break;
            }
            rebuildCloudRow();
        };
    });
}

void CloudBridge::takeLock() {
    if (!d->hasLink)
        return;
    releaseLockNow();
    acquireLockAsync();
}

void CloudBridge::heartbeat() {
    if (!d->hasLink || !d->lock || !d->lock->held() || m_worker.running())
        return;
    cloud::CloudLock* lock = d->lock.get();
    start(tr("Renewing the editing lock"), [this, lock](std::stop_token stop) -> CloudWorker::Finish {
        auto renewed = lock->refresh(stop);
        return [this, renewed = std::move(renewed)] {
            if (renewed || !d->hasLink)
                return;
            if (renewed.error().kind == cloud::StoreFailure::PreconditionFailed) {
                d->lockKind = LockKind::Other;
                d->lockText = tr("another editor");
                setMessage(tr("Another editor took over this project's editing lock. Your project is read-only here now; sync to merge your changes."), true);
                rebuildCloudRow();
            }
            // a network error is not a lost lock: the next minute tries again
        };
    });
}

QVariantMap CloudBridge::uploadPreview(const QString& locationId) {
    QVariantMap out{{QStringLiteral("ok"), false}};
    const Location* l = findLocation(locationId);
    if (!l) {
        out.insert(QStringLiteral("error"), tr("Choose where to store it."));
        return out;
    }
    if (l->purpose != QLatin1String("projects")) {
        out.insert(QStringLiteral("error"), tr("This location is for review links. Choose one for cloud projects."));
        return out;
    }
    if (!m_controller || !projectSaved()) {
        out.insert(QStringLiteral("error"), tr("Save the project first."));
        return out;
    }
    const auto text = io::toJson(*m_controller->snapshot());
    if (!text) {
        out.insert(QStringLiteral("error"), qs(text.error().message));
        return out;
    }
    QStringList notes;
    notes << tr("The project file goes up, with the names, notes, transcripts and the paths of its media. The media files themselves stay where they are.");
    if (l->kind == QLatin1String("folder"))
        notes << tr("A sync service uploads this folder; its own settings decide who can see it.");
    const QString blocked = blockedReason();
    out.insert(QStringLiteral("ok"), blocked.isEmpty());
    out.insert(QStringLiteral("error"), blocked);
    out.insert(QStringLiteral("bytes"), static_cast<qulonglong>(text->size()));
    out.insert(QStringLiteral("sizeText"), QLocale().formattedDataSize(static_cast<qint64>(text->size())));
    out.insert(QStringLiteral("destination"), l->name + QStringLiteral(" (") + kindName(l->kind) + QLatin1Char(')'));
    out.insert(QStringLiteral("notes"), notes);
    return out;
}

void CloudBridge::createCloudProject(const QString& locationId, const QString& name) {
    const Location* l = findLocation(locationId);
    if (!l || !m_controller) {
        setMessage(tr("Choose where to store it."), true);
        return;
    }
    if (const QString blocked = blockedReason(); !blocked.isEmpty()) {
        setMessage(blocked, true);
        return;
    }
    if (l->purpose != QLatin1String("projects")) {
        setMessage(tr("This location is for review links. Choose one for cloud projects."), true);
        return;
    }
    if (d->hasLink) {
        setMessage(tr("This project is already a cloud project."), true);
        return;
    }
    if (!projectSaved()) {
        setMessage(tr("Save the project first."), true);
        return;
    }
    if (m_lifecycle && m_lifecycle->dirty() && !m_lifecycle->save()) {
        setMessage(tr("The project could not be saved."), true);
        return;
    }
    const QString clean = name.trimmed().isEmpty() ? projectName() : name.trimmed().left(120);
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const ProjectSnapshot snapshot = m_controller->snapshot();
    const fs::path file = projectFile();
    const fs::path cache = cacheFolder();
    const cloud::Identity who = identity();
    start(tr("Uploading \"%1\"").arg(clean), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        // the storage must keep its promises about conditional writes before a project is trusted to it
        const cloud::StoreCheck check = cloud::checkStore(*store, m_random, stop);
        if (!check.reachable || !check.conditionalWrites) {
            const QString why = check.problems.empty() ? tr("The storage cannot be reached.") : qs(check.problems.front());
            return [this, why] { setMessage(tr("This storage cannot hold cloud projects: %1").arg(why), true); };
        }
        cloud::CloudProjects projects(*store, who, m_clock, m_random, QCoreApplication::applicationVersion().toStdString());
        auto created = projects.create(*snapshot, s(clean), s(locationId), cache, progressSink(tr("Uploading \"%1\"").arg(clean)), stop);
        return [=, this, created = std::move(created)] {
            if (!created) {
                setMessage(qs(created.error().message), true);
                return;
            }
            if (auto written = cloud::writeCloudLink(file, created->link); !written) {
                setMessage(tr("The project is uploaded, but its link could not be saved: %1").arg(qs(written.error().message)), true);
                return;
            }
            d->link = created->link;
            d->hasLink = true;
            d->guard = {};
            setMessage(tr("Uploaded \"%1\" as revision 1. Open it on another computer from File > Cloud Project.").arg(clean));
            rebuildCloudRow();
            emit cloudProjectReady();
            acquireLockAsync();
        };
    });
}

void CloudBridge::refreshRemoteProjects(const QString& locationId) {
    const Location* l = findLocation(locationId);
    if (!l) {
        setMessage(tr("Choose a location."), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const cloud::Identity who = identity();
    start(tr("Looking in \"%1\"").arg(l->name), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        cloud::CloudProjects projects(*store, who, m_clock, m_random);
        auto list = projects.list(stop);
        return [=, this, list = std::move(list)] {
            m_remoteRows.clear();
            if (!list) {
                setMessage(qs(list.error().message), true);
            } else {
                for (const cloud::RemoteProject& p : *list) {
                    m_remoteRows.append(QVariantMap{{QStringLiteral("projectId"), qs(p.projectId)},
                                                    {QStringLiteral("name"), p.head.name.empty() ? qs(p.projectId) : qs(p.head.name)},
                                                    {QStringLiteral("revision"), static_cast<int>(p.head.revision)},
                                                    {QStringLiteral("savedBy"), qs(p.head.savedBy)},
                                                    {QStringLiteral("savedText"), whenText(p.head.savedAt)},
                                                    {QStringLiteral("label"), qs(p.head.label)}});
                }
                setMessage(list->empty() ? tr("No cloud projects here yet.") : tr("%n cloud project(s) found.", nullptr, static_cast<int>(list->size())));
            }
            emit remoteChanged();
        };
    });
}

void CloudBridge::openCloudProject(const QString& locationId, const QString& projectId, const QUrl& folderUrl) {
    const Location* l = findLocation(locationId);
    const QString folder = folderUrl.isLocalFile() ? folderUrl.toLocalFile() : folderUrl.toString();
    if (!l || folder.isEmpty() || !cloud::validSegment(s(projectId))) {
        setMessage(tr("Choose a project and a folder to put it in."), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const fs::path target = pathFromUtf8(s(QDir::cleanPath(folder)));
    const fs::path cache = cacheFolder();
    const cloud::Identity who = identity();
    start(tr("Downloading the project"), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        cloud::CloudProjects projects(*store, who, m_clock, m_random);
        std::size_t quarantined = 0;
        auto got = projects.open(s(projectId), s(locationId), target, cache, &quarantined, progressSink(tr("Downloading the project")), stop);
        if (!got)
            return [this, error = qs(got.error().message)] { setMessage(error, true); };
        // the working copy: a new file next to nothing else of ours, never over an existing one
        std::string name = got->second.name;
        for (char& c : name) {
            if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
                c = '_';
        }
        if (name.empty() || name == "." || name == "..")
            name = "Cloud project";
        fs::path file = target / pathFromUtf8(name + ".wabcut");
        for (int n = 2; fs::exists(file) && n < 1000; ++n)
            file = target / pathFromUtf8(name + " (" + std::to_string(n) + ").wabcut");
        std::error_code ec;
        fs::create_directories(target, ec);
        if (auto saved = io::saveProject(got->first, file, io::SaveOptions{0}); !saved)
            return [this, error = qs(saved.error().message)] { setMessage(tr("The project could not be saved: %1").arg(error), true); };
        if (auto written = cloud::writeCloudLink(file, got->second); !written)
            return [this, error = qs(written.error().message)] { setMessage(error, true); };
        return [=, this] {
            if (!m_lifecycle || !m_lifecycle->open(QUrl::fromLocalFile(qs(pathToUtf8(file))))) {
                setMessage(tr("The project was downloaded to %1 but could not be opened.").arg(QDir::toNativeSeparators(qs(pathToUtf8(file)))), true);
                return;
            }
            afterOpen(file, locationId);
            QString text = tr("Opened \"%1\" (revision %2).").arg(qs(got->second.name)).arg(static_cast<int>(got->second.baseRevision));
            if (quarantined > 0)
                text += QLatin1Char(' ') + tr("%n media item(s) pointed at a network share of another computer; they are not used. Relink them.", nullptr, static_cast<int>(quarantined));
            setMessage(text);
        };
    });
}

void CloudBridge::afterOpen(const fs::path& file, const QString& locationId) {
    onDocumentChanged();
    if (d->linkedFile != file)
        return;
    const int found = relinkMedia();
    if (found > 0)
        m_message += QLatin1Char(' ') + tr("%n media item(s) found on this computer.", nullptr, found);
    emit cloudProjectReady();
    (void)locationId;
}

int CloudBridge::relinkMedia() {
    if (!m_lifecycle || !d->hasLink)
        return 0;
    const Location* l = findLocation(qs(d->link.location));
    QList<QUrl> folders;
    if (l) {
        for (const QString& f : l->mediaFolders)
            folders.append(QUrl::fromLocalFile(f));
    }
    const fs::path project = projectFile();
    if (!project.empty())
        folders.append(QUrl::fromLocalFile(qs(pathToUtf8(project.parent_path()))));
    return m_lifecycle->relinkAll(folders);
}

// ---- sync ----

bool CloudBridge::syncStepIntact() const {
    if (!d->guard.active || !m_controller)
        return true;
    const std::vector<std::string> labels = m_controller->session().undoLabels();
    return labels.size() >= d->guard.count && labels[d->guard.count - 1] == kSyncStepLabel;
}

cloud::SyncRequest CloudBridge::syncRequest(bool sendAnyway) const {
    cloud::SyncRequest request;
    request.local = *m_controller->snapshot();
    request.link = d->link;
    request.cacheFolder = cacheFolder();
    request.projectFolder = projectFile().parent_path();
    const QString blocked = blockedReason();
    QString why;
    if (!blocked.isEmpty())
        why = blocked;
    else if (d->lockKind == LockKind::Other)
        why = tr("%1 is editing this project, so nothing is sent from here now.").arg(d->lockText);
    else if (!sendAnyway && !syncStepIntact())
        why = tr("You undid or replaced the last cloud sync. Redo it to get the cloud changes back, or send your version anyway.");
    request.mayPush = why.isEmpty();
    request.pushBlockedBecause = s(why);
    return request;
}

void CloudBridge::syncNow(bool sendAnyway) {
    if (!d->hasLink || !m_controller) {
        setMessage(tr("This project is not a cloud project."), true);
        return;
    }
    cloud::SyncRequest request = syncRequest(sendAnyway);
    request.label.clear();
    startSync(std::move(request));
}

void CloudBridge::resolveConflicts(const QString& mode, const QVariantMap& choices) {
    if (!d->hasLink || !m_controller)
        return;
    cloud::SyncRequest request = syncRequest(true);
    for (const QVariant& v : m_conflictRows) {
        const QString id = v.toMap().value(QStringLiteral("id")).toString();
        const QString pick = choices.contains(id) ? choices.value(id).toString() : mode;
        request.choices[s(id)] = pick == QLatin1String("theirs") ? MergeChoice::Theirs : MergeChoice::Mine;
    }
    request.resolveRemaining = true;
    startSync(std::move(request));
}

void CloudBridge::startSync(cloud::SyncRequest request) {
    const Location* l = findLocation(qs(d->link.location));
    if (!l) {
        setMessage(tr("The storage location of this project was removed. Add it again."), true);
        return;
    }
    if (m_ai && l->kind != QLatin1String("folder") && m_ai->offlineMode()) {
        setMessage(blockedReason(), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const std::uint64_t revision = m_controller->session().revision();
    const cloud::Identity who = identity();
    const QString phase = tr("Syncing with \"%1\"").arg(l->name);
    start(phase, [=, this, request = std::move(request)](std::stop_token stop) mutable -> CloudWorker::Finish {
        cloud::CloudProjects projects(*store, who, m_clock, m_random, QCoreApplication::applicationVersion().toStdString());
        auto result = projects.sync(request, progressSink(phase), stop);
        return [=, this, result = std::move(result)]() mutable {
            if (!result) {
                setMessage(qs(result.error().message), true);
                return;
            }
            applySyncResult(std::move(*result), revision);
        };
    });
}

void CloudBridge::applySyncResult(cloud::SyncResult result, std::uint64_t startedAtRevision) {
    QStringList notes;
    for (const std::string& n : result.notes) {
        if (!n.empty())
            notes << qs(n);
    }
    const auto finish = [&](const QString& text, bool error = false) {
        QString all = text;
        if (!notes.isEmpty())
            all += QLatin1Char(' ') + notes.join(QLatin1Char(' '));
        setMessage(all, error);
    };
    const auto writeLink = [&] { return !projectFile().empty() && cloud::writeCloudLink(projectFile(), result.link).has_value(); };
    m_conflictRows.clear();
    switch (result.kind) {
    case cloud::SyncKind::UpToDate:
        finish(tr("Up to date. The cloud copy is revision %1.").arg(static_cast<int>(result.remoteHead.revision)));
        break;
    case cloud::SyncKind::PushBlocked:
        finish(tr("Your changes were not sent."), true);
        break;
    case cloud::SyncKind::NeedsChoices:
        for (const MergeConflict& c : result.conflicts)
            m_conflictRows.append(QVariantMap{{QStringLiteral("id"), qs(c.id)}, {QStringLiteral("what"), qs(c.what)}});
        finish(tr("You and the cloud copy both changed the same things. Choose which version to keep for each; nothing was changed yet."), true);
        break;
    case cloud::SyncKind::Pushed:
        d->link = result.link;
        writeLink();
        finish(tr("Sent your changes as revision %1.").arg(static_cast<int>(result.link.baseRevision)));
        break;
    case cloud::SyncKind::FastForwarded:
    case cloud::SyncKind::Merged:
    case cloud::SyncKind::MergedAndPushed: {
        if (m_controller->session().revision() != startedAtRevision) {
            finish(tr("The project changed while it was syncing, so the cloud changes were not applied. Sync again."), true);
            break;
        }
        if (result.state) {
            auto committed = m_controller->session().commit(kSyncStepLabel, *result.state);
            if (!committed) {
                finish(tr("The cloud changes could not be applied: %1").arg(qs(committed.error().message)), true);
                break;
            }
            d->guard.active = true;
            d->guard.count = m_controller->session().undoCount();
        }
        d->link = result.link;
        writeLink();
        if (d->history && result.state)
            d->history->checkpoint(QStringLiteral("auto"), {});
        QString text = result.kind == cloud::SyncKind::FastForwarded
                           ? tr("Brought in the cloud changes (revision %1).").arg(static_cast<int>(result.link.baseRevision))
                       : result.kind == cloud::SyncKind::Merged
                           ? tr("Merged the cloud changes into your project. They are not sent yet.")
                           : tr("Merged the cloud changes with yours and sent the result as revision %1.").arg(static_cast<int>(result.link.baseRevision));
        if (!result.changes.empty())
            text += QLatin1Char(' ') + qs(result.changes.substr(0, 300));
        finish(text);
        break;
    }
    }
    if (result.kind != cloud::SyncKind::UpToDate && result.kind != cloud::SyncKind::PushBlocked && result.kind != cloud::SyncKind::NeedsChoices)
        refreshHistory();
    rebuildCloudRow();
}

void CloudBridge::setAutoSync(bool on) {
    if (!d->hasLink || projectFile().empty())
        return;
    d->link.autoSync = on;
    (void)cloud::writeCloudLink(projectFile(), d->link);
    rebuildCloudRow();
}

bool CloudBridge::unlinkProject() {
    if (!d->hasLink)
        return false;
    releaseLockNow();
    std::error_code ec;
    fs::remove(cloud::cloudLinkPath(projectFile()), ec);
    d->hasLink = false;
    d->link = {};
    d->guard = {};
    d->lockKind = LockKind::None;
    d->lockText.clear();
    m_conflictRows.clear();
    m_revisionRows.clear();
    m_unmergedRows.clear();
    emit historyChanged();
    rebuildCloudRow();
    setMessage(tr("The project is no longer a cloud project. The copy on the storage stays where it is."));
    return true;
}

void CloudBridge::refreshHistory() {
    if (!d->hasLink)
        return;
    const Location* l = findLocation(qs(d->link.location));
    if (!l)
        return;
    auto opened = openStore(*l);
    if (!opened)
        return;
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const std::string id = d->link.projectId;
    const cloud::Identity who = identity();
    if (m_worker.running())
        return;
    start(tr("Reading the cloud history"), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        cloud::CloudProjects projects(*store, who, m_clock, m_random);
        auto head = projects.head(id, nullptr, stop);
        auto list = projects.revisions(id, 50, stop);
        auto lost = projects.unmerged(id, 50, stop);
        return [=, this, head = std::move(head), list = std::move(list), lost = std::move(lost)] {
            m_revisionRows.clear();
            m_unmergedRows.clear();
            if (list) {
                for (const cloud::RevisionMeta& m : *list) {
                    m_revisionRows.append(QVariantMap{{QStringLiteral("revision"), static_cast<int>(m.revision)},
                                                      {QStringLiteral("savedBy"), qs(m.savedBy)},
                                                      {QStringLiteral("savedText"), whenText(m.savedAt)},
                                                      {QStringLiteral("label"), qs(m.label)},
                                                      {QStringLiteral("sizeText"), QLocale().formattedDataSize(static_cast<qint64>(m.bytes))},
                                                      {QStringLiteral("current"), head && head->revision == m.revision && head->sha256 == m.sha256},
                                                      {QStringLiteral("mine"), d->hasLink && d->link.baseSha == m.sha256}});
                }
            }
            if (lost) {
                for (const cloud::RevisionMeta& m : *lost) {
                    m_unmergedRows.append(QVariantMap{{QStringLiteral("revision"), static_cast<int>(m.revision)},
                                                      {QStringLiteral("savedBy"), qs(m.savedBy)},
                                                      {QStringLiteral("savedText"), whenText(m.savedAt)}});
                }
            }
            emit historyChanged();
        };
    });
}

void CloudBridge::openRevisionAsCopy(int revision, const QUrl& folderUrl) {
    if (!d->hasLink)
        return;
    const Location* l = findLocation(qs(d->link.location));
    const QString folder = folderUrl.isLocalFile() ? folderUrl.toLocalFile() : folderUrl.toString();
    if (!l || folder.isEmpty()) {
        setMessage(tr("Choose a folder to put the copy in."), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const std::string id = d->link.projectId;
    const std::string name = d->link.name;
    const fs::path target = pathFromUtf8(s(QDir::cleanPath(folder)));
    const fs::path cache = cacheFolder();
    const cloud::Identity who = identity();
    start(tr("Downloading revision %1").arg(revision), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        cloud::CloudProjects projects(*store, who, m_clock, m_random);
        auto list = projects.revisions(id, 200, stop);
        if (!list)
            return [this, error = qs(list.error().message)] { setMessage(error, true); };
        const auto it = std::find_if(list->begin(), list->end(), [&](const cloud::RevisionMeta& m) { return m.revision == revision; });
        if (it == list->end())
            return [this] { setMessage(tr("That revision is no longer on the storage."), true); };
        auto state = projects.fetch(id, *it, cache, progressSink(tr("Downloading revision %1").arg(revision)), stop);
        if (!state)
            return [this, error = qs(state.error().message)] { setMessage(error, true); };
        std::string base = name.empty() ? std::string("Cloud project") : name;
        for (char& c : base) {
            if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
                c = '_';
        }
        std::error_code ec;
        fs::create_directories(target, ec);
        fs::path file = target / pathFromUtf8(base + " (cloud revision " + std::to_string(revision) + ").wabcut");
        for (int n = 2; fs::exists(file) && n < 1000; ++n)
            file = target / pathFromUtf8(base + " (cloud revision " + std::to_string(revision) + ", " + std::to_string(n) + ").wabcut");
        if (auto saved = io::saveProject(*state, file, io::SaveOptions{0}); !saved)
            return [this, error = qs(saved.error().message)] { setMessage(error, true); };
        return [=, this] {
            if (m_lifecycle && m_lifecycle->openAlongside(QUrl::fromLocalFile(qs(pathToUtf8(file)))))
                setMessage(tr("Opened revision %1 as a separate project.").arg(revision));
            else
                setMessage(tr("Saved revision %1 to %2.").arg(revision).arg(QDir::toNativeSeparators(qs(pathToUtf8(file)))));
        };
    });
}

} // namespace wabcut::app
