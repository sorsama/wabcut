#include <wabcut/script/Headless.h>

#include <wabcut/script/Protocol.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace wabcut::script {

namespace {

/// The host's delegate for a run on this thread: wake() ends a wait, notifications and prompts are collected.
class Waiter : public HostDelegate {
public:
    void wake() override {
        {
            std::lock_guard lock(m_mutex);
            m_woken = true;
        }
        m_cv.notify_one();
    }
    void notify(const Notification& n) override { notifications.push_back(n); }
    void prompt(const PromptRequest& p) override { prompts.push_back(p); }
    /// No editor window to keep responsive: model commands run on this thread, between two pumps.
    bool runLongCommand(LongCommand job) override {
        longCommands.push_back(std::move(job));
        return true;
    }

    void wait(std::chrono::milliseconds max) {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, max, [this] { return m_woken; });
        m_woken = false;
    }

    std::vector<Notification> notifications;
    std::vector<PromptRequest> prompts;
    std::vector<LongCommand> longCommands;

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_woken = false;
};

} // namespace

std::expected<HeadlessReport, Error> runHeadless(HeadlessRun run) {
    if (!run.answers.is_array()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the prompt answers must be a JSON array"});
    }
    if (run.session == nullptr || run.registry == nullptr) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a headless run needs a session and a registry"});
    }
    HeadlessReport report;
    EditSession& session = *run.session;
    report.revisionBefore = session.revision();
    const auto listener = session.addListener([&report](const ChangeEvent& change) {
        if (change.kind == ChangeKind::Edit) {
            report.undoLabels.emplace_back(change.label);
        }
    });

    Waiter waiter;
    HostSetup setup;
    setup.bundle = std::move(run.bundle);
    setup.granted = run.granted;
    setup.session = run.session;
    setup.registry = run.registry;
    setup.services = std::move(run.services);
    setup.helper = std::move(run.helper);
    setup.storageFile = std::move(run.storageFile);
    setup.wabcutVersion = std::move(run.wabcutVersion);
    setup.limits = run.limits;
    auto host = ExtensionHost::start(std::move(setup), waiter);
    if (!host) {
        session.removeListener(listener);
        return std::unexpected(host.error());
    }

    bool finished = false;
    (*host)->invoke(run.command, run.args, [&](const CallResult& result) {
        report.call = result;
        finished = true;
    });
    std::size_t answered = 0;
    bool timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + run.timeout;
    while (!finished) {
        if (std::chrono::steady_clock::now() > deadline) {
            timedOut = true;
            break;
        }
        waiter.wait(std::chrono::milliseconds(50));
        (*host)->pump();
        std::vector<LongCommand> jobs = std::move(waiter.longCommands);
        waiter.longCommands.clear();
        for (LongCommand& job : jobs) {
            command::CommandEngine engine(session, *run.registry, job.policy);
            job.finish(command::toJson(engine.executeJson(job.document)));
        }
        std::vector<PromptRequest> prompts = std::move(waiter.prompts);
        waiter.prompts.clear();
        for (const PromptRequest& p : prompts) {
            Json answer = answered < run.answers.size() ? run.answers[answered] : defaultPromptAnswer(p.form);
            ++answered;
            Json record{{"form", p.form}, {"answer", answer}};
            if (answer.is_null()) {
                (*host)->answerPrompt(p.prompt, std::nullopt);
            } else if (!(*host)->answerPrompt(p.prompt, std::make_optional(answer))) {
                record["error"] = "the answer does not fit the form; the prompt was cancelled";
                (*host)->answerPrompt(p.prompt, std::nullopt);
            }
            report.prompts.push_back(std::move(record));
        }
    }
    // What the extension logged after it finished (the lines are already on their way).
    waiter.wait(std::chrono::milliseconds(20));
    (*host)->pump();
    report.stop = (*host)->stopReport();
    (*host)->stop();
    for (const LogLine& line : (*host)->log()) {
        report.log.push_back(line);
    }
    report.notifications = std::move(waiter.notifications);
    if (timedOut) {
        report.call = CallResult{false, {}, "timeLimit",
                                 "the run took longer than " + std::to_string(run.timeout.count() / 1000) + " s"};
    }
    session.removeListener(listener);
    report.revisionAfter = session.revision();
    return report;
}

Json toJson(const HeadlessReport& report) {
    Json j{{"ok", report.call.ok}};
    if (report.call.ok) {
        j["result"] = report.call.result;
    } else {
        j["error"] = {{"kind", report.call.errorKind}, {"message", report.call.error}};
    }
    Json notifications = Json::array();
    for (const Notification& n : report.notifications) {
        notifications.push_back({{"level", n.level}, {"message", n.message}});
    }
    Json log = Json::array();
    for (const LogLine& line : report.log) {
        log.push_back({{"level", line.level}, {"message", line.message}});
    }
    j["notifications"] = notifications;
    j["prompts"] = report.prompts;
    j["log"] = log;
    j["revision"] = {{"before", report.revisionBefore}, {"after", report.revisionAfter}};
    j["undo"] = report.undoLabels;
    if (report.stop) {
        j["stopped"] = {{"state", toString(report.stop->state)},
                        {"reason", report.stop->reason},
                        {"memoryLimit", report.stop->memoryLimit},
                        {"timeLimit", report.stop->timeLimit}};
    }
    return j;
}

} // namespace wabcut::script
