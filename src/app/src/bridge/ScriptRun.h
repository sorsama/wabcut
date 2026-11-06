#pragma once

// ScriptRun (0.7, docs/python.md): one Python script run against the open project.
//
// The script is the user's own Python, started when the script is run. It reaches the project through the `wabcut` package
// (python/wabcut), which speaks the typed command protocol - the same one `wabcut-cli mcp` serves - over a named pipe or Unix
// socket that exists for this one run: its name is random, only this user can open it, and the first thing a client sends is
// a one-time token that only the script's environment holds. There is no network listener.
//
// The script never touches the live project. It works on a scratch copy of it (like CommandJob does for a long command), in
// a worker thread, so the editor stays responsive however long a command takes. When the script ends cleanly, the copy's
// result becomes ONE undo step on the live project - if the project did not change meanwhile; otherwise nothing is applied.
// A script that fails, is stopped, or loses its connection leaves the project exactly as it was.
//
// What the script may do is what the user allowed: the capabilities (Read always; EditSequence, EditProject, MediaAnalysis
// as chosen) and, for commands that read or write files, only inside the project's folder. The McpServer enforces it, not the
// script. Every tool call is counted (and could be audited) without its parameters.
//
// Threads: ScriptRun and its callbacks live on the GUI thread; ScriptServer (the scratch session, the command server and the
// socket) lives on its own thread and never blocks the GUI.

#include "ScriptSupport.h"

#include <wabcut/command/Command.h>
#include <wabcut/model/EditSession.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QLocalServer;
class QLocalSocket;
class QProcess;
class QThread;
class QTimer;

namespace wabcut::command {
class CommandServices;
}
namespace wabcut::command::mcp {
class McpServer;
}

namespace wabcut::app {

struct ScriptRunOptions {
    QString scriptPath;
    QString name;              ///< shown in the output and in the undo step's name
    ProjectSnapshot base;      ///< the project the script starts from
    command::CapabilitySet allowed = command::CapabilitySet::of({command::Capability::Read});
    /// Folders the commands that read or write files may use (the project's folder when "Files" was allowed); empty: none.
    std::vector<std::filesystem::path> fileRoots;
    std::shared_ptr<command::CommandServices> services;
    scripts::PythonRuntime python;
    QString packageFolder;     ///< the folder that holds the `wabcut` package (goes on PYTHONPATH)
    QString cliPath;           ///< wabcut-cli, for scripts that open other projects (WABCUT_CLI)
    QString appVersion;
    QString workingFolder;     ///< the script's working directory (default: its own folder)
    std::uint64_t sequence = 0; ///< the sequence open in the timeline: what commands work on when a call names none (0: none)
    QString auditFile;         ///< one JSON line per command (the script's name, the tool, the outcome - never parameters); "" none
    std::chrono::milliseconds callTimeout{600'000};
};

struct ScriptOutcome {
    bool ok = false;            ///< the script ended with exit code 0
    bool cancelled = false;
    int exitCode = 0;
    int commands = 0;           ///< tool calls the script made
    int edits = 0;              ///< of them, the ones that changed the project
    QString message;            ///< one sentence for the user (why it failed, what was kept)
    ProjectSnapshot base;       ///< the project the run started from
    ProjectSnapshot result;     ///< the scratch copy as the script left it (when ok)
    bool changed() const { return ok && base && result && result.get() != base.get(); }
};

/// The worker: the scratch session, the command server and the local socket. Lives on its own thread.
class ScriptServer : public QObject {
    Q_OBJECT
public:
    struct Config {
        ProjectSnapshot base;
        command::CapabilitySet allowed;
        std::vector<std::filesystem::path> fileRoots;
        std::shared_ptr<command::CommandServices> services;
        QByteArray token;
        QString address;
        std::string appVersion;
        std::string label;
        std::uint64_t sequence = 0;
        QString auditFile;
        std::chrono::milliseconds callTimeout{600'000};
    };
    explicit ScriptServer(Config config);
    ~ScriptServer() override;

