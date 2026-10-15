#include <wabcut/command/mcp/McpServer.h>

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

namespace wabcut::command::mcp {

namespace {

constexpr std::array<std::string_view, 2> kLegacyVersions{"2025-11-25", "2025-06-18"};

// JSON-RPC 2.0 and MCP error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kUnsupportedProtocolVersion = -32022;

const std::string kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
const std::string kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
const std::string kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

const std::string kProjectSummaryUri = "wabcut://project/summary";
const std::string kSequenceUriPrefix = "wabcut://sequence/";

const std::string kUndoTool = "edit_undo";
const std::string kRedoTool = "edit_redo";
const std::string kBatchTool = "edit_batch";
const std::string kSaveTool = "project_save";

enum class Era {
    Modern, ///< per-request _meta (2026-07-28)
    Legacy, ///< after an initialize handshake (2025-11-25, 2025-06-18)
};

/// A JSON-RPC error response, thrown by request handlers.
struct RpcError {
    int code;
    std::string message;
    Json data;
};

std::string dump(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

bool validId(const Json& id) { return id.is_string() || id.is_number_integer(); }

bool isBlank(std::string_view line) {
    return std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// --- Tool definitions ------------------------------------------------------------------------------------

Json errorSchema() {
    return Json{{"type", "object"},
                {"properties", Json{{"code", Json{{"type", "string"}}},
                                    {"message", Json{{"type", "string"}}},
                                    {"details", Json::object()}}},
                {"required", Json::array({"code", "message"})}};
}

Json readOutputSchema() {
    return Json{{"type", "object"},
                {"properties", Json{{"ok", Json{{"type", "boolean"}}},
                                    {"revision", Json{{"type", "integer"}, {"minimum", 0}}},
                                    // Most commands answer an object; some a list (llm.local.models).
                                    {"data", Json{{"description", "The command's result."}}},
                                    {"error", errorSchema()}}},
                {"required", Json::array({"ok", "revision"})}};
}

Json editOutputSchema() {
    const Json boolean{{"type", "boolean"}};
    const Json revision{{"type", "integer"}, {"minimum", 0}};
    return Json{
        {"type", "object"},
        {"properties",
         Json{{"ok", boolean},
              {"revision", schema::described(revision, "The session revision after the call; pass it as expectedRevision next.")},
              {"basedOnRevision", schema::described(revision, "The revision the edit was applied to.")},
              {"assumedRevision", schema::described(boolean, "true when no expectedRevision was given and the current one was used.")},
              {"dryRun", boolean},
              {"committed", schema::described(boolean, "Whether the project changed (one undo step).")},
              {"replayed", schema::described(boolean, "Answered from the idempotency cache.")},
              {"changes", schema::described(Json{{"type", "object"}},
                                    "What changed per sequence: tracks with [start, end) timecode ranges, added / removed "
                                    "/ modified clip ids.")},
              {"layout", schema::described(Json{{"type", "array"}},
                                   "The resulting clips of the changed tracks (sequence.summary format).")},
              {"createdIds", Json{{"type", "array"}, {"items", Json{{"type", "integer"}}}}},
              {"data", schema::described(Json{{"type", "object"}},
                                 "The command's result data (as the CLI returns it), when it has one.")},
              {"warnings", Json{{"type", "array"}, {"items", Json{{"type", "string"}}}}},
              {"steps", schema::described(Json{{"type", "array"}}, "edit_batch: per-command results.")},
              {"undone", schema::described(Json{{"type", "string"}}, "edit_undo: the undone step.")},
              {"redone", schema::described(Json{{"type", "string"}}, "edit_redo: the redone step.")},
              {"canUndo", boolean},
              {"canRedo", boolean},
              {"saved", schema::described(boolean, "The project file was written (autosave).")},
              {"error", errorSchema()}}},
        {"required", Json::array({"ok", "revision"})}};
}

Json saveOutputSchema() {
    return Json{{"type", "object"},
                {"properties", Json{{"ok", Json{{"type", "boolean"}}},
                                    {"revision", Json{{"type", "integer"}, {"minimum", 0}}},
                                    {"path", Json{{"type", "string"}}},
                                    {"copy", Json{{"type", "boolean"}}},
                                    {"error", errorSchema()}}},
                {"required", Json::array({"ok", "revision"})}};
}

Json annotations(std::string title, bool readOnly, bool destructive, bool idempotent) {
    return Json{{"title", std::move(title)},
                {"readOnlyHint", readOnly},
                {"destructiveHint", destructive},
                {"idempotentHint", idempotent},
                {"openWorldHint", false}};
}

Json revisionParam(std::string description) { return schema::integer(std::move(description), 0); }

void addEnvelope(Json& inputSchema) {
    Json& props = inputSchema["properties"];
    props["dryRun"] = schema::boolean(
        "Preview only: report the change and the resulting layout without committing. Default false.", false);
    props["expectedRevision"] =
        revisionParam("The revision your view is based on (every result reports `revision`). A stale one fails with "
                      "StaleRevision instead of editing. Default: the current revision.");
    props["idempotencyKey"] = schema::string("Makes retries safe: a repeated call with the same key returns the first "
                                             "result instead of editing again.");
}

std::string trackLabelIn(const Sequence& seq, TrackId id) {
    const auto pos = seq.trackPosition(id);
    if (!pos) {
        return std::to_string(id.value);
    }
    return (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1);
}

/// A copy name for project_save: a plain file name ending in .wabcut, no directories, no device names.
bool safeCopyName(const std::string& name) {
    constexpr std::string_view kSuffix = ".wabcut";
    if (name.size() <= kSuffix.size() || name.size() > 128 || name.front() == '.' || name.front() == ' ' ||
        !name.ends_with(kSuffix) || name.find("..") != std::string::npos) {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')')) {
            return false;
        }
    }
    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static constexpr std::array<std::string_view, 4> kDevices{"CON", "PRN", "AUX", "NUL"};
    const bool device = std::find(kDevices.begin(), kDevices.end(), stem) != kDevices.end() ||
                        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
                         std::isdigit(static_cast<unsigned char>(stem[3])));
    return !device;
}

} // namespace

