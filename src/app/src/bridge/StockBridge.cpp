#include "StockBridge.h"

#include "AiController.h"
#include "CloudBridgeInternal.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/Http.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/model/MediaLocator.h>

#include <QColor>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace fs = std::filesystem;
using namespace cloud_detail;

namespace {

constexpr int kMaxThumbWidth = 480;

QString fileLabel(const cloud::StockFile& f) {
    QStringList parts;
    if (f.width > 0 && f.height > 0)
        parts << QStringLiteral("%1 × %2").arg(f.width).arg(f.height);
    parts << qs(f.extension).toUpper();
    if (f.fps > 0.0)
        parts << QStringLiteral("%1 fps").arg(QString::number(f.fps, 'g', 4));
    if (f.bytes > 0)
        parts << QLocale().formattedDataSize(static_cast<qint64>(f.bytes));
    return parts.join(QStringLiteral(" · "));
}

QString durationText(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// ---- the thumbnails ----

void StockCache::put(const QString& key, const QImage& image) {
    std::lock_guard lock(mutex);
    if (!images.contains(key))
        order << key;
    images.insert(key, image);
    while (order.size() > 200) { // a screenful of pages, no more
        images.remove(order.front());
        order.pop_front();
    }
}

bool StockCache::has(const QString& key) {
    std::lock_guard lock(mutex);
    return images.contains(key);
}

void StockCache::clear() {
    std::lock_guard lock(mutex);
    images.clear();
    order.clear();
}

QImage StockImages::requestImage(const QString& id, QSize* size, const QSize& requested) {
    QImage image;
    {
        std::lock_guard lock(m_cache->mutex);
        image = m_cache->images.value(id.section(QLatin1Char('?'), 0, 0));
    }
    if (image.isNull()) { // not fetched yet: a quiet placeholder of the usual shape
        image = QImage(320, 180, QImage::Format_RGB32);
        image.fill(QColor(0x24, 0x24, 0x2b));
    }
    if (size)
        *size = image.size();
    if (requested.isValid() && requested.width() > 0 && requested.width() < image.width())
        image = image.scaledToWidth(requested.width(), Qt::SmoothTransformation);
    return image;
}

// ---- the bridge ----

StockBridge::StockBridge(QObject* parent) : QObject(parent), m_cache(std::make_shared<StockCache>()) {
    m_clock = cloud::systemClock();
    rebuildProviders();
}

StockBridge::~StockBridge() {
    m_worker.stop();
    m_thumbs.stop();
}

StockBridge* StockBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new StockBridge;
    if (qmlEngine) {
        bridge->setControllers(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"),
                               qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"),
                               qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController"));
        qmlEngine->addImageProvider(QStringLiteral("wabcut-stock"), new StockImages(bridge->m_cache));
    }
    return bridge;
}

void StockBridge::setControllers(ProjectController* controller, ProjectLifecycle* lifecycle, AiController* ai) {
    m_controller = controller;
    m_lifecycle = lifecycle;
    m_ai = ai;
    rebuildProviders();
    if (ai) {
        connect(ai, &AiController::privacyChanged, this, &StockBridge::blockedChanged);
        connect(ai, &AiController::shuttingDown, this, [this] {
            m_worker.stop();
            m_thumbs.stop();
        });
    }
    if (lifecycle)
        connect(lifecycle, &ProjectLifecycle::documentChanged, this, &StockBridge::blockedChanged);
}

ai::IHttpTransport& StockBridge::transport() {
    if (m_testTransport)
        return *m_testTransport;
    return m_ai->transport();
}

ai::SecretStore& StockBridge::secretStore() {
    if (m_testSecrets)
        return *m_testSecrets;
    return m_ai->platform().keys().store();
}

QString StockBridge::blockedReason() const {
    if (m_ai && m_ai->offlineMode())
        return tr("Offline mode is on, so stock media cannot be searched or downloaded. Turn it off under Preferences > AI.");
    if (m_ai && m_ai->projectLock())
        return tr("This project is locked to no-cloud work, so nothing is searched or downloaded for it. Unlock it under Project > Privacy.");
    return {};
}

bool StockBridge::canAdd() const { return m_lifecycle && !m_lifecycle->demo(); }

bool StockBridge::hasKey() {
    if (!m_testSecrets && !m_ai)
        return false;
    return keyOf(m_provider).has_value();
}

std::optional<std::string> StockBridge::keyOf(cloud::StockProvider provider) {
    const std::string id = cloud::stockProviderId(provider);
    if (const auto it = m_keys.find(id); it != m_keys.end())
        return it->second.empty() ? std::nullopt : std::optional<std::string>(it->second);
    auto read = secretStore().read("stock:" + id);
    std::string key = read && *read ? std::string((*read)->reveal()) : std::string();
    if (!key.empty())
        ai::Redactor::global().addSecret(key);
    m_keys[id] = key;
    return key.empty() ? std::nullopt : std::optional<std::string>(key);
}

void StockBridge::rebuildProviders() {
    QVariantList rows;
    for (const cloud::StockProvider p : {cloud::StockProvider::Pexels, cloud::StockProvider::Pixabay}) {
        const cloud::StockLicence licence = cloud::stockLicence(p);
        bool has = false;
        if (m_testSecrets || m_ai)
            has = keyOf(p).has_value();
        rows.append(QVariantMap{{QStringLiteral("id"), qs(cloud::stockProviderId(p))},
                                {QStringLiteral("name"), qs(cloud::stockProviderName(p))},
                                {QStringLiteral("hasKey"), has},
                                {QStringLiteral("licence"), qs(licence.name)},
                                {QStringLiteral("licenceUrl"), qs(licence.url)},
                                {QStringLiteral("summary"), qs(licence.summary)},
                                {QStringLiteral("attribution"), qs(licence.attribution)},
                                {QStringLiteral("homepage"), qs(licence.homepage)},
                                {QStringLiteral("keyUrl"), qs(licence.keyUrl)}});
    }
    m_providerRows = rows;
    emit providersChanged();
}

void StockBridge::setMessage(const QString& text, bool error) {
    m_message = text;
    m_messageIsError = error;
    emit messageChanged();
}

void StockBridge::setProvider(const QString& id) {
    const auto p = cloud::stockProviderFromId(id.toStdString());
    if (!p || *p == m_provider)
        return;
    m_provider = *p;
    emit searchChanged();
    emit providersChanged();
}

void StockBridge::setKind(const QString& kind) {
    const cloud::StockKind next = kind == QLatin1String("photo") ? cloud::StockKind::Photo : cloud::StockKind::Video;
    if (next == m_kind)
        return;
    m_kind = next;
    emit searchChanged();
}

void StockBridge::setOrientation(const QString& orientation) {
    const QString next = orientation == QLatin1String("landscape") || orientation == QLatin1String("portrait") || orientation == QLatin1String("square") ? orientation : QString();
    if (next == m_orientation)
        return;
    m_orientation = next;
    emit searchChanged();
}

void StockBridge::search(const QString& text) {
    m_query = text.trimmed();
    emit searchChanged();
    runSearch(1, false);
}

void StockBridge::loadMore() {
    if (m_hasMore && !m_worker.running())
        runSearch(m_page + 1, true);
}

void StockBridge::runSearch(int page, bool append) {
    if (const QString blocked = blockedReason(); !blocked.isEmpty()) {
        setMessage(blocked, true);
        return;
    }
    if (!m_testTransport && !m_ai) {
        setMessage(tr("The network is not available."), true);
        return;
    }
    cloud::StockQuery query;
    query.provider = m_provider;
    query.kind = m_kind;
    query.text = m_query.toStdString();
    query.page = page;
    query.orientation = m_orientation.toStdString();
    if (const std::string problem = cloud::stockQueryProblem(query); !problem.empty()) {
        setMessage(qs(problem), true);
        return;
    }
    const auto key = keyOf(m_provider);
    if (!key) {
        setMessage(tr("Enter your %1 key first: open the settings below and paste it. It is free.").arg(qs(cloud::stockProviderName(m_provider))), true);
        return;
    }
    if (m_worker.running())
        return;
    if (!m_client)
        m_client = std::make_unique<cloud::StockClient>(transport(), m_clock);
    cloud::StockClient* client = m_client.get();
    const std::string secret = *key;
    m_progress = -1.0;
    const bool started = m_worker.start(this, m_synchronous, [=, this](std::stop_token stop) -> CloudWorker::Finish {
        auto page1 = client->search(query, ai::Secret(secret), stop);
        return [=, this, page1 = std::move(page1)]() mutable {
            emit busyChanged();
            if (!page1) {
                setMessage(qs(page1.error().message), true);
                return;
            }
            if (!append)
                m_items.clear();
            for (cloud::StockItem& item : page1->items)
                m_items.push_back(std::move(item));
            m_page = page;
            m_total = page1->total;
            m_hasMore = page1->hasMore || (m_provider == cloud::StockProvider::Pixabay && static_cast<int>(m_items.size()) < page1->total && !page1->items.empty());
            m_quota = page1->quotaRemaining;
            rebuildResults();
            setMessage(m_items.empty() ? tr("Nothing found for \"%1\".").arg(m_query) : QString());
            fetchThumbnails();
        };
    });
    if (started)
        emit busyChanged();
}

void StockBridge::rebuildResults() {
    QVariantList rows;
    for (const cloud::StockItem& item : m_items) {
        const QString key = qs(cloud::stockProviderId(item.provider)) + (item.kind == cloud::StockKind::Video ? QStringLiteral("-v-") : QStringLiteral("-p-")) + qs(item.id);
        QVariantList files;
        int preferred = -1;
        int lastWidth = -1;
        int lastHeight = -1;
        for (std::size_t i = 0; i < item.files.size() && files.size() < 5; ++i) {
            const cloud::StockFile& f = item.files[i];
            if (f.width == lastWidth && f.height == lastHeight)
                continue; // the same size twice (an SD and an HD file of one picture): the first is shown
            lastWidth = f.width;
            lastHeight = f.height;
            const QString shortLabel = item.kind == cloud::StockKind::Video ? QStringLiteral("%1p").arg(std::min(f.width, f.height))
                                                                            : QStringLiteral("%1 px").arg(std::max(f.width, f.height));
            if (preferred < 0 && f.width > 0 && f.width <= 1920) // full HD by default, not a 4K download
                preferred = static_cast<int>(files.size());
            files.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)}, {QStringLiteral("label"), fileLabel(f)}, {QStringLiteral("shortLabel"), shortLabel}});
        }
        if (preferred < 0)
            preferred = files.isEmpty() ? 0 : static_cast<int>(files.size()) - 1;
        rows.append(QVariantMap{{QStringLiteral("key"), key},
                                {QStringLiteral("provider"), qs(cloud::stockProviderId(item.provider))},
                                {QStringLiteral("providerName"), qs(cloud::stockProviderName(item.provider))},
                                {QStringLiteral("id"), qs(item.id)},
                                {QStringLiteral("kind"), item.kind == cloud::StockKind::Video ? QStringLiteral("video") : QStringLiteral("photo")},
                                {QStringLiteral("title"), qs(item.description)},
                                {QStringLiteral("creator"), qs(item.creator)},
                                {QStringLiteral("creatorUrl"), qs(item.creatorUrl)},
                                {QStringLiteral("pageUrl"), qs(item.pageUrl)},
                                {QStringLiteral("width"), item.width},
                                {QStringLiteral("height"), item.height},
                                {QStringLiteral("durationText"), item.durationSeconds > 0.0 ? durationText(item.durationSeconds) : QString()},
                                {QStringLiteral("credit"), qs(cloud::stockCreditLine(item))},
                                {QStringLiteral("thumb"), QStringLiteral("image://wabcut-stock/") + key},
                                {QStringLiteral("files"), files},
                                {QStringLiteral("preferredFile"), preferred}});
    }
    m_resultRows = rows;
    emit resultsChanged();
}

