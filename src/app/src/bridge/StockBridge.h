#pragma once

// StockBridge: the Stock panel (0.7, docs/cloud.md), the QML singleton `Stock`. Search free videos and pictures on Pexels and
// Pixabay with the user's own API key, see where each comes from (provider, maker, licence, the credit they ask for), and add one
// to the project: it is downloaded into the project's media folder and imported into a "Stock" bin in ONE undo step, with its
// licence, page and credit line in the Rights and Clearance record (File > Export > Rights and Clearance Log; the credits roll).
//
//   * Keys. The user enters a key per service (free to make: the panel links to where). It goes to the OS keychain (entry
//     "stock:<service>"), is checked with a three-result search, and is never shown, logged or put in a project or a setting.
//   * Network. Everything goes through the AI platform's transport: proxies, TLS, the redacted request log, and offline mode and
//     the project's no-cloud lock, which stop searches and downloads with the reason shown. The key is sent to the service's API
//     host only. Thumbnails are fetched through the same transport into a small memory cache (no picture is kept on disk), and
//     shown by an image provider - no remote address is ever given to the interface.
//   * Off the GUI thread: searches, thumbnails, downloads (progress, Cancel). A download has a size limit, checks that the file is
//     what it says, and leaves nothing behind when it fails or is cancelled.
//   * Where files go: <project>.wabcut-media/Stock next to the saved project, else Documents/Wabcut/Stock.

#include "CloudBridge.h"

#include <wabcut/cloud/Stock.h>

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

class AiController;
class QJSEngine;
class QQmlEngine;

namespace wabcut::ai {
class IHttpTransport;
class SecretStore;
} // namespace wabcut::ai

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

/// The thumbnails the panel shows (small, in memory only), shared by the bridge that fetches them and the image provider.
struct StockCache {
    std::mutex mutex;
    QHash<QString, QImage> images;
    QStringList order;
    void put(const QString& key, const QImage& image);
    bool has(const QString& key);
    void clear();
};

/// image://wabcut-stock/<key>
class StockImages final : public QQuickImageProvider {
public:
    explicit StockImages(std::shared_ptr<StockCache> cache) : QQuickImageProvider(QQuickImageProvider::Image), m_cache(std::move(cache)) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

private:
    std::shared_ptr<StockCache> m_cache;
};

class StockBridge : public QObject {
    Q_OBJECT

    /// [{ id, name, hasKey, licence, licenceUrl, summary, attribution, homepage, keyUrl }]
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged FINAL)
    /// "pexels" | "pixabay"
    Q_PROPERTY(QString provider READ provider WRITE setProvider NOTIFY searchChanged FINAL)
    /// "video" | "photo"
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY searchChanged FINAL)
    /// "" | "landscape" | "portrait" | "square"
    Q_PROPERTY(QString orientation READ orientation WRITE setOrientation NOTIFY searchChanged FINAL)
    Q_PROPERTY(QString query READ query NOTIFY searchChanged FINAL)
    /// [{ key, provider, providerName, id, kind, title, creator, creatorUrl, pageUrl, width, height, sizeText, durationText, credit,
    ///    thumb (image url), files: [{ index, label }] }]
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged FINAL)
    Q_PROPERTY(int total READ total NOTIFY resultsChanged FINAL)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY resultsChanged FINAL)
    /// The service says it has this many requests left in its window (-1: unknown).
    Q_PROPERTY(int quota READ quota NOTIFY resultsChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool downloading READ downloading NOTIFY busyChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged FINAL)
    /// Offline mode / the project's no-cloud lock: why nothing can be searched or downloaded ("" when it can).
    Q_PROPERTY(QString blockedReason READ blockedReason NOTIFY blockedChanged FINAL)
    /// A project is open to put downloads into.
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY blockedChanged FINAL)
    /// The key of the chosen service is saved.
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY providersChanged FINAL)
    Q_PROPERTY(int thumbSerial READ thumbSerial NOTIFY thumbnailsChanged FINAL)

