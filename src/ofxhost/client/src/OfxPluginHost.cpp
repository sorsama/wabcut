#include <wabcut/ofxhost/OfxPluginHost.h>

#include <wabcut/plughost/Ipc.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <optional>
#include <thread>

using nlohmann::json;
namespace ipc = wabcut::plughost;

namespace wabcut::ofxhost {

std::string_view toString(OfxHealth health) {
    switch (health) {
    case OfxHealth::Running:
        return "running";
    case OfxHealth::Crashed:
        return "crashed";
    case OfxHealth::Failed:
        return "failed";
    case OfxHealth::Hung:
        return "hung";
    }
    return "?";
}

namespace {

std::atomic<std::uint64_t> gNextInstance{1};
/// Consecutive pictures a plug-in may fail before it counts as failed (a plug-in that cannot take one picture may take the
/// next).
constexpr int kFailuresBeforeFailed = 3;

std::string hex32(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", static_cast<unsigned>(v));
    return text;
}

/// `j[key]` as a T, or `fallback`: what the helper sends is untrusted (a plug-in can corrupt it), so reading it never throws.
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

/// Why a request did not come back with an answer.
struct RequestError {
    enum class Kind : std::uint8_t { Refused, Dead, TimedOut } kind = Kind::Refused;
    std::string message;
};

} // namespace

// ---- OfxHostProcess -------------------------------------------------------------------------------------------------

constexpr std::size_t kMaxReplyBytes = std::size_t{1} << 20; ///< one reply line from the helper
constexpr int kMaxReplyDepth = 64;                              ///< how deep it may nest

/// Whether `text` nests arrays and objects at most `limit` deep (brackets inside strings do not count).
static bool nestingWithin(const std::string& text, int limit) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > limit) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return true;
}

class OfxHostProcess : public std::enable_shared_from_this<OfxHostProcess> {
public:
    static std::expected<std::shared_ptr<OfxHostProcess>, Error> start(const OfxHostOptions& options, const std::filesystem::path& binary) {
        std::string error;
        auto child = ipc::ChildProcess::start(options.helper, {"--serve", ipc::narrow(binary.wstring())}, options.logFile, error,
                                              ipc::ChildLimits::restricted(options.helperMemoryBytes));
        if (!child) {
            return std::unexpected(Error(ErrorCode::IoError, "cannot start the OpenFX helper: " + error));
        }
        auto process = std::shared_ptr<OfxHostProcess>(new OfxHostProcess());
        process->m_child = std::move(child);
        process->m_reader = std::thread([raw = process.get()] { raw->readLoop(); });
        const auto hello = process->request(json{{"op", "hello"}}, options.requestTimeout);
        if (!hello) {
            return std::unexpected(Error(ErrorCode::IoError, hello.error().message));
        }
        if (field<std::uint32_t>(*hello, "version", 0u) != kProtocolVersion) {
            return std::unexpected(Error(ErrorCode::VersionMismatch, "the OpenFX helper speaks another protocol version"));
        }
        return process;
    }

