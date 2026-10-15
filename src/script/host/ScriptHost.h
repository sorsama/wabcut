#pragma once

// wabcut-scripthost: runs one extension's JavaScript in a QJSEngine and speaks the protocol of
// wabcut/script/Protocol.h on stdin / stdout (docs/extensions.md).
//
// The engine lives on the main thread, which runs a Qt event loop (promise reactions are delivered through it). A
// reader thread takes the editor's lines: replies to the script's requests and cancels go straight to the waiting
// main thread, everything else is queued to it. Every event the main thread handles, and so every piece of
// JavaScript, runs under the watchdog: past the call time limit it interrupts the engine (QJSEngine::setInterrupted,
// which aborts the script without a catch), and the call reports "timeLimit". Time spent waiting for the editor
// does not count.
//
// The editor checks everything this process says; the checks here (prompt forms, storage bounds, rates) only turn
// a script's mistake into a JavaScript error instead of a protocol violation that would end the process.

#include <wabcut/script/Storage.h>

#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace wabcut::script::host {

/// stdout, shared by the threads: one message per line.
class Output {
public:
    /// Writes `message`; a message larger than the editor reads is not sent (a done becomes an error instead).
    void write(const Json& message);

private:
    std::mutex m_mutex;
};

/// Interrupts the engine when the main thread stays in one section (an event, a call) past the limit.
class Watchdog {
public:
    explicit Watchdog(QJSEngine& engine);
    ~Watchdog();
    void setLimit(std::chrono::milliseconds limit) { m_limit = limit; }
    std::chrono::milliseconds limit() const { return m_limit; }
    void arm();
    /// True when the limit was reached since arm().
    bool disarm();
    bool fired() const { return m_fired; }
    /// While the script waits for the editor.
    void pause();
    void resume();

private:
    void run();

    QJSEngine& m_engine;
    std::chrono::milliseconds m_limit{5000};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<std::chrono::steady_clock::time_point> m_deadline;
    std::chrono::steady_clock::duration m_remaining{};
    bool m_paused = false;
    bool m_quit = false;
    std::atomic<bool> m_fired{false};
    std::thread m_thread;
};

class Runtime;

/// What the prelude (prelude.js) calls; reachable from no script.
class Bridge : public QObject {
    Q_OBJECT
public:
    explicit Bridge(Runtime& runtime) : m_runtime(runtime) {}

    /// A request to the editor (commands.run, commands.list, commands.describe); the reply as JSON text.
    Q_INVOKABLE QString call(const QString& method, const QString& params);
    /// notify, toast, prompt, registerCommand, subscribe, log.
    Q_INVOKABLE void post(const QString& type, const QString& body);
    /// A handler finished: `error` or `result` (JSON text).
    Q_INVOKABLE void done(double call, const QString& error, const QString& result);
    /// The value as JSON text, "" when there is none.
    Q_INVOKABLE QString storageGet(const QString& key);
    Q_INVOKABLE void storageSet(const QString& key, const QString& value);
    Q_INVOKABLE bool storageRemove(const QString& key);
    Q_INVOKABLE QStringList storageKeys();

private:
    QString callImpl(const QString& method, const QString& params);
    void postImpl(const QString& type, const QString& body);
    void storageSetImpl(const QString& key, const QString& value);

    Runtime& m_runtime;
};

class Runtime {
public:
    explicit Runtime(Output& output);
    ~Runtime();

    // ---- main thread ----
    void handle(const Json& message);
    /// Around every event the main thread handles (the application's notify()).
    void enterSection();
    void leaveSection();

    // ---- the reader thread ----
    void deliverReply(std::uint64_t id, Json reply);
    void cancel(std::uint64_t call);
    /// The editor closed the pipe (or asked to shut down): waits end.
    void editorGone();

private:
    friend class Bridge;

    void hello(const Json& message);
    void load(const Json& message);
    void invoke(const Json& message);
    void cancelled(std::uint64_t call);
    /// Calls a dispatch function; logs what a listener threw past the prelude.
    QJSValue dispatch(const char* name, const QJSValueList& args);
    QJSValue fromJson(const Json& value);
    std::optional<Json> request(const std::string& method, Json params);
    void finishCall(std::uint64_t call, Json error, Json result);
    void post(const std::string& type, Json body);
    void log(const std::string& level, const std::string& message);
    void flushStorage();
    void throwError(QJSValue::ErrorType type, const std::string& message);

    Output& m_output;
    QJSEngine m_engine;
    Bridge m_bridge;
    Watchdog m_watchdog;
    QJSValue m_dispatch;
    QJSValue m_jsonParse;
    bool m_loaded = false;
    int m_depth = 0;
    bool m_limitReported = false; ///< this section's time-limit stop was reported (by a call, listener or load)
    std::string m_extensionId;

    // Calls: open ones, and the one whose script runs right now (the reader thread cancels it).
    std::set<std::uint64_t> m_open;
    std::mutex m_runningMutex;
    std::uint64_t m_running = 0;
    bool m_cancelInterrupted = false;

    // Requests waiting for the editor's reply.
    std::mutex m_replyMutex;
    std::condition_variable m_replyCv;
    std::map<std::uint64_t, Json> m_replies;
    std::uint64_t m_nextRequest = 1;
    bool m_abortWaits = false;
    bool m_editorGone = false;

    KeyValueStore m_storage;
    std::set<std::string> m_dirtyKeys;
    std::set<std::string> m_commands;
    std::size_t m_openPrompts = 0;

    // Rates: log lines per second, notifications per minute.
    std::chrono::steady_clock::time_point m_logWindow;
    int m_logsInWindow = 0;
    int m_logsDropped = 0;
    std::chrono::steady_clock::time_point m_notifyWindow;
    int m_notifiesInWindow = 0;
};

} // namespace wabcut::script::host
