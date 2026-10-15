#pragma once

// The editor's side of one running extension (docs/extensions.md, "Runtime").
//
// Each extension runs in its own wabcut-scripthost process, the way audio plugins and AI models run out of process:
// a script that loops, runs out of memory or crashes the JavaScript engine takes only its own process down. On
// Windows the process lives in a job object that caps its memory, forbids child processes and denies it the
// clipboard and other processes' windows; its JavaScript has no file, network or process API at all (the scripts
// arrive as text in the hello message). The host's watchdog interrupts a script that runs past the time limit; if it
// does not stop, the editor cancels the call and then kills the process.
//
// What a script asks for goes through the same typed command engine as every other client (CommandEngine): limited
// to the capabilities the user granted, commands that run an AI model only with MediaAnalysis (and still under the
// host's consent, spend cap and no-cloud services), every edit one undo step labelled "<extension name>: <title>".
//
// Threads: every member belongs to the session's thread (the GUI thread in the app), like CommandEngine. A watcher
// thread collects the helper's lines and calls the delegate's wake(); pump() then handles them on the session thread.
// The editor never waits for a script; a script waits for the editor.

#include <wabcut/command/Engine.h>
#include <wabcut/model/Error.h>
#include <wabcut/script/Events.h>
#include <wabcut/script/Extension.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::script {

/// A call never runs longer than this many times callTime in all (S5-P2-1), however often the script asks the editor
/// for something; time the user spends on its prompts and long editor jobs it waits for do not count.
inline constexpr int kCallHardLimitFactor = 6;

struct HostLimits {
    /// Script time per call (a handler, an event listener, loading); the helper's watchdog interrupts it.
    std::chrono::milliseconds callTime{5000};
    /// Past callTime + grace without progress (a request to the editor, a prompt answered: never a log line, a
    /// storage write or a notification), the editor cancels the call; past another grace it kills the helper. The
    /// same once the call has run kCallHardLimitFactor x callTime in all.
    std::chrono::milliseconds grace{2000};
    /// Until the helper has evaluated the scripts.
    std::chrono::milliseconds startTime{15000};
    /// The helper process's memory cap (Windows job object): an allocation past it fails and ends the process.
    std::size_t memoryBytes = std::size_t{512} * 1024 * 1024;
    int notificationsPerMinute = 30;
    /// Prompts are shown only while a call the user started runs, at most this many a minute (S5-P2-2).
    int promptsPerMinute = 5;
    /// Requests to the editor and storage writes: a token bucket each, refilled per second, up to the burst (S5-P2-1).
    int requestsPerSecond = 100;
    int requestBurst = 400;
    int storageWritesPerSecond = 100;
    int storageWriteBurst = 1000;
    std::size_t maxPendingPrompts = 4;
    std::size_t logLines = 500;
};

struct HostSetup {
    ExtensionBundle bundle;
    /// What the extension may do (PermissionStore::effective).
    command::CapabilitySet granted;
    EditSession* session = nullptr;
    const command::CommandRegistry* registry = nullptr; ///< editingCommands() in the app and the CLI
    /// The host's services (transcripts, AI consent, spend cap...): the same as for every other client.
    std::shared_ptr<command::CommandServices> services;
    std::filesystem::path helper;      ///< wabcut-scripthost(.exe); scriptHostPath() by default
    std::filesystem::path storageFile; ///< the extension's key-value store (Storage.h); empty = not kept
    std::string wabcutVersion;
    HostLimits limits;
};

enum class HostState {
    Starting, ///< the helper is evaluating the scripts
    Ready,
    Stopped,  ///< stop() (or the editor shut it down)
    Failed,   ///< the scripts did not load (an exception, a time limit at load, no ready in time)
    Crashed,  ///< the helper ended on its own: a crash, the memory cap, a broken protocol, a hang killed
};
std::string_view toString(HostState state);

struct CallResult {
    bool ok = false;
    Json result;           ///< what the handler returned (JSON), when ok
    std::string errorKind; ///< exception, timeLimit, cancelled, stopped, unknownCommand, result
    std::string error;
};

struct RegisteredCommand {
    std::string id;
    std::string title;
};

struct Notification {
    std::string level; ///< info, warning, error, toast
    std::string message;
};

struct PromptRequest {
    std::uint64_t prompt = 0;
    Json form; ///< validated (Protocol.h)
};

struct StopReport {
    HostState state = HostState::Stopped;
    std::string reason;
    bool memoryLimit = false; ///< the process reached its memory cap
    bool timeLimit = false;   ///< killed because a script would not stop
};

struct LogLine {
    std::string level;
    std::string message;
};

/// A request of an extension that names a command running an AI model (CommandSpec::runsModel): it can take
/// minutes, so the host never runs it inside pump() (S5-P2-1) and hands it to its owner instead.
struct LongCommand {
    Json document;                ///< a command request or batch, as the script sent it
    command::EnginePolicy policy; ///< the extension's own: run it with exactly these grants and undo labels
    /// Call once, on the session thread, with command::toJson(result). The call's time limits wait until then.
    std::function<void(Json result)> finish;
};

