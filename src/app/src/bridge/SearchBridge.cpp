#include "SearchBridge.h"

#include "RedactedPictures.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/search/HelperEmbedder.h>
#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickImageProvider>
#include <QThread>

#include <chrono>
#include <cmath>
#include <list>
#include <map>

namespace wabcut::app {

namespace search = ai::search;

namespace {

constexpr std::size_t kResultLimit = 60;

/// "1:05" or "1:02:05".
QString clockText(Ticks t) {
    const auto total = static_cast<qint64>(std::floor(std::max(0.0, t.toSeconds())));
    const qint64 h = total / 3600;
    const qint64 m = (total / 60) % 60;
    const qint64 s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

/// A result card for QML.
QVariantMap card(const search::SearchHit& hit) {
    const QString media = mediaKey(hit.media);
    QVariantMap c;
    c.insert(QStringLiteral("key"), media + QLatin1Char(':') + QString::number(hit.range.start.value));
    c.insert(QStringLiteral("mediaId"), media);
    c.insert(QStringLiteral("mediaName"), QString::fromStdString(hit.mediaName));
    c.insert(QStringLiteral("sourceIn"), QString::number(hit.range.start.value));
    c.insert(QStringLiteral("sourceOut"), QString::number(hit.range.end().value));
    //: Smart search result: the source range of the hit, e.g. "0:05 – 0:10"
    c.insert(QStringLiteral("rangeText"), SearchBridge::tr("%1 – %2").arg(clockText(hit.range.start), clockText(hit.range.end())));
    c.insert(QStringLiteral("thumbnail"),
             QStringLiteral("image://wabcut-search/%1/%2").arg(media).arg(QString::number(hit.thumbnail.value)));
    c.insert(QStringLiteral("score"), hit.score);
    c.insert(QStringLiteral("visual"), hit.similarity.has_value());
    c.insert(QStringLiteral("similarity"), hit.similarity ? static_cast<double>(*hit.similarity) : 0.0);
    c.insert(QStringLiteral("transcript"), hit.relevance.has_value());
    c.insert(QStringLiteral("snippet"), QString::fromStdString(hit.snippet));
    QString rich;
    for (std::size_t i = 0; i < hit.words.size(); ++i) {
        if (hit.spaced && i > 0) {
            rich += QLatin1Char(' ');
        }
        const QString word = QString::fromStdString(hit.words[i]).toHtmlEscaped();
        rich += i < hit.wordMatched.size() && hit.wordMatched[i] ? QStringLiteral("<b>%1</b>").arg(word) : word;
    }
    c.insert(QStringLiteral("snippetRich"), rich);
    QStringList matched;
    for (const std::string& w : hit.matchedWords) {
        matched.append(QString::fromStdString(w));
    }
    c.insert(QStringLiteral("matched"), matched);
    const QJsonObject drag{{QStringLiteral("media"), media},
                           {QStringLiteral("sourceIn"), QString::number(hit.range.start.value)},
                           {QStringLiteral("sourceOut"), QString::number(hit.range.end().value)}};
    c.insert(QStringLiteral("hit"), QString::fromUtf8(QJsonDocument(drag).toJson(QJsonDocument::Compact)));
    return c;
}

QString sizeText(std::uint64_t bytes) {
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / 1e6, 0, 'f', 0);
}

} // namespace

// --- Shared state (also seen by the thumbnail provider and the assistant's commands) ---------------------------------

struct SearchBridge::Shared {
    std::mutex mutex;
    std::shared_ptr<search::FootageSearch> search;
    bool openTried = false;
    std::string openError;
    std::filesystem::path projectFile;
    std::shared_ptr<const ProjectState> state;

    // The search worker: the newest query wins.
    std::condition_variable wake;
    std::optional<std::pair<quint64, QString>> pending;
    std::stop_source current;
    std::atomic<quint64> generation{0};
    std::atomic<int> jobs{0};

