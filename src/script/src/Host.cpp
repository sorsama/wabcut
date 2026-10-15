#include <wabcut/script/Host.h>

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/script/Manifest.h>
#include <wabcut/script/Protocol.h>
#include <wabcut/script/Storage.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace wabcut::script {

namespace {

using command::Capability;
using command::CapabilitySet;

/// Lines from the helper waiting for pump(); more means it floods the editor.
constexpr std::size_t kMaxQueuedLines = 10'000;
/// A reply larger than this is refused (the helper reads lines of up to 16 MiB).
constexpr std::size_t kMaxReplyBytes = 8 * 1024 * 1024;
constexpr auto kStorageSaveDelay = std::chrono::milliseconds(1000);
constexpr auto kTick = std::chrono::milliseconds(100);

/// JSON text for the wire; text that is not UTF-8 (a clip name from an old file) is replaced, never thrown on.
std::string wire(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

/// What an extension may run: its capability granted, MediaAnalysis too for a model, never a command on named files.
bool allowedBy(const command::CommandSpec& spec, CapabilitySet granted) {
    return granted.has(spec.capability) && (!spec.runsModel || granted.has(Capability::MediaAnalysis)) &&
           !spec.touchesFiles;
}

/// True when `document` (a request or a batch) names a command that runs an AI model.
bool runsModel(const Json& document, const command::CommandRegistry& registry) {
    const auto models = [&](const command::CommandRequest& r) {
        const command::CommandSpec* spec = registry.find(r.command, r.version);
        return spec && spec->runsModel;
    };
    if (command::isBatchJson(document)) {
        const auto batch = command::batchFromJson(document);
        return batch && std::ranges::any_of(batch->commands, models);
    }
    const auto request = command::requestFromJson(document);
    return request && models(*request);
}

/// Stops an extension's request once the call's script time is up (S5-P2-1): a long batch never holds the editor
/// longer than that (the engine asks before each command and before committing).
class DeadlineControl final : public command::ExecutionControl {
public:
    explicit DeadlineControl(std::chrono::steady_clock::time_point deadline) : m_deadline(deadline) {}
    std::optional<command::CommandError> checkpoint(std::string_view) override {
        if (std::chrono::steady_clock::now() < m_deadline) {
            return std::nullopt;
        }
        return command::CommandError{command::ErrorCode::TimedOut,
                                     "the extension's request ran past its time limit: ask for less at once", nullptr};
    }

private:
    std::chrono::steady_clock::time_point m_deadline;
};

Json describeSpec(const command::CommandSpec& spec, CapabilitySet granted, bool full) {
    Json j{{"name", spec.name},
           {"version", spec.version},
           {"title", spec.title},
           {"capability", command::toString(spec.capability)},
           {"runsModel", spec.runsModel},
           {"touchesFiles", spec.touchesFiles},
           {"allowed", allowedBy(spec, granted)}};
    if (full) {
        j["description"] = spec.description;
        j["params"] = spec.params;
        j["example"] = spec.example;
        j["aliases"] = spec.aliases;
    }
    return j;
}

} // namespace

std::string_view toString(HostState state) {
    switch (state) {
    case HostState::Starting:
        return "starting";
    case HostState::Ready:
        return "ready";
    case HostState::Stopped:
        return "stopped";
    case HostState::Failed:
        return "failed";
    case HostState::Crashed:
        return "crashed";
    }
    return "stopped";
}

std::filesystem::path scriptHostPath() {
#ifdef _WIN32
    return ai::currentExecutableDir() / "wabcut-scripthost.exe";
#else
    return ai::currentExecutableDir() / "wabcut-scripthost";
#endif
}

// ---- The watcher: the helper's lines, collected off the session thread ------------------------------------------

class ExtensionHost::Watcher {
public:
    Watcher(std::unique_ptr<ai::ChildProcess> child, HostDelegate& delegate)
        : m_child(std::move(child)), m_delegate(delegate), m_reader([this] { read(); }), m_writer([this] { write(); }) {}
    ~Watcher() {
        {
            std::lock_guard lock(m_mutex);
            m_quit = true;
        }
        m_outCv.notify_all();
        m_child->kill(); // unblocks a write the helper never reads
        m_reader.join();
        m_writer.join();
    }

    ai::ChildProcess& child() { return *m_child; }
    void setTicking(bool ticking) { m_ticking = ticking; }

