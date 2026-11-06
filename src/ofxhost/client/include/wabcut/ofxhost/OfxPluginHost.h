#pragma once

// The editor's side of the OpenFX helpers (0.7, docs/plugins.md, "OpenFX video plug-ins"): wabcut-ofxhost processes and the
// plug-in instances in them.
//
// One helper process per plug-in binary (a .ofx file), hosting every instance of the plug-ins in it, as the audio plug-in
// host does: the crash domain is the code that crashed, and a heavy plug-in loads once. The helper runs held to little more
// than computing (ChildLimits: no other processes, no desktop, a memory cap), with a minimal environment.
//
// A crash is noticed the moment the helper's control pipe closes: its instances report Crashed (with the exit code), the
// editor bypasses them and badges the effect. A render that takes longer than the timeout ends the helper (Hung). Neither
// reaches the editor's own threads as anything but an error.
//
// Pictures go through one shared block per instance (Protocol.h); the control channel is the synchronisation.
//
// Threading: instantiate() and render() block on the helper and belong on a worker, never the GUI thread; they may be
// called from any thread (an instance renders one picture at a time).

#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/Protocol.h>

#include <wabcut/model/Error.h>

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace wabcut::ofxhost {

struct OfxHostOptions {
    /// The helper (default: wabcut-ofxhost next to this executable).
    std::filesystem::path helper;
    /// Where the helpers write diagnostics (their stderr); empty: discarded.
    std::filesystem::path logFile;
    /// How long creating an instance or attaching a picture block may take.
    std::chrono::milliseconds requestTimeout{20000};
    /// How long one picture may take where it is being played (the preview): past it the instance counts as hung and the
    /// helper is ended.
    std::chrono::milliseconds previewRenderTimeout{4000};
    /// The same for an offline render (export, Render In to Out): heavy plug-ins take their time there.
    std::chrono::milliseconds offlineRenderTimeout{60000};
    /// The most memory one helper may commit (0: no cap).
    std::uint64_t helperMemoryBytes = std::uint64_t{6} << 30;
};

enum class OfxHealth : std::uint8_t {
    Running,
    Crashed, ///< its helper process died
    Failed,  ///< the plug-in reported an error again and again
    Hung,    ///< a picture took longer than the timeout
};
std::string_view toString(OfxHealth health);

struct OfxRenderRequest {
    int width = 0;
    int height = 0;
    double time = 0.0;        ///< the plug-in's time, in frames
    double fps = 24.0;
    double pixelAspect = 1.0;
    bool hasAlpha = true;     ///< the picture has an alpha channel (else it is opaque)
    /// The parameter values by the plug-in's parameter names (OfxParams.h renderParams()): a number, an array or a string.
    std::shared_ptr<const nlohmann::json> params;
};

class OfxHostProcess;

class OfxRemoteInstance {
public:
    ~OfxRemoteInstance(); ///< destroys the helper's instance (without waiting)
    OfxRemoteInstance(const OfxRemoteInstance&) = delete;
    OfxRemoteInstance& operator=(const OfxRemoteInstance&) = delete;

    const OfxPluginInfo& plugin() const noexcept { return m_plugin; }
    std::uint64_t id() const noexcept { return m_id; }
    OfxHealth health() const noexcept;
    /// Why it is not running ("the plug-in crashed (wabcut-ofxhost exited with 0xC0000005)").
    std::string problem() const;
    std::uint64_t renders() const noexcept { return m_renders.load(std::memory_order_relaxed); }
    /// The helper's time for the last picture, in milliseconds.
    double lastMilliseconds() const noexcept { return m_lastMs.load(std::memory_order_relaxed); }
    /// The plug-in's pictures change from frame to frame even with constant parameters (a noise or a time-based effect), so a
    /// cached picture is only good for its own frame.
    bool frameVarying() const noexcept { return m_frameVarying; }

    /// Processes `input` (float RGBA, premultiplied, rows top-down, width x height) into `output` (same layout). Errors:
    /// IoError (the helper crashed, hung or is gone: the instance is Crashed or Hung from here on), Unsupported (the plug-in
    /// failed this picture), InvalidArgument (a picture that is too large).
    std::expected<void, Error> render(const OfxRenderRequest& request, const float* input, float* output, bool offline);

private:
    friend class OfxPluginHost;
    friend class OfxHostProcess;
    OfxRemoteInstance() = default;

    struct Block;
    std::expected<void, Error> ensureBlock(std::uint64_t pixels);
    void markDead(OfxHealth health, std::string reason);

    std::shared_ptr<OfxHostProcess> m_process;
    std::unique_ptr<Block> m_block;
    OfxPluginInfo m_plugin;
    std::uint64_t m_id = 0;
    std::chrono::milliseconds m_previewTimeout{4000};
    std::chrono::milliseconds m_offlineTimeout{60000};
    std::chrono::milliseconds m_requestTimeout{20000};
    std::mutex m_mutex; ///< one picture at a time
    mutable std::mutex m_stateMutex;
    OfxHealth m_health = OfxHealth::Running;
    std::string m_problem;
    int m_failures = 0;
    bool m_frameVarying = false;
    std::atomic<std::uint64_t> m_renders{0};
    std::atomic<double> m_lastMs{0.0};
};

class OfxPluginHost : public std::enable_shared_from_this<OfxPluginHost> {
public:
    static std::shared_ptr<OfxPluginHost> create(OfxHostOptions options = {});
    ~OfxPluginHost(); ///< stops every helper
    OfxPluginHost(const OfxPluginHost&) = delete;
    OfxPluginHost& operator=(const OfxPluginHost&) = delete;

    /// A new instance of `plugin` for pictures of about `width` x `height` at `fps` (the plug-in's project size and rate).
    std::expected<std::shared_ptr<OfxRemoteInstance>, Error> instantiate(const OfxPluginInfo& plugin, int width, int height, double fps);

    const OfxHostOptions& options() const noexcept { return m_options; }
    /// Helper processes running now (diagnostics, tests).
    std::size_t processCount() const;

private:
    explicit OfxPluginHost(OfxHostOptions options);
    std::expected<std::shared_ptr<OfxHostProcess>, Error> processFor(const OfxPluginInfo& plugin);

    struct State;
    OfxHostOptions m_options;
    std::unique_ptr<State> m_state;
};

} // namespace wabcut::ofxhost
