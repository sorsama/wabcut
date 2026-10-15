#include <wabcut/plughost/PluginHost.h>

#include <wabcut/plughost/Ipc.h>

#include <wabcut/audio/RealTime.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <thread>

using nlohmann::json;

namespace wabcut::plughost {

using audio::rt::CheckedMutex;

std::string_view toString(InstanceHealth health) {
    switch (health) {
    case InstanceHealth::Running:
        return "running";
    case InstanceHealth::Crashed:
        return "crashed";
    case InstanceHealth::Failed:
        return "failed";
    case InstanceHealth::Hung:
        return "hung";
    }
    return "?";
}

namespace {

std::atomic<std::uint64_t> gNextInstance{1};

std::string hex32(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", static_cast<unsigned>(v));
    return text;
}

/// `j[key]` as a T, or `fallback` when `j` is not an object, has no such key or holds another type there. What the
/// helper sends is untrusted (a plugin can corrupt it): reading it never throws.
template <class T>
T field(const json& j, const char* key, T fallback) {
    try {
        if (j.is_object()) {
            if (const auto it = j.find(key); it != j.end() && !it->is_null()) {
                return it->get<T>();
            }
        }
    } catch (const json::exception&) {
    }
    return fallback;
}

} // namespace

// ---- HostProcess ------------------------------------------------------------------------------------------------

class HostProcess : public std::enable_shared_from_this<HostProcess> {
public:
    static std::expected<std::shared_ptr<HostProcess>, Error> start(const HostOptions& options) {
        std::string error;
        auto child = ChildProcess::start(options.helper, {"--serve"}, options.logFile, error);
        if (!child) {
            return std::unexpected(Error(ErrorCode::IoError, "cannot start the plugin helper: " + error));
        }
        auto process = std::shared_ptr<HostProcess>(new HostProcess());
        process->m_child = std::move(child);
        process->m_reader = std::thread([raw = process.get()] { raw->readLoop(); });
        auto hello = process->request(json{{"op", "hello"}}, options.requestTimeout);
        if (!hello) {
            return std::unexpected(hello.error());
        }
        if (field<std::uint32_t>(*hello, "version", 0u) != kProtocolVersion) {
            return std::unexpected(Error(ErrorCode::VersionMismatch, "the plugin helper speaks another protocol version"));
        }
        return process;
    }

    ~HostProcess() {
        if (m_child && m_child->running()) {
            send(json{{"op", "quit"}});
            if (!m_child->wait(std::chrono::milliseconds(2000))) {
                m_child->terminate();
            }
        }
        if (m_child) {
            m_child->terminate();
        }
        if (m_reader.joinable()) {
            m_reader.join();
        }
    }

    std::expected<json, Error> request(json message, std::chrono::milliseconds timeout) {
        std::uint64_t id = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_dead) {
                return std::unexpected(Error(ErrorCode::IoError, m_reason));
            }
            id = m_nextId++;
            m_replies[id] = std::nullopt;
        }
        message["id"] = id;
        if (!write(message.dump())) {
            std::lock_guard lock(m_mutex);
            m_replies.erase(id);
            return std::unexpected(Error(ErrorCode::IoError, "the plugin helper is gone"));
        }
        std::unique_lock lock(m_mutex);
        const bool answered = m_changed.wait_for(lock, timeout, [&] { return m_dead || m_replies[id].has_value(); });
        std::optional<json> reply = std::move(m_replies[id]);
        m_replies.erase(id);
        if (!reply) {
            return std::unexpected(Error(ErrorCode::IoError, answered ? m_reason : "the plugin helper did not answer in time"));
        }
        if (!field<bool>(*reply, "ok", false)) {
            return std::unexpected(Error(ErrorCode::Unsupported, field<std::string>(*reply, "error", "the plugin helper refused")));
        }
        return std::move(*reply);
    }

    /// Fire and forget.
    void send(json message) { write(message.dump()); }

    bool alive() const {
        std::lock_guard lock(m_mutex);
        return !m_dead;
    }
    std::string reason() const {
        std::lock_guard lock(m_mutex);
        return m_reason;
    }

    void attach(const std::shared_ptr<PluginInstance>& instance) {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_instances, [](const std::weak_ptr<PluginInstance>& w) { return w.expired(); });
        m_instances.push_back(instance);
        if (m_dead) {
            instance->m_health.store(static_cast<std::uint8_t>(InstanceHealth::Crashed));
        }
    }

    std::size_t liveInstances() const {
        std::lock_guard lock(m_mutex);
        return static_cast<std::size_t>(std::count_if(m_instances.begin(), m_instances.end(),
                                                      [](const std::weak_ptr<PluginInstance>& w) { return !w.expired(); }));
    }