    ~OfxHostProcess() {
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

    std::uint32_t pid() const { return m_child->pid(); }

    std::expected<json, RequestError> request(json message, std::chrono::milliseconds timeout) {
        std::uint64_t id = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_dead) {
                return std::unexpected(RequestError{RequestError::Kind::Dead, m_reason});
            }
            id = m_nextId++;
            m_replies[id] = std::nullopt;
        }
        message["id"] = id;
        if (!write(message.dump(-1, ' ', false, json::error_handler_t::replace))) {
            std::lock_guard lock(m_mutex);
            m_replies.erase(id);
            return std::unexpected(RequestError{RequestError::Kind::Dead, "the OpenFX helper is gone"});
        }
        std::unique_lock lock(m_mutex);
        const bool answered = m_changed.wait_for(lock, timeout, [&] { return m_dead || m_replies[id].has_value(); });
        std::optional<json> reply = std::move(m_replies[id]);
        m_replies.erase(id);
        if (!reply) {
            if (m_dead) {
                return std::unexpected(RequestError{RequestError::Kind::Dead, m_reason});
            }
            (void)answered;
            return std::unexpected(RequestError{RequestError::Kind::TimedOut, "the plug-in did not answer in time"});
        }
        if (!field<bool>(*reply, "ok", false)) {
            return std::unexpected(RequestError{RequestError::Kind::Refused, field<std::string>(*reply, "error", "the plug-in refused")});
        }
        return std::move(*reply);
    }

    /// Fire and forget.
    void send(json message) { write(message.dump(-1, ' ', false, json::error_handler_t::replace)); }

    bool alive() const {
        std::lock_guard lock(m_mutex);
        return !m_dead;
    }
    std::string reason() const {
        std::lock_guard lock(m_mutex);
        return m_reason;
    }

    /// Ends the helper because a plug-in in it hung: its instances are Hung, not Crashed.
    void endHung(const std::string& reason) {
        {
            std::lock_guard lock(m_mutex);
            if (m_dead) {
                return;
            }
            m_hung = true;
            m_reason = reason;
        }
        m_child->terminate();
        // The reader thread sees the pipe close and marks the process dead: wait for it (briefly), so the caller sees the
        // helper gone.
        std::unique_lock lock(m_mutex);
        m_changed.wait_for(lock, std::chrono::seconds(2), [&] { return m_dead; });
    }

    void attach(const std::shared_ptr<OfxRemoteInstance>& instance) {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_instances, [](const std::weak_ptr<OfxRemoteInstance>& w) { return w.expired(); });
        m_instances.push_back(instance);
        if (m_dead) {
            instance->markDead(m_hung ? OfxHealth::Hung : OfxHealth::Crashed, m_reason);
        }
    }

private:
    OfxHostProcess() = default;

    bool write(const std::string& line) {
        std::lock_guard lock(m_writeMutex);
        return m_child->writeLine(line);
    }

    void readLoop() {
        // The reader thread's body: nothing the helper sends may end the editor (an exception here would).
        try {
            while (auto line = m_child->readLine()) {
                // A reply is a few hundred bytes of flat JSON: a long or deeply nested line is the helper misbehaving, and
                // parsing and copying it would recurse as deep as it nests.
                if (line->size() > kMaxReplyBytes || !nestingWithin(*line, kMaxReplyDepth)) {
                    continue;
                }
                const json message = json::parse(*line, nullptr, false);
                if (!message.is_object()) {
                    continue;
                }
                if (const auto it = message.find("id"); it != message.end() && it->is_number_unsigned()) {
                    std::lock_guard lock(m_mutex);
                    if (const auto reply = m_replies.find(it->get<std::uint64_t>()); reply != m_replies.end()) {
                        reply->second = message;
                    }
                    m_changed.notify_all();
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ofx] the OpenFX helper's channel failed: %s\n", e.what());
            m_child->terminate(); // treated as a crash below
        }
        // The control pipe closed: the helper exited (crashed, was stopped, or was ended for hanging).
        const auto code = m_child->wait(std::chrono::milliseconds(2000));
        std::vector<std::shared_ptr<OfxRemoteInstance>> victims;
        OfxHealth health = OfxHealth::Crashed;
        std::string reason;
        {
            std::lock_guard lock(m_mutex);
            m_dead = true;
            if (!m_hung) {
                m_reason = code ? "the plug-in crashed (wabcut-ofxhost exited with " + hex32(*code) + ")" : "the OpenFX helper stopped";
            }
            health = m_hung ? OfxHealth::Hung : OfxHealth::Crashed;
            reason = m_reason;
            for (auto& w : m_instances) {
                if (auto instance = w.lock()) {
                    victims.push_back(std::move(instance));
                }
            }
            m_changed.notify_all();
        }
        for (auto& instance : victims) {
            instance->markDead(health, reason);
        }
    }

    std::unique_ptr<ipc::ChildProcess> m_child;
    std::thread m_reader;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::map<std::uint64_t, std::optional<json>> m_replies;
    std::uint64_t m_nextId = 1;
    bool m_dead = false;
    bool m_hung = false;
    std::string m_reason;
    std::vector<std::weak_ptr<OfxRemoteInstance>> m_instances;
    std::mutex m_writeMutex;
};

// ---- OfxRemoteInstance ----------------------------------------------------------------------------------------------

struct OfxRemoteInstance::Block {
    std::unique_ptr<ipc::SharedMemory> memory;
    std::uint64_t capacity = 0;
    std::uint32_t generation = 0;
    std::string name;
    bool announced = false; ///< the helper knows this block (created or attached)
};

