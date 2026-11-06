#pragma once

// LocalModelsController: the QML singleton "LocalModels" behind Settings > AI > Local models (r07, docs/ai.md "Local
// model library"; qml/panels/ai/LocalLlmPage.qml and qml/panels/ai/local/).
//
//   - Recommended: the curated, tested local assistant models (ModelStore's tiers and Phi-4 mini) with their fit on
//     this computer, the recommended one first.
//   - Browse: a Hugging Face search of GGUF models (wabcut/ai/ModelLibrary.h), sorted and filtered here, with each
//     model's details (quantizations, fit, benchmarks with their source, licence) on demand.
//   - Transcription: the whisper.cpp models of ModelStore's manifest; the chosen one is what local transcription uses
//     (transcriptionModelId(); "" = automatic).
//   - Downloads: one queue (wabcut/ai/ModelDownloads.h) with progress, pause / resume, cancel; models land in the
//     storage folder (ModelStore::defaultRoot()) and, when done, in the library index, ModelStore's registered models
//     and AiController's registry (provider "local-llm"), so routing and the local assistant see them.
//   - Installed: what is on disk, with its size, Remove, and per-model launch settings (context, GPU layers, threads).
//
// Everything that leaves the computer goes through AiController::transport() (proxies, TLS, the redacted request log,
// offline mode and the project's no-cloud lock). A Hugging Face access token (provider id "huggingface" in the key
// ring, when one is saved) is sent to https://huggingface.co only, for gated models. Searches, details and downloads run
// off the GUI thread; results come back through queued calls, newest request wins.

#include <wabcut/ai/ModelLibrary.h>
#include <wabcut/ai/llm/SystemProbe.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class AiController;
class QJSEngine;
class QQmlEngine;

namespace wabcut::ai {
class DownloadQueue;
class IHttpTransport;
struct DownloadState;
} // namespace wabcut::ai

namespace wabcut::app {

/// AiController::load() calls it at start: the storage folder (QSettings ai/localModelFolder), the launch settings of
/// the curated models (ai/localLaunch/<id>) and every downloaded library model (index -> ModelStore registration,
/// launch settings, AiController::registerLocalModel).
void loadLocalModelLibrary(AiController& ai);

class LocalModelsController : public QObject {
    Q_OBJECT

    /// Curated local assistant models: [{id, kind: "curated", name, repo, publisher, params, size, sizeBytes, quant,
    /// license, licenseName, permissive, tasks, taskNames, score, scoreText, fit ("gpu"|"cpu"|"no"|"unknown"), fitText,
    /// ram, vram, installed, downloading, progress, recommended, inUse, toolCalls}]
    Q_PROPERTY(QVariantList recommended READ recommended NOTIFY libraryChanged)
    /// The browse list after filters and sorting: rows like `recommended` (kind "hub") plus downloads, downloadsText,
    /// likes, likesText, updated, created, gated, publisherKind, curated, ownEvals.
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    /// How many results the filters hide.
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY resultsChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY resultsChanged)
    /// What the browse list says about itself: an error, "No GGUF model matches", offline, ...
    Q_PROPERTY(QString searchMessage READ searchMessage NOTIFY resultsChanged)
    /// A search ran this session (the list is not just empty because nothing was asked yet).
    Q_PROPERTY(bool searched READ searched NOTIFY resultsChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY resultsChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    /// "recommended", "benchmark", "downloads", "likes", "trending", "newest", "updated", "smallest"
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY queryChanged)
    /// {task, fitsOnly, size, params, quant, permissiveOnly, language, publisher, installedOnly}
    Q_PROPERTY(QVariantMap filters READ filters NOTIFY queryChanged)
    /// Choices for the controls: [{id, name}] each.
    Q_PROPERTY(QVariantList sortOptions READ sortOptions CONSTANT)
    Q_PROPERTY(QVariantList taskOptions READ taskOptions CONSTANT)
    Q_PROPERTY(QVariantList sizeOptions READ sizeOptions CONSTANT)
    Q_PROPERTY(QVariantList paramOptions READ paramOptions CONSTANT)
    Q_PROPERTY(QVariantList quantOptions READ quantOptions CONSTANT)
    Q_PROPERTY(QVariantList publisherOptions READ publisherOptions CONSTANT)
    Q_PROPERTY(QVariantList languageOptions READ languageOptions CONSTANT)