    /// Asks the call that is running now to stop (thread-safe).
    void requestCancel() { m_cancel = true; }
    /// The scratch copy as it is now (call after stopped(); thread-safe).
    ProjectSnapshot result() const;
    int commands() const { return m_commands; }
    int edits() const { return m_edits; }
    bool wasConnected() const { return m_everConnected; }
    /// The script's session ended with an error (`notifications/wabcut-abort`): what it did is not applied.
    bool aborted() const { return m_aborted; }
    /// The scratch copy differs from the project the script started from (an edit that was rolled back or undone is no change).
    bool changed() const { return m_changed; }

public slots:
    void start();
    void stop();

signals:
    void listening(const QString& address);
    void failed(const QString& message);
    void connected();
    void activity(int commands, int edits);
    void stopped();

private:
    class Transport;
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void drop(const QString& why, bool graceful = false);
    bool takeLine(QByteArray& line);
    void hello(const QByteArray& line);
    void writeAudit(const QString& event, const QString& tool, bool ok, const QString& code, bool committed, bool dryRun,
                    std::uint64_t revision) const;

    Config m_config;
    std::unique_ptr<EditSession> m_session;
    std::unique_ptr<command::mcp::McpServer> m_mcp;
    std::unique_ptr<Transport> m_transport;
    QLocalServer* m_server = nullptr;
    QLocalSocket* m_socket = nullptr;
    QTimer* m_helloTimer = nullptr;
    QByteArray m_buffer;
    bool m_authenticated = false;
    bool m_everConnected = false;
    std::atomic<bool> m_aborted{false};
    std::atomic<bool> m_changed{false};
    ProjectSnapshot m_initial; ///< the scratch copy before the script touched it
    bool m_stopped = false;
    int m_badAttempts = 0;
    std::atomic<bool> m_cancel{false};
    std::atomic<int> m_commands{0};
    std::atomic<int> m_edits{0};
    mutable std::mutex m_resultMutex;
    ProjectSnapshot m_result;
    std::string m_currentRequest; ///< the id of the request being served (JSON), for cancelling it
    bool m_inHandle = false;      ///< a request is being served: signals that would re-enter wait
    bool m_pendingDisconnect = false;
};

class ScriptRun : public QObject {
    Q_OBJECT
public:
    using OutputHandler = std::function<void(const QString& kind, const QString& text)>; ///< kind: out | err | info
    using FinishedHandler = std::function<void(const ScriptOutcome&)>;
    using ProgressHandler = std::function<void(int commands, int edits)>;

    ScriptRun(ScriptRunOptions options, OutputHandler output, FinishedHandler finished, ProgressHandler progress = {},
              QObject* parent = nullptr);
    ~ScriptRun() override;

    /// Starts the worker, then the script. The handlers run on the GUI thread.
    void start();
    /// Stops the script and discards what it did.
    void cancel();
    bool running() const { return m_state != State::Idle && m_state != State::Done; }

private:
    enum class State { Idle, Starting, Running, Stopping, Done };
    void onListening(const QString& address);
    void onProcessFinished(int exitCode, int status);
    void onServerStopped();
    void maybeFinish();
    void fail(const QString& message);
    void finish(ScriptOutcome outcome);
    void drainOutput(bool error);

    ScriptRunOptions m_options;
    OutputHandler m_output;
    FinishedHandler m_finished;
    ProgressHandler m_progress;
    State m_state = State::Idle;
    QThread* m_thread = nullptr;
    QPointer<ScriptServer> m_server;
    QProcess* m_process = nullptr;
    QByteArray m_token;
    QByteArray m_outBuffer;
    QByteArray m_errBuffer;
    bool m_cancelled = false;
    bool m_processDone = false;
    bool m_serverDone = false;
    int m_exitCode = 0;
    bool m_crashed = false;
    QString m_failure;
};

} // namespace wabcut::app
