#pragma once

// The editor's side of the plugin helpers (A.6, docs/plugins.md): wabcut-plughost processes and the plugin instances
// in them.
//
// One helper process per plugin binary (a .clap file, a .vst3 bundle) by default, hosting every instance of the
// plugins in it:
//   * the crash domain is the code that crashed: a crash takes down that plugin's instances only, which would most
//     likely crash the same way anyway, and never the editor or other plugins;
//   * instances of one plugin share one copy of its code and data (a heavy plugin loads once, not per instance);
//   * plugins that coordinate their own instances (shared caches, licences, "link" features) keep working.
// HostOptions::processPerInstance isolates every instance instead (diagnosis of a misbehaving plugin).
//
// A crash is noticed the moment the helper's control pipe closes: its instances report Crashed, their processors
// bypass them (the dry signal, still latency-compensated) and the UI badges them; a new instance starts a new helper.
//
// Threading: instantiate() and the control calls (state, editor) block on the helper and belong on a non-real-time
// thread. The audio-side members of PluginInstance (marked) are lock-free and never block unless they say they wait.

#include <wabcut/plughost/PluginCatalog.h>
#include <wabcut/plughost/Protocol.h>

#include <wabcut/model/Error.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::plughost {

struct HostOptions {
    /// The helper (default: wabcut-plughost next to this executable).
    std::filesystem::path helper;
    /// Where the helpers write diagnostics (their stderr); empty: discarded.
    std::filesystem::path logFile;
    /// How long a control request (create an instance, save its state) may take.
    std::chrono::milliseconds requestTimeout{20000};
    /// Offline renders (export, audio::ScopedOfflineRender): how long one block may take before the instance counts as
    /// hung (bypassed).
    std::chrono::milliseconds blockTimeout{3000};
    /// Rendering ahead of playback (the player's feeder): the same, kept well below the player's lead so one slow plugin
    /// cannot starve every track.
    std::chrono::milliseconds playbackBlockTimeout{50};
    /// A plugin effect first played outside an offline render is loaded on a worker (helper start, create, state): its
    /// processor plays the dry signal, latency-compensated, until the plugin is ready, so neither the player's feeder
    /// nor the thread that starts playback waits for a helper. Off: loaded on the calling thread.
    bool deferInstantiation = true;
    /// One helper per instance instead of per plugin binary.
    bool processPerInstance = false;
};

enum class InstanceHealth : std::uint8_t {
    Running,
    Crashed, ///< its helper process died
    Failed,  ///< the plugin reported an error while processing
    Hung,    ///< a block took longer than HostOptions::blockTimeout
};

std::string_view toString(InstanceHealth health);

class HostProcess;

class PluginInstance {
public:
    ~PluginInstance(); ///< destroys the helper's instance (without waiting)
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;

    const PluginInfo& plugin() const noexcept { return m_plugin; }
    std::uint64_t id() const noexcept { return m_id; }
    double sampleRate() const noexcept { return m_sampleRate; }
    /// The plugin's own latency in frames (reported when it was last activated: it may ask for a restart with another
    /// one), at most maxLatencyFrames(sampleRate()) (more fails the instance).
    int latency() const noexcept;
    /// Bumped each time the helper read the plugin's latency again (a restart the plugin asked for).
    std::uint32_t latencyGeneration() const noexcept;
    bool hasEditor() const noexcept { return m_editor; }

    InstanceHealth health() const noexcept;
    /// Why it is not running ("the plugin crashed (wabcut-plughost exited with 0xC0000005)").
    std::string problem() const;
    /// Blocks the audio callback had to play dry because the helper had not returned them in time.
    std::uint64_t missedBlocks() const noexcept { return m_missed.load(std::memory_order_relaxed); }
    /// Blocks in which the plugin returned invalid samples (NaN or infinity), which were replaced by the dry signal.
    std::uint64_t invalidBlocks() const noexcept { return m_invalid.load(std::memory_order_relaxed); }
    /// Bumped whenever the plugin says its state changed (its editor): save it into the project again.
    std::uint64_t stateChanges() const noexcept;