private:
    HostProcess() = default;

    bool write(const std::string& line) {
        std::lock_guard lock(m_writeMutex);
        return m_child->writeLine(line);
    }

    void readLoop() {
        // The reader thread's body: nothing the helper sends may end the editor (an exception here would).
        try {
            while (auto line = m_child->readLine()) {
                const json message = json::parse(*line, nullptr, false);
                if (!message.is_object()) {
                    continue;
                }
                // A reply carries the request's id (an unsigned number); anything else is not ours and is dropped.
                if (const auto it = message.find("id"); it != message.end() && it->is_number_unsigned()) {
                    std::lock_guard lock(m_mutex);
                    if (const auto reply = m_replies.find(it->get<std::uint64_t>()); reply != m_replies.end()) {
                        reply->second = message;
                    }
                    m_changed.notify_all();
                }
                // Events ("stateChanged", "latencyChanged", "editorClosed") are also counted in shared memory; nothing to
                // do here.
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[plugins] the plugin helper's channel failed: %s\n", e.what());
            m_child->terminate(); // treated as a crash below
        }
        // The control pipe closed: the helper exited (crashed, or was stopped).
        const auto code = m_child->wait(std::chrono::milliseconds(2000));
        std::vector<std::shared_ptr<PluginInstance>> victims;
        {
            std::lock_guard lock(m_mutex);
            m_dead = true;
            m_reason = code ? "the plugin crashed (wabcut-plughost exited with " + hex32(*code) + ")" : "the plugin helper stopped";
            for (auto& w : m_instances) {
                if (auto instance = w.lock()) {
                    victims.push_back(std::move(instance));
                }
            }
            m_changed.notify_all();
        }
        for (auto& instance : victims) {
            instance->m_health.store(static_cast<std::uint8_t>(InstanceHealth::Crashed), std::memory_order_release);
        }
    }

    std::unique_ptr<ChildProcess> m_child;
    std::thread m_reader;
    mutable CheckedMutex m_mutex;
    std::condition_variable_any m_changed;
    std::map<std::uint64_t, std::optional<json>> m_replies;
    std::uint64_t m_nextId = 1;
    bool m_dead = false;
    std::string m_reason;
    std::vector<std::weak_ptr<PluginInstance>> m_instances;
    CheckedMutex m_writeMutex;
};

// ---- PluginInstance ---------------------------------------------------------------------------------------------

struct PluginInstance::Resources {
    std::unique_ptr<SharedMemory> memory;
    std::unique_ptr<NamedEvent> work;
    std::unique_ptr<NamedEvent> done;
};

PluginInstance::~PluginInstance() {
    if (m_process && m_process->alive()) {
        m_process->send(json{{"op", "destroy"}, {"instance", m_id}});
    }
    if (m_shared != nullptr) {
        m_shared->~InstanceShared();
    }
}

namespace {

/// The latency an instance's shared memory reports, or -1 when it is over the limit (a plugin bug: the instance fails).
std::int64_t checkedLatency(const InstanceShared* shared, double sampleRate) noexcept {
    if (shared == nullptr) {
        return 0;
    }
    const auto latency = static_cast<std::int64_t>(shared->latency.load(std::memory_order_acquire));
    return latency <= maxLatencyFrames(sampleRate) ? latency : -1;
}

} // namespace

int PluginInstance::latency() const noexcept {
    const std::int64_t latency = checkedLatency(m_shared, m_sampleRate);
    return latency >= 0 ? static_cast<int>(latency) : 0;
}

std::uint32_t PluginInstance::latencyGeneration() const noexcept {
    return m_shared != nullptr ? m_shared->latencyGeneration.load(std::memory_order_acquire) : 0;
}

InstanceHealth PluginInstance::health() const noexcept {
    const auto h = static_cast<InstanceHealth>(m_health.load(std::memory_order_acquire));
    if (h == InstanceHealth::Running && m_shared != nullptr &&
        (m_shared->status.load(std::memory_order_acquire) == static_cast<std::uint32_t>(InstanceStatus::Failed) ||
         checkedLatency(m_shared, m_sampleRate) < 0)) {
        return InstanceHealth::Failed;
    }
    return h;
}