    std::shared_ptr<search::FootageSearch> open() {
        std::lock_guard lock(mutex);
        if (!search && !openTried) {
            openTried = true;
            search::SearchHostOptions options;
            options.projectFile = projectFile;
            auto opened = search::openFootageSearch(options);
            if (opened) {
                search = std::move(*opened);
            } else {
                openError = opened.error().message;
            }
        }
        return search;
    }
};

/// image://wabcut-search/<media key>/<ticks>: the frame of a hit, decoded off the GUI thread.
class SearchThumbnailProvider final : public QQuickImageProvider {
public:
    explicit SearchThumbnailProvider(std::weak_ptr<SearchBridge::Shared> shared)
        : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading),
          m_shared(std::move(shared)) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        QImage image = frame(id);
        if (image.isNull()) {
            image = QImage(16, 9, QImage::Format_RGB888);
            image.fill(Qt::black);
        }
        if (requestedSize.isValid() && !requestedSize.isEmpty()) {
            image = image.scaled(requestedSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }
        if (size) {
            *size = image.size();
        }
        return image;
    }

private:
    QImage frame(const QString& id) {
        const QStringList parts = id.split(QLatin1Char('/'));
        const auto shared = m_shared.lock();
        const auto media = parts.size() == 2 ? detail::parseId<MediaId>(QLatin1Char('m'), parts[0]) : std::nullopt;
        bool ok = false;
        const qint64 ticks = parts.size() == 2 ? parts[1].toLongLong(&ok) : 0;
        if (!shared || !media || !ok) {
            return {};
        }
        std::shared_ptr<search::FootageSearch> searchService;
        std::shared_ptr<const ProjectState> state;
        {
            std::lock_guard lock(shared->mutex);
            searchService = shared->search;
            state = shared->state;
        }
        const MediaItem* item = state ? state->findMedia(*media) : nullptr;
        if (!searchService || !item) {
            return {};
        }
        // Cached with the media's source redactions: a changed redaction is a new picture (Codex wave-4 P2-9).
        const QString key = id + QStringLiteral("#") + QString::number(redacted::keyOf(*state, *media), 16);
        {
            std::lock_guard lock(m_cacheMutex);
            if (const auto it = m_cache.find(key); it != m_cache.end()) {
                return it->second;
            }
        }
        const Ticks times[] = {Ticks{ticks}};
        auto frames = searchService->access()->frames(*item, times, 384, {});
        if (!frames || frames->empty() || !frames->front().valid()) {
            return {};
        }
        const search::Image& picture = frames->front();
        QImage image(picture.rgb.data(), picture.width, picture.height, picture.width * 3, QImage::Format_RGB888);
        image = image.copy(); // own the pixels
        redacted::apply(image, *state, *media, Ticks{ticks});
        std::lock_guard lock(m_cacheMutex);
        m_order.push_back(key);
        m_cache[key] = image;
        while (m_order.size() > 96) {
            m_cache.erase(m_order.front());
            m_order.pop_front();
        }
        return image;
    }

    std::weak_ptr<SearchBridge::Shared> m_shared;
    std::mutex m_cacheMutex;
    std::map<QString, QImage> m_cache;
    std::list<QString> m_order;
};

// --- SearchBridge -----------------------------------------------------------------------------------------------------

SearchBridge* SearchBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SearchBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setProjectFile(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        if (!qmlEngine->imageProvider(QStringLiteral("wabcut-search"))) {
            qmlEngine->addImageProvider(QStringLiteral("wabcut-search"), new SearchThumbnailProvider(bridge->m_shared));
        }
    }
    return bridge;
}

SearchBridge::SearchBridge(QObject* parent) : QObject(parent), m_shared(std::make_shared<Shared>()) {
    m_searchThread = std::jthread([this](std::stop_token stop) { searchLoop(stop); });
}

SearchBridge::~SearchBridge() {
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->current.request_stop();
    }
    m_searchThread.request_stop();
    m_shared->wake.notify_all();
    m_indexThread.request_stop();
    m_downloadThread.request_stop();
    // jthreads join here, before the members they post to go away.
    m_searchThread = {};
    m_indexThread = {};
    m_downloadThread = {};
}