const cloud::StockItem* StockBridge::itemOf(const QString& key) const {
    for (const cloud::StockItem& item : m_items) {
        const QString k = qs(cloud::stockProviderId(item.provider)) + (item.kind == cloud::StockKind::Video ? QStringLiteral("-v-") : QStringLiteral("-p-")) + qs(item.id);
        if (k == key)
            return &item;
    }
    return nullptr;
}

void StockBridge::fetchThumbnails() {
    if (!m_client)
        return;
    std::vector<std::pair<QString, cloud::StockItem>> wanted;
    for (const QVariant& v : m_resultRows) {
        const QVariantMap row = v.toMap();
        const QString key = row.value(QStringLiteral("key")).toString();
        if (m_cache->has(key))
            continue;
        if (const cloud::StockItem* item = itemOf(key))
            wanted.emplace_back(key, *item);
    }
    if (wanted.empty() || m_thumbs.running())
        return;
    cloud::StockClient* client = m_client.get();
    QPointer<StockBridge> self(this);
    m_thumbs.start(this, m_synchronous, [self, client, wanted = std::move(wanted)](std::stop_token stop) -> CloudWorker::Finish {
        for (const auto& [key, item] : wanted) {
            if (stop.stop_requested())
                break;
            auto bytes = client->thumbnail(item, stop);
            if (!bytes)
                continue;
            QImage image = QImage::fromData(reinterpret_cast<const uchar*>(bytes->data()), static_cast<int>(bytes->size()));
            if (image.isNull())
                continue;
            if (image.width() > kMaxThumbWidth)
                image = image.scaledToWidth(kMaxThumbWidth, Qt::SmoothTransformation);
            if (!self)
                break;
            self->m_cache->put(key, image);
            QMetaObject::invokeMethod(self.data(), [self] {
                if (self) {
                    ++self->m_thumbSerial;
                    emit self->thumbnailsChanged();
                }
            }, Qt::QueuedConnection);
        }
        return {};
    });
}

