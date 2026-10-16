#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/ai/Transport.h>
#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/Sha256.h>
#include <fstream>
#include <algorithm>
#include <random>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace wabcut::ai::llm {
namespace {
AiError failure(std::string message) { return {AiErrorCode::InvalidRequest, std::move(message)}; }
std::string utf8(const std::filesystem::path& p) {
    const auto s = p.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
std::string randomKey() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    for (int i = 0; i < 8; ++i) {
        const auto n = random();
        for (int shift = 0; shift < 32; shift += 4) value.push_back(hex[(n >> shift) & 15]);
    }
    return value;
}
int randomPort() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    const SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) { WSACleanup(); return 0; }
#else
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0) port = ntohs(address.sin_port);
    }
#ifdef _WIN32
    closesocket(fd); WSACleanup();
#else
    close(fd);
#endif
    return port;
}
}
#ifdef _WIN32
/// The process listening on 127.0.0.1:`port` (or 0.0.0.0:`port`), if any.
static std::optional<std::uint64_t> loopbackListener(int port) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
    std::vector<unsigned char> buffer(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return std::nullopt;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        const bool loopback = row.dwLocalAddr == htonl(INADDR_LOOPBACK) || row.dwLocalAddr == htonl(INADDR_ANY);
        if (loopback && ntohs(static_cast<u_short>(row.dwLocalPort)) == port) return row.dwOwningPid;
    }
    return std::nullopt;
}
#endif
std::expected<LaunchPlan, AiError> planLaunch(const ModelInfo& model, const SystemProbe& probe,
                                            bool cpuFallback, bool cudaAccepted) {
    // A model outside the tiers (Phi-4 mini) is planned like a 32-layer, 8-KV-head model at 8192 tokens.
    static constexpr TierSpec kOther{"other", "Other", "", 4.0, 32, 8, 128, 8192, false};
    const TierSpec* tier = tierForModel(model.id);
    AdmissionOptions options;
    options.cpuFallback = cpuFallback;
    options.cudaAccepted = cudaAccepted;
    options.allowLargeOnCpu = true;
    const Admission a = admit(tier ? *tier : kOther, model, probe, options);
    if (!a.fits) return std::unexpected(failure(a.reason));
    LaunchPlan p;
    p.runtimeId = a.runtimeId;
    p.ramNeeded = a.ramNeeded;
    p.vramNeeded = a.vramNeeded;
    p.threads = a.threads;
    p.gpu = a.gpu;
    p.reason = a.reason;
    p.context = a.context;
    p.reasoningOff = tier && tier->hybridThinking;
    return p;
}
LocalRuntime::LocalRuntime(IHttpTransport& t) : m_transport(t) {}
LocalRuntime::~LocalRuntime() { stop(); }
void LocalRuntime::stopLocked() {
    m_child.reset(); m_key = Secret{}; m_status.baseUrl.clear(); m_status.state = RuntimeStatus::State::Stopped;
}
void LocalRuntime::stop() { std::lock_guard lock(m_mutex); stopLocked(); m_status.message.clear(); }
void LocalRuntime::setPressure(bool value) {
    std::lock_guard lock(m_mutex); m_pressure = value;
    if (value) { stopLocked(); m_status.state = RuntimeStatus::State::Suspended;
        m_status.message = "Local assistant paused for playback or export"; }
}
RuntimeStatus LocalRuntime::status() const { std::lock_guard lock(m_mutex); return m_status; }
Secret LocalRuntime::key() const { std::lock_guard lock(m_mutex); return m_key.clone(); }
std::uint64_t LocalRuntime::processId() const { std::lock_guard lock(m_mutex); return m_child ? m_child->processId() : 0; }
void LocalRuntime::touch() { std::lock_guard lock(m_mutex); m_lastUse = std::chrono::steady_clock::now(); }
void LocalRuntime::setBusy(bool value) { std::lock_guard lock(m_mutex); m_busy = value; m_lastUse = std::chrono::steady_clock::now(); }
void LocalRuntime::tick() {
    std::lock_guard lock(m_mutex);
    if (!m_child) return;
    if (m_child->exitCode()) {
        stopLocked(); m_status.state = RuntimeStatus::State::Failed; m_status.message = "Local assistant exited";
    } else if (!m_busy && m_status.state == RuntimeStatus::State::Ready &&
               std::chrono::steady_clock::now() - m_lastUse >= m_options.idleTimeout) {
        stopLocked(); m_status.message = "Unloaded while idle";
    }
}
std::expected<void, AiError> LocalRuntime::start(const std::filesystem::path& executable,
    const std::filesystem::path& weights, const ModelInfo& model, const LaunchPlan& plan,
    std::stop_token token, Options options) {
    std::unique_lock lock(m_mutex);
    if (m_pressure || m_child) return std::unexpected(failure("local assistant is busy or suspended"));
    // Called from worker threads that do not catch (review wave 4, P2-6): file checks report errors, never throw.
    std::error_code weightsError, executableError;
    if (!std::filesystem::is_regular_file(weights, weightsError) || !std::filesystem::is_regular_file(executable, executableError))
        return std::unexpected(failure("download the verified runtime and model first"));
    const int port = options.choosePort ? options.choosePort() : randomPort();
    if (port <= 0 || port > 65535) return std::unexpected(failure("could not allocate a loopback port"));
    m_key = Secret(randomKey()); m_options = std::move(options);
    m_status = {RuntimeStatus::State::Starting, model.id, plan.runtimeId, "Loading local assistant",
                "http://127.0.0.1:" + std::to_string(port) + "/v1"};
    ChildProcessOptions child;
    child.executable = executable; child.workingDirectory = executable.parent_path(); child.lowPriority = true;
    child.arguments = {"--model", utf8(weights), "--alias", model.id, "--host", "127.0.0.1", "--port",
        std::to_string(port), "--api-key", std::string(m_key.reveal()), "--jinja", "--ctx-size",
        std::to_string(plan.context), "--parallel", "1", "--threads", std::to_string(plan.threads), "--gpu-layers",
        plan.gpu ? "99" : "0", "--no-webui", "--no-warmup", "--cache-ram", "0"};
    // No prompt cache beyond the one slot (llama-server keeps up to 8 GiB of RAM for it by default), and Qwen3's hybrid
    // models answer without a thinking block (Tiers.h).
    if (plan.reasoningOff) child.arguments.insert(child.arguments.end(), {"--reasoning", "off"});
    auto spawned = ChildProcess::spawn(child);
    std::fill(child.arguments[9].begin(), child.arguments[9].end(), '\0');
    if (!spawned) { stopLocked(); return std::unexpected(failure("could not start local assistant")); }
    m_child = std::move(*spawned);
    const auto deadline = std::chrono::steady_clock::now() + m_options.startupTimeout;
    const std::string base = m_status.baseUrl;
    const auto fail = [&](std::string message) -> std::expected<void, AiError> {
        stopLocked(); m_status.state = RuntimeStatus::State::Failed; m_status.message = message;
        return std::unexpected(failure(std::move(message)));
    };
    while (std::chrono::steady_clock::now() < deadline) {
        if (token.stop_requested()) return fail("local assistant start cancelled");
        if (!m_child || m_pressure || m_status.baseUrl != base) return std::unexpected(failure("local assistant stopped"));
        if (m_child->exitCode()) return fail("local assistant exited while loading (possibly a port conflict)");
#ifdef _WIN32
        // The key goes only to our own child (review wave 4, P2-2): another process that grabbed the port between
        // randomPort() and the child's bind would otherwise receive it with the first readiness probe.
        const auto owner = loopbackListener(port);
        if (!owner) {
            lock.unlock(); interruptibleSleep(std::chrono::milliseconds(50), token); lock.lock();
            continue; // not listening yet: nothing is sent
        }
        if (*owner != m_child->processId()) return fail("another program is listening on the local assistant's port");
#endif
        HttpRequest request;
        request.method = "GET"; request.url = base + "/models"; // authenticated, unlike /health
        request.headers = {{"Authorization", "Bearer " + std::string(m_key.reveal())}};
        request.timeout = request.idleTimeout = std::chrono::milliseconds(500);
        lock.unlock(); auto response = m_transport.send(request, {}, token); lock.lock();
        if (!m_child || m_status.baseUrl != base) return std::unexpected(failure("local assistant stopped"));
        if (response && response->ok()) {
            auto j = Json::parse(response->body, nullptr, false);
            if (const auto* data = jsonArray(j, "data"); data && !data->empty() &&
                jsonStringOr((*data)[0], "id") == model.id && !m_child->exitCode()) {
                m_status.state = RuntimeStatus::State::Ready; m_status.message = "Local assistant ready";
                m_lastUse = std::chrono::steady_clock::now(); return {};
            }
        }
        lock.unlock(); interruptibleSleep(std::chrono::milliseconds(50), token); lock.lock();
    }
    if (!m_child || m_status.baseUrl != base) return std::unexpected(failure("local assistant stopped"));
    return fail("local assistant load timed out");
}
namespace {
constexpr const char* kManifestName = ".wabcut-runtime.json";

std::optional<std::string> fileSha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0) hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(got)));
    }
    if (in.bad()) return std::nullopt;
    return hash.finishHex();
}