void SearchBridge::setController(ProjectController* controller) {
    if (m_controller == controller) {
        return;
    }
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    const auto update = [this] {
        std::shared_ptr<const ProjectState> next = m_controller ? m_controller->snapshot() : nullptr;
        bool mediaChanged = false;
        {
            std::lock_guard lock(m_shared->mutex);
            const auto& before = m_shared->state;
            mediaChanged = !before || !next || before->media.size() != next->media.size() ||
                           !std::equal(before->media.begin(), before->media.end(), next->media.begin(),
                                       [](const MediaPtr& a, const MediaPtr& b) { return a == b; });
            m_shared->state = std::move(next);
        }
        if (mediaChanged) {
            refreshStatus();
        }
    };
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, update);
    }
    update();
}

void SearchBridge::setProjectFile(ProjectLifecycle* file) {
    if (m_file) {
        disconnect(m_file, nullptr, this, nullptr);
    }
    m_file = file;
    const auto update = [this] {
        const std::filesystem::path path = projectFilePath();
        std::lock_guard lock(m_shared->mutex);
        m_shared->projectFile = path;
        if (m_shared->search) {
            if (auto* access = dynamic_cast<search::MediaSearchAccess*>(m_shared->search->access().get())) {
                access->setProjectFile(path);
            }
        }
    };
    if (file) {
        connect(file, &ProjectLifecycle::documentChanged, this, update);
    }
    update();
}

std::filesystem::path SearchBridge::projectFilePath() const {
    return m_file ? std::filesystem::path(m_file->filePath().toStdWString()) : std::filesystem::path();
}

void SearchBridge::setSearch(std::shared_ptr<search::FootageSearch> searchService) {
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->search = std::move(searchService);
        m_shared->openTried = true;
    }
    emit statusChanged();
    refreshStatus();
}

std::shared_ptr<search::FootageSearch> SearchBridge::footageSearch() { return m_shared->open(); }

std::function<std::shared_ptr<search::FootageSearch>()> SearchBridge::searchOpener() const {
    std::weak_ptr<Shared> shared = m_shared;
    return [shared]() -> std::shared_ptr<search::FootageSearch> {
        const auto s = shared.lock();
        return s ? s->open() : nullptr;
    };
}

// --- Status --------------------------------------------------------------------------------------------------------------

bool SearchBridge::visualAvailable() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->search && m_shared->search->embedder() != nullptr;
}

QString SearchBridge::visualStatus() const {
    std::shared_ptr<search::FootageSearch> s;
    {
        std::lock_guard lock(m_shared->mutex);
        s = m_shared->search;
    }
    if (!s) {
        return tr("Smart search finds what clips show and say, on this computer.");
    }
    if (const auto embedder = s->embedder()) {
        const QString device = QString::fromStdString(embedder->device());
        //: Smart search status: the model and what it runs on, e.g. "SigLIP 2 · DirectML"
        return device.isEmpty() ? tr("SigLIP 2, on this computer") : tr("SigLIP 2 · %1").arg(device);
    }
    search::SearchHostOptions options;
    const std::string reason = search::visualSearchUnavailableReason(options);
    return reason.empty() ? tr("Searching transcripts only") : tr("Searching transcripts only: %1").arg(QString::fromStdString(reason));
}

bool SearchBridge::modelDownloadable() const {
    if (visualAvailable()) {
        return false; // a model is already running
    }
    search::SearchHostOptions options;
    return search::visualSearchUnavailableReason(options) == "the SigLIP 2 model is not downloaded";
}

QString SearchBridge::modelLicense() const {
    const auto files = search::siglipModelFiles();
    return files.empty() || files.front() == nullptr ? QString() : QString::fromStdString(files.front()->licenseCard);
}

QString SearchBridge::modelSize() const {
    std::uint64_t bytes = 0;
    for (const ai::ModelInfo* m : search::siglipModelFiles()) {
        bytes += m ? m->size : 0;
    }
    return sizeText(bytes);
}