OfxRemoteInstance::~OfxRemoteInstance() {
    if (m_process && m_process->alive()) {
        m_process->send(json{{"op", "destroy"}, {"instance", m_id}});
    }
}

OfxHealth OfxRemoteInstance::health() const noexcept {
    std::lock_guard lock(m_stateMutex);
    return m_health;
}

std::string OfxRemoteInstance::problem() const {
    std::lock_guard lock(m_stateMutex);
    return m_problem;
}

void OfxRemoteInstance::markDead(OfxHealth health, std::string reason) {
    std::lock_guard lock(m_stateMutex);
    if (m_health == OfxHealth::Running) {
        m_health = health;
        m_problem = std::move(reason);
    }
}

std::expected<void, Error> OfxRemoteInstance::ensureBlock(std::uint64_t pixels) {
    if (pixels == 0 || pixels > kMaxFramePixels) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the picture is too large for OpenFX plug-ins"));
    }
    if (m_block && m_block->capacity >= pixels) {
        return {};
    }
    // Grow with some room, so a preview at another resolution does not make a new block every time.
    std::uint64_t capacity = std::max<std::uint64_t>(pixels, m_block ? m_block->capacity + m_block->capacity / 4 : pixels);
    capacity = std::min(capacity, kMaxFramePixels);
    auto next = std::make_unique<Block>();
    next->generation = (m_block ? m_block->generation : 0) + 1;
    next->capacity = capacity;
    next->name = blockName(ipc::currentProcessId(), m_id, next->generation);
    std::string error;
    next->memory = ipc::SharedMemory::create(next->name, blockBytes(capacity), error);
    if (!next->memory) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot allocate the picture block: " + error));
    }
    auto* header = new (next->memory->data()) FrameHeader();
    header->capacityPixels = capacity;
    if (m_block && m_block->announced) {
        const auto reply = m_process->request(json{{"op", "attach"}, {"instance", m_id}, {"memory", next->name}, {"capacity", capacity}},
                                              m_requestTimeout);
        if (!reply) {
            return std::unexpected(Error(ErrorCode::IoError, reply.error().message));
        }
        next->announced = true;
    }
    m_block = std::move(next);
    return {};
}