void StockBridge::waitForThumbnails() {
    for (int i = 0; i < 2000 && m_thumbs.running(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents();
}

void StockBridge::saveKey(const QString& providerId, const QString& key) {
    const auto p = cloud::stockProviderFromId(providerId.toStdString());
    const std::string clean = key.trimmed().toStdString();
    if (!p) {
        return;
    }
    if (clean.empty()) {
        setMessage(tr("Paste the key first."), true);
        return;
    }
    if (clean.size() > 200 || clean.find_first_of(" \t\r\n&?=\"") != std::string::npos) {
        setMessage(tr("That does not look like a key. Copy it again from the service's page."), true);
        return;
    }
    if (const QString blocked = blockedReason(); !blocked.isEmpty()) {
        // a key cannot be checked while offline; it is kept so the check can happen later
        if (!secretStore().write("stock:" + cloud::stockProviderId(*p), ai::Secret(clean))) {
            setMessage(tr("The key could not be saved in the system's key store."), true);
            return;
        }
        ai::Redactor::global().addSecret(clean);
        m_keys[cloud::stockProviderId(*p)] = clean;
        rebuildProviders();
        setMessage(tr("The key is saved, but it was not checked: %1").arg(blocked));
        return;
    }
    if (!m_testTransport && !m_ai) {
        setMessage(tr("The network is not available."), true);
        return;
    }
    if (!m_client)
        m_client = std::make_unique<cloud::StockClient>(transport(), m_clock);
    cloud::StockClient* client = m_client.get();
    const cloud::StockProvider provider = *p;
    if (m_worker.running())
        return;
    const bool started = m_worker.start(this, m_synchronous, [=, this](std::stop_token stop) -> CloudWorker::Finish {
        auto checked = client->checkKey(provider, ai::Secret(clean), stop);
        return [=, this] {
            emit busyChanged();
            // Only a key the service refused is thrown away; an unreachable service says nothing about the key.
            if (!checked && checked.error().kind == cloud::StoreFailure::Unauthorized) {
                setMessage(tr("%1 refused this key. Check that you copied all of it.").arg(qs(cloud::stockProviderName(provider))), true);
                return;
            }
            if (!secretStore().write("stock:" + cloud::stockProviderId(provider), ai::Secret(clean))) {
                setMessage(tr("The key could not be saved in the system's key store."), true);
                return;
            }
            ai::Redactor::global().addSecret(clean);
            m_keys[cloud::stockProviderId(provider)] = clean;
            rebuildProviders();
            setMessage(checked ? tr("The key works and is saved.") : tr("The key is saved, but it could not be checked now: %1").arg(qs(checked.error().message)));
        };
    });
    if (started)
        emit busyChanged();
}

void StockBridge::removeKey(const QString& providerId) {
    const auto p = cloud::stockProviderFromId(providerId.toStdString());
    if (!p)
        return;
    (void)secretStore().remove("stock:" + cloud::stockProviderId(*p));
    m_keys[cloud::stockProviderId(*p)].clear();
    rebuildProviders();
    setMessage(tr("The key is removed from this computer."));
}

fs::path StockBridge::downloadFolder() const {
    if (!m_folderOverride.empty())
        return m_folderOverride;
    if (m_controller && !m_controller->projectFile().empty()) {
        fs::path media = m_controller->projectFile();
        media += "-media";
        return media / "Stock";
    }
    return pathFromUtf8(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdString()) / "Wabcut" / "Stock";
}

void StockBridge::cancel() {
    m_worker.cancel();
}

void StockBridge::add(const QString& key, int fileIndex) {
    const cloud::StockItem* found = itemOf(key);
    if (!found || fileIndex < 0 || fileIndex >= static_cast<int>(found->files.size())) {
        setMessage(tr("Choose a picture or video first."), true);
        return;
    }
    if (const QString blocked = blockedReason(); !blocked.isEmpty()) {
        setMessage(blocked, true);
        return;
    }
    if (!canAdd()) {
        setMessage(tr("Open or start a project to add media to."), true);
        return;
    }
    if (m_worker.running())
        return;
    if (!m_client)
        m_client = std::make_unique<cloud::StockClient>(transport(), m_clock);
    cloud::StockClient* client = m_client.get();
    const cloud::StockItem item = *found;
    const cloud::StockFile file = item.files[static_cast<std::size_t>(fileIndex)];
    const fs::path folder = downloadFolder();
    m_downloading = true;
    m_progress = 0.0;
    emit busyChanged();
    QPointer<StockBridge> self(this);
    const cloud::Progress sink = [self](std::uint64_t done, std::uint64_t total) {
        if (self && total > 0) {
            QMetaObject::invokeMethod(self.data(), [self, fraction = static_cast<double>(done) / static_cast<double>(total)] {
                if (self && self->m_downloading) {
                    self->m_progress = fraction;
                    emit self->busyChanged();
                }
            }, Qt::QueuedConnection);
        }
        return true;
    };
    m_worker.start(this, m_synchronous, [=, this](std::stop_token stop) -> CloudWorker::Finish {
        auto got = client->download(item, file, folder, sink, stop);
        return [=, this, got = std::move(got)] {
            m_downloading = false;
            m_progress = -1.0;
            emit busyChanged();
            if (!got) {
                setMessage(got.error().kind == cloud::StoreFailure::Cancelled ? tr("Cancelled. Nothing was kept.")
                                                                              : tr("The download failed: %1").arg(qs(got.error().message)),
                           got.error().kind != cloud::StoreFailure::Cancelled);
                return;
            }
            const MediaRights rights = cloud::stockRights(item, cloud::formatIsoUtc(m_clock()));
            const QString path = QUrl::fromLocalFile(qs(pathToUtf8(got->file))).toLocalFile();
            QVariantMap record{{QStringLiteral("source"), qs(rights.source)},
                               {QStringLiteral("licence"), qs(rights.licence)},
                               {QStringLiteral("reference"), qs(rights.reference)},
                               {QStringLiteral("territory"), qs(rights.territory)},
                               {QStringLiteral("creditLine"), qs(rights.creditLine)},
                               {QStringLiteral("notes"), qs(rights.notes)},
                               {QStringLiteral("status"), qs(std::string(toString(rights.status)))}};
            const QVariantMap summary = m_lifecycle->importFiles({QUrl::fromLocalFile(path)},
                                                                 QVariantMap{{QStringLiteral("undoLabel"), QStringLiteral("Add Stock Media")},
                                                                             {QStringLiteral("binName"), QStringLiteral("Stock")},
                                                                             {QStringLiteral("rights"), QVariantMap{{path, record}}}});
            const int added = summary.value(QStringLiteral("added")).toInt();
            const int existing = summary.value(QStringLiteral("existing")).toInt();
            if (added > 0) {
                setMessage(tr("Added to the Stock bin. Credit: %1. The licence is recorded under Rights and Clearance.").arg(qs(rights.creditLine)));
                emit mediaAdded(QFileInfo(path).fileName());
            } else if (existing > 0) {
                setMessage(tr("That file is already in the project."));
            } else {
                setMessage(tr("The file was downloaded but could not be imported: %1").arg(summary.value(QStringLiteral("failed")).toList().isEmpty()
                                                                                              ? QString()
                                                                                              : summary.value(QStringLiteral("failed")).toList().front().toMap().value(QStringLiteral("error")).toString()),
                           true);
            }
        };
    });
}

bool StockBridge::openPage(const QString& key) {
    const cloud::StockItem* item = itemOf(key);
    if (!item || !cloud::stockHostAllowed(item->provider, item->pageUrl))
        return false;
    return QDesktopServices::openUrl(QUrl(qs(item->pageUrl)));
}

bool StockBridge::openLicence(const QString& providerId) {
    const auto p = cloud::stockProviderFromId(providerId.toStdString());
    return p && QDesktopServices::openUrl(QUrl(qs(cloud::stockLicence(*p).url)));
}

bool StockBridge::openKeyPage(const QString& providerId) {
    const auto p = cloud::stockProviderFromId(providerId.toStdString());
    return p && QDesktopServices::openUrl(QUrl(qs(cloud::stockLicence(*p).keyUrl)));
}

} // namespace wabcut::app