/// What the host tells its owner. wake() comes from the watcher thread; everything else on the session thread,
/// inside pump().
class HostDelegate {
public:
    virtual ~HostDelegate() = default;
    /// Run `job` where long work runs - the app's command-job worker, with its progress and Cancel - and return true;
    /// a command engine on the session with job.policy runs it as the extension would. False (the default): the
    /// script gets a PermissionDenied error saying the editor cannot run it here.
    virtual bool runLongCommand(LongCommand /*job*/) { return false; }
    /// The helper sent something (or a deadline passed): call pump() on the session thread soon.
    virtual void wake() = 0;
    virtual void notify(const Notification&) {}
    /// Show the form; answer with answerPrompt() (also a cancel).
    virtual void prompt(const PromptRequest&) {}
    /// A prompt the extension can no longer use (it stopped): close it.
    virtual void promptClosed(std::uint64_t /*prompt*/) {}
    virtual void commandsChanged() {}
    virtual void stopped(const StopReport&) {}
    virtual void log(const LogLine&) {}
};

/// <directory of the running executable>/wabcut-scripthost(.exe)
std::filesystem::path scriptHostPath();

class ExtensionHost {
public:
    using Done = std::function<void(const CallResult&)>;
    using Clock = std::chrono::steady_clock;

    /// Starts the helper and sends it the scripts. Errors: NotFound (no helper), IoError (the process or its
    /// sandbox could not be set up), InvalidArgument (a setup without session or registry).
    static std::expected<std::unique_ptr<ExtensionHost>, Error> start(HostSetup setup, HostDelegate& delegate);
    ~ExtensionHost(); ///< stop()
    ExtensionHost(const ExtensionHost&) = delete;
    ExtensionHost& operator=(const ExtensionHost&) = delete;

    /// Runs a registered command's handler with `args`; `done` runs on the session thread (in pump(), or at once
    /// when the call cannot start). Calls made while the scripts load wait for them.
    std::uint64_t invoke(std::string commandId, Json args, Done done);
    /// Passes an editor event to the extension's listeners (throttled; only kinds it listens to).
    void publish(Event event);
    /// The user's answers (validated against the form) or nullopt for Cancel. False for an unknown prompt or
    /// answers that do not fit the form (nothing is sent then).
    bool answerPrompt(std::uint64_t prompt, std::optional<Json> values);
    /// Handles what the helper sent, the deadlines and the held events; never blocks.
    void pump();
    /// Asks the helper to shut down and ends it; pending calls fail with "stopped".
    void stop();

    HostState state() const { return m_state; }
    const Manifest& manifest() const { return m_setup.bundle.manifest; }
    command::CapabilitySet granted() const { return m_setup.granted; }
    const std::vector<RegisteredCommand>& commands() const { return m_commands; }
    const std::deque<LogLine>& log() const { return m_log; }
    const std::optional<StopReport>& stopReport() const { return m_stopReport; }
    /// Prompts waiting for an answer (headless runs answer them).
    std::vector<PromptRequest> pendingPrompts() const;
    /// No call running, no prompt open (and not starting).
    bool idle() const;
    /// When pump() has something to do next without a message (a deadline, a held event, a storage save).
    std::optional<Clock::time_point> nextDeadline() const;

private:
    struct Call {
        Done done;
        Clock::time_point deadline;
        Clock::time_point hardDeadline; ///< kCallHardLimitFactor x callTime after it was sent (pauses excluded)
        bool cancelled = false;
        std::string commandId;
        Json args;
        bool sent = false;
    };
    class Watcher;

    ExtensionHost(HostSetup setup, HostDelegate& delegate);
    void handle(const std::string& line);
    void handleRequest(const Json& message);
    void sendReply(const Json& id, Json body);
    void send(const Json& message);
    void sendCall(std::uint64_t id, Call& call);
    void finish(std::uint64_t id, CallResult result);
    void ended(HostState state, std::string reason, bool killed, bool timeLimit = false);
    void touchCalls(Clock::time_point now);
    /// Prompts open or long editor jobs running: the calls' deadlines wait (they are not script time).
    bool paused() const { return !m_prompts.empty() || m_longJobs > 0; }
    void pause(Clock::time_point now);
    void resume(Clock::time_point now);
    bool callRunning() const;
    /// A token from `bucket` (refilled at `perSecond` up to `burst`), or false when it is empty.
    bool takeToken(double& bucket, Clock::time_point& refilled, int perSecond, int burst, Clock::time_point now);
    void saveStorage();
    void addLog(std::string level, std::string message);

    HostSetup m_setup;
    HostDelegate& m_delegate;
    command::EnginePolicy m_policy; ///< the engine's (LongCommand::policy)
    std::unique_ptr<command::CommandEngine> m_engine;
    std::unique_ptr<Watcher> m_watcher;
    HostState m_state = HostState::Starting;
    Clock::time_point m_startDeadline;
    std::map<std::uint64_t, Call> m_calls;
    std::uint64_t m_nextCall = 1;
    std::map<std::uint64_t, Json> m_prompts; ///< open prompts and their forms
    std::vector<RegisteredCommand> m_commands;
    std::array<bool, kEventKinds> m_subscribed{};
    EventThrottle m_throttle;
    std::deque<Clock::time_point> m_recentNotifications;
    bool m_notificationsDropped = false;
    std::deque<Clock::time_point> m_recentPrompts;
    std::size_t m_longJobs = 0;
    std::optional<Clock::time_point> m_pausedSince;
    double m_requestTokens = 0.0;
    Clock::time_point m_requestsRefilled;
    double m_storageTokens = 0.0;
    Clock::time_point m_storageRefilled;
    bool m_storageDropped = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); ///< for runLongCommand's finish after the host is gone
    std::deque<LogLine> m_log;
    std::optional<StopReport> m_stopReport;
    std::optional<Clock::time_point> m_storageSaveAt;
    struct Storage;
    std::unique_ptr<Storage> m_storage;
};

} // namespace wabcut::script
