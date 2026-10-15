#pragma once

// The audio plugins of the host, for the plugins.* commands (A.6, docs/plugins.md). Pure interface: the host provides
// the implementation (wabcut_plugins' catalog and scanner, in the app and wabcut-cli) as PluginServices, alone or in a
// command::ServiceBundle.
//
//   plugins.list   (Read)           the plugins this computer has, by effect id ("plugin.vst3.<id>"), with their
//                                   parameters; the blocklist; the folders scanned
//   plugins.scan   (MediaAnalysis)  scans the plugin folders (new and changed files, each in its own helper process);
//                                   adds, removes or unblocks folders and files. It runs third-party code, so the
//                                   assistant never gets it (it has MediaAnalysis only for named analysis reads), and
//                                   MCP serves it only with --allow-analysis.
// Plugin effects themselves go on clips and tracks through clip.addEffect / audio.addEffect ("plugin": the effect id)
// and the other effect and keyframe commands, like any effect.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

struct PluginParamListing {
    std::string key; ///< the effect parameter key, "p<id>"
    std::string name;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool automatable = true;
};

struct PluginListing {
    std::string effect; ///< "plugin.<format>.<id>"
    std::string format; ///< "clap", "vst3"
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;
    std::string path;
    int latency = 0;
    bool editor = false;
    bool effectPlugin = true; ///< false: an instrument (listed, not usable as an effect)
    std::string problem;
    std::vector<PluginParamListing> params;
};

struct BlockedPluginListing {
    std::string path;
    std::string reason;
};

struct PluginScanOptions {
    bool rescan = false;                   ///< scan unchanged files again too
    std::vector<std::string> addFolders;   ///< user folders to add (remembered)
    std::vector<std::string> removeFolders;
    std::vector<std::string> unblock;      ///< files to take off the blocklist (scanned again)
    bool clearBlocklist = false;
};

struct PluginScanSummary {
    int files = 0;
    int scanned = 0;
    int cached = 0;
    int skipped = 0;
    int plugins = 0;
    int added = 0;
    std::vector<BlockedPluginListing> blocked; ///< blocked by this scan
    std::vector<std::string> errors;
    bool cancelled = false;
};

class PluginLibrary {
public:
    virtual ~PluginLibrary() = default;
    virtual std::vector<PluginListing> plugins() const = 0;
    virtual std::vector<BlockedPluginListing> blocked() const = 0;
    virtual std::vector<std::string> folders() const = 0;
    virtual std::expected<PluginScanSummary, Error> scan(const PluginScanOptions& options, std::stop_token stop,
                                                        const std::function<void(double)>& progress) = 0;
};

class PluginServices final : public command::CommandServices {
public:
    explicit PluginServices(std::shared_ptr<PluginLibrary> library) : m_library(std::move(library)) {}
    PluginLibrary* library() const { return m_library.get(); }

private:
    std::shared_ptr<PluginLibrary> m_library;
};

/// plugins.list and plugins.scan (registerAudioCommands adds them).
void registerPluginCommands(command::CommandRegistry& registry);

} // namespace wabcut::audio::edit