void SearchBridge::refreshStatus() {
    std::shared_ptr<search::FootageSearch> s;
    std::shared_ptr<const ProjectState> state;
    {
        std::lock_guard lock(m_shared->mutex);
        s = m_shared->search;
        state = m_shared->state;
    }
    if (m_indexing) {
        return; // the job reports its own progress
    }
    if (!s || !state) {
        m_indexText.clear();
        m_indexDetail.clear();
        m_upToDate = false;
    } else {
        const search::SearchStatus status = s->status(*state);
        m_upToDate = status.indexed == status.media;
        //: Smart search pill: media indexed for search, e.g. "Indexed 9/12"
        m_indexText = status.media == 0 ? QString() : tr("Indexed %1/%2").arg(status.indexed).arg(status.media);
        m_indexDetail = visualStatus();
        m_indexProgress = status.media == 0 ? 0.0 : static_cast<double>(status.indexed) / static_cast<double>(status.media);
    }
    emit indexChanged();
}

// --- Searching -----------------------------------------------------------------------------------------------------------

void SearchBridge::search(const QString& query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        clear();
        return;
    }
    m_query = trimmed;
    {
        std::lock_guard lock(m_shared->mutex);
        const quint64 generation = ++m_shared->generation;
        m_shared->pending = std::make_pair(generation, trimmed);
        m_shared->current.request_stop(); // an older search stops
    }
    m_shared->wake.notify_all();
    if (!m_searching) {
        m_searching = true;
        emit searchingChanged();
    }
}

void SearchBridge::clear() {
    {
        std::lock_guard lock(m_shared->mutex);
        ++m_shared->generation;
        m_shared->pending.reset();
        m_shared->current.request_stop();
    }
    m_query.clear();
    m_results.clear();
    m_message.clear();
    m_searching = false;
    emit searchingChanged();
    emit resultsChanged();
}

void SearchBridge::searchLoop(std::stop_token stop) {
    for (;;) {
        std::pair<quint64, QString> job;
        std::stop_token cancel;
        std::shared_ptr<const ProjectState> state;
        {
            std::unique_lock lock(m_shared->mutex);
            m_shared->wake.wait(lock, [&] { return stop.stop_requested() || m_shared->pending.has_value(); });
            if (stop.stop_requested()) {
                return;
            }
            job = *m_shared->pending;
            m_shared->pending.reset();
            m_shared->current = std::stop_source();
            cancel = m_shared->current.get_token();
            state = m_shared->state;
            ++m_shared->jobs;
        }
        QVariantList cards;
        QString message;
        const auto searchService = m_shared->open();
        if (!searchService) {
            std::lock_guard lock(m_shared->mutex);
            message = tr("Smart search is not available: %1").arg(QString::fromStdString(m_shared->openError));
        } else if (state) {
            auto results = searchService->search(*state, job.second.toStdString(), search::SearchFilters{}, kResultLimit, cancel);
            if (results) {
                for (const search::SearchHit& hit : results->hits) {
                    cards.append(card(hit));
                }
                if (!results->notIndexed.empty()) {
                    //: Smart search: some media of the project are not in the search index yet
                    message = tr("%n clip(s) not indexed yet", nullptr, static_cast<int>(results->notIndexed.size()));
                } else if (cards.isEmpty()) {
                    message = tr("No matches");
                }
            } else if (results.error().code != ErrorCode::Cancelled) {
                message = QString::fromStdString(results.error().message);
            } else {
                --m_shared->jobs;
                continue; // superseded: the newer query answers
            }
        }
        const quint64 generation = job.first;
        QMetaObject::invokeMethod(
            this, [this, generation, query = job.second, cards = std::move(cards), message] { publishResults(generation, query, cards, message); },
            Qt::QueuedConnection);
        --m_shared->jobs;
    }
}

