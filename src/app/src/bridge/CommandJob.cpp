#include "CommandJob.h"

#include "ProjectController.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditSession.h>

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::app {

namespace {

/// Cancels through `cancelled`; reports "... (NN%)" checkpoints as progress. 0.5: in a batch the engine's "step i of n:
/// <command>" checkpoints are not shown; they place the next percentages in the whole batch's progress.
class ProgressControl final : public command::ExecutionControl {
public:
    ProgressControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        if (m_report) {
            static const QRegularExpression percent(QStringLiteral("\\((\\d+)%\\)\\s*$"));
            static const QRegularExpression step(QStringLiteral("^step (\\d+) of (\\d+): "));
            QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
            if (const auto stepMatch = step.match(text); stepMatch.hasMatch()) {
                m_step = std::max(0, stepMatch.captured(1).toInt() - 1);
                m_steps = std::max(1, stepMatch.captured(2).toInt());
            } else {
                const auto match = percent.match(text);
                double progress = -1.0;
                if (match.hasMatch()) {
                    progress = (m_step + match.captured(1).toDouble() / 100.0) / m_steps;
                    text = text.left(match.capturedStart()).trimmed();
                }
                m_report(text, progress);
            }
        }
        if (m_cancelled.load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }

private:
    const std::atomic<bool>& m_cancelled;
    std::function<void(QString, double)> m_report;
    int m_step = 0;  ///< 0.5: the batch command running (0-based)
    int m_steps = 1; ///< and how many there are
};

/// How many times a job runs again because the project changed while it ran, before it gives up.
constexpr int kMaxReruns = 3;

command::CommandResult failed(const std::string& message) {
    command::CommandResult result;
    result.error = command::CommandError{};
    result.error->message = message;
    return result;
}

} // namespace

QString commandErrorText(const command::CommandResult& result) {
    if (result.ok())
        return {};
    if (result.error->code == command::ErrorCode::Cancelled)
        return QCoreApplication::translate("wabcut::app::CommandJob", "Cancelled");
    return QString::fromStdString(result.error->message);
}

command::CommandResult CommandJob::runNow(ProjectController* controller, std::shared_ptr<command::CommandServices> services,
                                          const std::string& name, command::Json params) {
    if (!controller)
        return failed("no project");
    command::EnginePolicy policy;
    policy.services = std::move(services);
    command::CommandEngine engine(controller->session(), ai::edit::editingCommands(), policy);
    command::CommandRequest request;
    request.command = name;
    if (!params.contains("sequence"))
        params["sequence"] = controller->currentSequenceId().value;
    request.params = std::move(params);
    return engine.execute(request);
}

bool CommandJob::start(QObject* context, ProjectController* controller, std::shared_ptr<command::CommandServices> services,
                       Request request, Progress progress, Done done) {
    std::vector<Request> requests;
    requests.push_back(std::move(request));
    return startBatch(context, controller, std::move(services), std::move(requests), std::move(progress), std::move(done));
}

bool CommandJob::startBatch(QObject* context, ProjectController* controller,
                            std::shared_ptr<command::CommandServices> services, std::vector<Request> requests,
                            Progress progress, Done done) {
    if (!controller || m_job.running() || requests.empty())
        return false;
    for (Request& request : requests) {
        if (!request.params.contains("sequence"))
            request.params["sequence"] = controller->currentSequenceId().value;
    }
    return launch(context, controller, std::move(services), std::move(requests), std::move(progress), std::move(done), 0);
}

bool CommandJob::launch(QObject* context, ProjectController* controller, std::shared_ptr<command::CommandServices> services,
                        std::vector<Request> requests, Progress progress, Done done, int attempt) {
    const ProjectSnapshot base = controller->snapshot();
    QPointer<QObject> guard(context);
    QPointer<ProjectController> live(controller);
    const bool synchronous = m_synchronous;
    // Progress from the worker reaches the GUI thread queued (directly when synchronous).
    auto report = [guard, progress, synchronous](QString text, double fraction) {
        if (!progress)
            return;
        if (synchronous) {
            progress(text, fraction);
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, progress, text = std::move(text), fraction] {
                if (guard)
                    progress(text, fraction);
            },
            Qt::QueuedConnection);
    };
    return m_job.start(context, synchronous,
                       [this, guard, base, services, requests, report, progress, live, done,
                        attempt](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
                           EditSession scratch(*base);
                           command::EnginePolicy policy;
                           policy.services = services;
                           command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
                           ProgressControl control(cancelled, report);
                           command::CommandResult result;
                           if (requests.size() == 1) {
                               command::CommandRequest commandRequest;
                               commandRequest.command = requests.front().command;
                               commandRequest.params = requests.front().params;
                               result = engine.execute(commandRequest, &control);
                           } else {
                               // 0.5: one engine batch, so the copy holds one step (and the live commit is one too).
                               command::CommandBatch batch;
                               batch.label = requests.front().label;
                               for (const Request& r : requests) {
                                   command::CommandRequest commandRequest;
                                   commandRequest.command = r.command;
                                   commandRequest.params = r.params;
                                   batch.commands.push_back(std::move(commandRequest));
                               }
                               result = engine.execute(batch, &control);
                           }
                           const ProjectSnapshot after = scratch.snapshot();
                           return [this, guard, base, after, services, requests, progress, live, done, attempt,
                                   result = std::move(result)]() mutable {
                               if (live && result.ok() && after.get() != base.get()) {
                                   if (live->snapshot().get() == base.get()) {
                                       // Nothing changed meanwhile: the copy's result is the next undo step.
                                       if (auto committed = live->session().commit(requests.front().label, *after); !committed)
                                           result = failed(committed.error().message);
                                   } else if (attempt < kMaxReruns && guard) {
                                       // The project changed while the job ran: again on a copy of it as it is now, on a
                                       // worker (cached work), committed the same way; `done` comes from that run.
                                       if (launch(guard, live, services, requests, progress, done, attempt + 1))
                                           return;
                                       result = failed("the job could not start again on the changed project");
                                   } else {
                                       result = failed("the project kept changing while this ran: nothing was applied (run it again)");
                                   }
                               }
                               if (done)
                                   done(result);
                           };
                       },
                       // A worker that throws (outside the engine's own guard) ends with a failed result.
                       [done](const QString& message) -> BridgeJob::Finish {
                           return [done, message] {
                               if (done)
                                   done(failed(message.toStdString()));
                           };
                       });
}

} // namespace wabcut::app
