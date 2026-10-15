#pragma once

// Smart search in the app (AI-8, docs/ai.md "Smart search"): the `SmartSearch` QML singleton behind the Library's
// Smart toggle, over ai::search::FootageSearch (wabcut_ai_search_media).
//
//   * search(query): runs on a worker thread, the newest query wins (an older one is cancelled); results are result
//     cards (matched-frame thumbnail, source-range badge, transcript snippet with the matched words) that drag onto the
//     timeline as "application/x-wabcut-search-hit" and are inserted there with their source range (dropHit).
//   * indexing: ensureIndexed() when Smart is turned on indexes what is missing in the background; the pill shows
//     "Indexing 3/12" with progress, then "Indexed 12/12". Everything runs on this computer.
//   * the model: SigLIP 2 is downloaded on first use (downloadModel(), after the license card) into the ModelStore,
//     verified by SHA-256; without it (or without the wabcut-embed helper) transcripts are still searched.
//
// Thumbnails come from the image provider "wabcut-search" (image://wabcut-search/<media key>/<ticks>), which decodes
// the matched frame through the search's media access.

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace wabcut::ai::search {
class FootageSearch;
}

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

class SearchBridge : public QObject {
    Q_OBJECT
    /// Visual search runs (helper built, model downloaded).
    Q_PROPERTY(bool visualAvailable READ visualAvailable NOTIFY statusChanged FINAL)
    /// "SigLIP 2 · DirectML", or why visual search is off.
    Q_PROPERTY(QString visualStatus READ visualStatus NOTIFY statusChanged FINAL)
    /// The helper is built but the model is missing: offer the download.
    Q_PROPERTY(bool modelDownloadable READ modelDownloadable NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString modelLicense READ modelLicense CONSTANT FINAL)
    Q_PROPERTY(QString modelSize READ modelSize CONSTANT FINAL)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadChanged FINAL)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY downloadChanged FINAL)

    Q_PROPERTY(QString query READ query NOTIFY resultsChanged FINAL)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged FINAL)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged FINAL)
    /// What the last search wants the user to know ("2 clips are not indexed yet", an error); "" = nothing.
    Q_PROPERTY(QString message READ message NOTIFY resultsChanged FINAL)

    Q_PROPERTY(bool indexing READ indexing NOTIFY indexChanged FINAL)
    Q_PROPERTY(double indexProgress READ indexProgress NOTIFY indexChanged FINAL)
    /// The pill: "Indexing 3/12", "Indexed 12/12", "" when the project has no media.
    Q_PROPERTY(QString indexText READ indexText NOTIFY indexChanged FINAL)
    Q_PROPERTY(QString indexDetail READ indexDetail NOTIFY indexChanged FINAL)
    Q_PROPERTY(bool upToDate READ upToDate NOTIFY indexChanged FINAL)

public:
    static SearchBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit SearchBridge(QObject* parent = nullptr);
    ~SearchBridge() override; ///< cancels the jobs and waits for them

    void setController(ProjectController* controller);
    void setProjectFile(ProjectLifecycle* file);
    /// Replaces the search (tests: fixtures); the default is opened on first use (openFootageSearch).
    void setSearch(std::shared_ptr<ai::search::FootageSearch> search);
    /// The search the commands and the assistant use (opened on first use; null when the index cannot open).
    std::shared_ptr<ai::search::FootageSearch> footageSearch();
    /// footageSearch() for other threads (the assistant's commands): safe to call from any thread, after this bridge
    /// is gone too (it then reports that smart search is not available).
    std::function<std::shared_ptr<ai::search::FootageSearch>()> searchOpener() const;

    bool visualAvailable() const;
    QString visualStatus() const;
    bool modelDownloadable() const;
    QString modelLicense() const;
    QString modelSize() const;
    bool downloading() const { return m_downloading; }
    double downloadProgress() const { return m_downloadProgress; }
    QString query() const { return m_query; }
    QVariantList results() const { return m_results; }
    bool searching() const { return m_searching; }
    QString message() const { return m_message; }
    bool indexing() const { return m_indexing; }
    double indexProgress() const { return m_indexProgress; }
    QString indexText() const { return m_indexText; }
    QString indexDetail() const { return m_indexDetail; }
    bool upToDate() const { return m_upToDate; }

    /// Searches (asynchronously; the newest query wins). An empty query clears the results.
    Q_INVOKABLE void search(const QString& query);
    Q_INVOKABLE void clear();
    /// Indexes whatever media is not indexed yet, in the background (no-op while indexing).
    Q_INVOKABLE void ensureIndexed();
    Q_INVOKABLE void cancelIndexing();
    /// Recounts what is indexed (the pill).
    Q_INVOKABLE void refreshStatus();
    /// Downloads SigLIP 2 (412 MB, verified) into the model store; then visual search turns on.
    Q_INVOKABLE void downloadModel();
    Q_INVOKABLE void cancelDownload();
    /// Inserts (overwrite: overwrites) a dragged hit at `timecode` on `trackId`, with its source range: one undo step,
    /// like a library drop. `hit` is the card's "hit" string.
    Q_INVOKABLE bool dropHit(const QString& hit, const QString& timecode, const QString& trackId, bool overwrite);

    /// Tests: waits until no search, index or download job runs.
    bool waitForIdle(int timeoutMs);

    /// State shared with the worker threads, the thumbnail provider and searchOpener() (SearchBridge.cpp).
    struct Shared;

signals:
    void statusChanged();
    void downloadChanged();
    void resultsChanged();
    void searchingChanged();
    void indexChanged();
    /// A dropped hit could not be placed, a download failed, ...
    void notice(const QString& text);

private:
    void searchLoop(std::stop_token stop);
    void publishResults(quint64 generation, QString query, QVariantList results, QString message);
    void finishIndexing(QString error);
    std::filesystem::path projectFilePath() const;

    std::shared_ptr<Shared> m_shared;
    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_file;

    QString m_query;
    QVariantList m_results;
    QString m_message;
    bool m_searching = false;
    bool m_indexing = false;
    double m_indexProgress = 0.0;
    QString m_indexText;
    QString m_indexDetail;
    bool m_upToDate = false;
    bool m_downloading = false;
    double m_downloadProgress = 0.0;

    std::jthread m_searchThread;
    std::jthread m_indexThread;
    std::jthread m_downloadThread;
};

/// QML registration through create().
struct SearchBridgeForeign {
    Q_GADGET
    QML_FOREIGN(SearchBridge)
    QML_NAMED_ELEMENT(SmartSearch)
    QML_SINGLETON
public:
    static SearchBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return SearchBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