    /// Queues `line` for the helper and returns at once: the session thread never waits for a pipe. False when the
    /// helper has stopped reading (more than kMaxUnreadBytes queued).
    bool send(std::string line) {
        {
            std::lock_guard lock(m_mutex);
            if (m_outBytes + line.size() > kMaxUnreadBytes) {
                return false;
            }
            m_outBytes += line.size();
            m_out.push_back(std::move(line));
        }
        m_outCv.notify_one();
        return true;
    }

    std::vector<std::string> take(bool& closed, bool& overflow) {
        std::lock_guard lock(m_mutex);
        std::vector<std::string> out(std::make_move_iterator(m_lines.begin()), std::make_move_iterator(m_lines.end()));
        m_lines.clear();
        closed = m_closed;
        overflow = m_overflow;
        return out;
    }

private:
    static constexpr std::size_t kMaxUnreadBytes = std::size_t{64} * 1024 * 1024;

    void read() {
        std::string line;
        auto lastTick = Clock::now();
        for (;;) {
            {
                std::lock_guard lock(m_mutex);
                if (m_quit) {
                    return;
                }
            }
            const auto status = m_child->readLine(line, kTick);
            if (status == ai::ChildProcess::ReadStatus::Line) {
                bool flood = false;
                {
                    std::lock_guard lock(m_mutex);
                    flood = m_lines.size() >= kMaxQueuedLines;
                    if (flood) {
                        m_overflow = true;
                    } else {
                        m_lines.push_back(std::move(line));
                    }
                }
                m_delegate.wake();
                if (flood) {
                    return;
                }
                continue;
            }
            if (status != ai::ChildProcess::ReadStatus::Timeout) {
                {
                    std::lock_guard lock(m_mutex);
                    (status == ai::ChildProcess::ReadStatus::Overflow ? m_overflow : m_closed) = true;
                }
                m_delegate.wake();
                return;
            }
            if (m_ticking && Clock::now() - lastTick >= kTick) {
                lastTick = Clock::now();
                m_delegate.wake(); // deadlines and held events need pump() without a message
            }
        }
    }

    void write() {
        for (;;) {
            std::string line;
            {
                std::unique_lock lock(m_mutex);
                m_outCv.wait(lock, [this] { return m_quit || !m_out.empty(); });
                if (m_quit) {
                    return;
                }
                line = std::move(m_out.front());
                m_out.pop_front();
                m_outBytes -= line.size();
            }
            if (!m_child->writeLine(line)) {
                return; // the helper is gone; pump() sees it close
            }
        }
    }

    std::unique_ptr<ai::ChildProcess> m_child;
    HostDelegate& m_delegate;
    std::mutex m_mutex;
    std::condition_variable m_outCv;
    std::deque<std::string> m_lines;
    std::deque<std::string> m_out;
    std::size_t m_outBytes = 0;
    bool m_closed = false;
    bool m_overflow = false;
    bool m_quit = false;
    std::atomic<bool> m_ticking{true};
    // Last: they start in the constructor and use the members above.
    std::thread m_reader;
    std::thread m_writer;
};

struct ExtensionHost::Storage {
    KeyValueStore kv;
};

// ---- Lifecycle -------------------------------------------------------------------------------------------------

ExtensionHost::ExtensionHost(HostSetup setup, HostDelegate& delegate)
    : m_setup(std::move(setup)), m_delegate(delegate) {
    command::EnginePolicy policy;
    policy.allowed = m_setup.granted;
    policy.allowModelCommands = m_setup.granted.has(Capability::MediaAnalysis);
    policy.allowFileCommands = false; // no file access, whatever the grants
    policy.services = m_setup.services;
    policy.undoLabelPrefix = m_setup.bundle.manifest.name;
    m_policy = policy;
    m_engine = std::make_unique<command::CommandEngine>(*m_setup.session, *m_setup.registry, std::move(policy));
    m_requestTokens = m_setup.limits.requestBurst;
    m_storageTokens = m_setup.limits.storageWriteBurst;
    m_requestsRefilled = m_storageRefilled = Clock::now();
    m_storage = std::make_unique<Storage>();
    if (!m_setup.storageFile.empty()) {
        m_storage->kv = KeyValueStore::load(m_setup.storageFile);
    }
}

ExtensionHost::~ExtensionHost() {
    *m_alive = false; // a long editor job that finishes later finds no host
    stop();
}

std::expected<std::unique_ptr<ExtensionHost>, Error> ExtensionHost::start(HostSetup setup, HostDelegate& delegate) {
    if (setup.session == nullptr || setup.registry == nullptr) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "an extension host needs a session and a registry"});
    }
    // Never more than the manifest asks for and extensions may have, whatever the caller passes.
    setup.granted = {setup.granted.bits & setup.bundle.manifest.requested().bits & grantableCapabilities().bits};
    if (setup.helper.empty()) {
        setup.helper = scriptHostPath();
    }
    std::unique_ptr<ExtensionHost> host(new ExtensionHost(std::move(setup), delegate));
    const HostSetup& s = host->m_setup;

    ai::ChildProcessOptions options;
    options.executable = s.helper;
    options.arguments = {"--extension", s.bundle.manifest.id}; // names the process in task lists
    options.memoryLimitBytes = s.limits.memoryBytes;
    options.noChildProcesses = true;
    options.restrictUi = true;
    auto child = ai::ChildProcess::spawn(options);
    if (!child) {
        return std::unexpected(child.error());
    }
    host->m_watcher = std::make_unique<Watcher>(std::move(*child), delegate);

    Json scripts = Json::array();
    for (const ScriptSource& script : s.bundle.scripts) {
        scripts.push_back({{"path", script.path}, {"source", script.source}});
    }
    host->send(helloMessage(s.bundle.manifest, s.granted, s.wabcutVersion,
                            static_cast<int>(s.limits.callTime.count()), scripts, host->m_storage->kv.values()));
    host->m_startDeadline = Clock::now() + s.limits.startTime;
    return host;
}