std::string PluginInstance::problem() const {
    switch (health()) {
    case InstanceHealth::Running:
        return {};
    case InstanceHealth::Crashed:
        return m_process ? m_process->reason() : "the plugin crashed";
    case InstanceHealth::Failed:
        if (m_invalidOutput.load(std::memory_order_acquire)) {
            return "the plugin kept producing invalid samples (NaN or infinity)";
        }
        if (checkedLatency(m_shared, m_sampleRate) < 0) {
            return "the plugin reports a latency of more than " + std::to_string(kMaxLatencySeconds) + " s";
        }
        return "the plugin reported an error while processing";
    case InstanceHealth::Hung:
        return "the plugin stopped responding";
    }
    return {};
}

std::uint64_t PluginInstance::stateChanges() const noexcept {
    return m_shared != nullptr ? m_shared->stateChanges.load(std::memory_order_acquire) : 0;
}

std::expected<std::string, Error> PluginInstance::saveState() {
    auto reply = m_process->request(json{{"op", "saveState"}, {"instance", m_id}}, std::chrono::milliseconds(20000));
    if (!reply) {
        return std::unexpected(reply.error());
    }
    return field<std::string>(*reply, "state", std::string());
}

std::expected<void, Error> PluginInstance::loadState(std::string_view base64) {
    auto reply = m_process->request(json{{"op", "loadState"}, {"instance", m_id}, {"state", std::string(base64)}},
                                    std::chrono::milliseconds(20000));
    if (!reply) {
        return std::unexpected(reply.error());
    }
    return {};
}

std::expected<std::vector<std::pair<std::uint32_t, double>>, Error> PluginInstance::parameterValues() {
    auto reply = m_process->request(json{{"op", "params"}, {"instance", m_id}}, std::chrono::milliseconds(20000));
    if (!reply) {
        return std::unexpected(reply.error());
    }
    std::vector<std::pair<std::uint32_t, double>> out;
    const json params = field<json>(*reply, "params", json::array());
    if (!params.is_array()) {
        return out;
    }
    for (const json& p : params) {
        if (p.is_object() && p.contains("value") && p["value"].is_number()) {
            out.emplace_back(field<std::uint32_t>(p, "id", 0u), field<double>(p, "value", 0.0));
        }
    }
    return out;
}

std::expected<void, Error> PluginInstance::openEditor(const std::string& title) {
    if (!running()) {
        return std::unexpected(Error(ErrorCode::Unsupported, problem()));
    }
    auto reply = m_process->request(json{{"op", "openEditor"}, {"instance", m_id}, {"title", title}}, std::chrono::milliseconds(20000));
    if (!reply) {
        return std::unexpected(reply.error());
    }
    return {};
}

void PluginInstance::closeEditor() {
    if (m_process && m_process->alive()) {
        m_process->send(json{{"op", "closeEditor"}, {"instance", m_id}});
    }
}

void PluginInstance::setKnownState(std::uint64_t hash) noexcept {
    m_knownChanges.store(stateChanges(), std::memory_order_relaxed);
    m_knownHash.store(hash, std::memory_order_release);
}

bool PluginInstance::hasKnownState(std::uint64_t hash) const noexcept {
    return m_knownHash.load(std::memory_order_acquire) == hash && m_knownChanges.load(std::memory_order_relaxed) == stateChanges();
}

void PluginInstance::signalWork() noexcept { m_resources->work->signal(); }

namespace {
std::atomic<std::uint64_t> gRealtimeWaits{0};
} // namespace

bool PluginInstance::waitDone(std::chrono::milliseconds timeout) noexcept {
    if (audio::rt::inRealtimeSection()) {
        gRealtimeWaits.fetch_add(1, std::memory_order_relaxed); // a bug: the audio callback must never wait
    }
    return m_resources->done->wait(timeout);
}

std::uint64_t PluginHost::realtimeWaits() noexcept { return gRealtimeWaits.load(std::memory_order_relaxed); }

void PluginInstance::markHung() noexcept {
    std::uint8_t running = static_cast<std::uint8_t>(InstanceHealth::Running);
    m_health.compare_exchange_strong(running, static_cast<std::uint8_t>(InstanceHealth::Hung));
}

void PluginInstance::markInvalid() noexcept {
    std::uint8_t running = static_cast<std::uint8_t>(InstanceHealth::Running);
    if (m_health.compare_exchange_strong(running, static_cast<std::uint8_t>(InstanceHealth::Failed))) {
        m_invalidOutput.store(true, std::memory_order_release);
    }
}

// ---- PluginHost -------------------------------------------------------------------------------------------------