std::span<const std::string_view> legacyProtocolVersions() { return kLegacyVersions; }

// --- Server ---------------------------------------------------------------------------------------------

struct McpServer::Impl {
    struct Call;

    Impl(EditSession& s, McpServerOptions o, const CommandRegistry& r)
        : session(s), options(normalized(std::move(o))), registry(r), engine(s, r, policyFor(options)),
          savedRevision(s.revision()) {}

    /// An MCP client is an LLM agent steered by what it reads (project content included): it never picks files outside
    /// the folders the user allowed, and never loads native code (S5-P1-1). Read commands that run a local model
    /// (search.footage, shorts.find, takes.find) are served read-only: they read the project and its media and never
    /// change them (docs/mcp.md); analysis commands need --allow-analysis.
    static EnginePolicy policyFor(const McpServerOptions& o) {
        EnginePolicy policy{o.allowed, 1024, o.services};
        policy.allowFileCommands = false;
        policy.fileRoots = o.fileRoots;
        policy.deniedCommands = {std::string(kNeverServed)};
        return policy;
    }

    /// Loads native plug-in code from the folders it is given: for the app's own UI only.
    static constexpr std::string_view kNeverServed = "plugins.scan";

    static McpServerOptions normalized(McpServerOptions o) {
        // Read is always granted; analysis commands only when asked for; network commands are never served here.
        CapabilitySet allowed = CapabilitySet::of({Capability::Read});
        for (const Capability c : {Capability::EditSequence, Capability::EditProject, Capability::MediaAnalysis}) {
            if (o.allowed.has(c)) {
                allowed.bits |= CapabilitySet::of({c}).bits;
            }
        }
        o.allowed = allowed;
        if (!o.clock) {
            o.clock = [] { return std::chrono::steady_clock::now(); };
        }
        if (!o.log) {
            o.log = [](std::string_view) {};
        }
        return o;
    }

    bool canEdit() const {
        return options.allowed.has(Capability::EditSequence) || options.allowed.has(Capability::EditProject);
    }
    bool canSave() const { return canEdit() && !options.projectPath.empty(); }

    // --- Output -----------------------------------------------------------------------------------------

    void send(const Json& message) { transport->write(dump(message)); }

    void reply(const Json& id, Json result, Era era) {
        if (era == Era::Modern) {
            result["resultType"] = "complete";
        }
        send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    }