/// The extracted files with sizes and hashes, relative to `directory`; nullopt when it cannot be read completely.
std::optional<Json> manifestOf(const std::filesystem::path& directory) {
    Json files = Json::array();
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(directory, ec), end;
    if (ec) return std::nullopt;
    for (; it != end; it.increment(ec)) {
        if (ec) return std::nullopt;
        const bool regular = it->is_regular_file(ec);
        if (ec) return std::nullopt;
        if (!regular || it->path().filename() == kManifestName) continue;
        const auto size = it->file_size(ec);
        if (ec) return std::nullopt;
        const auto sha = fileSha256(it->path());
        if (!sha) return std::nullopt;
        const auto relative = std::filesystem::relative(it->path(), directory, ec);
        if (ec) return std::nullopt;
        files.push_back({{"path", utf8(relative)}, {"size", size}, {"sha256", *sha}});
    }
    if (ec) return std::nullopt;
    std::sort(files.begin(), files.end(), [](const Json& a, const Json& b) { return a["path"].get<std::string>() < b["path"].get<std::string>(); });
    return std::make_optional(std::move(files));
}

/// Whether `directory` holds exactly what its manifest says, for these archives.
bool intact(const std::filesystem::path& directory, const std::vector<std::string>& archives) {
    std::ifstream in(directory / kManifestName, std::ios::binary);
    if (!in) return false;
    const Json manifest = Json::parse(in, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("archives") || manifest["archives"] != Json(archives)) return false;
    const auto actual = manifestOf(directory);
    return actual && manifest.contains("files") && manifest["files"] == *actual;
}

