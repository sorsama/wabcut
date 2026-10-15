#pragma once

// HistoryBridge (M5m, QML singleton `History`): the project's version history (C.9, History.h) in the Review panel.
//
//   * The history lives next to the saved project file (ProjectHistory::folderFor); an unsaved project has none
//     (`available` false and `reason` says why). It is opened again when the project file changes (open, save as).
//   * Checkpoints (history.checkpoint, never an undo step): when the project is saved, every `autoIntervalSeconds` of
//     editing (only when something changed since the last one), before an AI agent applies a plan
//     (checkpointBeforeAgent()), and "Save version" with a name.
//   * The list (newest first), naming a version, Restore (history.restore: one undo step; the project as it was is kept
//     as a version first), Restore a sequence as a new one, Export a version as a project file (history.export).
//   * Compare two versions (or a version and the project as it is now): the model's diffProjects, as readable lines and
//     per-sequence clip rows; the current sequence's changed ranges are drawn on the timeline (TimelineView external
//     preview: the old ranges struck through, the new ones highlighted) until the comparison is cleared or the project
//     changes.
//
// Principle 9: QML sees version ids (numbers), keys ("s3") and display text.

#include <wabcut/model/History.h>
#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

class HistoryBridge : public QObject {
    Q_OBJECT
    /// The project has a version history (it is saved to a file and its history folder opened).
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged FINAL)
    /// Why there is none ("" when available).
    Q_PROPERTY(QString reason READ reason NOTIFY availabilityChanged FINAL)
    /// Newest first: [{ id, createdText, createdAt (ISO UTC), kind ("save" | "auto" | "agent" | "manual" | "restore"),
    /// kindText, label, named, title (the label, else the kind's text), sequences, clips, sizeText, latest }].
    Q_PROPERTY(QVariantList versions READ versions NOTIFY versionsChanged FINAL)
    /// "1.2 MB on disk".
    Q_PROPERTY(QString diskText READ diskText NOTIFY versionsChanged FINAL)
    /// The comparison shown: { from, to (version id or "current"), fromText, toText, empty, summary: [lines],
    /// sequences: [{ key ("s3"), name, kind ("added" | "removed" | "changed"), countsText, settings: [names],
    /// current (it is the sequence shown), clips: [{ name, kind, track, beforeText, afterText, whatText }] }],
    /// mediaText, notesText, stale (the project changed since) }; empty when nothing is compared.
    Q_PROPERTY(QVariantMap comparison READ comparison NOTIFY comparisonChanged FINAL)
    /// Seconds of editing between automatic checkpoints (default 300).
    Q_PROPERTY(int autoIntervalSeconds READ autoIntervalSeconds WRITE setAutoIntervalSeconds NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit HistoryBridge(QObject* parent = nullptr);
    ~HistoryBridge() override;
    static HistoryBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// Saves are noticed through the lifecycle's documentChanged (the project is clean again after a change).
    void setLifecycle(ProjectLifecycle* lifecycle);
    /// History options for the folders opened from now on (tests).
    void setHistoryOptions(HistoryOptions options) { m_options = options; }

    bool available() const { return m_history != nullptr; }
    QString reason() const { return m_reason; }
    QVariantList versions() const { return m_versions; }
    QString diskText() const { return m_diskText; }
    QVariantMap comparison() const { return m_comparison; }
    int autoIntervalSeconds() const { return m_autoSeconds; }
    void setAutoIntervalSeconds(int seconds);
    QString message() const { return m_message; }

    /// The services the history commands run with (HistoryServices over the open history), or null.
    std::shared_ptr<command::CommandServices> services() const { return m_services; }
    ProjectHistory* history() const { return m_history.get(); }

    /// Records the project now (history.checkpoint): kind "save" | "auto" | "agent" | "manual" | "restore". "" or why not.
    Q_INVOKABLE QString checkpoint(const QString& kind, const QString& label = {});
    /// "Save version": a named manual checkpoint.
    Q_INVOKABLE QString saveVersion(const QString& label);
    /// Names (or, with "", unnames) a version.
    Q_INVOKABLE QString name(int id, const QString& label);
    /// Goes back to a version (one undo step). "" or why not.
    Q_INVOKABLE QString restore(int id);
    /// A sequence of a version ("s3") as a new sequence next to the current ones ("" name: "<name> (version <id>)").
    Q_INVOKABLE QString restoreSequence(int id, const QString& sequenceKey, const QString& name = {});
    /// The sequences a version had: [{ key, name }].
    Q_INVOKABLE QVariantList sequencesOf(int id) const;
    /// Writes a version as a project file of its own (history.export).
    Q_INVOKABLE QString exportVersion(int id, const QUrl& file, bool overwrite = false);
    /// Compares two versions: each a version id or "current". "" or why not.
    Q_INVOKABLE QString compare(const QVariant& from, const QVariant& to = QStringLiteral("current"));
    Q_INVOKABLE void clearComparison();
    /// Reads the list again.
    Q_INVOKABLE void refresh();
    /// Opens the history of the controller's project file again (after open / save as; tests call it directly).
    Q_INVOKABLE void reopen();

public slots:
    /// The project was saved: a "save" checkpoint.
    void onSaved();
    /// Before an AI agent applies a plan: an "agent" checkpoint.
    void checkpointBeforeAgent();
    /// The auto timer: an "auto" checkpoint when the project changed since the last one.
    void autoCheckpoint();

signals:
    void availabilityChanged();
    void versionsChanged();
    void comparisonChanged();
    void settingsChanged();
    void messageChanged();

private:
    void setMessage(const QString& message);
    void onDocumentChanged();
    void onRevisionChanged();
    QString run(const char* command, const QVariantMap& params); ///< small integer and string params only
    void clearPreview();

    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_lifecycle;
    HistoryOptions m_options;
    std::shared_ptr<ProjectHistory> m_history;
    std::shared_ptr<command::CommandServices> m_services;
    std::filesystem::path m_file;
    QString m_reason;
    QVariantList m_versions;
    QString m_diskText;
    QVariantMap m_comparison;
    bool m_previewShown = false;
    QTimer m_autoTimer;
    int m_autoSeconds = 300;
    int m_checkpointRevision = -1; ///< ProjectController::revision() at the last checkpoint
    bool m_wasDirty = false;
    QString m_message;
};

struct HistoryBridgeForeign {
    Q_GADGET
    QML_FOREIGN(HistoryBridge)
    QML_NAMED_ELEMENT(History)
    QML_SINGLETON
public:
    static HistoryBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return HistoryBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