    void replyError(const Json& id, int code, std::string message, Json data = nullptr) {
        Json error{{"code", code}, {"message", std::move(message)}};
        if (!data.is_null()) {
            error["data"] = std::move(data);
        }
        send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}});
    }

    void notify(const char* method, Json params) {
        send(Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
    }

    // --- Input ------------------------------------------------------------------------------------------

    void handleLine(std::string_view line) {
        if (line.size() > options.maxMessageBytes) {
            replyError(nullptr, kInvalidRequest, "Message too large",
                       Json{{"limitBytes", options.maxMessageBytes}});
            return;
        }
        if (isBlank(line)) {
            return;
        }
        bool tooDeep = false;
        const std::size_t maxDepth = options.maxJsonDepth;
        Json message = Json::parse(
            std::string(line),
            [&](int depth, Json::parse_event_t, Json&) {
                if (static_cast<std::size_t>(depth) > maxDepth) {
                    tooDeep = true;
                    return false;
                }
                return true;
            },
            false);
        if (message.is_discarded() && !tooDeep) {
            replyError(nullptr, kParseError, "Parse error: invalid JSON");
            return;
        }
        if (tooDeep) {
            replyError(nullptr, kInvalidRequest, "Message nested too deeply", Json{{"limitDepth", maxDepth}});
            return;
        }
        if (message.is_array()) {
            replyError(nullptr, kInvalidRequest, "Invalid request: JSON-RPC batches are not supported");
            return;
        }
        if (!message.is_object()) {
            replyError(nullptr, kInvalidRequest, "Invalid request: expected a JSON-RPC message object");
            return;
        }
        const bool hasId = message.contains("id");
        const Json id = hasId && validId(message["id"]) ? message["id"] : Json(nullptr);
        if (!message.contains("method")) {
            if (message.contains("result") || message.contains("error")) {
                options.log("ignoring a response: this server sends no requests");
                return;
            }
            replyError(id, kInvalidRequest, "Invalid request: no method");
            return;
        }
        if (message.value("jsonrpc", Json()) != "2.0" || !message["method"].is_string()) {
            replyError(id, kInvalidRequest, "Invalid request: \"jsonrpc\" must be \"2.0\" and \"method\" a string");
            return;
        }
        if (hasId && !validId(message["id"])) {
            replyError(nullptr, kInvalidRequest, "Invalid request: \"id\" must be a string or an integer");
            return;
        }
        const std::string method = message["method"].get<std::string>();
        const Json params = message.contains("params") ? message["params"] : Json::object();
        if (!params.is_object()) {
            if (hasId) {
                replyError(id, kInvalidParams, "Invalid params: \"params\" must be an object");
            }
            return;
        }
        if (!hasId) {
            handleNotification(method, params);
            return;
        }
        if (takeCancelled(id)) {
            options.log("skipping request " + dump(id) + ": cancelled before it ran");
            return;
        }
        try {
            handleRequest(id, method, params);
        } catch (const RpcError& e) {
            replyError(id, e.code, e.message, e.data);
        } catch (const std::exception& e) {
            options.log(std::string("internal error: ") + e.what());
            replyError(id, kInternalError, std::string("Internal error: ") + e.what());
        }
    }

    void handleNotification(const std::string& method, const Json& params) {
        if (method == "notifications/initialized") {
            legacyReady = true;
        } else if (method == "notifications/cancelled") {
            cancel(params);
        } else {
            options.log("ignoring notification " + method);
        }
    }

    /// Reads what has already arrived while a call runs: cancellations apply at once, the rest waits.
    void drain() {
        while (auto line = transport->poll()) {
            Json message = Json::parse(*line, nullptr, false);
            if (!message.is_discarded() && message.is_object() && !message.contains("id") &&
                message.value("method", Json()) == "notifications/cancelled") {
                cancel(message.value("params", Json::object()));
            } else {
                pending.push_back(std::move(*line));
            }
        }
    }

    void cancel(const Json& params) {
        if (!params.is_object() || !params.contains("requestId") || !validId(params["requestId"])) {
            return; // malformed cancellations are ignored
        }
        const Json& requestId = params["requestId"];
        if (current && current->id == requestId) {
            current->cancelled = true;
            options.log("cancelling request " + dump(requestId));
            return;
        }
        // A request that arrived while a call ran and still waits: skip it when its turn comes. Cancellations of
        // unknown or finished requests are ignored.
        const bool queued = std::any_of(pending.begin(), pending.end(), [&](const std::string& line) {
            const Json message = Json::parse(line, nullptr, false);
            return message.is_object() && message.contains("method") && message.contains("id") &&
                   message["id"] == requestId;
        });
        if (queued) {
            cancelledIds.push_back(requestId);
        }
    }

    bool takeCancelled(const Json& id) {
        const auto it = std::find(cancelledIds.begin(), cancelledIds.end(), id);
        if (it == cancelledIds.end()) {
            return false;
        }
        cancelledIds.erase(it);
        return true;
    }

    // --- Requests ---------------------------------------------------------------------------------------

    Era eraOf(const Json& params, bool required) const {
        const Json* meta = params.contains("_meta") && params["_meta"].is_object() ? &params["_meta"] : nullptr;
        if (meta && meta->contains(kMetaProtocolVersion)) {
            const Json& version = (*meta)[kMetaProtocolVersion];
            if (!version.is_string()) {
                throw RpcError{kInvalidParams, "Invalid params: the protocol version must be a string", nullptr};
            }
            if (version.get<std::string>() != kProtocolVersion) {
                throw RpcError{kUnsupportedProtocolVersion, "Unsupported protocol version",
                               Json{{"supported", Json::array({std::string(kProtocolVersion)})}, {"requested", version}}};
            }
            if (required && (!meta->contains(kMetaClientCapabilities) || !(*meta)[kMetaClientCapabilities].is_object())) {
                throw RpcError{kInvalidParams, "Invalid params: _meta needs " + kMetaClientCapabilities, nullptr};
            }
            return Era::Modern;
        }
        if (legacyVersion) {
            return Era::Legacy;
        }
        if (!required) {
            return Era::Modern;
        }
        Json supported = Json::array({std::string(kProtocolVersion)});
        for (const std::string_view v : kLegacyVersions) {
            supported.push_back(std::string(v));
        }
        throw RpcError{kInvalidParams,
                       "Invalid params: send _meta \"" + kMetaProtocolVersion + "\": \"" + std::string(kProtocolVersion) +
                           "\" with each request, or initialize first",
                       Json{{"supported", std::move(supported)}}};
    }

    void handleRequest(const Json& id, const std::string& method, const Json& params) {
        if (method == "initialize") {
            initialize(id, params);
            return;
        }
        if (method == "server/discover") {
            reply(id, discover(), Era::Modern);
            return;
        }
        if (method == "ping") {
            reply(id, Json::object(), eraOf(params, false));
            return;
        }
        static const std::array<std::string_view, 5> kMethods{"tools/list", "tools/call", "resources/list",
                                                              "resources/templates/list", "resources/read"};
        if (std::find(kMethods.begin(), kMethods.end(), method) == kMethods.end()) {
            throw RpcError{kMethodNotFound, "Method not found: " + method, nullptr};
        }
        const Era era = eraOf(params, true);
        if (method == "tools/list") {
            reply(id, Json{{"tools", tools()}}, era);
        } else if (method == "tools/call") {
            callTool(id, params, era);
        } else if (method == "resources/list") {
            reply(id, Json{{"resources", resources()}}, era);
        } else if (method == "resources/templates/list") {
            reply(id,
                  Json{{"resourceTemplates",
                        Json::array({Json{{"uriTemplate", kSequenceUriPrefix + "{id}"},
                                          {"name", "sequence"},
                                          {"title", "Sequence layout"},
                                          {"description", "Compact JSON of one sequence: tracks and their clips, marks, "
                                                          "markers, plus the session revision."},
                                          {"mimeType", "application/json"}}})}},
                  era);
        } else {
            if (!params.contains("uri") || !params["uri"].is_string()) {
                throw RpcError{kInvalidParams, "Invalid params: resources/read needs a uri", nullptr};
            }
            reply(id, readResource(params["uri"].get<std::string>()), era);
        }
    }

    Json capabilities() const {
        return Json{{"tools", Json{{"listChanged", false}}}, {"resources", Json{{"subscribe", false}, {"listChanged", false}}}};
    }

    Json serverInfo() const {
        return Json{{"name", "wabcut"}, {"title", "Wabcut"}, {"version", options.serverVersion}};
    }

    std::string instructions() const {
        std::string text =
            "Wabcut video editor. Start with project_summary (or the wabcut://project/summary resource) and "
            "sequence_summary. Times are timecodes in the sequence's rate (\"00:01:02:03\") or ticks; tracks are "
            "\"V1\" / \"A1\"; clips are ids or {\"track\", \"time\"}. ";
        if (!canEdit()) {
            return text + "This server is read-only: edit tools are not available.";
        }
        text += "Preview an edit with dryRun: true, then commit it with the expectedRevision from the last result. "
                "Each edit is one undo step (edit_undo / edit_redo); edit_batch applies several commands as one step.";
        if (canSave()) {
            text += " project_save writes the project file.";
        }
        return text;
    }

    void initialize(const Json& id, const Json& params) {
        if (legacyVersion) {
            throw RpcError{kInvalidRequest, "Invalid request: already initialized", nullptr};
        }
        if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
            throw RpcError{kInvalidParams, "Invalid params: initialize needs a protocolVersion", nullptr};
        }
        const std::string requested = params["protocolVersion"].get<std::string>();
        // Legacy negotiation: the requested revision if served, otherwise the newest legacy one.
        const bool served = std::find(kLegacyVersions.begin(), kLegacyVersions.end(), requested) != kLegacyVersions.end();
        legacyVersion = served ? requested : std::string(kLegacyVersions.front());
        std::string client = "a client";
        if (params.contains("clientInfo") && params["clientInfo"].is_object()) {
            client = params["clientInfo"].value("name", client);
        }
        options.log(client + " initialized: requested " + requested + ", serving " + *legacyVersion);
        reply(id,
              Json{{"protocolVersion", *legacyVersion},
                   {"capabilities", capabilities()},
                   {"serverInfo", serverInfo()},
                   {"instructions", instructions()}},
              Era::Legacy);
    }

    Json discover() const {
        return Json{{"supportedVersions", Json::array({std::string(kProtocolVersion)})},
                    {"capabilities", capabilities()},
                    {"_meta", Json{{kMetaServerInfo, serverInfo()}}},
                    {"instructions", instructions()}};
    }

    // --- Tools ------------------------------------------------------------------------------------------

    std::vector<const CommandSpec*> servedCommands() const {
        std::vector<const CommandSpec*> out;
        const auto specs = registry.all();
        for (std::size_t i = 0; i < specs.size(); ++i) {
            if (i + 1 < specs.size() && specs[i + 1].name == specs[i].name) {
                continue; // newest version only
            }
            const bool files = specs[i].touchesFiles &&
                               (options.fileRoots.empty() || specs[i].fileParams.empty() || specs[i].name == kNeverServed);
            if (options.allowed.has(specs[i].capability) && !files) {
                out.push_back(&specs[i]);
            }
        }
        return out;
    }

    Json catalogTool(const CommandSpec& spec) const {
        Json input = spec.params;
        if (spec.mutates()) {
            addEnvelope(input);
        }
        const bool read = !spec.mutates();
        return Json{{"name", toolNameFor(spec.name)},
                    {"title", spec.title},
                    {"description", spec.description},
                    {"inputSchema", std::move(input)},
                    {"outputSchema", read ? readOutputSchema() : editOutputSchema()},
                    {"annotations", annotations(spec.title, read, !read, read)}};
    }

    Json batchInputSchema() const {
        std::vector<std::string> names;
        for (const CommandSpec* spec : servedCommands()) {
            names.push_back(spec->name);
        }
        Json item = schema::object({{"command", schema::enumeration(names, "A command (see the other tools; dotted name).")},
                                    {"params", Json{{"type", "object"}, {"description", "The command's parameters."}}}},
                                   {"command"});
        Json s = schema::object({{"commands", schema::array(std::move(item), "Commands, applied in order.", 1)},
                                 {"label", schema::string("Undo label. Default \"Batch (N commands)\".")}},
                                {"commands"});
        s["properties"]["commands"]["maxItems"] = 1000;
        s["properties"]["label"]["maxLength"] = 200;
        addEnvelope(s);
        return s;
    }

    Json tools() const {
        Json list = Json::array();
        for (const CommandSpec* spec : servedCommands()) {
            list.push_back(catalogTool(*spec));
        }
        if (!canEdit()) {
            return list;
        }
        list.push_back(Json{{"name", kBatchTool},
                            {"title", "Batch"},
                            {"description", "Applies several commands in order as ONE undo step: all of them or none. "
                                            "Later commands see the earlier ones' result. Read commands report in steps."},
                            {"inputSchema", batchInputSchema()},
                            {"outputSchema", editOutputSchema()},
                            {"annotations", annotations("Batch", false, true, false)}});
        const Json undoRedoInput = schema::object(
            {{"expectedRevision", revisionParam("Only act if the project is still at this revision.")}});
        list.push_back(Json{{"name", kUndoTool},
                            {"title", "Undo"},
                            {"description", "Undoes the last change of this session (one edit or batch)."},
                            {"inputSchema", undoRedoInput},
                            {"outputSchema", editOutputSchema()},
                            {"annotations", annotations("Undo", false, true, false)}});
        list.push_back(Json{{"name", kRedoTool},
                            {"title", "Redo"},
                            {"description", "Redoes the last undone change."},
                            {"inputSchema", undoRedoInput},
                            {"outputSchema", editOutputSchema()},
                            {"annotations", annotations("Redo", false, true, false)}});
        if (canSave()) {
            list.push_back(Json{
                {"name", kSaveTool},
                {"title", "Save Project"},
                {"description", "Writes the project file (atomically, keeping rotated .bak copies). With copyName, writes "
                                "a copy next to it instead, e.g. \"Rough cut v2.wabcut\"."},
                {"inputSchema", schema::object({{"copyName", schema::string("File name of a copy in the project's folder "
                                                                            "(no directories), ending in .wabcut.")}})},
                {"outputSchema", saveOutputSchema()},
                {"annotations", annotations("Save Project", false, false, true)}});
        }
        return list;
    }

    // --- Tool calls -------------------------------------------------------------------------------------

    struct Call : ExecutionControl {
        Impl& server;
        Json id;
        std::optional<Json> progressToken;
        std::chrono::steady_clock::time_point deadline;
        double progress = 0;
        bool cancelled = false;

        Call(Impl& s, Json requestId, std::optional<Json> token)
            : server(s), id(std::move(requestId)), progressToken(std::move(token)),
              deadline(s.options.clock() + s.options.callTimeout) {}

        std::optional<CommandError> checkpoint(std::string_view message) override {
            server.drain();
            if (cancelled) {
                return CommandError{ErrorCode::Cancelled, "cancelled by the client", nullptr};
            }
            if (server.options.clock() >= deadline) {
                return CommandError{ErrorCode::TimedOut,
                                    "the call ran longer than " + std::to_string(server.options.callTimeout.count()) +
                                        " ms; nothing was changed",
                                    nullptr};
            }
            if (progressToken) {
                progress += 1;
                server.notify("notifications/progress",
                              Json{{"progressToken", *progressToken}, {"progress", progress},
                                   {"message", std::string(message)}});
            }
            return std::nullopt;
        }
    };

    static Json toolResult(Json structured) {
        Json text{{"type", "text"}, {"text", dump(structured)}};
        return Json{{"content", Json::array({std::move(text)})}, {"structuredContent", std::move(structured)},
                    {"isError", false}};
    }

    Json toolError(const CommandError& error) const {
        Json e{{"code", toString(error.code)}, {"message", error.message}};
        std::string text = std::string(toString(error.code)) + ": " + error.message;
        if (!error.details.is_null()) {
            e["details"] = error.details;
            text += "\n" + dump(error.details);
        }
        Json structured{{"ok", false}, {"revision", session.revision()}, {"error", std::move(e)}};
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", std::move(text)}}})},
                    {"structuredContent", std::move(structured)},
                    {"isError", true}};
    }

    Json readOnlyRefusal(const std::string& tool) const {
        return toolError(CommandError{ErrorCode::PermissionDenied,
                                      tool + " changes the project, but this Wabcut MCP server is read-only. Ask the user "
                                             "to restart it with --allow-edit.",
                                      nullptr});
    }

    void callTool(const Json& id, const Json& params, Era era) {
        if (!params.contains("name") || !params["name"].is_string()) {
            throw RpcError{kInvalidParams, "Invalid params: tools/call needs a tool name", nullptr};
        }
        const std::string name = params["name"].get<std::string>();
        Json args = params.contains("arguments") && !params["arguments"].is_null() ? params["arguments"] : Json::object();
        if (!args.is_object()) {
            throw RpcError{kInvalidParams, "Invalid params: arguments must be an object", nullptr};
        }
        std::optional<Json> token;
        if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("progressToken") &&
            validId(params["_meta"]["progressToken"])) {
            token = params["_meta"]["progressToken"];
        }
        Call call(*this, id, token);
        current = &call;
        struct Reset {
            Call*& slot;
            ~Reset() { slot = nullptr; }
        } reset{current};

        std::optional<Json> result;
        if (name == kBatchTool || name == kUndoTool || name == kRedoTool || name == kSaveTool) {
            if (!canEdit()) {
                result = readOnlyRefusal(name);
            } else if (name == kBatchTool) {
                result = callBatch(args, call);
            } else if (name == kSaveTool) {
                result = canSave() ? callSave(args)
                                   : toolError(CommandError{ErrorCode::PermissionDenied,
                                                            "this server has no project file to save to", nullptr});
            } else {
                result = callUndoRedo(args, name == kUndoTool);
            }
        } else {
            const CommandSpec* spec = registry.find(name);
            if (!spec || spec->capability == Capability::Network ||
                (spec->capability == Capability::MediaAnalysis && !options.allowed.has(Capability::MediaAnalysis))) {
                throw RpcError{kInvalidParams, "Unknown tool: " + name, nullptr};
            }
            if (options.allowed.has(spec->capability)) {
                result = callCommand(*spec, args, call);
            } else {
                result = readOnlyRefusal(toolNameFor(spec->name));
            }
        }
        if (!result) {
            return; // cancelled: the client no longer wants an answer
        }
        reply(id, std::move(*result), era);
    }

    /// Moves dryRun / expectedRevision / idempotencyKey from the arguments into the request.
    std::optional<CommandError> takeEnvelope(Json& args, bool& dryRun, std::optional<std::uint64_t>& expectedRevision,
                                             std::optional<std::string>& idempotencyKey) const {
        if (args.contains("dryRun")) {
            if (!args["dryRun"].is_boolean()) {
                return CommandError{ErrorCode::InvalidParams, "dryRun must be true or false", Json{{"path", "/dryRun"}}};
            }
            dryRun = args["dryRun"].get<bool>();
            args.erase("dryRun");
        }
        if (args.contains("expectedRevision")) {
            const Json& v = args["expectedRevision"];
            if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<std::int64_t>() < 0)) {
                return CommandError{ErrorCode::InvalidParams, "expectedRevision must be a non-negative integer",
                                    Json{{"path", "/expectedRevision"}}};
            }
            expectedRevision = v.is_number_unsigned() ? v.get<std::uint64_t>()
                                                      : static_cast<std::uint64_t>(v.get<std::int64_t>());
            args.erase("expectedRevision");
        }
        if (args.contains("idempotencyKey")) {
            if (!args["idempotencyKey"].is_string() || args["idempotencyKey"].get<std::string>().empty()) {
                return CommandError{ErrorCode::InvalidParams, "idempotencyKey must be a non-empty string",
                                    Json{{"path", "/idempotencyKey"}}};
            }
            idempotencyKey = args["idempotencyKey"].get<std::string>();
            args.erase("idempotencyKey");
        }
        return std::nullopt;
    }

    /// Fills in the revision a commit is based on, or refuses when the options require the client to name it.
    std::optional<CommandError> baseRevision(bool dryRun, std::optional<std::uint64_t>& expectedRevision,
                                             bool& assumed) const {
        if (dryRun || expectedRevision) {
            return std::nullopt;
        }
        if (options.requireExpectedRevision) {
            return CommandError{ErrorCode::InvalidParams,
                                "pass expectedRevision to commit (the project is at revision " +
                                    std::to_string(session.revision()) + ")",
                                Json{{"current", session.revision()}}};
        }
        expectedRevision = session.revision();
        assumed = true;
        return std::nullopt;
    }

    std::optional<Json> callCommand(const CommandSpec& spec, Json args, Call& call) {
        if (!spec.mutates()) {
            CommandRequest request{spec.name, spec.version, std::move(args), std::nullopt, std::nullopt, false};
            const CommandResult r = engine.execute(request, &call);
            if (call.cancelled) {
                return std::nullopt;
            }
            if (!r.ok()) {
                return toolError(*r.error);
            }
            return toolResult(Json{{"ok", true}, {"revision", r.revision}, {"data", r.data}});
        }
        CommandRequest request;
        request.command = spec.name;
        request.version = spec.version;
        if (auto bad = takeEnvelope(args, request.dryRun, request.expectedRevision, request.idempotencyKey)) {
            return toolError(*bad);
        }
        bool assumed = false;
        if (auto bad = baseRevision(request.dryRun, request.expectedRevision, assumed)) {
            return toolError(*bad);
        }
        request.params = std::move(args);
        const ProjectSnapshot before = session.snapshot();
        const std::uint64_t beforeRevision = session.revision();
        const CommandResult r = engine.execute(request, &call);
        if (call.cancelled) {
            return std::nullopt;
        }
        return editResult(r, *before, beforeRevision, assumed);
    }

    std::optional<Json> callBatch(const Json& input, Call& call) {
        Json args = input;
        CommandBatch batch;
        if (auto bad = takeEnvelope(args, batch.dryRun, batch.expectedRevision, batch.idempotencyKey)) {
            return toolError(*bad);
        }
        if (auto issues = validateAgainstSchema(batchInputSchema(), input); !issues.empty()) {
            return toolError(CommandError{ErrorCode::InvalidParams, issues.front().path + " " + issues.front().message,
                                          Json{{"path", issues.front().path}}});
        }
        for (const Json& item : args["commands"]) {
            CommandRequest request;
            request.command = item["command"].get<std::string>();
            request.params = item.contains("params") ? item["params"] : Json::object();
            batch.commands.push_back(std::move(request));
        }
        batch.label = args.value("label", std::string());
        bool assumed = false;
        if (auto bad = baseRevision(batch.dryRun, batch.expectedRevision, assumed)) {
            return toolError(*bad);
        }
        const ProjectSnapshot before = session.snapshot();
        const std::uint64_t beforeRevision = session.revision();
        const CommandResult r = engine.execute(batch, &call);
        if (call.cancelled) {
            return std::nullopt;
        }
        return editResult(r, *before, beforeRevision, assumed);
    }

    Json editResult(const CommandResult& r, const ProjectState& before, std::uint64_t beforeRevision, bool assumed) {
        if (!r.ok()) {
            return toolError(*r.error);
        }
        const ProjectSnapshot now = session.snapshot();
        const ProjectState& after = r.preview ? *r.preview : *now;
        Json s{{"ok", true},
               {"dryRun", r.dryRun},
               {"committed", r.committed},
               {"basedOnRevision", beforeRevision},
               {"revision", r.revision},
               {"changes", changeSummary(r.changes, before, after)},
               {"layout", layoutOf(r.changes, after)}};
        if (assumed) {
            s["assumedRevision"] = true;
        }
        if (r.replayed) {
            s["replayed"] = true;
        }
        if (!r.createdIds.empty()) {
            s["createdIds"] = r.createdIds;
        }
        // The command's own result (pasted clip ids, a note's id, ...), exactly as the CLI and the engine return it
        // (C.11: single edit tools used to return it only inside edit_batch steps).
        if (!r.data.is_null()) {
            s["data"] = r.data;
        }
        Json warnings = r.warnings;
        if (!r.steps.empty()) {
            Json steps = Json::array();
            for (const StepResult& step : r.steps) {
                Json entry{{"command", step.command}};
                if (!step.createdIds.empty()) {
                    entry["createdIds"] = step.createdIds;
                }
                if (!step.data.is_null()) {
                    entry["data"] = step.data;
                }
                steps.push_back(std::move(entry));
            }
            s["steps"] = std::move(steps);
        }
        if (r.committed) {
            afterChange(s, warnings);
        }
        if (!warnings.empty()) {
            s["warnings"] = std::move(warnings);
        }
        return toolResult(std::move(s));
    }

    Json callUndoRedo(const Json& args, bool undo) {
        if (auto issues = validateAgainstSchema(
                schema::object({{"expectedRevision", revisionParam("")}}), args);
            !issues.empty()) {
            return toolError(CommandError{ErrorCode::InvalidParams, issues.front().path + " " + issues.front().message,
                                          Json{{"path", issues.front().path}}});
        }
        const std::uint64_t revision = session.revision();
        if (args.contains("expectedRevision") && args["expectedRevision"].get<std::uint64_t>() != revision) {
            return toolError(CommandError{ErrorCode::StaleRevision,
                                          "the project is at revision " + std::to_string(revision) + ", not " +
                                              args["expectedRevision"].dump(),
                                          Json{{"expected", args["expectedRevision"]}, {"current", revision}}});
        }
        if (undo ? !session.canUndo() : !session.canRedo()) {
            return toolError(CommandError{ErrorCode::EditRejected, undo ? "nothing to undo" : "nothing to redo", nullptr});
        }
        const std::string label = (undo ? session.undoLabel() : session.redoLabel()).value_or(std::string());
        const ProjectSnapshot before = session.snapshot();
        const std::optional<ChangeSet> changes = undo ? session.undo() : session.redo();
        const ProjectSnapshot after = session.snapshot();
        Json s{{"ok", true},
               {"committed", true},
               {"basedOnRevision", revision},
               {"revision", session.revision()},
               {undo ? "undone" : "redone", label},
               {"changes", changeSummary(changes.value_or(ChangeSet{}), *before, *after)},
               {"layout", layoutOf(changes.value_or(ChangeSet{}), *after)},
               {"canUndo", session.canUndo()},
               {"canRedo", session.canRedo()}};
        Json warnings = Json::array();
        afterChange(s, warnings);
        if (!warnings.empty()) {
            s["warnings"] = std::move(warnings);
        }
        return toolResult(std::move(s));
    }

    Json callSave(const Json& args) {
        if (auto issues = validateAgainstSchema(schema::object({{"copyName", schema::string("")}}), args);
            !issues.empty()) {
            return toolError(CommandError{ErrorCode::InvalidParams, issues.front().path + " " + issues.front().message,
                                          Json{{"path", issues.front().path}}});
        }
        std::filesystem::path path = options.projectPath;
        if (args.contains("copyName")) {
            const std::string name = args["copyName"].get<std::string>();
            if (!safeCopyName(name)) {
                return toolError(CommandError{ErrorCode::InvalidParams,
                                              "copyName must be a plain file name ending in .wabcut (letters, digits, "
                                              "space, _ - . ( ); no folders)",
                                              Json{{"path", "/copyName"}}});
            }
            path = options.projectPath.parent_path() / std::filesystem::path(std::u8string(name.begin(), name.end()));
        }
        const bool copy = path != options.projectPath;
        if (auto saved = io::saveProject(session.stateForSave(), path); !saved) {
            return toolError(CommandError{ErrorCode::Internal, "cannot save: " + saved.error().message, nullptr});
        }
        if (!copy) {
            savedRevision = session.revision();
        }
        options.log("saved " + pathToUtf8(path));
        const std::u8string utf8 = path.u8string();
        return toolResult(Json{{"ok", true},
                               {"revision", session.revision()},
                               {"path", std::string(utf8.begin(), utf8.end())},
                               {"copy", copy}});
    }

    /// Autosave after a change.
    void afterChange(Json& structured, Json& warnings) {
        if (options.saveMode != SaveMode::Auto || options.projectPath.empty()) {
            return;
        }
        if (auto saved = io::saveProject(session.stateForSave(), options.projectPath); !saved) {
            warnings.push_back("autosave failed: " + saved.error().message);
            options.log("autosave failed: " + saved.error().message);
            return;
        }
        savedRevision = session.revision();
        structured["saved"] = true;
    }

    // --- Summaries ----------------------------------------------------------------------------------------

    /// The ChangeSet with track labels and [start, end) timecodes instead of ids and ticks.
    static Json changeSummary(const ChangeSet& changes, const ProjectState& before, const ProjectState& after) {
        Json out = toJson(changes);
        for (std::size_t i = 0; i < changes.sequences.size(); ++i) {
            const SequenceChange& sc = changes.sequences[i];
            const Sequence* now = after.findSequence(sc.sequence);
            const Sequence* old = before.findSequence(sc.sequence);
            const Sequence* ref = now ? now : old;
            if (!ref) {
                continue;
            }
            Json tracks = Json::array();
            for (const TrackChange& tc : sc.tracks) {
                const Sequence& labelFrom = now && now->findTrack(tc.track) ? *now : (old ? *old : *ref);
                Json t{{"track", trackLabelIn(labelFrom, tc.track)}};
                if (!tc.ranges.empty()) {
                    Json ranges = Json::array();
                    for (const TimeRange& r : tc.ranges) {
                        ranges.push_back(Json::array({ticksToTimecode(r.start, ref->frameRate, ref->dropFrame),
                                                      ticksToTimecode(r.end(), ref->frameRate, ref->dropFrame)}));
                    }
                    t["ranges"] = std::move(ranges);
                }
                if (tc.added) {
                    t["added"] = true;
                }
                if (tc.removed) {
                    t["removed"] = true;
                }
                if (tc.headerChanged) {
                    t["header"] = true;
                }
                if (tc.transitionsChanged) {
                    t["transitions"] = true;
                }
                tracks.push_back(std::move(t));
            }
            out["sequences"][i]["tracks"] = std::move(tracks);
        }
        return out;
    }

    /// The resulting clips on the changed tracks of each changed sequence (sequence.summary format).
    Json layoutOf(const ChangeSet& changes, const ProjectState& state) const {
        Json out = Json::array();
        const CommandSpec* summary = registry.find("sequence.summary");
        if (!summary) {
            return out;
        }
        for (const SequenceChange& sc : changes.sequences) {
            const Sequence* seq = state.findSequence(sc.sequence);
            if (!seq) {
                continue;
            }
            Json tracks = Json::array();
            for (const TrackChange& tc : sc.tracks) {
                if (!tc.removed && seq->findTrack(tc.track)) {
                    tracks.push_back(tc.track.value);
                }
            }
            Json params{{"sequence", sc.sequence.value}, {"maxClips", 200}};
            if (!tracks.empty()) {
                params["tracks"] = tracks;
            }
            try {
                ExecutionContext context(state, params);
                Json data = summary->execute(context).data;
                if (tracks.empty()) {
                    data["tracks"] = Json::array(); // marks / markers / settings only
                }
                out.push_back(std::move(data));
            } catch (const CommandFailure&) {
                // A layout is a convenience; the edit result stands without it.
            }
        }
        return out;
    }

    // --- Resources ----------------------------------------------------------------------------------------

    Json resources() const {
        Json list = Json::array({Json{{"uri", kProjectSummaryUri},
                                      {"name", "project-summary"},
                                      {"title", "Project summary"},
                                      {"description", "Sequences (rate, length, track and clip counts) and media counts, "
                                                      "plus the session revision."},
                                      {"mimeType", "application/json"}}});
        const ProjectSnapshot state = session.snapshot();
        for (const SequencePtr& seq : state->sequences) {
            list.push_back(Json{{"uri", kSequenceUriPrefix + std::to_string(seq->id.value)},
                                {"name", "sequence-" + std::to_string(seq->id.value)},
                                {"title", "Sequence: " + seq->name},
                                {"description", "Tracks and their clips (compact arrays in clipFields order), marks, "
                                                "markers, plus the session revision."},
                                {"mimeType", "application/json"}});
        }
        return list;
    }

    Json readResource(const std::string& uri) {
        CommandRequest request;
        if (uri == kProjectSummaryUri) {
            request.command = "project.summary";
        } else if (uri.starts_with(kSequenceUriPrefix)) {
            const std::string id = uri.substr(kSequenceUriPrefix.size());
            if (id.empty() || id.size() > 19 ||
                !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                throw RpcError{kInvalidParams, "Resource not found", Json{{"uri", uri}}};
            }
            request.command = "sequence.summary";
            request.params = Json{{"sequence", std::stoull(id)}};
        } else {
            throw RpcError{kInvalidParams, "Resource not found", Json{{"uri", uri}}};
        }
        const CommandResult r = engine.execute(request);
        if (!r.ok()) {
            throw RpcError{kInvalidParams, "Resource not found", Json{{"uri", uri}, {"reason", r.error->message}}};
        }
        Json content = r.data;
        content["revision"] = r.revision;
        return Json{{"contents", Json::array({Json{{"uri", uri}, {"mimeType", "application/json"}, {"text", dump(content)}}})}};
    }

    // --- Lifetime -----------------------------------------------------------------------------------------

    void finish() {
        if (options.saveMode != SaveMode::OnExit || options.projectPath.empty() || session.revision() == savedRevision) {
            return;
        }
        if (auto saved = io::saveProject(session.stateForSave(), options.projectPath); !saved) {
            options.log("saving on exit failed: " + saved.error().message);
            return;
        }
        savedRevision = session.revision();
        options.log("saved " + pathToUtf8(options.projectPath) + " on exit");
    }

    EditSession& session;
    McpServerOptions options;
    const CommandRegistry& registry;
    CommandEngine engine;
    std::uint64_t savedRevision;

    McpTransport* transport = nullptr;
    std::deque<std::string> pending;
    std::deque<Json> cancelledIds;
    std::optional<std::string> legacyVersion;
    bool legacyReady = false;
    Call* current = nullptr;
};

McpServer::McpServer(EditSession& session, McpServerOptions options, const CommandRegistry& registry)
    : m(std::make_unique<Impl>(session, std::move(options), registry)) {}

McpServer::~McpServer() = default;

void McpServer::handle(std::string_view line, McpTransport& transport) {
    m->transport = &transport;
    m->handleLine(line);
    // Messages that arrived while a call ran, in order.
    while (!m->pending.empty()) {
        std::string next = std::move(m->pending.front());
        m->pending.pop_front();
        m->handleLine(next);
    }
}

void McpServer::serve(McpTransport& transport) {
    while (auto line = transport.read()) {
        handle(*line, transport);
    }
    m->finish();
}

Json McpServer::tools() const { return m->tools(); }

std::uint64_t McpServer::savedRevision() const { return m->savedRevision; }

} // namespace wabcut::command::mcp
