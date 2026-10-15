#pragma once

// CommandJob (M5l): one catalog command run off the GUI thread for a bridge (Enhance Speech, Separate Stems,
// Synchronize, Create Multi-Camera Source Sequence), the BridgeJob pattern with the command engine:
//   * the worker runs the command on a copy of the project (a scratch EditSession) and reports its "... (NN%)"
//     checkpoints as progress; cancel() stops it at the next checkpoint;
//   * on the GUI thread the copy's result becomes one undo step when the project has not changed since; when it has,
//     the command runs again on a copy of the project as it is now, on a worker like the first time (the expensive
//     part is cached: Enhance Speech reuses its take, the audio sync its analysis), so the user's edits in the
//     meantime are never overwritten and the GUI thread never runs it (review wave 5, A5-P2-24).
// runNow() runs a quick command on the live project on the calling thread.

#include "ShortsBridge.h" // BridgeJob

#include <wabcut/command/Command.h>

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <string>

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class CommandJob {
public:
    struct Request {
        std::string command;
        command::Json params;
        std::string label; ///< the undo step's name when the copy's result is committed
    };
    using Progress = std::function<void(const QString& text, double progress)>; ///< progress 0..1, or -1
    using Done = std::function<void(const command::CommandResult& result)>;

    bool running() const { return m_job.running(); }
    void cancel() { m_job.cancel(); }
    /// Cancels and waits for the worker (BridgeJob::stop).
    void stop() { m_job.stop(); }
    /// Runs on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    bool synchronous() const { return m_synchronous; }

    /// Starts the command; false when a job is running. `progress` and `done` are called on the GUI thread (or the
    /// calling thread when synchronous); `done` after the result was committed.
    bool start(QObject* context, ProjectController* controller, std::shared_ptr<command::CommandServices> services,
               Request request, Progress progress, Done done);

    /// A quick command on the live project now (one undo step for an edit).
    static command::CommandResult runNow(ProjectController* controller, std::shared_ptr<command::CommandServices> services,
                                         const std::string& name, command::Json params);

private:
    /// Runs on a snapshot of the project; when the project changed meanwhile, again (up to 3 times) - never on the GUI
    /// thread (review wave 5, A5-P2-24).
    bool launch(QObject* context, ProjectController* controller, std::shared_ptr<command::CommandServices> services,
                Request request, Progress progress, Done done, int attempt);

    BridgeJob m_job;
    bool m_synchronous = false;
};

/// The error text of a result ("" when it succeeded); "Cancelled" for a cancel.
QString commandErrorText(const command::CommandResult& result);

} // namespace wabcut::app
