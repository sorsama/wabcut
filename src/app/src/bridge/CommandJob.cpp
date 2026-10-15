#include "CommandJob.h"

#include "ProjectController.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditSession.h>

#include <QCoreApplication>
#include <QRegularExpression>

namespace wabcut::app {

namespace {

/// Cancels through `cancelled`; reports "... (NN%)" checkpoints as progress.
class ProgressControl final : public command::ExecutionControl {
public:
    ProgressControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        if (m_report) {
            static const QRegularExpression percent(QStringLiteral("\\((\\d+)%\\)\\s*$"));
            QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
            const auto match = percent.match(text);
            double progress = -1.0;
            if (match.hasMatch()) {
                progress = match.captured(1).toDouble() / 100.0;
                text = text.left(match.capturedStart()).trimmed();
            }
            m_report(text, progress);
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
    if (!controller || m_job.running())
        return false;
    if (!request.params.contains("sequence"))
        request.params["sequence"] = controller->currentSequenceId().value;
    return launch(context, controller, std::move(services), std::move(request), std::move(progress), std::move(done), 0);
}

bool CommandJob::launch(QObject* context, ProjectController* controller, std::shared_ptr<command::CommandServices> services,
                        Request request, Progress progress, Done done, int attempt) {
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
                       [this, guard, base, services, request, report, progress, live, done,
                        attempt](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
                           EditSession scratch(*base);
                           command::EnginePolicy policy;
                           policy.services = services;
                           command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
                           ProgressControl control(cancelled, report);
                           command::CommandRequest commandRequest;
                           commandRequest.command = request.command;
                           commandRequest.params = request.params;
                           command::CommandResult result = engine.execute(commandRequest, &control);
                           const ProjectSnapshot after = scratch.snapshot();
                           return [this, guard, base, after, services, request, progress, live, done, attempt,
                                   result = std::move(result)]() mutable {
                               if (live && result.ok() && after.get() != base.get()) {
                                   if (live->snapshot().get() == base.get()) {
                                       // Nothing changed meanwhile: the copy's result is the next undo step.
                                       if (auto committed = live->session().commit(request.label, *after); !committed)
                                           result = failed(committed.error().message);
                                   } else if (attempt < kMaxReruns && guard) {
                                       // The project changed while the job ran: again on a copy of it as it is now, on a
                                       // worker (cached work), committed the same way; `done` comes from that run.
                                       if (launch(guard, live, services, request, progress, done, attempt + 1))
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