std::expected<void, Error> OfxRemoteInstance::render(const OfxRenderRequest& r, const float* input, float* output, bool offline) {
    std::lock_guard lock(m_mutex);
    if (const OfxHealth h = health(); h != OfxHealth::Running) {
        return std::unexpected(Error(ErrorCode::IoError, problem()));
    }
    if (r.width <= 0 || r.height <= 0 || r.width > kMaxFrameSide || r.height > kMaxFrameSide || input == nullptr || output == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "bad picture"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(r.width) * static_cast<std::uint64_t>(r.height);
    if (auto block = ensureBlock(pixels); !block) {
        return block;
    }
    auto* base = static_cast<std::uint8_t*>(m_block->memory->data());
    std::memcpy(base + inputOffset(), input, static_cast<std::size_t>(pixels) * kPixelBytes);
    json message{{"op", "render"},    {"instance", m_id}, {"width", r.width},        {"height", r.height}, {"time", r.time},
                 {"fps", r.fps},      {"par", r.pixelAspect}, {"hasAlpha", r.hasAlpha}};
    message["params"] = r.params ? *r.params : json::object();
    const auto timeout = offline ? m_offlineTimeout : m_previewTimeout;
    const auto reply = m_process->request(std::move(message), timeout);
    if (!reply) {
        switch (reply.error().kind) {
        case RequestError::Kind::TimedOut: {
            const std::string within = timeout.count() < 1000 ? std::to_string(timeout.count()) + " ms" : std::to_string(timeout.count() / 1000) + " s";
            const std::string why = "the plug-in did not return a picture within " + within;
            m_process->endHung(why);
            markDead(OfxHealth::Hung, why);
            return std::unexpected(Error(ErrorCode::IoError, why));
        }
        case RequestError::Kind::Dead: {
            markDead(OfxHealth::Crashed, reply.error().message);
            return std::unexpected(Error(ErrorCode::IoError, reply.error().message));
        }
        case RequestError::Kind::Refused: {
            // The plug-in failed this picture. A few in a row make it Failed (bypassed, badged).
            std::lock_guard state(m_stateMutex);
            if (++m_failures >= kFailuresBeforeFailed && m_health == OfxHealth::Running) {
                m_health = OfxHealth::Failed;
                m_problem = reply.error().message;
            }
            return std::unexpected(Error(ErrorCode::Unsupported, reply.error().message));
        }
        }
        return std::unexpected(Error(ErrorCode::IoError, "the OpenFX helper failed"));
    }
    {
        std::lock_guard state(m_stateMutex);
        m_failures = 0;
    }
    m_lastMs.store(field<double>(*reply, "ms", 0.0), std::memory_order_relaxed);
    m_renders.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(output, base + outputOffset(m_block->capacity), static_cast<std::size_t>(pixels) * kPixelBytes);
    return {};
}

// ---- OfxPluginHost --------------------------------------------------------------------------------------------------

struct OfxPluginHost::State {
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<OfxHostProcess>> processes; ///< by binary
};

OfxPluginHost::OfxPluginHost(OfxHostOptions options) : m_options(std::move(options)), m_state(std::make_unique<State>()) {
    if (m_options.helper.empty()) {
        m_options.helper = OfxCatalog::defaultHelper();
    }
}

OfxPluginHost::~OfxPluginHost() = default;

std::shared_ptr<OfxPluginHost> OfxPluginHost::create(OfxHostOptions options) {
    return std::shared_ptr<OfxPluginHost>(new OfxPluginHost(std::move(options)));
}

std::size_t OfxPluginHost::processCount() const {
    std::lock_guard lock(m_state->mutex);
    return static_cast<std::size_t>(std::count_if(m_state->processes.begin(), m_state->processes.end(), [](const auto& entry) {
        const auto p = entry.second.lock();
        return p && p->alive();
    }));
}

std::expected<std::shared_ptr<OfxHostProcess>, Error> OfxPluginHost::processFor(const OfxPluginInfo& plugin) {
    std::lock_guard lock(m_state->mutex);
    const std::string key = ipc::narrow(plugin.path.lexically_normal().wstring());
    if (const auto it = m_state->processes.find(key); it != m_state->processes.end()) {
        if (auto existing = it->second.lock(); existing && existing->alive()) {
            return existing;
        }
    }
    auto started = OfxHostProcess::start(m_options, plugin.path);
    if (!started) {
        return std::unexpected(started.error());
    }
    m_state->processes[key] = *started;
    return *started;
}

std::expected<std::shared_ptr<OfxRemoteInstance>, Error> OfxPluginHost::instantiate(const OfxPluginInfo& plugin, int width, int height,
                                                                                    double fps) {
    if (width <= 0 || height <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "bad picture size"));
    }
    auto process = processFor(plugin);
    if (!process) {
        return std::unexpected(process.error());
    }
    std::shared_ptr<OfxRemoteInstance> instance(new OfxRemoteInstance());
    instance->m_process = *process;
    instance->m_plugin = plugin;
    instance->m_id = gNextInstance.fetch_add(1);
    instance->m_previewTimeout = m_options.previewRenderTimeout;
    instance->m_offlineTimeout = m_options.offlineRenderTimeout;
    instance->m_requestTimeout = m_options.requestTimeout;
    {
        std::lock_guard lock(instance->m_mutex);
        if (auto block = instance->ensureBlock(static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)); !block) {
            return std::unexpected(block.error());
        }
    }
    const auto reply = (*process)->request(json{{"op", "create"},
                                                {"instance", instance->m_id},
                                                {"plugin", plugin.id},
                                                {"width", width},
                                                {"height", height},
                                                {"fps", fps},
                                                {"memory", instance->m_block->name},
                                                {"capacity", instance->m_block->capacity}},
                                           m_options.requestTimeout);
    if (!reply) {
        return std::unexpected(Error(reply.error().kind == RequestError::Kind::Refused ? ErrorCode::Unsupported : ErrorCode::IoError,
                                     reply.error().message));
    }
    instance->m_block->announced = true;
    instance->m_frameVarying = field<bool>(*reply, "frameVarying", false);
    (*process)->attach(instance);
    return instance;
}

} // namespace wabcut::ofxhost
