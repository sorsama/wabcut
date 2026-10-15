#include <wabcut/command/mcp/McpCli.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/SnapshotReleaser.h>

#include <charconv>
#include <cstdio>

namespace wabcut::command::mcp {

namespace {

void logLine(std::string_view message) {
    std::fprintf(stderr, "wabcut mcp: %.*s\n", static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

} // namespace

std::string_view mcpUsage() {
    return "usage: wabcut-cli mcp --project <file.wabcut> [--allow-edit] [--allow-analysis] [--allow-files] "
           "[--files-folder <dir>]... [--save-on-exit | --autosave] [--timeout-ms <n>]\n"
           "Serves the project to MCP clients (Claude Desktop/Code, Codex, ...) on stdin/stdout. Read-only unless\n"
           "--allow-edit. --allow-analysis also serves the analysis commands (transcript.transcribe on this computer,\n"
           "audio.loudness), which decode media: raise --timeout-ms for long files. Commands that read or write files\n"
           "(review packages, CDL/LUT files, project.merge, lottie.import) are served only with --allow-files, and then\n"
           "only inside the project's folder and each --files-folder; plug-in scans never. --save-on-exit saves when\n"
           "the client disconnects, --autosave after every change. See docs/mcp.md.\n";
}

std::expected<McpCliOptions, std::string> parseMcpArguments(std::span<const std::string_view> args) {
    McpCliOptions options;
    bool saveFlag = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--project" && i + 1 < args.size()) {
            options.project = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--allow-edit") {
            options.allowEdit = true;
        } else if (arg == "--allow-analysis") {
            options.allowAnalysis = true;
        } else if (arg == "--allow-files") {
            options.allowFiles = true;
        } else if (arg == "--files-folder" && i + 1 < args.size()) {
            options.allowFiles = true;
            options.filesFolders.push_back(pathFromUtf8(std::string(args[++i])));
        } else if (arg == "--save-on-exit" || arg == "--autosave") {
            if (saveFlag) {
                return std::unexpected(std::string("--save-on-exit and --autosave exclude each other"));
            }
            saveFlag = true;
            options.saveMode = arg == "--autosave" ? SaveMode::Auto : SaveMode::OnExit;
        } else if (arg == "--timeout-ms" && i + 1 < args.size()) {
            const std::string_view text = args[++i];
            long long ms = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), ms);
            if (ec != std::errc{} || end != text.data() + text.size() || ms < 1 || ms > 3'600'000) {
                return std::unexpected("--timeout-ms needs a number of milliseconds from 1 to 3600000");
            }
            options.timeout = std::chrono::milliseconds(ms);
        } else {
            return std::unexpected("unknown argument '" + std::string(arg) + "'");
        }
    }
    if (options.project.empty()) {
        return std::unexpected(std::string("--project <file.wabcut> is required"));
    }
    if (saveFlag && !options.allowEdit) {
        return std::unexpected(std::string("--save-on-exit and --autosave need --allow-edit"));
    }
    return options;
}

int runMcpStdio(const McpCliOptions& options, std::string serverVersion, const CommandRegistry& registry,
                std::shared_ptr<CommandServices> services) {
    auto loaded = io::loadProject(options.project);
    if (!loaded) {
        logLine("cannot load " + pathToUtf8(options.project) + ": " + loaded.error().message);
        return 2;
    }
    EditSession session(std::move(*loaded));
    // P.1: a long agent session evicts undo steps; they are freed as they go, not when the server exits.
    SnapshotReleaser releaser(session);
    McpServerOptions serverOptions;
    serverOptions.allowed = options.allowEdit
                                ? CapabilitySet::of({Capability::Read, Capability::EditSequence, Capability::EditProject})
                                : CapabilitySet::of({Capability::Read});
    if (options.allowAnalysis) {
        serverOptions.allowed.bits |= CapabilitySet::of({Capability::MediaAnalysis}).bits;
    }
    serverOptions.projectPath = std::filesystem::absolute(options.project);
    if (options.allowFiles) {
        serverOptions.fileRoots.push_back(serverOptions.projectPath.parent_path());
        for (const std::filesystem::path& folder : options.filesFolders) {
            serverOptions.fileRoots.push_back(std::filesystem::absolute(folder));
        }
    }
    serverOptions.saveMode = options.saveMode;
    serverOptions.callTimeout = options.timeout;
    serverOptions.serverVersion = std::move(serverVersion);
    serverOptions.log = logLine;
    serverOptions.services = std::move(services);
    const std::size_t maxMessageBytes = serverOptions.maxMessageBytes;
    McpServer server(session, std::move(serverOptions), registry);
    logLine("serving " + pathToUtf8(options.project) + (options.allowEdit ? " (edits allowed)" : " (read-only)") +
            (options.allowAnalysis ? " (analysis allowed)" : "") + (options.allowFiles ? " (files in allowed folders)" : "") +
            ", MCP " + std::string(kProtocolVersion) + " (2025-11-25 / 2025-06-18 via initialize)");
    StdioTransport transport(maxMessageBytes);
    server.serve(transport);
    logLine("client disconnected");
    return 0;
}

} // namespace wabcut::command::mcp
