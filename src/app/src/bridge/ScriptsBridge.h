#pragma once

// ScriptsBridge (0.7, QML singleton `Scripts`, docs/python.md): run a Python script against the open project.
//
//   * File > Run Python Script… (and the command palette: "Run Python Script…" plus the recent scripts) opens the Scripts
//     dialog. The user picks a .py; the dialog shows its name, what it asks for (comment lines at the top of the script:
//     `# wabcut-permissions: Read, EditSequence`), and the Python Wabcut found (3.11 or newer, looked for when the dialog
//     opens - none is bundled). Read is always on; each other permission is a switch the user can leave off, and a script
//     only gets what it asks for AND the user switched on, the way extensions do (docs/extensions.md). The choice can be
//     remembered for this exact content of the script (its SHA-256): an edited script asks again.
//   * Run starts the script (ScriptRun): its output and errors stream into the dialog, Cancel stops it. The script works on a
//     copy of the project; when it ends cleanly the result is ONE undo step ("Script: <name>") on the open project, and when
//     it fails, is stopped, or the project changed meanwhile, nothing is applied.
//   * The project is not edited while a script runs: the dialog is modal, and a change made some other way would stop the
//     script's edits from being applied (they would be lost, so Wabcut says so instead).
//
// Tests point it at their own folders and Python (setPaths, setPythonOverride) before anything runs. Principle 9: QML sees
// text, keys and flags.

#include "ScriptSupport.h"

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <chrono>
#include <functional>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class PaletteProvider;
class ProjectController;
class ProjectLifecycle;
class ScriptRun;
struct ScriptOutcome;

/// The dialog's output pane: one row per line, newest last, at most 3000 (the oldest go first).
class ScriptOutputModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { KindRole = Qt::UserRole + 1, TextRole };
    explicit ScriptOutputModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(m_rows.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    /// Lines are added in batches (every 40 ms, and at once with flush()), so a script that prints a lot does not make the
    /// dialog redraw for each line.
    void append(const QString& kind, const QString& text);
    void flush();
    void clear();
    /// "kind: text" lines for Copy.
    QString plainText() const;

private:
    struct Row {
        QString kind;
        QString text;
    };
    static constexpr int kMaxRows = 3000;
    QList<Row> m_rows;
    QList<Row> m_pending;
    QTimer m_flushTimer;
};

class ScriptsBridge : public QObject {
    Q_OBJECT
    /// "idle" (no script chosen), "ready" (chosen, waiting for Run), "running", "finished".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString scriptPath READ scriptPath NOTIFY scriptChanged FINAL)
    Q_PROPERTY(QString scriptName READ scriptName NOTIFY scriptChanged FINAL)
    Q_PROPERTY(QString scriptDescription READ scriptDescription NOTIFY scriptChanged FINAL)
    /// Why the chosen file cannot be run (not a .py, unreadable, too large), or "".
    Q_PROPERTY(QString problem READ problem NOTIFY scriptChanged FINAL)
    /// Header names that are not permissions ("Network" is never given), shown as a note.
    Q_PROPERTY(QStringList unknownPermissions READ unknownPermissions NOTIFY scriptChanged FINAL)
    /// [{ key, title, explanation, requested, chosen, fixed, available, note }]: Read, EditSequence, EditProject,
    /// MediaAnalysis, Files.
    Q_PROPERTY(QVariantList permissions READ permissions NOTIFY permissionsChanged FINAL)
    Q_PROPERTY(bool remember READ remember WRITE setRemember NOTIFY permissionsChanged FINAL)
    /// "Python 3.12.4 (C:\...)" or why none was found; pythonChecked is false while the search runs.
    Q_PROPERTY(QString pythonText READ pythonText NOTIFY pythonChanged FINAL)
    Q_PROPERTY(bool pythonOk READ pythonOk NOTIFY pythonChanged FINAL)
    Q_PROPERTY(bool pythonChecked READ pythonChecked NOTIFY pythonChanged FINAL)
    Q_PROPERTY(bool canRun READ canRun NOTIFY stateChanged FINAL)
    Q_PROPERTY(QObject* output READ output CONSTANT FINAL)
    /// The outcome sentence once finished ("Done: 12 edits as one undo step, 'Script: Add chapters'"), else "".
    Q_PROPERTY(QString summary READ summary NOTIFY outcomeChanged FINAL)
    Q_PROPERTY(bool succeeded READ succeeded NOTIFY outcomeChanged FINAL)
    Q_PROPERTY(int commandCount READ commandCount NOTIFY progressChanged FINAL)
    Q_PROPERTY(int editCount READ editCount NOTIFY progressChanged FINAL)
    /// Recent scripts, newest first: [{ path, name, folder }].
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged FINAL)

