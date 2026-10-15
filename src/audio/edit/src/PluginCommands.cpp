// plugins.list and plugins.scan (A.6, PluginLibrary.h).

#include <wabcut/audio/edit/PluginLibrary.h>

#include <wabcut/command/Schema.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

using command::Capability;
using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

PluginLibrary& libraryOf(ExecutionContext& c) {
    auto* services = command::findService<PluginServices>(c.services());
    if (services == nullptr || services->library() == nullptr) {
        fail(ErrorCode::InvalidRequest, "no audio plugin hosting here (the app and wabcut-cli on Windows have it)");
    }
    return *services->library();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

Json blockedJson(const std::vector<BlockedPluginListing>& blocked) {
    Json out = Json::array();
    for (const BlockedPluginListing& b : blocked) {
        out.push_back(Json{{"path", b.path}, {"reason", b.reason}});
    }
    return out;
}

std::vector<std::string> strings(ExecutionContext& c, const char* key) {
    std::vector<std::string> out;
    if (c.has(key)) {
        for (const Json& v : c.params()[key]) {
            out.push_back(v.get<std::string>());
        }
    }
    return out;
}

CommandOutput list(ExecutionContext& c) {
    PluginLibrary& library = libraryOf(c);
    const std::string format = c.string("format");
    const std::string query = lower(c.string("query"));
    const bool withParams = c.boolean("params", false);
    Json plugins = Json::array();
    for (const PluginListing& p : library.plugins()) {
        if ((!format.empty() && p.format != format) ||
            (!query.empty() && lower(p.name).find(query) == std::string::npos && lower(p.vendor).find(query) == std::string::npos)) {
            continue;
        }
        Json j{{"effect", p.effect},     {"format", p.format},   {"name", p.name},       {"vendor", p.vendor},
               {"version", p.version},   {"category", p.category}, {"latency", p.latency}, {"editor", p.editor},
               {"effectPlugin", p.effectPlugin}, {"parameterCount", p.params.size()}};
        if (!p.problem.empty()) {
            j["problem"] = p.problem;
        }
        if (withParams) {
            Json params = Json::array();
            for (const PluginParamListing& q : p.params) {
                params.push_back(Json{{"key", q.key},
                                      {"name", q.name},
                                      {"min", q.minValue},
                                      {"max", q.maxValue},
                                      {"default", q.defaultValue},
                                      {"automatable", q.automatable}});
            }
            j["params"] = std::move(params);
        }
        plugins.push_back(std::move(j));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"plugins", std::move(plugins)}, {"blocked", blockedJson(library.blocked())}, {"folders", library.folders()}};
    return out;
}

CommandOutput scan(ExecutionContext& c) {
    PluginLibrary& library = libraryOf(c);
    PluginScanOptions options;
    options.rescan = c.boolean("rescan", false);
    options.addFolders = strings(c, "addFolders");
    options.removeFolders = strings(c, "removeFolders");
    options.unblock = strings(c, "unblock");
    options.clearBlocklist = c.boolean("clearBlocklist", false);
    c.checkpoint("scanning audio plugins");
    // Progress through the request's checkpoints; a stop asked for there stops the scan between two files.
    std::stop_source stop;
    std::exception_ptr stopped;
    int lastPercent = -1;
    auto result = library.scan(options, stop.get_token(), [&](double fraction) {
        const int percent = static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
        if (percent == lastPercent || stopped) {
            return;
        }
        lastPercent = percent;
        try {
            c.checkpoint("scanning audio plugins (" + std::to_string(percent) + "%)");
        } catch (...) {
            stopped = std::current_exception();
            stop.request_stop();
        }
    });
    if (stopped) {
        std::rethrow_exception(stopped);
    }
    if (!result) {
        fail(ErrorCode::Internal, "scanning audio plugins: " + result.error().message);
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"files", result->files},     {"scanned", result->scanned}, {"cached", result->cached},
                    {"skipped", result->skipped}, {"plugins", result->plugins}, {"added", result->added},
                    {"blocked", blockedJson(result->blocked)}, {"errors", result->errors}, {"cancelled", result->cancelled}};
    for (const BlockedPluginListing& b : result->blocked) {
        out.warnings.push_back(b.path + " went on the blocklist: " + b.reason);
    }
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

void registerPluginCommands(command::CommandRegistry& registry) {
    add(registry,
        {"plugins.list", 1, "List Audio Plugins",
         "The audio plugins (CLAP, VST3) this computer has, by effect id (\"plugin.vst3.<class id>\", \"plugin.clap.<plugin id>\"): "
         "name, vendor, version, category, latency (frames), whether it has an editor, and with \"params\" its parameters "
         "(keys \"p<id>\", ranges, automatable). Put one on clips or tracks with clip.addEffect / audio.addEffect (\"plugin\"); "
         "its parameters are keyframed like any effect's. Also the blocklist (plugins that crashed or hung while scanned) and "
         "the folders scanned.",
         Capability::Read,
         schema::object({{"format", schema::enumeration({"clap", "vst3"}, "Only plugins of this format.")},
                         {"query", schema::string("Only plugins whose name or vendor contains this (any case).")},
                         {"params", schema::boolean("Include every plugin's parameters.", false)}}),
         Json{{"query", "nova"}, {"params", true}}, {}, list});
    add(registry,
        command::touchesFiles({"plugins.scan", 1, "Scan Audio Plugins",
         "Scans the audio plugin folders (Windows: Common Files\\CLAP and \\VST3, and the user's folders): new and changed files, "
         "each in its own helper process, so a plugin that crashes or hangs while scanned goes on the blocklist and nothing "
         "else is disturbed. Unchanged files keep their cached result (\"rescan\" scans them again). Adds or removes user "
         "folders and takes files off the blocklist first. Runs plugin code: not available to the assistant.",
         Capability::MediaAnalysis,
         schema::object({{"rescan", schema::boolean("Scan unchanged files again too.", false)},
                         {"addFolders", schema::array(schema::string("A folder."), "User plugin folders to add (remembered).")},
                         {"removeFolders", schema::array(schema::string("A folder."), "User plugin folders to remove.")},
                         {"unblock", schema::array(schema::string("A plugin file."), "Files to take off the blocklist.")},
                         {"clearBlocklist", schema::boolean("Empty the blocklist first.", false)}}),
         Json{{"addFolders", Json::array({"D:\\Plugins"})}}, {}, scan}));
}

} // namespace wabcut::audio::edit