void ExtensionHost::stop() {
    if (m_state == HostState::Stopped || m_state == HostState::Failed || m_state == HostState::Crashed) {
        return;
    }
    if (m_watcher) {
        send(Json{{"type", "shutdown"}});
        m_watcher->child().wait(std::chrono::milliseconds(500));
    }
    ended(HostState::Stopped, "stopped", true);
}

void ExtensionHost::ended(HostState state, std::string reason, bool killed, bool timeLimit) {
    if (m_state == HostState::Stopped || m_state == HostState::Failed || m_state == HostState::Crashed) {
        return;
    }
    StopReport report;
    report.state = state;
    report.timeLimit = timeLimit;
    if (m_watcher) {
        if (killed) {
            m_watcher->child().kill();
            m_watcher->child().wait(std::chrono::milliseconds(2000));
        }
        const auto peak = m_watcher->child().peakMemoryBytes();
        // At the cap the next allocation fails; how close the peak got depends on its size.
        const std::size_t cap = m_setup.limits.memoryBytes;
        const std::size_t slack = std::max(cap / 10, std::size_t{16} * 1024 * 1024);
        report.memoryLimit = state != HostState::Stopped && cap > 0 && peak && *peak + slack >= cap;
        m_watcher->setTicking(false);
    }
    if (report.memoryLimit) {
        reason = "it reached its memory limit (" + std::to_string(m_setup.limits.memoryBytes / (1024 * 1024)) +
                 " MiB)";
    }
    report.reason = reason;
    m_state = state;
    m_stopReport = report;
    if (state != HostState::Stopped) {
        addLog("error", m_setup.bundle.manifest.name + " " + std::string(toString(state)) + ": " + reason);
    }

    std::map<std::uint64_t, Call> calls = std::move(m_calls);
    m_calls.clear();
    for (const auto& [id, prompt] : m_prompts) {
        m_delegate.promptClosed(id);
    }
    m_prompts.clear();
    m_longJobs = 0;
    m_pausedSince.reset();
    saveStorage();
    for (auto& [id, call] : calls) {
        if (call.done) {
            call.done(CallResult{false, {}, "stopped", "the extension " + std::string(toString(state)) + ": " + reason});
        }
    }
    m_delegate.stopped(report);
}

// ---- Calls, events, prompts ------------------------------------------------------------------------------------