void SearchBridge::publishResults(quint64 generation, QString query, QVariantList results, QString message) {
    if (generation != m_shared->generation.load()) {
        return; // a newer query (or clear()) came after this one
    }
    m_query = std::move(query);
    m_results = std::move(results);
    m_message = std::move(message);
    m_searching = false;
    emit searchingChanged();
    emit resultsChanged();
}

// --- Indexing ------------------------------------------------------------------------------------------------------------

void SearchBridge::ensureIndexed() {
    if (m_indexing) {
        return;
    }
    const auto searchService = footageSearch();
    std::shared_ptr<const ProjectState> state;
    {
        std::lock_guard lock(m_shared->mutex);
        state = m_shared->state;
    }
    if (!searchService || !state) {
        refreshStatus();
        return;
    }
    const search::SearchStatus status = searchService->status(*state);
    if (status.media == 0 || status.indexed == status.media) {
        refreshStatus();
        return;
    }
    m_indexing = true;
    m_indexProgress = 0.0;
    //: Smart search pill while indexing starts
    m_indexText = tr("Indexing 0/%1").arg(status.media);
    m_indexDetail = visualStatus();
    emit indexChanged();
    ++m_shared->jobs;
    m_indexThread = std::jthread([this, searchService, state](std::stop_token stop) {
        auto report = searchService->index(*state, {},
                                           [this, &stop](const search::IndexProgress& p) {
                                               const QString text = tr("Indexing %1/%2").arg(p.index + 1).arg(p.count);
                                               const QString detail = QString::fromStdString(p.media);
                                               const double fraction = p.fraction;
                                               QMetaObject::invokeMethod(
                                                   this,
                                                   [this, text, detail, fraction] {
                                                       if (!m_indexing) {
                                                           return;
                                                       }
                                                       m_indexText = text;
                                                       m_indexDetail = detail;
                                                       m_indexProgress = fraction;
                                                       emit indexChanged();
                                                   },
                                                   Qt::QueuedConnection);
                                               return !stop.stop_requested();
                                           },
                                           stop);
        QString error;
        if (!report) {
            error = report.error().code == ErrorCode::Cancelled ? QString() : QString::fromStdString(report.error().message);
        } else if (!report->errors.empty()) {
            error = QString::fromStdString(report->errors.front());
        }
        QMetaObject::invokeMethod(this, [this, error] { finishIndexing(error); }, Qt::QueuedConnection);
        --m_shared->jobs;
    });
}

void SearchBridge::cancelIndexing() { m_indexThread.request_stop(); }

void SearchBridge::finishIndexing(QString error) {
    m_indexing = false;
    refreshStatus();
    if (!error.isEmpty()) {
        m_indexDetail = error;
        emit indexChanged();
    }
    if (!m_query.isEmpty()) {
        search(m_query); // the new entries count now
    }
}

// --- The model -----------------------------------------------------------------------------------------------------------