std::optional<std::filesystem::path> findServer(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(directory, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() == "llama-server.exe") {
            std::error_code e;
            if (it->is_regular_file(e) && !e) return it->path();
        }
    }
    return std::nullopt;
}
}

std::expected<std::filesystem::path, AiError> extractRuntime(const std::filesystem::path& root, std::span<const RuntimeArchive> archives,
    const std::filesystem::path& extractor, std::stop_token stop) {
    try {
        if (archives.empty() || !extractor.is_absolute()) return std::unexpected(failure("unknown runtime or untrusted extractor"));
        std::string name = "runtime";
        std::vector<std::string> shas;
        for (const auto& a : archives) {
            if (a.sha256.size() != 64) return std::unexpected(failure("runtime archive without a SHA-256"));
            name += "-" + a.sha256.substr(0, 16);
            shas.push_back(a.sha256);
        }
        const auto directory = root / name;
        std::error_code ec;
        // Leftovers of earlier versions, random-named and interrupted extractions; one still in use stays.
        const auto sweep = [&] {
            std::error_code e;
            for (std::filesystem::directory_iterator it(root, e), end; !e && it != end; it.increment(e)) {
                const auto entry = utf8(it->path().filename());
                std::error_code d;
                if (entry.starts_with("runtime-") && it->path() != directory && it->is_directory(d) && !d) {
                    std::error_code ignored;
                    std::filesystem::remove_all(it->path(), ignored);
                }
            }
        };
        if (std::filesystem::is_directory(directory, ec) && intact(directory, shas)) {
            sweep();
            if (auto server = findServer(directory)) return *server;
        }
        const auto partial = root / (name + ".partial-" + randomKey().substr(0, 8));
        std::filesystem::create_directories(partial, ec);
        if (ec) return std::unexpected(failure("cannot create runtime directory"));
        const auto abandon = [&](std::string message) -> std::expected<std::filesystem::path, AiError> {
            std::error_code ignored;
            std::filesystem::remove_all(partial, ignored);
            return std::unexpected(failure(std::move(message)));
        };
        for (const auto& archive : archives) {
            ChildProcessOptions options;
            options.executable = extractor; options.lowPriority = true;
            options.arguments = {"-xf", utf8(archive.file), "-C", utf8(partial)};
            auto process = ChildProcess::spawn(options);
            if (!process) return abandon("cannot extract verified runtime");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            while (!(*process)->wait(std::chrono::milliseconds(50)))
                if (stop.stop_requested() || std::chrono::steady_clock::now() > deadline)
                    return abandon("runtime extraction cancelled or timed out");
            if ((*process)->exitCode() != 0) return abandon("runtime extraction failed");
        }
        const auto files = manifestOf(partial);
        if (!files) return abandon("cannot read the extracted runtime");
        {
            std::ofstream out(partial / kManifestName, std::ios::binary | std::ios::trunc);
            out << Json{{"archives", shas}, {"files", *files}}.dump();
            if (!out) return abandon("cannot write the runtime manifest");
        }
        std::filesystem::remove_all(directory, ec);
        ec.clear();
        std::filesystem::rename(partial, directory, ec);
        if (ec) return abandon("cannot install the extracted runtime (is an older copy still running?)");
        sweep();
        if (auto server = findServer(directory)) return *server;
        return std::unexpected(failure("verified archive did not contain llama-server"));
    } catch (const std::exception&) {
        return std::unexpected(failure("runtime extraction failed (file system error)"));
    }
}

std::expected<std::filesystem::path, AiError> installRuntime(ModelStore& store, const LaunchPlan& plan,
    IDownloadTransport* transport, const std::filesystem::path& extractor, std::stop_token stop,
    const std::function<void(const DownloadProgress&)>& progress) {
    const auto* info = findModel(plan.runtimeId);
    if (!info || info->kind != ModelKind::Runtime || info->license != "MIT" || !extractor.is_absolute())
        return std::unexpected(failure("unknown runtime or untrusted extractor"));
    std::vector<const ModelInfo*> archives{info};
    if (plan.runtimeId == "llama-server-win-cuda-12.4-x64") archives.push_back(findModel("cudart-win-cuda-12.4-x64"));
    std::vector<RuntimeArchive> verified;
    for (const auto* archive : archives) {
        if (!archive) return std::unexpected(failure("runtime dependency missing"));
        auto path = store.ensure(*archive, transport, progress, stop);
        if (!path) return std::unexpected(failure(path.error().message));
        if (auto v = store.verify(*archive); !v) return std::unexpected(failure(v.error().message));
        verified.push_back({*path, archive->sha256});
    }
    return extractRuntime(store.root(), verified, extractor, stop);
}
}
