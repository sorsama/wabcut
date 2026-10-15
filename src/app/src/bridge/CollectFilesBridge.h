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
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

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
    /// trimmed, size, notes: [...] }; empty before the first.
    Q_PROPERTY(QVariantMap result READ result NOTIFY stateChanged FINAL)
    /// Where the dialog proposes to put the capsule: the project's folder (or Documents).
    Q_PROPERTY(QUrl suggestedFolder READ suggestedFolder NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString suggestedName READ suggestedName NOTIFY stateChanged FINAL)

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

    /// options: { name, trim (bool), handles (seconds), proxies (bool), fonts (bool), analysis (bool), unused (bool:
    /// include media no sequence uses) }. "About 1.4 GB of 210 GB free".
    Q_INVOKABLE QString estimate(const QUrl& folder, const QVariantMap& options);
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
    void finished(bool ok);

private:
    struct Inputs;
    std::expected<Inputs, QString> gather(const QUrl& folder, const QVariantMap& options);
    void onProgress(const editor::CapsuleProgress& p);
    void onDone(const std::expected<editor::CapsuleResult, Error>& r);

    bool m_running = false;
    double m_progress = 0.0;
    QString m_phase;
    QString m_current;
    QVariantMap m_result;
    std::filesystem::path m_lastFolder;
    std::filesystem::path m_tempDir; // analysis sidecars of the running job
    struct Gate {
        std::mutex mutex;
        CollectFilesBridge* owner = nullptr;
    };
    std::shared_ptr<Gate> m_gate = std::make_shared<Gate>();
    std::jthread m_job;
};

} // namespace wabcut::app
