#pragma once

// wabcut-plughost: one loaded plugin binary (a CLAP file or a VST3 bundle) and the instances created from it. The
// format backends (ClapHost.cpp, Vst3Host.cpp) implement these; main.cpp drives them from the control channel.
//
// Threads, as both formats require: everything but process() and reset() runs on the helper's main thread (the
// Windows message loop that also hosts editor windows); process() and reset() run on the instance's audio thread.

#include <wabcut/plughost/Protocol.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::plughost {

/// A path as UTF-8 for messages (review A5-P1-8: path::string() converts to the ANSI code page and throws for names
/// outside it).
inline std::string utf8Path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

struct ParamInfo {
    std::uint32_t id = 0;
    std::string name;
    std::string module; ///< a group the plugin names ("EQ/Band 1"), or empty
    std::string unit;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool automatable = true;
    bool stepped = false;
    bool hidden = false; ///< read-only or hidden from the user: not offered as an effect parameter
};

struct PluginDescription {
    std::string format; ///< "clap" or "vst3"
    std::string id;     ///< CLAP: the plugin id; VST3: the processor class id (32 hex digits)
    std::string name;
    std::string vendor;
    std::string version;
    std::string category; ///< "Fx", "Fx|EQ", "audio-effect;equalizer", ...
    bool effect = true;   ///< an audio effect (instruments and note effects are listed but not used)
};

/// What the plugin tells its host outside the audio thread.
struct InstanceCallbacks {
    std::function<void()> stateChanged;   ///< its editor changed the state (save it again)
    std::function<void()> latencyChanged; ///< it asks for a restart with a new latency
    std::function<void()> editorClosed;   ///< the user closed its editor window
};

class HostedInstance {
public:
    virtual ~HostedInstance() = default;

    virtual std::vector<ParamInfo> params() = 0;
    virtual std::optional<double> paramValue(std::uint32_t id) = 0;
    /// Activates for processing: stereo in and out, blocks of at most kBlockFrames. Main thread.
    virtual bool activate(double sampleRate, std::string& error) = 0;
    virtual void deactivate() = 0;
    virtual int latency() = 0;
    /// One block: slot.input -> slot.output, with slot.events. Audio thread. False: the plugin reported an error.
    virtual bool process(BlockSlot& slot) noexcept = 0;
    /// Clears the plugin's DSP state (a seek). Audio thread, between process() calls.
    virtual void reset() noexcept = 0;
    virtual std::optional<std::vector<std::uint8_t>> saveState() = 0;
    virtual bool loadState(const std::vector<std::uint8_t>& state) = 0;
    virtual bool hasEditor() = 0;
    /// Opens the plugin's editor in a window of its own (Editor.cpp); false when it has none or cannot.
    virtual bool openEditor(const std::string& title, std::string& error) = 0;
    virtual void closeEditor() = 0;
};

class PluginModule {
public:
    virtual ~PluginModule() = default;
    /// The plugins in this binary.
    virtual std::vector<PluginDescription> plugins() = 0;
    virtual std::unique_ptr<HostedInstance> create(const std::string& id, InstanceCallbacks callbacks, std::string& error) = 0;
};

/// Loads a CLAP (.clap) or VST3 (.vst3 file or bundle) binary.
std::shared_ptr<PluginModule> loadClapModule(const std::filesystem::path& path, std::string& error);
std::shared_ptr<PluginModule> loadVst3Module(const std::filesystem::path& path, std::string& error);
std::shared_ptr<PluginModule> loadModule(const std::filesystem::path& path, std::string& error);

/// Main-thread work posted from other threads (CLAP's request_callback, the control reader).
void postToMainThread(std::function<void()> work);

} // namespace wabcut::plughost