    /// The plugin's state (its chunk, base64).
    std::expected<std::string, Error> saveState();
    std::expected<void, Error> loadState(std::string_view base64);
    /// Current parameter values, by id.
    std::expected<std::vector<std::pair<std::uint32_t, double>>, Error> parameterValues();
    /// Opens the plugin's own editor in a window of the helper ("Edit plugin..."); a second call brings it forward.
    std::expected<void, Error> openEditor(const std::string& title);
    void closeEditor();

    /// The state the instance is known to hold: a hash of the base64 it was created or loaded with, or last saved,
    /// valid until the plugin reports a change (its editor). Pooling and in-place updates rely on it.
    void setKnownState(std::uint64_t hash) noexcept;
    bool hasKnownState(std::uint64_t hash) const noexcept;

    // ---- Audio side (PluginEffect's processor) --------------------------------------------------------------
    InstanceShared& shared() noexcept { return *m_shared; }
    /// The helper has blocks to do (never blocks).
    void signalWork() noexcept;
    /// Waits up to `timeout` for the helper to complete a block. Not on the audio callback.
    bool waitDone(std::chrono::milliseconds timeout) noexcept;
    bool running() const noexcept { return health() == InstanceHealth::Running; }
    void countMissed() noexcept { m_missed.fetch_add(1, std::memory_order_relaxed); }
    void countInvalid() noexcept { m_invalid.fetch_add(1, std::memory_order_relaxed); }
    void markHung() noexcept;
    /// Fails the instance from here on because it keeps returning invalid samples (bypassed, badged). RT-safe.
    void markInvalid() noexcept;
    std::chrono::milliseconds blockTimeout() const noexcept { return m_blockTimeout; }
    /// Processors playing it: one at a time, except while a rebuilt processor takes over from the one it replaces. The
    /// pool takes the instance back when the last one goes (dropUser() returns true).
    void addUser() noexcept { m_users.fetch_add(1, std::memory_order_acq_rel); }
    bool dropUser() noexcept { return m_users.fetch_sub(1, std::memory_order_acq_rel) == 1; }

private:
    friend class PluginHost;
    friend class HostProcess;
    PluginInstance() = default;

    struct Resources;
    std::shared_ptr<HostProcess> m_process;
    std::unique_ptr<Resources> m_resources;
    InstanceShared* m_shared = nullptr;
    PluginInfo m_plugin;
    std::uint64_t m_id = 0;
    double m_sampleRate = 48000.0;
    bool m_editor = false;
    std::chrono::milliseconds m_blockTimeout{3000};
    std::atomic<std::uint8_t> m_health{static_cast<std::uint8_t>(InstanceHealth::Running)};
    std::atomic<bool> m_invalidOutput{false}; ///< failed by the editor side: invalid samples
    std::atomic<std::uint64_t> m_missed{0};
    std::atomic<std::uint64_t> m_invalid{0};
    std::atomic<int> m_users{0};
    std::atomic<std::uint64_t> m_knownHash{0};
    std::atomic<std::uint64_t> m_knownChanges{0};
};

class PluginHost : public std::enable_shared_from_this<PluginHost> {
public:
    static std::shared_ptr<PluginHost> create(HostOptions options = {});
    ~PluginHost(); ///< stops every helper
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /// A new instance of `plugin` at `sampleRate`, from its saved `state` (base64; empty: the plugin's defaults).
    std::expected<std::shared_ptr<PluginInstance>, Error> instantiate(const PluginInfo& plugin, double sampleRate,
                                                                      std::string_view state = {});

    const HostOptions& options() const noexcept { return m_options; }
    /// Helper processes running now (diagnostics, tests).
    std::size_t processCount() const;
    /// Instances alive now.
    std::vector<std::shared_ptr<PluginInstance>> instances() const;
    /// Waits on a helper made inside a real-time section (the audio callback): always 0 (tests check it).
    static std::uint64_t realtimeWaits() noexcept;

private:
    explicit PluginHost(HostOptions options);
    std::expected<std::shared_ptr<HostProcess>, Error> processFor(const PluginInfo& plugin);

    struct State;
    HostOptions m_options;
    std::unique_ptr<State> m_state;
};

} // namespace wabcut::plughost