    /// The open model card (openModel / openInstalled / openCurated): {id, kind, name, repo, ..., loading, error,
    /// variants: [{quant, size, sizeBytes, bits, files, fit, fitText, isDefault, installed}], defaultQuant,
    /// projector {size, path} or null, scores: [{metric, name, description, value, mode, note, source}], scoreSource,
    /// scoreDate, scoreNote, hubUrl, revision, contextLength, architecture, baseModel, relation, licenseNeedsAcceptance}
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)

    /// The whisper.cpp models: [{id, name, size, sizeBytes, installed, selected, recommended, englishOnly, downloading,
    /// progress}]
    Q_PROPERTY(QVariantList whisperModels READ whisperModels NOTIFY libraryChanged)
    /// The local transcription model the user chose ("" = automatic) and what that means now.
    Q_PROPERTY(QString whisperModel READ whisperModel WRITE setWhisperModel NOTIFY libraryChanged)
    Q_PROPERTY(QString whisperModelName READ whisperModelName NOTIFY libraryChanged)

    /// [{id, name, kind ("assistant"|"transcription"), quant, size, sizeBytes, license, inUse, library, context,
    /// gpuLayers, threads}]
    Q_PROPERTY(QVariantList installed READ installed NOTIFY libraryChanged)
    /// [{id, name, status ("queued"|"running"|"paused"|"done"|"failed"), progress, receivedText, speedText, etaText,
    /// error, canPause, canResume}]
    Q_PROPERTY(QVariantList downloads READ downloads NOTIFY downloadsChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadsChanged)

    Q_PROPERTY(QString storageFolder READ storageFolder NOTIFY libraryChanged)
    /// "12.3 GB used here · 120.4 GB free"
    Q_PROPERTY(QString storageText READ storageText NOTIFY libraryChanged)
    Q_PROPERTY(bool customStorage READ customStorage NOTIFY libraryChanged)
    /// "NVIDIA GeForce RTX 3060, 9.8 GB free · 24.1 GB RAM free · 16 threads"
    Q_PROPERTY(QString machine READ machine NOTIFY libraryChanged)
    /// Offline mode or the project's no-cloud lock: no search, no download.
    Q_PROPERTY(bool offline READ offline NOTIFY offlineChanged)
    /// A Hugging Face access token is available (key ring entry "huggingface" or WABCUT_HUGGINGFACE_API_KEY).
    Q_PROPERTY(bool hasHubToken READ hasHubToken NOTIFY libraryChanged)

public:
    explicit LocalModelsController(QObject* parent = nullptr);
    ~LocalModelsController() override;
    static LocalModelsController* create(QQmlEngine* engine, QJSEngine* jsEngine);

    void setAiController(AiController* ai);
    /// Tests: the HTTP transport for searches and downloads instead of AiController::transport(), a fixed machine and
    /// free disk space, no Hugging Face token lookup.
    void setTransportForTesting(wabcut::ai::IHttpTransport* transport);
    void setProbeForTesting(std::optional<wabcut::ai::llm::SystemProbe> probe);
    void setFreeSpaceForTesting(std::optional<std::uint64_t> bytes);

    /// The whisper model local transcription uses: the chosen one (QSettings ai/localWhisperModel) or, automatically,
    /// large-v3-turbo (5-bit) when one is installed or a GPU with 2 GB free is there, else base. TranscriptBridge reads it.
    static QString transcriptionModelId();
    /// What "automatic" picks now.
    static QString automaticWhisperModel();

    QVariantList recommended() const { return m_recommended; }
    QVariantList results() const { return m_results; }
    int hiddenCount() const { return m_hidden; }
    bool searching() const { return m_searching; }
    QString searchMessage() const { return m_searchMessage; }
    bool searched() const { return m_searched; }
    bool hasMore() const { return !m_nextPage.isEmpty(); }
    QString query() const { return m_query; }
    void setQuery(const QString& query);
    QString sortKey() const;
    void setSortKey(const QString& key);
    QVariantMap filters() const;
    QVariantList sortOptions() const;
    QVariantList taskOptions() const;
    QVariantList sizeOptions() const;
    QVariantList paramOptions() const;
    QVariantList quantOptions() const;
    QVariantList publisherOptions() const;
    QVariantList languageOptions() const;
    QVariantMap detail() const { return m_detail; }
    QVariantList whisperModels() const { return m_whisper; }
    QString whisperModel() const;
    void setWhisperModel(const QString& id);
    QString whisperModelName() const;
    QVariantList installed() const { return m_installed; }
    QVariantList downloads() const { return m_downloads; }
    bool downloading() const;
    QString storageFolder() const;
    QString storageText() const { return m_storageText; }
    bool customStorage() const;
    QString machine() const { return m_machine; }
    bool offline() const;
    bool hasHubToken() const { return m_hasToken; }

    /// Probes the computer again and re-reads what is installed (the page calls it when it opens).
    Q_INVOKABLE void refresh();
    /// A new Hugging Face search with the query and sort order.
    Q_INVOKABLE void search();
    Q_INVOKABLE void loadMore();
    /// Sets one filter ("task", "fitsOnly", "size", "params", "quant", "permissiveOnly", "language", "publisher",
    /// "installedOnly"); the list updates without a new search.
    Q_INVOKABLE void setFilter(const QString& key, const QVariant& value);
    Q_INVOKABLE void resetFilters();

    /// Model cards: a Hugging Face repository (fetched), a curated model, a downloaded library model (offline).
    Q_INVOKABLE void openModel(const QString& repo);
    Q_INVOKABLE void openCurated(const QString& id);
    Q_INVOKABLE void openInstalled(const QString& id);
    Q_INVOKABLE void closeModel();

    /// Queues a download: a quantization of the open Hugging Face model (with its vision projector when `withVision`),
    /// a curated model or a whisper model. Returns "" or why not (offline, licence not accepted, already there, ...).
    /// `use`: when it is done, use it (the assistant starts / transcription switches to it).
    Q_INVOKABLE QString download(const QString& repo, const QString& quant, bool acceptLicense = false, bool withVision = true,
                                 bool use = false);
    Q_INVOKABLE QString downloadCurated(const QString& id, bool use = false);
    Q_INVOKABLE QString downloadWhisper(const QString& id, bool use = false);
    Q_INVOKABLE bool pause(const QString& id);
    Q_INVOKABLE bool resume(const QString& id);
    Q_INVOKABLE bool cancel(const QString& id);
    Q_INVOKABLE void clearFinished();
    /// Deletes a downloaded model (library, curated or whisper). Returns "" or why not.
    Q_INVOKABLE QString remove(const QString& id);

    /// Starts the local assistant with a downloaded model (and makes it the assistant's model when it can edit).
    Q_INVOKABLE QString useForAssistant(const QString& id);
    /// Makes a whisper model the local transcription model ("" = automatic); downloads it first when needed.
    Q_INVOKABLE QString useForTranscription(const QString& id);

    /// {context, gpuLayers, threads} (0 / -1 = automatic) of a downloaded or curated model.
    Q_INVOKABLE QVariantMap launchOptions(const QString& id) const;
    Q_INVOKABLE bool setLaunchOptions(const QString& id, int context, int gpuLayers, int threads);

    /// The storage folder for new downloads (a path or a file:// URL); "" restores the default. Models already
    /// downloaded stay where they are (the page says so). False for a folder that cannot be created or written.
    Q_INVOKABLE bool setStorageFolder(const QString& folder);
    /// A URL to open the storage folder in the file manager.
    Q_INVOKABLE QString storageFolderUrl() const;

