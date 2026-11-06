#pragma once

// CollectFilesBridge: File > Collect Files… (I.4), the portable project capsule, for QML (singleton `Collect`).
// Owner: Graphics/playback. The work is editor::collectProject() on a worker thread; this class gathers its inputs on
// the GUI thread - the project as it would be saved (with the proxy registry, MediaReadiness), the fonts its titles and
// captions use (Windows' font registry; fonts that ship with Windows are listed, not copied: they may not be
// redistributed), the analysis results of its media (AnalysisCache sidecars) - and reports progress and the
// verification result.

#include <wabcut/editor/Capsule.h>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

/// Card #173: the analysis results (transcripts, shots, beats) of the media in `only` (sorted by id; see
/// editor::capsuleMedia: the media the capsule really holds) as portable sidecars, one per media file, written to
/// `tempDir`; the cache is the one under `cacheRoot`. Reads the cache and the files: call on a worker.
std::vector<editor::CapsuleExtraFile> collectAnalysisSidecars(const ProjectState& state, const std::vector<MediaId>& only,
                                                              const std::filesystem::path& tempDir,
                                                              const std::filesystem::path& cacheRoot);
/// What those sidecars would take, from the cache's entry sizes: nothing is written, hashed or parsed.
std::int64_t collectAnalysisSidecarBytes(const ProjectState& state, const std::vector<MediaId>& only,
                                         const std::function<bool()>& keepGoing, const std::filesystem::path& cacheRoot);

class CollectFilesBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Collect)
    QML_SINGLETON

    Q_PROPERTY(bool running READ running NOTIFY stateChanged FINAL)
    /// 0..1 while running.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    /// "Checking free space", "Copying", "Trimming", "Saving the project", "Verifying the capsule".
    Q_PROPERTY(QString phase READ phase NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString current READ current NOTIFY progressChanged FINAL)
    /// The last run: { ok, cancelled, error, folder (url), project (url), verified, offline, mismatches, files, media,
    /// trimmed, sequences: [names], size, notes: [...] }; empty before the first.
    Q_PROPERTY(QVariantMap result READ result NOTIFY stateChanged FINAL)
    /// Where the dialog proposes to put the capsule: the project's folder (or Documents).
    Q_PROPERTY(QUrl suggestedFolder READ suggestedFolder NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString suggestedName READ suggestedName NOTIFY stateChanged FINAL)
    /// 0.7 (card #173): the size estimate requestEstimate() works out on a worker thread ("About 1.4 GB (210 GB free)";
    /// the project's error text when there is nothing to collect), and whether one is being worked out.
    Q_PROPERTY(QString estimateText READ estimateText NOTIFY estimateChanged FINAL)
    Q_PROPERTY(bool estimating READ estimating NOTIFY estimateChanged FINAL)

public:
    explicit CollectFilesBridge(QObject* parent = nullptr);
    ~CollectFilesBridge() override;
    static CollectFilesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool running() const { return m_running; }
    double progress() const { return m_progress; }
    QString phase() const { return m_phase; }
    QString current() const { return m_current; }
    QVariantMap result() const { return m_result; }
    QUrl suggestedFolder() const;
    QString suggestedName() const;
    QString estimateText() const { return m_estimateText; }
    bool estimating() const { return m_estimating; }

    /// options: { name, trim (bool), handles (seconds, 0 to 10; with trim), proxies (bool), fonts (bool), analysis
    /// (bool), unused (bool: include media no sequence uses), sequences (indices in ProjectController.sequenceNames of
    /// the sequences to collect, with what they play; absent: all of them; a list naming none is refused) }. "About 1.4 GB of 210 GB free". Works the size out right here, on the calling
    /// thread (tests, scripts); the dialog uses requestEstimate().
    Q_INVOKABLE QString estimate(const QUrl& folder, const QVariantMap& options);
    /// 0.7 (card #173): the same on a worker thread, so toggling an option never freezes the window on a slow or sleeping
    /// drive. The answer lands in estimateText; a newer request replaces an older one that is still running (it stops
    /// early and its answer is dropped). Ignored while collecting.
    Q_INVOKABLE void requestEstimate(const QUrl& folder, const QVariantMap& options);
    /// Starts collecting into `folder` (the capsule folder is created inside it). False when already running or the
    /// project is not available.
    Q_INVOKABLE bool start(const QUrl& folder, const QVariantMap& options);
    Q_INVOKABLE void cancel();
    /// Shows the last capsule in the file manager.
    Q_INVOKABLE bool reveal();

    /// Tests: waits for the running job (processing events). False on timeout.
    bool waitFinished(int timeoutMs);
    /// The fonts titles and captions of the current project use, resolved to files: { family: [files] } (Windows),
    /// and whether each is a Windows font that is not copied.
    QVariantList fontsInUse() const;

signals:
    void stateChanged();
    void progressChanged();
    void estimateChanged();
    void finished(bool ok);

private:
    struct Inputs;
    std::expected<Inputs, QString> gather(const QUrl& folder, const QVariantMap& options);
    void onProgress(const editor::CapsuleProgress& p);
    void onDone(const std::expected<editor::CapsuleResult, Error>& r);
    void onEstimate(std::uint64_t generation, std::int64_t bytes, std::int64_t freeBytes);
    QString estimateSentence(std::int64_t bytes, std::int64_t freeBytes) const;

    bool m_running = false;
    double m_progress = 0.0;
    QString m_phase;
    QString m_current;
    QVariantMap m_result;
    std::filesystem::path m_lastFolder;
    std::filesystem::path m_tempDir; // analysis sidecars of the running job
    QString m_estimateText;
    bool m_estimating = false;
    std::uint64_t m_estimateGeneration = 0;
    /// The newest estimate request, readable by a worker that should stop because a newer one was made.
    std::shared_ptr<std::atomic<std::uint64_t>> m_estimateLatest = std::make_shared<std::atomic<std::uint64_t>>(0);
    struct Gate {
        std::mutex mutex;
        CollectFilesBridge* owner = nullptr;
    };
    std::shared_ptr<Gate> m_gate = std::make_shared<Gate>();
    std::jthread m_job;
};

} // namespace wabcut::app