struct PluginHost::State {
    mutable CheckedMutex mutex;
    std::map<std::string, std::shared_ptr<HostProcess>> processes; ///< by plugin binary (or instance)
    std::vector<std::weak_ptr<PluginInstance>> instances;
};

PluginHost::PluginHost(HostOptions options) : m_options(std::move(options)), m_state(std::make_unique<State>()) {
    if (m_options.helper.empty()) {
        m_options.helper = PluginCatalog::defaultHelper();
    }
}

std::shared_ptr<PluginHost> PluginHost::create(HostOptions options) {
    return std::shared_ptr<PluginHost>(new PluginHost(std::move(options)));
}

PluginHost::~PluginHost() {
    std::map<std::string, std::shared_ptr<HostProcess>> processes;
    {
        std::lock_guard lock(m_state->mutex);
        processes.swap(m_state->processes);
    }
    processes.clear(); // each quits its helper
}

std::expected<std::shared_ptr<HostProcess>, Error> PluginHost::processFor(const PluginInfo& plugin) {
    std::string key = narrow(plugin.path.lexically_normal().wstring());
    if (m_options.processPerInstance) {
        key += "#" + std::to_string(gNextInstance.load());
    }
    {
        std::lock_guard lock(m_state->mutex);
        if (const auto it = m_state->processes.find(key); it != m_state->processes.end()) {
            if (it->second->alive()) {
                return it->second;
            }
            m_state->processes.erase(it); // it crashed: the next instance gets a new helper
        }
    }
    auto started = HostProcess::start(m_options);
    if (!started) {
        return std::unexpected(started.error());
    }
    std::lock_guard lock(m_state->mutex);
    m_state->processes[key] = *started;
    return *started;
}

std::expected<std::shared_ptr<PluginInstance>, Error> PluginHost::instantiate(const PluginInfo& plugin, double sampleRate,
                                                                                std::string_view state) {
    auto process = processFor(plugin);
    if (!process) {
        return std::unexpected(process.error());
    }
    const std::uint64_t id = gNextInstance.fetch_add(1);
    const InstanceNames names = instanceNames(currentProcessId(), id);
    auto resources = std::make_unique<PluginInstance::Resources>();
    std::string error;
    resources->memory = SharedMemory::create(names.memory, sizeof(InstanceShared), error);
    if (resources->memory) {
        resources->work = NamedEvent::create(names.work, error);
        resources->done = NamedEvent::create(names.done, error);
    }
    if (!resources->memory || !resources->work || !resources->done) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot create the instance's shared memory: " + error));
    }
    auto* shared = new (resources->memory->data()) InstanceShared();
    shared->channels = 2;
    shared->sampleRate = static_cast<std::uint32_t>(sampleRate);
    auto reply = (*process)->request(json{{"op", "create"},
                                          {"instance", id},
                                          {"path", narrow(plugin.path.wstring())},
                                          {"plugin", plugin.id},
                                          {"sampleRate", sampleRate},
                                          {"memory", names.memory},
                                          {"work", names.work},
                                          {"done", names.done},
                                          {"state", std::string(state)}},
                                     m_options.requestTimeout);
    if (!reply) {
        shared->~InstanceShared();
        return std::unexpected(Error(reply.error().code, plugin.name + ": " + reply.error().message));
    }
    auto instance = std::shared_ptr<PluginInstance>(new PluginInstance());
    instance->m_process = *process;
    instance->m_resources = std::move(resources);
    instance->m_shared = shared;
    instance->m_plugin = plugin;
    instance->m_id = id;
    instance->m_sampleRate = sampleRate;
    instance->m_editor = field<bool>(*reply, "editor", false);
    instance->m_blockTimeout = m_options.blockTimeout;
    (*process)->attach(instance);
    std::lock_guard lock(m_state->mutex);
    std::erase_if(m_state->instances, [](const std::weak_ptr<PluginInstance>& w) { return w.expired(); });
    m_state->instances.push_back(instance);
    return instance;
}

std::size_t PluginHost::processCount() const {
    std::lock_guard lock(m_state->mutex);
    return static_cast<std::size_t>(std::count_if(m_state->processes.begin(), m_state->processes.end(),
                                                  [](const auto& p) { return p.second->alive(); }));
}

std::vector<std::shared_ptr<PluginInstance>> PluginHost::instances() const {
    std::lock_guard lock(m_state->mutex);
    std::vector<std::shared_ptr<PluginInstance>> out;
    for (const auto& w : m_state->instances) {
        if (auto i = w.lock()) {
            out.push_back(std::move(i));
        }
    }
    return out;
}

} // namespace wabcut::plughost