std::uint64_t ExtensionHost::invoke(std::string commandId, Json args, Done done) {
    if (m_state != HostState::Starting && m_state != HostState::Ready) {
        if (done) {
            done(CallResult{false, {}, "stopped", "the extension is not running"});
        }
        return 0;
    }
    if (m_state == HostState::Ready && std::none_of(m_commands.begin(), m_commands.end(),
                                                    [&](const RegisteredCommand& c) { return c.id == commandId; })) {
        if (done) {
            done(CallResult{false, {}, "unknownCommand",
                            m_setup.bundle.manifest.name + " has no command \"" + commandId + "\""});
        }
        return 0;
    }
    const std::uint64_t id = m_nextCall++;
    Call& call = m_calls[id];
    call.done = std::move(done);
    call.commandId = std::move(commandId);
    call.args = std::move(args);
    if (m_state == HostState::Ready) {
        sendCall(id, call);
    }
    return id;
}

void ExtensionHost::sendCall(std::uint64_t id, Call& call) {
    call.sent = true;
    const auto now = Clock::now();
    call.hardDeadline = now + m_setup.limits.callTime * kCallHardLimitFactor;
    call.deadline = std::min(now + m_setup.limits.callTime + m_setup.limits.grace, call.hardDeadline);
    if (m_watcher) {
        m_watcher->setTicking(true);
    }
    send(Json{{"type", "invoke"}, {"call", id}, {"command", call.commandId}, {"args", call.args}});
}

void ExtensionHost::finish(std::uint64_t id, CallResult result) {
    const auto it = m_calls.find(id);
    if (it == m_calls.end()) {
        return; // late (cancelled and answered) or unknown
    }
    Done done = std::move(it->second.done);
    m_calls.erase(it); // before done(): it may invoke again
    if (done) {
        done(result);
    }
}

void ExtensionHost::publish(Event event) {
    // Events tell what the user does and opens: only for an extension that may read the project (S5-P2-3).
    if (m_state != HostState::Ready || !m_setup.granted.has(Capability::Read) ||
        !m_subscribed[static_cast<std::size_t>(event.kind)]) {
        return;
    }
    if (event.kind == EventKind::ProjectSaved && event.data.contains("file") && event.data["file"].is_string()) {
        // The file's name, never where it lives (that path names the user's account).
        event.data["file"] = pathToUtf8(pathFromUtf8(event.data["file"].get<std::string>()).filename());
    }
    if (auto now = m_throttle.offer(std::move(event), Clock::now())) {
        send(Json{{"type", "event"}, {"event", toString(now->kind)}, {"data", now->data}, {"coalesced", now->coalesced}});
    } else if (m_watcher) {
        m_watcher->setTicking(true);
    }
}

bool ExtensionHost::answerPrompt(std::uint64_t prompt, std::optional<Json> values) {
    const auto it = m_prompts.find(prompt);
    if (it == m_prompts.end()) {
        return false;
    }
    Json answer;
    if (values) {
        auto checked = validatePromptAnswer(it->second, *values);
        if (!checked) {
            return false;
        }
        answer = std::move(*checked);
    }
    m_prompts.erase(it);
    send(Json{{"type", "promptAnswer"}, {"prompt", prompt}, {"values", answer}});
    resume(Clock::now()); // the script was waiting for the user, not running
    return true;
}

std::vector<PromptRequest> ExtensionHost::pendingPrompts() const {
    std::vector<PromptRequest> out;
    for (const auto& [id, form] : m_prompts) {
        out.push_back({id, form});
    }
    return out;
}

bool ExtensionHost::idle() const { return m_state != HostState::Starting && m_calls.empty() && m_prompts.empty(); }

std::optional<ExtensionHost::Clock::time_point> ExtensionHost::nextDeadline() const {
    std::optional<Clock::time_point> next;
    const auto consider = [&](Clock::time_point t) { next = next ? std::min(*next, t) : t; };
    if (m_state == HostState::Starting) {
        consider(m_startDeadline);
    }
    if (m_state == HostState::Starting || m_state == HostState::Ready) {
        if (!paused()) {
            for (const auto& [id, call] : m_calls) {
                if (call.sent) {
                    consider(call.deadline);
                }
            }
        }
        if (auto due = m_throttle.nextDue()) {
            consider(*due);
        }
    }
    if (m_storageSaveAt) {
        consider(*m_storageSaveAt);
    }
    return next;
}

void ExtensionHost::touchCalls(Clock::time_point now) {
    for (auto& [id, call] : m_calls) {
        if (call.sent && !call.cancelled) {
            call.deadline = std::min(now + m_setup.limits.callTime + m_setup.limits.grace, call.hardDeadline);
        }
    }
}

void ExtensionHost::pause(Clock::time_point now) {
    if (!m_pausedSince) {
        m_pausedSince = now;
    }
}