public:
    explicit ScriptsBridge(QObject* parent = nullptr);
    ~ScriptsBridge() override;
    static ScriptsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setLifecycle(ProjectLifecycle* lifecycle);
    /// What commands need besides the project (transcripts, analysis): built on first use, the assistant's.
    void setServicesFactory(std::function<std::shared_ptr<command::CommandServices>()> factory);
    /// Where the grants live, the folder with the `wabcut` package and wabcut-cli (tests; the app: <app data>, the folder
    /// next to the executable).
    void setPaths(const QString& grantFile, const QString& packageFolder, const QString& cliPath);
    /// Tests: a Python to use instead of searching (an empty path searches again).
    void setPythonOverride(const QString& path);
    /// Tests: longest a script's command may run.
    void setCallTimeout(std::chrono::milliseconds timeout) { m_callTimeout = timeout; }

    QString state() const { return m_state; }
    bool running() const { return m_state == QLatin1String("running"); }
    QString scriptPath() const { return m_path; }
    QString scriptName() const { return m_name; }
    QString scriptDescription() const { return m_header.description; }
    QString problem() const { return m_problem; }
    QStringList unknownPermissions() const { return m_header.unknown; }
    QVariantList permissions() const;
    bool remember() const { return m_remember; }
    void setRemember(bool remember);
    QString pythonText() const;
    bool pythonOk() const { return m_pythonChecked && m_python.ok(); }
    bool pythonChecked() const { return m_pythonChecked; }
    bool canRun() const;
    QObject* output() { return &m_output; }
    QString summary() const { return m_summary; }
    bool succeeded() const { return m_succeeded; }
    int commandCount() const { return m_commands; }
    int editCount() const { return m_edits; }
    QVariantList recent() const;

    /// Asks Main to show the dialog (the menu, the palette).
    Q_INVOKABLE void openDialog() { emit dialogRequested(); }
    /// Reads a script's header and what was allowed before; "" or the reason it cannot be run. The state becomes "ready".
    Q_INVOKABLE QString choose(const QUrl& file);
    Q_INVOKABLE QString chooseRecent(const QString& path);
    Q_INVOKABLE void setChosen(const QString& key, bool on);
    /// Starts the script with what is switched on. "" when it started, else the reason.
    Q_INVOKABLE QString run();
    /// Stops the running script; nothing it did is kept.
    Q_INVOKABLE void cancel();
    /// Back to choosing a script (the output is cleared).
    Q_INVOKABLE void reset();
    /// Uses this Python (a file chosen by the user) from now on; "" or the reason it cannot be used.
    Q_INVOKABLE void choosePython(const QUrl& file);
    /// Looks for Python again (after installing one).
    Q_INVOKABLE void findPython();
    Q_INVOKABLE void copyOutput() const;
    /// Forgets the permissions remembered for the chosen script.
    Q_INVOKABLE void forgetChoice();

    std::shared_ptr<PaletteProvider> paletteProvider();

signals:
    void stateChanged();
    void scriptChanged();
    void permissionsChanged();
    void pythonChanged();
    void outcomeChanged();
    void progressChanged();
    void recentChanged();
    /// Main shows the dialog.
    void dialogRequested();
    /// The run ended; `ok` is whether the project took the script's edits (or it made none).
    void finished(bool ok, const QString& summary);

private:
    class Provider;
    struct Permission {
        QString key;
        bool requested = false;
        bool chosen = false;
    };
    void setState(const QString& state);
    void resetRunData();
    void startPythonSearch();
    void onFinished(const ScriptOutcome& outcome);
    QString packageFolder() const;
    QString projectFolder() const;
    QStringList chosenKeys() const;

    ProjectController* m_controller = nullptr;
    ProjectLifecycle* m_lifecycle = nullptr;
    std::function<std::shared_ptr<command::CommandServices>()> m_servicesFactory;
    QString m_grantFile;
    QString m_packageFolder;
    QString m_cliPath;
    QString m_pythonOverride;
    std::chrono::milliseconds m_callTimeout{600'000};
    std::unique_ptr<scripts::ScriptGrantStore> m_store;

    QString m_state = QStringLiteral("idle");
    QString m_path;
    QString m_name;
    QString m_hash;
    QString m_problem;
    scripts::ScriptHeader m_header;
    QList<Permission> m_permissions;
    bool m_remember = true;
    bool m_hadRemembered = false;

    scripts::PythonRuntime m_python;
    bool m_pythonChecked = false;
    bool m_pythonSearching = false;
    int m_pythonSearch = 0; ///< the search that counts (a newer one replaces an older)

    ScriptOutputModel m_output;
    std::unique_ptr<ScriptRun> m_run;
    QString m_summary;
    bool m_succeeded = false;
    int m_commands = 0;
    int m_edits = 0;
    std::shared_ptr<PaletteProvider> m_provider;
};

/// QML registration through the factory (a default-constructible class registered as its own singleton would be built with
/// its default constructor, and create() would never run).
struct ScriptsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ScriptsBridge)
    QML_NAMED_ELEMENT(Scripts)
    QML_SINGLETON
public:
    static ScriptsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return ScriptsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