void SearchBridge::downloadModel() {
    if (m_downloading) {
        return;
    }
    auto* ai = qmlEngine(this) ? qmlEngine(this)->singletonInstance<AiController*>("Wabcut", "AiController") : nullptr;
    if (!ai) {
        emit notice(tr("The model cannot be downloaded here."));
        return;
    }
    if (ai->offlineMode()) {
        emit notice(tr("Offline mode is on: turn it off in Preferences > AI to download the search model."));
        return;
    }
    m_downloading = true;
    m_downloadProgress = 0.0;
    emit downloadChanged();
    ++m_shared->jobs;
    ai::IHttpTransport* network = &ai->transport();
    // The download borrows the transport: stopped and joined before AiController destroys it (A5-P2-19).
    disconnect(ai, &AiController::shuttingDown, this, nullptr);
    connect(ai, &AiController::shuttingDown, this, [this] {
        m_downloadThread.request_stop();
        if (m_downloadThread.joinable()) {
            m_downloadThread.join();
        }
    });
    m_downloadThread = std::jthread([this, network](std::stop_token stop) {
        ai::HttpDownloadTransport transport(*network);
        ai::ModelStore store(ai::ModelStore::defaultRoot());
        const auto files = search::siglipModelFiles();
        std::uint64_t total = 0;
        for (const ai::ModelInfo* m : files) {
            total += m ? m->size : 0;
        }
        std::uint64_t done = 0;
        QString error;
        for (const ai::ModelInfo* m : files) {
            if (!m) {
                continue;
            }
            auto ensured = store.ensure(
                *m, &transport,
                [this, done, total](const ai::DownloadProgress& p) {
                    const double fraction = total ? static_cast<double>(done + p.received) / static_cast<double>(total) : 0.0;
                    QMetaObject::invokeMethod(
                        this, [this, fraction] { m_downloadProgress = fraction; emit downloadChanged(); }, Qt::QueuedConnection);
                },
                stop);
            if (!ensured) {
                error = ensured.error().code == ErrorCode::Cancelled ? QString() : QString::fromStdString(ensured.error().message);
                break;
            }
            done += m->size;
        }
        QMetaObject::invokeMethod(
            this,
            [this, error, cancelled = stop.stop_requested()] {
                m_downloading = false;
                emit downloadChanged();
                if (!error.isEmpty()) {
                    emit notice(tr("The search model could not be downloaded: %1").arg(error));
                } else if (!cancelled) {
                    // Visual search on: the helper with the downloaded model, then index the frames.
                    if (const auto s = footageSearch(); s && modelDownloadable() == false) {
                        search::HelperEmbedderOptions options = search::siglipOptions(ai::ModelStore(ai::ModelStore::defaultRoot()));
                        options.threads = std::max(1, QThread::idealThreadCount() / 2);
                        s->setEmbedder(std::make_shared<search::HelperEmbedder>(std::move(options)));
                        s->setVisualUnavailableReason({});
                    }
                    emit statusChanged();
                    ensureIndexed();
                }
            },
            Qt::QueuedConnection);
        --m_shared->jobs;
    });
}

void SearchBridge::cancelDownload() { m_downloadThread.request_stop(); }

// --- Drops ---------------------------------------------------------------------------------------------------------------

bool SearchBridge::dropHit(const QString& hit, const QString& timecode, const QString& trackId, bool overwrite) {
    const QJsonObject json = QJsonDocument::fromJson(hit.toUtf8()).object();
    const auto media = detail::parseId<MediaId>(QLatin1Char('m'), json.value(QStringLiteral("media")).toString());
    bool inOk = false;
    bool outOk = false;
    const qint64 in = json.value(QStringLiteral("sourceIn")).toString().toLongLong(&inOk);
    const qint64 out = json.value(QStringLiteral("sourceOut")).toString().toLongLong(&outOk);
    const SequencePtr sequence = m_controller ? m_controller->currentSequence() : nullptr;
    if (!media || !inOk || !outOk || out <= in || !sequence) {
        return false;
    }
    const auto at = parseTimecode(timecode.toStdString(), sequence->frameRate, sequence->dropFrame);
    if (!at) {
        emit notice(QString::fromStdString(at.error().message));
        return false;
    }
    editor::MediaDrop drop;
    drop.media = *media;
    drop.sequence = sequence->id;
    drop.at = *at;
    drop.track = parseTrackKey(trackId).value_or(TrackId{});
    drop.mode = overwrite ? editor::DropMode::Overwrite : editor::DropMode::Insert;
    drop.sourceRange = TimeRange::fromStartEnd(Ticks{in}, Ticks{out});
    auto edited = editor::dropMedia(m_controller->session(), drop);
    if (!edited) {
        emit notice(QString::fromStdString(edited.error().message));
        return false;
    }
    m_controller->setSelection(edited->placed, edited->placed.empty() ? ClipId{} : edited->placed.front());
    return true;
}

bool SearchBridge::waitForIdle(int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (m_shared->jobs.load() == 0 && !m_searching && !m_indexing && !m_downloading) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return true;
        }
        QThread::msleep(5);
    }
    return false;
}

} // namespace wabcut::app