public:
    explicit StockBridge(QObject* parent = nullptr);
    ~StockBridge() override;
    static StockBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setControllers(ProjectController* controller, ProjectLifecycle* lifecycle, AiController* ai);
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    void setTransportForTesting(ai::IHttpTransport* transport) { m_testTransport = transport; }
    void setSecretStoreForTesting(ai::SecretStore* store) {
        m_testSecrets = store;
        rebuildProviders();
    }
    void setClockForTesting(cloud::UnixClock clock) { m_clock = std::move(clock); }
    void setDownloadFolderForTesting(std::filesystem::path folder) { m_folderOverride = std::move(folder); }
    /// Blocks until the thumbnails asked for so far are fetched (tests).
    void waitForThumbnails();

    QVariantList providers() const { return m_providerRows; }
    QString provider() const { return QString::fromStdString(cloud::stockProviderId(m_provider)); }
    void setProvider(const QString& id);
    QString kind() const { return m_kind == cloud::StockKind::Video ? QStringLiteral("video") : QStringLiteral("photo"); }
    void setKind(const QString& kind);
    QString orientation() const { return m_orientation; }
    void setOrientation(const QString& orientation);
    QString query() const { return m_query; }
    QVariantList results() const { return m_resultRows; }
    int total() const { return m_total; }
    bool hasMore() const { return m_hasMore; }
    int quota() const { return m_quota; }
    bool busy() const { return m_worker.running(); }
    bool downloading() const { return m_downloading; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    bool messageIsError() const { return m_messageIsError; }
    QString blockedReason() const;
    bool canAdd() const;
    bool hasKey();
    int thumbSerial() const { return m_thumbSerial; }

    /// Searches the chosen service (page 1).
    Q_INVOKABLE void search(const QString& text);
    Q_INVOKABLE void loadMore();
    /// Saves a key (checked first, with a small search; kept only when the service accepts it, unless it cannot be reached).
    Q_INVOKABLE void saveKey(const QString& provider, const QString& key);
    Q_INVOKABLE void removeKey(const QString& provider);
    /// Downloads `files[fileIndex]` of the result `key` and adds it to the project (one undo step "Add Stock Media").
    Q_INVOKABLE void add(const QString& key, int fileIndex);
    Q_INVOKABLE void cancel();
    /// Opens the item's page / the licence page in the browser (https on the service's own site only).
    Q_INVOKABLE bool openPage(const QString& key);
    Q_INVOKABLE bool openLicence(const QString& provider);
    Q_INVOKABLE bool openKeyPage(const QString& provider);

signals:
    void providersChanged();
    void searchChanged();
    void resultsChanged();
    void busyChanged();
    void messageChanged();
    void blockedChanged();
    void thumbnailsChanged();
    /// A download was added to the project.
    void mediaAdded(const QString& name);

private:
    ai::IHttpTransport& transport();
    ai::SecretStore& secretStore();
    std::optional<std::string> keyOf(cloud::StockProvider provider);
    void rebuildProviders();
    void rebuildResults();
    void setMessage(const QString& text, bool error = false);
    void runSearch(int page, bool append);
    void fetchThumbnails();
    std::filesystem::path downloadFolder() const;
    const cloud::StockItem* itemOf(const QString& key) const;

    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<AiController> m_ai;
    ai::IHttpTransport* m_testTransport = nullptr;
    ai::SecretStore* m_testSecrets = nullptr;
    cloud::UnixClock m_clock;
    std::filesystem::path m_folderOverride;
    bool m_synchronous = false;

    cloud::StockProvider m_provider = cloud::StockProvider::Pexels;
    cloud::StockKind m_kind = cloud::StockKind::Video;
    QString m_orientation;
    QString m_query;
    int m_page = 1;
    std::vector<cloud::StockItem> m_items;
    int m_total = 0;
    bool m_hasMore = false;
    int m_quota = -1;
    QVariantList m_providerRows;
    QVariantList m_resultRows;
    std::map<std::string, std::string> m_keys; ///< service id -> key, in memory only
    CloudWorker m_worker;
    CloudWorker m_thumbs;
    std::shared_ptr<StockCache> m_cache;
    std::unique_ptr<cloud::StockClient> m_client;
    bool m_downloading = false;
    double m_progress = -1.0;
    QString m_message;
    bool m_messageIsError = false;
    int m_thumbSerial = 0;
    std::mutex m_thumbMutex;
    std::vector<std::pair<QString, cloud::StockItem>> m_wanted;
};

/// The QML singleton `Stock`. The bridge is default-constructible, so Qt would call its constructor and skip create();
/// the foreign gadget keeps create() (it wires the controllers and registers the thumbnail image provider).
struct StockBridgeForeign {
    Q_GADGET
    QML_FOREIGN(StockBridge)
    QML_NAMED_ELEMENT(Stock)
    QML_SINGLETON
public:
    static StockBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return StockBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
