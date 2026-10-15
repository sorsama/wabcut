#pragma once
#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/llm/SystemProbe.h>
#include <mutex>
namespace wabcut::ai::llm {
struct LaunchPlan {
    std::string runtimeId;
    std::uint64_t ramNeeded = 0, vramNeeded = 0;
    int threads = 1;
    bool gpu = false;
    std::string reason;
    int context = 8192;        ///< --ctx-size: the tier's context (Tiers.h)
    bool reasoningOff = false; ///< --reasoning off: hybrid-thinking models answer directly
};
// One slot at the model's tier context (wabcut/ai/llm/Tiers.h admit(): weights, KV cache, compute buffers and the
// editor's reserve); a model that is not a tier is planned with a 32-layer shape. The user picked the model, so a
// large one may run on the CPU when RAM allows. Unknown memory fails closed.
std::expected<LaunchPlan, AiError> planLaunch(const ModelInfo&, const SystemProbe&, bool cpuFallback = true,
                                           bool cudaAccepted = false);
struct RuntimeStatus {
    enum class State { Stopped, Starting, Ready, Failed, Suspended };
    State state = State::Stopped;
    std::string model, backend, message, baseUrl;
};
// start runs on a worker thread. Host calls tick periodically, touch during use, pressure before playback/export.
class LocalRuntime {
public:
    explicit LocalRuntime(IHttpTransport&);
    ~LocalRuntime();
    struct Options {
        std::chrono::milliseconds startupTimeout{120000}, idleTimeout{120000};
        std::function<int()> choosePort; // tests only; production uses OS-assigned random loopback port
    };
    std::expected<void, AiError> start(const std::filesystem::path& executable, const std::filesystem::path& weights,
        const ModelInfo&, const LaunchPlan&, std::stop_token, Options);
    void stop();
    void setPressure(bool playbackOrExport);
    void touch();
    void setBusy(bool value);
    void tick();
    RuntimeStatus status() const;
    Secret key() const;
    /// The llama-server process (0 when stopped): the eval reads its memory use (AI-15).
    std::uint64_t processId() const;
private:
    void stopLocked();
    IHttpTransport& m_transport;
    mutable std::mutex m_mutex;
    std::unique_ptr<ChildProcess> m_child;
    RuntimeStatus m_status;
    Secret m_key;
    bool m_pressure = false;
    bool m_busy = false;
    Options m_options;
    std::chrono::steady_clock::time_point m_lastUse;
};
// Only official SHA-verified archives; extractor must be a trusted absolute OS tar path. The runtime is extracted once
// per archive set into <store>/runtime-<sha prefixes> and reused while every extracted file still matches its manifest
// (review wave 4, P2-1); other runtime-* directories are removed when nothing holds them open.
std::expected<std::filesystem::path, AiError> installRuntime(ModelStore&, const LaunchPlan&, IDownloadTransport*,
    const std::filesystem::path& extractor, std::stop_token,
    const std::function<void(const DownloadProgress&)>& progress = {});
/// A verified archive to extract: its file and SHA-256 (64 lowercase hex digits).
struct RuntimeArchive {
    std::filesystem::path file;
    std::string sha256;
};
/// The extraction step of installRuntime(): <root>/runtime-<first 16 hex of each archive's SHA-256> holds the archives'
/// files and a manifest of their sizes and SHA-256; an intact one is reused without running the extractor, a damaged or
/// partial one is replaced. Returns the llama-server executable inside. Never throws.
std::expected<std::filesystem::path, AiError> extractRuntime(const std::filesystem::path& root, std::span<const RuntimeArchive> archives,
    const std::filesystem::path& extractor, std::stop_token stop);
}