void ExtensionHost::resume(Clock::time_point now) {
    if (paused()) {
        return;
    }
    if (m_pausedSince) {
        // The time the user (or a long editor job) took is not the script's.
        const auto waited = now - *m_pausedSince;
        m_pausedSince.reset();
        for (auto& [id, call] : m_calls) {
            if (call.sent && !call.cancelled) {
                call.hardDeadline += waited;
            }
        }
    }
    touchCalls(now);
}

bool ExtensionHost::callRunning() const {
    return std::any_of(m_calls.begin(), m_calls.end(), [](const auto& c) { return c.second.sent && !c.second.cancelled; });
}

bool ExtensionHost::takeToken(double& bucket, Clock::time_point& refilled, int perSecond, int burst, Clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - refilled).count();
    refilled = now;
    bucket = std::min(static_cast<double>(burst), bucket + seconds * perSecond);
    if (bucket < 1.0) {
        return false;
    }
    bucket -= 1.0;
    return true;
}

// ---- What the helper says ----------------------------------------------------------------------------------------

void ExtensionHost::send(const Json& message) {
    if (m_watcher && !m_watcher->send(wire(message))) {
        ended(HostState::Crashed, "it stopped reading what the editor sends", true);
    }
}

void ExtensionHost::pump() {
    const auto running = [this] { return m_state == HostState::Starting || m_state == HostState::Ready; };
    if (!running() || !m_watcher) {
        return;
    }
    bool closed = false;
    bool overflow = false;
    for (const std::string& line : m_watcher->take(closed, overflow)) {
        handle(line);
        if (!running()) {
            return;
        }
    }
    if (overflow) {
        ended(HostState::Crashed, "it sent more than the editor reads", true);
        return;
    }
    if (closed) {
        const auto code = m_watcher->child().wait(std::chrono::milliseconds(2000));
        ended(m_state == HostState::Starting ? HostState::Failed : HostState::Crashed,
              "its process ended (" + (code ? ai::ChildProcess::describeExit(*code) : std::string("no exit code")) + ")",
              false);
        return;
    }

    const auto now = Clock::now();
    if (m_state == HostState::Starting && now > m_startDeadline) {
        ended(HostState::Failed,
              "it did not finish loading within " + std::to_string(m_setup.limits.startTime.count() / 1000) + " s", true,
              true);
        return;
    }
    if (!paused()) { // while the user answers a prompt (or a long editor job runs), the script is not running
        for (auto& [id, call] : m_calls) {
            if (!call.sent || now <= call.deadline) {
                continue;
            }
            if (!call.cancelled) {
                call.cancelled = true;
                call.deadline = now + m_setup.limits.grace;
                send(Json{{"type", "cancel"}, {"call", id}});
                continue;
            }
            ended(HostState::Crashed, "a script did not stop at its time limit", true, true);
            return;
        }
    }
    for (Event& e : m_throttle.due(now)) {
        send(Json{{"type", "event"}, {"event", toString(e.kind)}, {"data", e.data}, {"coalesced", e.coalesced}});
    }
    if (m_storageSaveAt && now >= *m_storageSaveAt) {
        saveStorage();
    }
    m_watcher->setTicking(nextDeadline().has_value());
}