signals:
    void libraryChanged();
    void resultsChanged();
    void queryChanged();
    void detailChanged();
    void downloadsChanged();
    void offlineChanged();
    /// A download finished: `ok` false with a reason for people.
    void downloadFinished(const QString& id, bool ok, const QString& message);
    /// A "use when done" or Use action ran: what happened, for a toast or the status line.
    void notice(const QString& message);

private:
    struct Pending; // what a finished download installs
    struct Hub;     // the transports
    wabcut::ai::IHttpTransport* transport();
    wabcut::ai::DownloadQueue& queue();
    wabcut::ai::llm::SystemProbe probe() const;
    void rebuildResults();
    void rebuildLibrary();
    void updateDownloads();
    void finished(const wabcut::ai::DownloadState& state);
    QString enqueue(const QString& id, const QString& name, std::vector<wabcut::ai::ModelInfo> files, bool use);
    void addRuntime(std::vector<wabcut::ai::ModelInfo>& files, std::uint64_t weightsBytes, double paramsB) const;
    QVariantMap entryRow(const wabcut::ai::library::Entry& entry) const;
    QVariantMap detailFor(const wabcut::ai::library::HubModel& model, const wabcut::ai::library::BenchmarkFamily* family,
                          const QString& kind) const;
    void readHubToken();
    void runSearch(const QString& url, bool append);
    QString fitKey(wabcut::ai::library::Fit fit) const;
    QString humanError(const QString& raw) const;

    QPointer<AiController> m_ai;
    wabcut::ai::IHttpTransport* m_testTransport = nullptr;
    std::optional<wabcut::ai::llm::SystemProbe> m_testProbe;
    std::optional<std::uint64_t> m_testFreeSpace;
    mutable std::optional<wabcut::ai::llm::SystemProbe> m_probe;
    std::unique_ptr<Hub> m_hub;
    std::unique_ptr<wabcut::ai::DownloadQueue> m_queue;
    std::map<std::string, std::unique_ptr<Pending>> m_pending;
    std::shared_ptr<std::atomic<int>> m_generation = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> m_detailGeneration = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    QVariantList m_recommended;
    QVariantList m_results;
    QVariantList m_whisper;
    QVariantList m_installed;
    QVariantList m_downloads;
    QVariantMap m_detail;
    std::optional<wabcut::ai::library::HubModel> m_detailModel; // the open Hugging Face model, with its files
    std::vector<wabcut::ai::library::Entry> m_entries;          // every search result, before filters
    wabcut::ai::library::Filter m_filter;
    wabcut::ai::library::SortKey m_sort = wabcut::ai::library::SortKey::Recommended;
    QString m_query;
    QString m_nextPage;
    QString m_searchMessage;
    QString m_storageText;
    QString m_machine;
    int m_hidden = 0;
    bool m_searching = false;
    bool m_searched = false;
    bool m_hasToken = false;
};

/// QML registration through create() (the class stays default-constructible for C++ and tests).
struct LocalModelsControllerForeign {
    Q_GADGET
    QML_FOREIGN(wabcut::app::LocalModelsController)
    QML_NAMED_ELEMENT(LocalModels)
    QML_SINGLETON

public:
    static LocalModelsController* create(QQmlEngine* engine, QJSEngine* jsEngine) {
        return LocalModelsController::create(engine, jsEngine);
    }
};

} // namespace wabcut::app
