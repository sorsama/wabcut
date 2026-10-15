#include "ScriptHost.h"

#include <wabcut/script/Protocol.h>

#include <QFile>

#include <cstdio>

namespace wabcut::script::host {

namespace {

constexpr int kLogsPerSecond = 200;
constexpr int kNotificationsPerMinute = 30;
constexpr std::size_t kMaxResultBytes = 1024 * 1024;
constexpr std::size_t kMaxOpenPrompts = 4;

std::string utf8(const QString& s) { return s.toStdString(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

std::string wire(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

std::string errorText(const QJSValue& error) {
    std::string text = utf8(error.toString());
    const QJSValue line = error.property(QStringLiteral("lineNumber"));
    const QJSValue file = error.property(QStringLiteral("fileName"));
    if (line.isNumber() && file.isString()) {
        std::string where = utf8(file.toString());
        if (const auto slash = where.rfind('/'); slash != std::string::npos) {
            where = where.substr(slash + 1);
        }
        text = where + ":" + std::to_string(line.toInt()) + ": " + text;
    }
    return text;
}

} // namespace

// ---- Output ------------------------------------------------------------------------------------------------------

void Output::write(const Json& message) {
    std::string line = wire(message);
    if (line.size() > kMaxHostMessageBytes) {
        if (message.value("type", "") != "done") {
            std::fprintf(stderr, "wabcut-scripthost: a %s message too large to send was dropped\n",
                         message.value("type", "?").c_str());
            return;
        }
        line = wire(Json{{"type", "done"},
                         {"call", message.at("call")},
                         {"error", {{"kind", "result"}, {"message", "the result is too large to send"}}}});
    }
    line += '\n';
    std::lock_guard lock(m_mutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

// ---- Watchdog ----------------------------------------------------------------------------------------------------

Watchdog::Watchdog(QJSEngine& engine) : m_engine(engine), m_thread([this] { run(); }) {}

Watchdog::~Watchdog() {
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void Watchdog::arm() {
    {
        std::lock_guard lock(m_mutex);
        m_fired = false;
        m_paused = false;
        m_deadline = std::chrono::steady_clock::now() + m_limit;
    }
    m_cv.notify_all();
}

bool Watchdog::disarm() {
    std::lock_guard lock(m_mutex);
    m_deadline.reset();
    m_paused = false;
    return m_fired;
}

void Watchdog::pause() {
    std::lock_guard lock(m_mutex);
    if (m_deadline) {
        m_remaining = *m_deadline - std::chrono::steady_clock::now();
        m_deadline.reset();
        m_paused = true;
    }
}

void Watchdog::resume() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_paused) {
            return;
        }
        m_paused = false;
        m_deadline = std::chrono::steady_clock::now() + std::max(m_remaining, std::chrono::steady_clock::duration{});
    }
    m_cv.notify_all();
}

void Watchdog::run() {
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        if (!m_deadline) {
            m_cv.wait(lock);
            continue;
        }
        if (std::chrono::steady_clock::now() >= *m_deadline) {
            m_deadline.reset();
            m_fired = true;
            m_engine.setInterrupted(true); // thread-safe; the running script aborts at once and cannot catch it
            continue;
        }
        m_cv.wait_until(lock, *m_deadline);
    }
}

// ---- Bridge ------------------------------------------------------------------------------------------------------

QString Bridge::call(const QString& method, const QString& params) {
    // A script controls these arguments: a member of another type must become a JavaScript error, never a C++
    // exception through the engine.
    try {
        return callImpl(method, params);
    } catch (const std::exception& e) {
        m_runtime.throwError(QJSValue::TypeError, std::string("wabcut: ") + e.what());
    }
    return {};
}

QString Bridge::callImpl(const QString& method, const QString& params) {
    const Json p = Json::parse(utf8(params), nullptr, false);
    if (!p.is_object()) {
        m_runtime.throwError(QJSValue::TypeError, "request params must be an object");
        return {};
    }
    if (p.dump().size() > kMaxHostMessageBytes - 4096) {
        m_runtime.throwError(QJSValue::RangeError, "the request is larger than 2 MiB: split it into smaller batches");
        return {};
    }
    const auto reply = m_runtime.request(utf8(method), p);
    if (!reply) {
        m_runtime.throwError(QJSValue::GenericError, "the call was cancelled");
        return {};
    }
    return qs(wire(*reply));
}

void Bridge::post(const QString& type, const QString& body) {
    // A script controls these arguments: a member of another type must become a JavaScript error, never a C++
    // exception through the engine.
    try {
        postImpl(type, body);
    } catch (const std::exception& e) {
        m_runtime.throwError(QJSValue::TypeError, std::string("wabcut: ") + e.what());
    }
}

void Bridge::postImpl(const QString& type, const QString& body) {
    const std::string t = utf8(type);
    Json b = Json::parse(utf8(body), nullptr, false);
    if (!b.is_object()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    Runtime& r = m_runtime;
    if (t == "log") {
        r.log(b.value("level", "log"), b.value("message", ""));
        return;
    }
    if (t == "notify" || t == "toast") {
        if (now - r.m_notifyWindow > std::chrono::minutes(1)) {
            r.m_notifyWindow = now;
            r.m_notifiesInWindow = 0;
        }
        if (++r.m_notifiesInWindow > kNotificationsPerMinute) {
            r.log("warn", "too many notifications: this one was not shown");
            return;
        }
        std::string message = b.value("message", "");
        if (message.empty()) {
            return;
        }
        const std::size_t limit = t == "toast" ? kMaxToastBytes : kMaxNotifyBytes;
        if (message.size() > limit) {
            std::size_t cut = limit;
            while (cut > 0 && (static_cast<unsigned char>(message[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            message.resize(cut);
        }
        b["message"] = message;
        if (t == "notify" && b.value("level", "info") != "warning" && b.value("level", "info") != "error") {
            b["level"] = "info";
        }
        r.post(t, b);
        return;
    }
    if (t == "prompt") {
        auto form = validatePromptForm(b.value("form", Json()));
        if (!form) {
            r.throwError(QJSValue::TypeError, "wabcut.ui.prompt: " + form.error());
            return;
        }
        if (r.m_openPrompts >= kMaxOpenPrompts) {
            r.throwError(QJSValue::GenericError, "wabcut.ui.prompt: too many prompts are open");
            return;
        }
        ++r.m_openPrompts;
        r.post("prompt", Json{{"prompt", b.at("prompt")}, {"form", std::move(*form)}});
        return;
    }
    if (t == "registerCommand") {
        const std::string id = b.value("id", "");
        const std::string title = b.value("title", "");
        if (!isValidCommandId(id) || title.empty() || title.size() > 100 || !isDisplayText(title, false)) {
            r.throwError(QJSValue::TypeError, "wabcut.ui.registerCommand(id, title, handler): ids match "
                                              "[A-Za-z][A-Za-z0-9._-]* (64 characters at most), titles are 1 to 100");
            return;
        }
        if (!r.m_commands.contains(id) && r.m_commands.size() >= kMaxRegisteredCommands) {
            r.throwError(QJSValue::RangeError, "an extension may register at most 64 commands");
            return;
        }
        r.m_commands.insert(id);
        r.post("registerCommand", b);
        return;
    }
    if (t == "subscribe") {
        r.post("subscribe", b);
    }
}

void Bridge::done(double call, const QString& error, const QString& result) {
    const auto id = static_cast<std::uint64_t>(call);
    if (!error.isEmpty()) {
        m_runtime.finishCall(id, Json::parse(utf8(error), nullptr, false), Json());
        return;
    }
    const std::string text = utf8(result);
    if (text.size() > kMaxResultBytes) {
        m_runtime.finishCall(id, Json{{"kind", "result"}, {"message", "the result is larger than 1 MiB"}}, Json());
        return;
    }
    m_runtime.finishCall(id, Json(), Json::parse(text, nullptr, false));
}

QString Bridge::storageGet(const QString& key) {
    const auto value = m_runtime.m_storage.get(utf8(key));
    return value ? qs(wire(*value)) : QString();
}

void Bridge::storageSet(const QString& key, const QString& value) {
    // A script controls these arguments: a member of another type must become a JavaScript error, never a C++
    // exception through the engine.
    try {
        storageSetImpl(key, value);
    } catch (const std::exception& e) {
        m_runtime.throwError(QJSValue::TypeError, std::string("wabcut: ") + e.what());
    }
}

void Bridge::storageSetImpl(const QString& key, const QString& value) {
    const std::string k = utf8(key);
    Json v = Json::parse(utf8(value), nullptr, false);
    if (auto stored = m_runtime.m_storage.set(k, std::move(v)); !stored) {
        m_runtime.throwError(QJSValue::RangeError, "wabcut.storage.set: " + stored.error().message);
        return;
    }
    m_runtime.m_dirtyKeys.insert(k);
}

bool Bridge::storageRemove(const QString& key) {
    const std::string k = utf8(key);
    const bool removed = m_runtime.m_storage.remove(k);
    if (removed) {
        m_runtime.m_dirtyKeys.insert(k);
    }
    return removed;
}

QStringList Bridge::storageKeys() {
    QStringList keys;
    for (const auto& [key, value] : m_runtime.m_storage.values().items()) {
        keys.push_back(qs(key));
    }
    return keys;
}

// ---- Runtime -----------------------------------------------------------------------------------------------------

Runtime::Runtime(Output& output) : m_output(output), m_bridge(*this), m_watchdog(m_engine) {
    QJSEngine::setObjectOwnership(&m_bridge, QJSEngine::CppOwnership);
    m_jsonParse = m_engine.globalObject().property(QStringLiteral("JSON")).property(QStringLiteral("parse"));
}

Runtime::~Runtime() = default;

void Runtime::throwError(QJSValue::ErrorType type, const std::string& message) { m_engine.throwError(type, qs(message)); }

QJSValue Runtime::fromJson(const Json& value) { return m_jsonParse.call({qs(wire(value))}); }

void Runtime::post(const std::string& type, Json body) {
    body["type"] = type;
    m_output.write(body);
}

void Runtime::log(const std::string& level, const std::string& message) {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_logWindow > std::chrono::seconds(1)) {
        if (m_logsDropped > 0) {
            m_output.write(Json{{"type", "log"},
                                {"level", "warn"},
                                {"message", std::to_string(m_logsDropped) + " log lines were dropped (too many)"}});
        }
        m_logWindow = now;
        m_logsInWindow = 0;
        m_logsDropped = 0;
    }
    if (++m_logsInWindow > kLogsPerSecond) {
        ++m_logsDropped;
        return;
    }
    std::string text = message;
    if (text.size() > kMaxLogBytes) {
        std::size_t cut = kMaxLogBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text.resize(cut);
    }
    m_output.write(Json{{"type", "log"}, {"level", level}, {"message", text}});
}

void Runtime::enterSection() {
    if (m_depth++ == 0) {
        m_limitReported = false;
        m_watchdog.arm();
    }
}

void Runtime::leaveSection() {
    if (--m_depth > 0) {
        return;
    }
    const bool fired = m_watchdog.disarm();
    bool cancelInterrupted = false;
    {
        std::lock_guard lock(m_runningMutex);
        cancelInterrupted = std::exchange(m_cancelInterrupted, false);
    }
    if (fired && !m_limitReported) {
        // Script that no call or listener owns ran past the limit: a promise reaction, say.
        log("error", "a script ran past its time limit (" + std::to_string(m_watchdog.limit().count()) +
                         " ms) and was stopped");
    }
    if (fired || cancelInterrupted) {
        m_engine.setInterrupted(false); // the next script may run
    }
    flushStorage();
}

void Runtime::flushStorage() {
    for (const std::string& key : m_dirtyKeys) {
        if (const auto value = m_storage.get(key)) {
            m_output.write(Json{{"type", "storageSet"}, {"key", key}, {"value", *value}});
        } else {
            m_output.write(Json{{"type", "storageRemove"}, {"key", key}});
        }
    }
    m_dirtyKeys.clear();
}

void Runtime::handle(const Json& message) {
    const std::string type = message.value("type", "");
    try {
        if (type == "hello") {
            hello(message);
        } else if (!m_loaded) {
            return;
        } else if (type == "invoke") {
            invoke(message);
        } else if (type == "event") {
            dispatch("event", {qs(message.value("event", "")), fromJson(message.value("data", Json::object())),
                               QJSValue(message.value("coalesced", 0))});
        } else if (type == "promptAnswer") {
            if (m_openPrompts > 0) {
                --m_openPrompts;
            }
            dispatch("promptAnswer", {QJSValue(static_cast<double>(message.value("prompt", std::uint64_t{0}))),
                                      fromJson(message.value("values", Json()))});
        } else if (type == "cancelled") {
            cancelled(message.at("call").get<std::uint64_t>());
        }
    } catch (const Json::exception& e) {
        log("error", std::string("wabcut-scripthost: a message from the editor was malformed: ") + e.what());
    }
}

void Runtime::hello(const Json& m) {
    try {
        load(m);
    } catch (const Json::exception& e) {
        m_output.write(Json{{"type", "loadFailed"}, {"message", std::string("a malformed hello: ") + e.what()}});
    }
}

void Runtime::load(const Json& m) {
    if (m_loaded) {
        return;
    }
    const auto fail = [&](const std::string& why) { m_output.write(Json{{"type", "loadFailed"}, {"message", why}}); };
    if (m.value("protocol", 0) != kScriptProtocolVersion) {
        fail("this wabcut-scripthost speaks protocol " + std::to_string(kScriptProtocolVersion));
        return;
    }
    const Json& extension = m.at("extension");
    m_extensionId = extension.value("id", "");
    m_watchdog.setLimit(std::chrono::milliseconds(std::clamp(m.at("limits").value("callMs", 5000), 50, 600'000)));
    const Json storage = m.value("storage", Json::object()); // named: items() refers into it
    for (const auto& [key, value] : storage.items()) {
        (void)m_storage.set(key, value); // past a bound: left out
    }

    QFile preludeFile(QStringLiteral(":/wabcut/prelude.js"));
    if (!preludeFile.open(QIODevice::ReadOnly)) {
        fail("wabcut-scripthost: the prelude is missing");
        return;
    }
    const QJSValue prelude = m_engine.evaluate(QString::fromUtf8(preludeFile.readAll()), QStringLiteral("wabcut:prelude.js"));
    if (prelude.isError()) {
        fail("wabcut-scripthost: " + errorText(prelude));
        return;
    }
    const Json init{{"wabcutVersion", m.value("wabcutVersion", "")}, {"extension", extension}};
    QJSValue global = m_engine.globalObject();
    const QJSValue api = prelude.call({m_engine.newQObject(&m_bridge), fromJson(init), global});
    if (api.isError() || !api.isObject()) {
        fail("wabcut-scripthost: " + errorText(api));
        return;
    }
    m_dispatch = api.property(QStringLiteral("dispatch"));
    global.setProperty(QStringLiteral("globalThis"), global); // V4 lacks it; scripts written for newer engines use it
    global.setProperty(QStringLiteral("wabcut"), api.property(QStringLiteral("wabcut")));
    global.setProperty(QStringLiteral("console"), api.property(QStringLiteral("console")));

    for (const Json& script : m.at("scripts")) {
        const std::string path = script.at("path").get<std::string>();
        const QJSValue result = m_engine.evaluate(qs(script.at("source").get<std::string>()),
                                                  qs("wabcut-extension:/" + m_extensionId + "/" + path));
        if (m_watchdog.fired()) {
            m_limitReported = true;
            fail(path + " ran past the time limit (" + std::to_string(m_watchdog.limit().count()) +
                 " ms) while loading");
            return;
        }
        if (result.isError()) {
            fail(errorText(result));
            return;
        }
    }
    m_loaded = true;
    m_output.write(Json{{"type", "ready"}});
}

QJSValue Runtime::dispatch(const char* name, const QJSValueList& args) {
    const QJSValue result = m_dispatch.property(QString::fromLatin1(name)).call(args);
    if (m_watchdog.fired()) {
        m_limitReported = true;
        log("error", "a script ran past its time limit (" + std::to_string(m_watchdog.limit().count()) +
                         " ms) and was stopped");
    } else if (result.isError()) {
        log("error", errorText(result));
    }
    return result;
}

void Runtime::invoke(const Json& m) {
    const auto call = m.at("call").get<std::uint64_t>();
    m_open.insert(call);
    {
        std::lock_guard lock(m_runningMutex);
        m_running = call;
    }
    const QJSValue result = m_dispatch.property(QStringLiteral("invoke"))
                                .call({QJSValue(static_cast<double>(call)), qs(m.value("command", "")),
                                       fromJson(m.value("args", Json::object()))});
    bool cancelledNow = false;
    {
        std::lock_guard lock(m_runningMutex);
        m_running = 0;
        cancelledNow = m_cancelInterrupted;
    }
    if (cancelledNow) {
        finishCall(call, Json{{"kind", "cancelled"}, {"message", "the editor cancelled the call"}}, Json());
    } else if (m_watchdog.fired()) {
        m_limitReported = true;
        const std::string why =
            "the script ran past its time limit (" + std::to_string(m_watchdog.limit().count()) + " ms) and was stopped";
        if (m_open.contains(call)) {
            finishCall(call, Json{{"kind", "timeLimit"}, {"message", why}}, Json());
        } else {
            log("error", why + " (after the call had answered: a promise reaction)");
        }
    } else if (result.isError()) {
        finishCall(call, Json{{"kind", "exception"}, {"message", errorText(result)}}, Json());
    }
    // Otherwise the prelude reported it (a plain return), or will (a promise).
}

void Runtime::finishCall(std::uint64_t call, Json error, Json result) {
    if (m_open.erase(call) == 0) {
        return; // finished already (cancelled, timed out) or unknown
    }
    if (!error.is_null()) {
        if (!error.is_object() || !error.contains("kind") || !error.contains("message")) {
            error = Json{{"kind", "exception"}, {"message", "the call failed"}};
        }
        m_output.write(Json{{"type", "done"}, {"call", call}, {"error", error}});
        return;
    }
    if (result.is_discarded()) {
        result = nullptr;
    }
    m_output.write(Json{{"type", "done"}, {"call", call}, {"result", std::move(result)}});
}

void Runtime::cancelled(std::uint64_t call) {
    finishCall(call, Json{{"kind", "cancelled"}, {"message", "the editor cancelled the call"}}, Json());
}

std::optional<Json> Runtime::request(const std::string& method, Json params) {
    std::uint64_t id = 0;
    {
        std::lock_guard lock(m_replyMutex);
        if (m_editorGone) {
            return std::nullopt;
        }
        id = m_nextRequest++;
        m_abortWaits = false;
    }
    m_output.write(Json{{"type", "request"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
    m_watchdog.pause(); // waiting for the editor is not script time
    std::unique_lock lock(m_replyMutex);
    m_replyCv.wait(lock, [&] { return m_replies.contains(id) || m_abortWaits || m_editorGone; });
    m_watchdog.resume();
    const auto it = m_replies.find(id);
    if (it == m_replies.end()) {
        return std::nullopt;
    }
    Json reply = std::move(it->second);
    m_replies.erase(it);
    return reply;
}

void Runtime::deliverReply(std::uint64_t id, Json reply) {
    {
        std::lock_guard lock(m_replyMutex);
        if (id == 0 || id >= m_nextRequest) {
            return;
        }
        reply.erase("type");
        reply.erase("id");
        m_replies[id] = std::move(reply);
    }
    m_replyCv.notify_all();
}

void Runtime::cancel(std::uint64_t call) {
    bool running = false;
    {
        std::lock_guard lock(m_runningMutex);
        if (m_running == call && call != 0) {
            running = true;
            m_cancelInterrupted = true;
            m_engine.setInterrupted(true); // thread-safe
        }
    }
    if (running) {
        std::lock_guard lock(m_replyMutex);
        m_abortWaits = true; // a request the script waits for ends too
    }
    m_replyCv.notify_all();
}

void Runtime::editorGone() {
    {
        std::lock_guard lock(m_replyMutex);
        m_editorGone = true;
    }
    m_replyCv.notify_all();
    m_engine.setInterrupted(true);
}

} // namespace wabcut::script::host