void ExtensionHost::handle(const std::string& line) {
    auto parsed = parseHostMessage(line);
    if (!parsed) {
        ended(HostState::Crashed, "it broke the protocol (" + parsed.error() + ")", true);
        return;
    }
    const Json& m = parsed->body;
    const auto now = Clock::now();
    // Only progress on a call moves its deadline (S5-P2-1): a request (the script waits for the editor, below), a
    // prompt answered. Log lines, storage writes, notifications and pongs never do, so a hostile helper cannot keep a
    // call alive by chattering.
    switch (parsed->type) {
    case HostMessageType::Ready:
        if (m_state == HostState::Starting) {
            m_state = HostState::Ready;
            std::vector<std::uint64_t> queued;
            for (const auto& [id, call] : m_calls) {
                queued.push_back(id);
            }
            for (const std::uint64_t id : queued) {
                Call& call = m_calls.at(id);
                if (std::none_of(m_commands.begin(), m_commands.end(),
                                 [&](const RegisteredCommand& c) { return c.id == call.commandId; })) {
                    finish(id, CallResult{false, {}, "unknownCommand",
                                          m_setup.bundle.manifest.name + " has no command \"" + call.commandId + "\""});
                } else {
                    sendCall(id, call);
                }
            }
        }
        break;
    case HostMessageType::LoadFailed:
        ended(HostState::Failed, m.at("message").get<std::string>(), true);
        break;
    case HostMessageType::Request:
        handleRequest(m);
        touchCalls(Clock::now()); // the editor's time is not script time (never past the hard limit)
        break;
    case HostMessageType::Done: {
        const auto id = m.at("call").get<std::uint64_t>();
        if (m.contains("error")) {
            finish(id, CallResult{false, {}, m["error"]["kind"].get<std::string>(), m["error"]["message"].get<std::string>()});
        } else {
            finish(id, CallResult{true, m.at("result"), {}, {}});
        }
        break;
    }
    case HostMessageType::Notify:
    case HostMessageType::Toast: {
        while (!m_recentNotifications.empty() && now - m_recentNotifications.front() > std::chrono::minutes(1)) {
            m_recentNotifications.pop_front();
        }
        if (m_recentNotifications.size() >= static_cast<std::size_t>(m_setup.limits.notificationsPerMinute)) {
            if (!m_notificationsDropped) {
                addLog("warn", "too many notifications: some were not shown");
            }
            m_notificationsDropped = true;
            break;
        }
        m_notificationsDropped = false;
        m_recentNotifications.push_back(now);
        m_delegate.notify(Notification{parsed->type == HostMessageType::Toast ? std::string("toast") : m.at("level").get<std::string>(),
                                       m.at("message").get<std::string>()});
        break;
    }
    case HostMessageType::Prompt: {
        const auto id = m.at("prompt").get<std::uint64_t>();
        if (m_prompts.contains(id)) {
            break;
        }
        while (!m_recentPrompts.empty() && now - m_recentPrompts.front() > std::chrono::minutes(1)) {
            m_recentPrompts.pop_front();
        }
        // A form is shown only while a call the user started runs (never at load or from an event listener), at most
        // promptsPerMinute a minute, so an extension cannot pop up a credential-style form on its own (S5-P2-2).
        std::string refused;
        if (!callRunning()) {
            refused = "a prompt outside a command the user ran was cancelled";
        } else if (m_recentPrompts.size() >= static_cast<std::size_t>(m_setup.limits.promptsPerMinute)) {
            refused = "too many prompts in a minute: one was cancelled";
        } else if (m_prompts.size() >= m_setup.limits.maxPendingPrompts) {
            refused = "too many open prompts: one was cancelled";
        }
        if (!refused.empty()) {
            send(Json{{"type", "promptAnswer"}, {"prompt", id}, {"values", nullptr}});
            addLog("warn", refused);
            break;
        }
        m_recentPrompts.push_back(now);
        m_prompts[id] = m.at("form");
        pause(now);
        m_delegate.prompt(PromptRequest{id, m.at("form")});
        break;
    }
    case HostMessageType::RegisterCommand: {
        const std::string id = m.at("id").get<std::string>();
        const std::string title = m.at("title").get<std::string>();
        const auto it = std::find_if(m_commands.begin(), m_commands.end(),
                                     [&](const RegisteredCommand& c) { return c.id == id; });
        if (it != m_commands.end()) {
            it->title = title;
        } else if (m_commands.size() < kMaxRegisteredCommands) {
            m_commands.push_back({id, title});
        } else {
            addLog("warn", "more than " + std::to_string(kMaxRegisteredCommands) + " commands: \"" + id + "\" ignored");
            break;
        }
        m_delegate.commandsChanged();
        break;
    }
    case HostMessageType::Subscribe:
        if (!m_setup.granted.has(Capability::Read)) {
            addLog("warn", "editor events need the Read capability: not subscribed");
            break;
        }
        if (const auto kind = eventKindFromString(m.at("event").get<std::string>())) {
            m_subscribed[static_cast<std::size_t>(*kind)] = m.at("on").get<bool>();
        }
        break;
    case HostMessageType::StorageSet:
    case HostMessageType::StorageRemove: {
        if (!takeToken(m_storageTokens, m_storageRefilled, m_setup.limits.storageWritesPerSecond,
                       m_setup.limits.storageWriteBurst, now)) {
            if (!m_storageDropped) {
                addLog("warn", "storage: too many writes at once: some were dropped");
            }
            m_storageDropped = true;
            break;
        }
        m_storageDropped = false;
        const std::string key = m.at("key").get<std::string>();
        if (parsed->type == HostMessageType::StorageRemove) {
            m_storage->kv.remove(key);
        } else if (auto stored = m_storage->kv.set(key, m.at("value")); !stored) {
            addLog("warn", "storage: " + stored.error().message);
            break;
        }
        if (!m_storageSaveAt) {
            m_storageSaveAt = now + kStorageSaveDelay; // many changes, one write
        }
        break;
    }
    case HostMessageType::Log:
        addLog(m.at("level").get<std::string>(), m.at("message").get<std::string>());
        break;
    case HostMessageType::Pong:
        break;
    }
}

void ExtensionHost::handleRequest(const Json& m) {
    const Json id = m.at("id");
    const std::string method = m.at("method").get<std::string>();
    const Json& params = m.at("params");
    Json reply = Json::object();
    const CapabilitySet granted = m_setup.granted;
    const auto now = Clock::now();
    if (!takeToken(m_requestTokens, m_requestsRefilled, m_setup.limits.requestsPerSecond, m_setup.limits.requestBurst,
                   now)) {
        sendReply(id, Json{{"error", {{"code", "RateLimited"}, {"message", "too many requests at once: slow down"}}}});
        return;
    }
    try {
        if (method == "commands.run" && granted.has(Capability::MediaAnalysis) &&
            runsModel(params.at("document"), *m_setup.registry)) {
            // A model run takes minutes: never inside pump() (S5-P2-1). The app runs it as a command job (progress,
            // Cancel); the call's time limits wait meanwhile.
            ++m_longJobs;
            pause(now);
            auto finish = [this, alive = m_alive, id](Json result) {
                if (!*alive || (m_state != HostState::Starting && m_state != HostState::Ready)) {
                    return; // the extension has ended meanwhile
                }
                if (m_longJobs > 0) {
                    --m_longJobs;
                }
                sendReply(id, Json{{"result", std::move(result)}});
                resume(Clock::now());
            };
            if (m_delegate.runLongCommand(LongCommand{params.at("document"), m_policy, std::move(finish)})) {
                return;
            }
            --m_longJobs;
            resume(now);
            reply["error"] = {{"code", "PermissionDenied"},
                              {"message", "this editor cannot run AI model commands for extensions here"}};
        } else if (method == "commands.run") {
            DeadlineControl control(now + m_setup.limits.callTime);
            reply["result"] = command::toJson(m_engine->executeJson(params.at("document"), &control));
        } else if (method == "commands.list") {
            Json list = Json::array();
            for (const command::CommandSpec& spec : m_setup.registry->all()) {
                list.push_back(describeSpec(spec, granted, false));
            }
            reply["result"] = list;
        } else {
            const std::string name = params.at("name").get<std::string>();
            if (const command::CommandSpec* spec = m_setup.registry->find(name)) {
                reply["result"] = describeSpec(*spec, granted, true);
            } else {
                reply["error"] = {{"code", "UnknownCommand"}, {"message", "unknown command \"" + name + "\""}};
            }
        }
    } catch (const std::exception& e) {
        reply.erase("result");
        reply["error"] = {{"code", "Internal"}, {"message", e.what()}};
    }
    sendReply(id, std::move(reply));
}

void ExtensionHost::sendReply(const Json& id, Json body) {
    Json reply{{"type", "reply"}, {"id", id}};
    reply.update(body);
    std::string text = wire(reply);
    if (text.size() > kMaxReplyBytes) {
        text = wire(Json{{"type", "reply"},
                         {"id", id},
                         {"error", {{"code", "InvalidRequest"},
                                    {"message", "the result is larger than 8 MiB: ask for less (from / to / limit)"}}}});
    }
    if (!m_watcher->send(std::move(text))) {
        ended(HostState::Crashed, "it stopped reading what the editor sends", true);
    }
}

void ExtensionHost::saveStorage() {
    m_storageSaveAt.reset();
    if (m_storage->kv.dirty()) {
        if (auto saved = m_storage->kv.save(); !saved) {
            addLog("error", "storage: " + saved.error().message);
        }
    }
}

void ExtensionHost::addLog(std::string level, std::string message) {
    m_log.push_back({std::move(level), std::move(message)});
    while (m_log.size() > m_setup.limits.logLines) {
        m_log.pop_front();
    }
    m_delegate.log(m_log.back());
}

} // namespace wabcut::script
