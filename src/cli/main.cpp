#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/autozoom/MediaAutoZoom.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/highlights/MediaHighlights.h>
#include <wabcut/ai/roughcut/MediaTakeProbe.h>
#include <wabcut/ai/roughcut/RoughCutCommands.h>
#include <wabcut/ai/segment/MediaSegment.h>
#include <wabcut/ai/translate/LocalTranslation.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/command/Cli.h>
#include <wabcut/command/mcp/McpCli.h>
#include <wabcut/editor/Activity.h>
#include <wabcut/editor/ColorFrames.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/render/GraphicRaster.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/media/FfmpegInfo.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include "Analyze.h"
#include "Extensions.h"
#include "LocalLlm.h"
#include "Scopes.h"
#include "Search.h"
#include "Subjects.h"
#include "Transcribe.h"
#include "TranscriptEdit.h"
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
#include <wabcut/plughost/PluginSystem.h>
#endif

#include <QGuiApplication>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <wabcut/platform/DllSearch.h>

namespace {

int printVersion() {
    std::printf("wabcut-cli %s\n", WABCUT_VERSION);
    std::printf("%s\n", wabcut::media::ffmpegVersionString().c_str());
    std::printf("hw device types:");
    for (const auto& t : wabcut::media::compiledHwDeviceTypes()) {
        std::printf(" %s", t.c_str());
    }
    std::printf("\n");
    return 0;
}

int printUsage() {
    std::printf("usage: wabcut-cli --version\n");
    std::printf("       wabcut-cli import <file> [--search <dir>]... [--json] [--carried] [--edl-rate "
                "<num>[/<den>]]\n");
    std::printf("                             [--experimental-prproj]\n");
    std::printf("       wabcut-cli export-timeline <project.wabcut> <out> [--format fcp7-xml|fcpxml|otio|edl] "
                "[--sequence <name|id>]\n");
    std::printf("                             [--fcpxml-version 1.10|1.11] [--json] [--carried]\n");
    std::printf("       wabcut-cli cmd <project.wabcut> (--json '<request or batch>' | --json-file <file>) "
                "[--dry-run] [--save]\n");
    std::printf("       wabcut-cli tools --format anthropic|openai|jsonschema\n");
    std::printf("       wabcut-cli render <project.wabcut> <out> [--preset <id>] "
                "[--range all|in-out|work-area]\n");
    std::printf("                             [--sequence <name|id>] [--encoder <name>] [--test-pattern] "
                "[--no-audio]\n");
    std::printf("       wabcut-cli scopes <project.wabcut> --at <timecode|ticks> [--sequence <name|id>] [--clip <id>] "
                "[--out <png>] [--json <file>] [--width <px>] [--zoom 1|2|5]\n");
    std::printf("       wabcut-cli mcp --project <file.wabcut> [--allow-edit] [--save-on-exit | --autosave] "
                "[--timeout-ms <n>]\n");
    std::printf("       wabcut-cli analyze <media> [--shots] [--beats] [--json] [--detector <name>] "
                "[--cache <dir> | --no-cache]\n");
    std::printf("       wabcut-cli subjects <media> [--device auto|gpu|cpu] [--threads <n>] [--no-cache] [--json] "
                "[--face-model <onnx> --object-model <onnx>]\n");
    std::printf("       wabcut-cli transcript-edit <project.wabcut> [--remove-fillers] [--remove-silences <s>] "
                "[--review] [--dry-run] [--save]\n");
    std::printf("       wabcut-cli captions <project.wabcut> [--sequence <id>] [--srt <out.srt>] [--vtt <out.vtt>] "
                "[--save]\n");
    std::printf("       wabcut-cli transcribe <media> [--provider auto|local|cloud:<model>] [--allow-cloud] "
                "[--language th|en|auto] [--json|--srt|--vtt]\n");
    std::printf("       wabcut-cli ext run <extension> <command> <project.wabcut> [--grant <capabilities> | "
                "--grant-requested] [--trust]\n");
    std::printf("                             [--args '<json>'] [--answers '<json array>'] [--save] [--timeout <s>]\n");
    std::printf("       wabcut-cli ext validate <folder> | ext list [<project.wabcut>]   (docs/extensions.md)\n");
    std::printf("\n");
    std::printf("import: reads a Final Cut Pro 7 XML, FCPXML (.fcpxml/.fcpxmld), OpenTimelineIO (.otio) or "
                "CMX3600 EDL\n");
    std::printf("        and prints the imported timeline and the import report (carried / approximated / "
                "dropped).\n");
    std::printf(
        "  --search <dir>           also look for missing media by file name under <dir> (repeatable)\n");
    std::printf("  --json                   print the result as JSON\n");
    std::printf("  --carried                also list every carried report entry (text output)\n");
    std::printf("  --edl-rate <n>[/<d>]     frame rate of an EDL (EDLs do not say), e.g. 25 or 24000/1001\n");
    std::printf("  --experimental-prproj    read Premiere .prproj files with the experimental reader\n");
    std::printf("export-timeline: writes a sequence (and the sequences it nests) as Final Cut Pro 7 XML, FCPXML,\n");
    std::printf("        OpenTimelineIO or a CMX3600 EDL for Premiere Pro, Resolve or Final Cut Pro, and prints the export\n");
    std::printf("        report (carried / approximated / dropped; docs/interchange.md).\n");
    std::printf("  --format <f>             fcp7-xml, fcpxml, otio or edl (default: from the extension of <out>)\n");
    std::printf("  --sequence <name|id>     the sequence to export (default: the first one)\n");
    std::printf("  --fcpxml-version <v>     1.10 (default) or 1.11\n");
    std::printf("cmd:    runs an edit command or batch (docs/commands.md) on a project and prints the result as "
                "JSON.\n");
    std::printf("  --dry-run                report what would change without changing anything\n");
    std::printf("  --save                   save the project back (with rotated .bak copies) after a change\n");
    std::printf("tools:  prints the command catalog as LLM tool definitions.\n");
    std::printf("render: exports a sequence of a project to <out> (progress on stderr; Ctrl+C cancels "
                "and leaves no file).\n");
    std::printf("  --preset <id>            match-source (default), youtube-1080p, youtube-4k, "
                "vertical-1080x1920,\n");
    std::printf("                           prores-422-hq, audio-wav\n");
    std::printf("  --range <r>              all (default), in-out (sequence marks), work-area\n");
    std::printf("  --sequence <name|id>     the sequence to export (default: the first one)\n");
    std::printf("  --encoder <name>         an FFmpeg video encoder, used without fallback (e.g. libx264, "
                "h264_nvenc)\n");
    std::printf("  --test-pattern           encode the synthetic 3-layer test pattern instead of the "
                "rendered timeline\n");
    std::printf("  --no-audio               video only\n");
    std::printf("  The container follows the extension of <out> (.mp4 .mov .mkv .wav); without one, the "
                "preset's.\n");
    std::printf("mcp:    serves the project to MCP clients on stdin/stdout, read-only unless --allow-edit "
                "(docs/mcp.md).\n");
    return 1;
}

std::optional<wabcut::Rate> parseRate(std::string_view text) {
    const auto slash = text.find('/');
    const std::string_view numText = text.substr(0, slash);
    const std::string_view denText =
        slash == std::string_view::npos ? std::string_view("1") : text.substr(slash + 1);
    std::int32_t num = 0;
    std::int32_t den = 0;
    const auto a = std::from_chars(numText.data(), numText.data() + numText.size(), num);
    const auto b = std::from_chars(denText.data(), denText.data() + denText.size(), den);
    if (a.ec != std::errc{} || b.ec != std::errc{} || a.ptr != numText.data() + numText.size() ||
        b.ptr != denText.data() + denText.size() || num <= 0 || den <= 0) {
        return std::nullopt;
    }
    return wabcut::Rate{num, den};
}

int runImport(int argc, char** argv) {
    namespace ic = wabcut::interchange;
    std::filesystem::path file;
    ic::ImportOptions options;
    ic::SummaryOptions summary;
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--search" && i + 1 < argc) {
            options.mediaSearchRoots.emplace_back(argv[++i]);
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--carried") {
            summary.listCarried = true;
        } else if (arg == "--experimental-prproj") {
            options.experimentalPremiereProject = true;
        } else if (arg == "--edl-rate" && i + 1 < argc) {
            const auto rate = parseRate(argv[++i]);
            if (!rate) {
                std::fprintf(stderr, "invalid --edl-rate '%s'\n", argv[i]);
                return 2;
            }
            options.edlFrameRate = rate;
        } else if (!arg.starts_with("--") && file.empty()) {
            file = std::filesystem::path(arg);
        } else {
            return printUsage();
        }
    }
    if (file.empty()) {
        return printUsage();
    }
    auto result = ic::ImportRouter::importFile(file, options);
    if (!result) {
        const ic::Detection detection = ic::ImportRouter::detect(file);
        std::fprintf(stderr, "import failed: %s\n", result.error().message.c_str());
        if (!detection.guidance.empty()) {
            std::fprintf(stderr, "%s\n", detection.guidance.c_str());
        }
        return 2;
    }
    const std::string out = json ? ic::toJson(*result) : ic::formatSummary(*result, summary);
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

// wabcut-cli export-timeline <project.wabcut> <out> [--format fcp7-xml|fcpxml|otio|edl] [--sequence <name|id>]
//                            [--fcpxml-version 1.10|1.11] [--json] [--carried]
int runExportTimeline(int argc, char** argv) {
    namespace ic = wabcut::interchange;
    if (argc < 4) {
        return printUsage();
    }
    const std::filesystem::path projectFile = wabcut::pathFromUtf8(argv[2]);
    const std::filesystem::path output = wabcut::pathFromUtf8(argv[3]);
    std::optional<ic::Format> format;
    std::string sequenceArg;
    ic::ExportOptions options;
    ic::SummaryOptions summary;
    bool json = false;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--format" && i + 1 < argc) {
            format = ic::exportFormatFromName(argv[++i]);
            if (!format) {
                std::fprintf(stderr, "unknown format '%s' (fcp7-xml, fcpxml, otio or edl)\n", argv[i]);
                return 2;
            }
        } else if (arg == "--sequence" && i + 1 < argc) {
            sequenceArg = argv[++i];
        } else if (arg == "--fcpxml-version" && i + 1 < argc) {
            options.fcpxmlVersion = argv[++i];
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--carried") {
            summary.listCarried = true;
        } else {
            return printUsage();
        }
    }
    if (!format) {
        format = ic::exportFormatFromName(wabcut::pathToUtf8(output.extension()).substr(output.extension().empty() ? 0 : 1));
        if (!format) {
            std::fprintf(stderr, "cannot tell the format from '%s': pass --format fcp7-xml, fcpxml, otio or edl\n",
                         argv[3]);
            return 2;
        }
    }
    auto project = wabcut::io::loadProject(projectFile);
    if (!project) {
        std::fprintf(stderr, "cannot load %s: %s\n", argv[2], project.error().message.c_str());
        return 2;
    }
    if (project->sequences.empty()) {
        std::fprintf(stderr, "the project has no sequence\n");
        return 2;
    }
    wabcut::SequencePtr sequence = project->sequences.front();
    if (!sequenceArg.empty()) {
        sequence = nullptr;
        for (const wabcut::SequencePtr& s : project->sequences) {
            if (s->name == sequenceArg || std::to_string(s->id.value) == sequenceArg) {
                sequence = s;
                break;
            }
        }
        if (!sequence) {
            std::fprintf(stderr, "no sequence named '%s'\n", sequenceArg.c_str());
            return 2;
        }
    }
    options.projectFile = projectFile;
    auto result = ic::exportSequenceToFile(*project, sequence->id, *format, output, options);
    if (!result) {
        std::fprintf(stderr, "export failed: %s\n", result.error().message.c_str());
        return 2;
    }
    const std::string out = json ? ic::toJson(*result) : ic::formatSummary(*result, summary);
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

int finish(const wabcut::command::CliOutcome& outcome) {
    std::fwrite(outcome.out.data(), 1, outcome.out.size(), stdout);
    std::fwrite(outcome.err.data(), 1, outcome.err.size(), stderr);
    return outcome.exitCode;
}

/// AI-11: take audio for the rough cut and takes commands, and the recordings' frames and cursor sidecars for auto zoom.
/// `localCaller`: a person at this computer (cmd) may name script and sidecar files; an MCP client may not.
std::vector<std::shared_ptr<wabcut::command::CommandServices>> creatorServicesFor(const std::filesystem::path& project, bool localCaller) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(project, ec);
    wabcut::ai::AudioEnergyServiceConfig energy;
    energy.cacheRoot = wabcut::ai::AnalysisCache::defaultRoot();
    auto probe = std::make_shared<wabcut::ai::roughcut::ServiceTakeProbe>(std::make_shared<wabcut::ai::AudioEnergyService>(energy), absolute);
    // No cloud model here: a headless request cannot answer a consent prompt, so outlines are matched locally.
    return {std::make_shared<wabcut::ai::roughcut::RoughCutServices>(wabcut::ai::roughcut::RoughCutServices::ChatOpener{}, probe,
                                                                     wabcut::ai::roughcut::RoughCutServices::TextEmbedder{}, localCaller),
            wabcut::ai::autozoom::mediaAutoZoomServices(absolute, localCaller)};
}

/// A.6: the audio plugins (the catalog from its cache, the helper, plugin effects in the engine and the model), for the
/// commands and for renders of projects with plugin effects. Nothing runs until a plugin effect plays.
void startAudioPlugins() {
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    wabcut::plughost::PluginSystem::start();
#endif
}

/// plugins.list / plugins.scan (A.6), when this build hosts plugins.
std::shared_ptr<wabcut::command::CommandServices> pluginServices() {
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    if (auto* plugins = wabcut::plughost::PluginSystem::current()) {
        return plugins->services();
    }
#endif
    return nullptr;
}

/// AI-12: captions.translate translates on this computer when the pair's OPUS-MT model is installed (nothing is
/// downloaded headless: the app downloads it). No cloud model here, as for outlines: a headless request cannot answer a
/// consent prompt.
std::shared_ptr<wabcut::command::CommandServices> translateServices() {
    namespace tr = wabcut::ai::translate;
    auto local = [](std::string_view source, std::string_view target) -> std::expected<std::unique_ptr<wabcut::ai::ITranslator>, wabcut::ai::AiError> {
        const tr::LocalPair* pair = tr::findLocalPair(source, target);
        if (pair == nullptr) {
            return std::unexpected(wabcut::ai::AiError(wabcut::ai::AiErrorCode::Unsupported, "no local model translates \"" + std::string(source) +
                                                                                                 "\" into \"" + std::string(target) + "\""));
        }
        const wabcut::ai::ModelStore store(wabcut::ai::ModelStore::defaultRoot());
        if (!tr::isLocalModelInstalled(store, pair->model)) {
            return std::unexpected(wabcut::ai::AiError(wabcut::ai::AiErrorCode::ModelMissing,
                                                       "the local model " + std::string(pair->model) + " is not downloaded (the app downloads it, " +
                                                           std::to_string(tr::localModelDownloadBytes(store, pair->model) / 1000000) + " MB)"));
        }
        tr::LocalTranslatorOptions options;
        options.modelDirectory = tr::localModelDirectory(store, pair->model);
        options.targetToken = std::string(pair->targetToken);
        return std::make_unique<tr::LocalTranslator>(std::move(options));
    };
    return std::make_shared<tr::TranslateServices>(local, tr::TranslateServices::ChatOpener{});
}

/// AI-14: mask.objectSelect / refine / propagate run SAM 2.1 on this computer when it is installed (nothing is downloaded
/// headless: the app downloads it); the mattes go to the analysis cache, where renders find them (installMatteSource).
std::shared_ptr<wabcut::command::CommandServices> objectMaskServicesFor(const std::filesystem::path& project) {
    std::error_code ec;
    wabcut::ai::segment::MediaSegmentOptions options;
    options.projectFile = std::filesystem::absolute(project, ec);
    return wabcut::ai::segment::mediaSegmentServices(std::move(options));
}

/// G.2: highlights.find measures recordings on this computer and keeps the measurements in the analysis cache.
std::shared_ptr<wabcut::command::CommandServices> highlightServicesFor(const std::filesystem::path& project) {
    std::error_code ec;
    wabcut::ai::highlights::MediaHighlightOptions options;
    options.cacheRoot = wabcut::ai::AnalysisCache::defaultRoot();
    return std::make_shared<wabcut::ai::highlights::HighlightServices>(
        std::make_shared<wabcut::ai::highlights::MediaHighlightProvider>(options, std::filesystem::absolute(project, ec)));
}

/// C.13: the user's graphic template library (template.save "library": "user", template.list, template.instantiate) is the
/// folder WABCUT_TEMPLATE_LIBRARY names; without it only the project's library exists.
std::shared_ptr<wabcut::command::CommandServices> graphicsServices() {
    const QString folder = qEnvironmentVariable("WABCUT_TEMPLATE_LIBRARY");
    // The renderer's text layout measures pins (graphic.resolve, templates), so what they hug is what renders draw (I.9).
    const std::shared_ptr<const wabcut::TextMeasurer> measurer(&wabcut::render::graphicTextMeasurer(),
                                                               [](const wabcut::TextMeasurer*) {});
    return std::make_shared<wabcut::command::GraphicsServices>(
        folder.isEmpty() ? std::filesystem::path() : wabcut::pathFromUtf8(folder.toStdString()), measurer);
}

/// What the editing catalog's commands need: the transcripts, the media analysis, the audio analysis, the smart
/// search of the project's media, the colour tools' frames and the audio plugins.
std::shared_ptr<wabcut::command::CommandServices> editingServicesFor(const std::filesystem::path& project, bool localCaller) {
    std::vector<std::shared_ptr<wabcut::command::CommandServices>> creator = creatorServicesFor(project, localCaller);
    return std::make_shared<wabcut::command::ServiceBundle>(std::vector<std::shared_ptr<wabcut::command::CommandServices>>{
        transcriptServicesFor(project), transcribeServicesFor(project), analysisServicesFor(project), audioServicesFor(project), searchServicesFor(project),
        reframeServicesFor(project), localLlmServices(), creator[0], creator[1], translateServices(),
        std::make_shared<wabcut::editor::ColorFrameService>(project),   // color.auto/match, scopes.read (I.5)
        std::make_shared<wabcut::editor::MediaTrackingService>(project), // track.mask, track.transform (I.6)
        objectMaskServicesFor(project),                                  // mask.objectSelect / refine / propagate (AI-14)
        highlightServicesFor(project),                                   // highlights.find / toReel / toShort (G.2)
        pluginServices(),                                                // plugins.list, plugins.scan (A.6)
        graphicsServices(),                                              // the user's template library (C.13)
        std::make_shared<wabcut::command::ActivityServices>(             // sequence.removeDeadTime (C.16)
            wabcut::editor::activityMeasure(project, [project](const wabcut::MediaItem& media) {
                // G.2's highlight measurements, when the recording was measured: nothing is decoded again.
                std::error_code ec;
                return wabcut::ai::highlights::cachedActivity(media, wabcut::ai::AnalysisCache::defaultRoot(),
                                                              std::filesystem::absolute(project, ec));
            }))});
}

/// The colour commands render frames on the GPU (QRhi), which needs a QGuiApplication on this (the main) thread.
/// No window is ever created; without a display (Linux CI) the offscreen platform plugin is used.
std::unique_ptr<QGuiApplication> guiFor(char* argv0) {
    static int qtArgc = 1;
    static char* qtArgv[] = {argv0, nullptr};
    if (QGuiApplication::instance() != nullptr) {
        return nullptr;
    }
#if defined(__linux__)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif
    return std::make_unique<QGuiApplication>(qtArgc, qtArgv);
}

// wabcut-cli cmd <project.wabcut> --json '<request or batch>' [--dry-run] [--save]
int runCmd(int argc, char** argv) {
    if (argc < 3) {
        return printUsage();
    }
    std::optional<std::string> json;
    bool dryRun = false;
    bool save = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--json" && i + 1 < argc) {
            json = argv[++i];
        } else if (arg == "--json-file" && i + 1 < argc) {
            std::ifstream file(argv[++i], std::ios::binary);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            std::stringstream text;
            text << file.rdbuf();
            json = text.str();
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--save") {
            save = true;
        } else {
            return printUsage();
        }
    }
    if (!json) {
        return printUsage();
    }
    const auto gui = guiFor(argv[0]);
    return finish(wabcut::command::runCommandCli(argv[2], *json, dryRun, save, wabcut::ai::edit::editingCommands(),
                                                 editingServicesFor(argv[2], true)));
}

// wabcut-cli mcp --project <file.wabcut> [--allow-edit] [--save-on-exit | --autosave] [--timeout-ms <n>]
// stdout belongs to the protocol: usage problems go to stderr.
int runMcp(int argc, char** argv) {
    namespace mcp = wabcut::command::mcp;
    std::vector<std::string_view> args(argv + 2, argv + argc);
    const auto options = mcp::parseMcpArguments(args);
    if (!options) {
        std::fprintf(stderr, "wabcut-cli mcp: %s\n%.*s", options.error().c_str(),
                     static_cast<int>(mcp::mcpUsage().size()), mcp::mcpUsage().data());
        return 2;
    }
    const auto gui = guiFor(argv[0]);
    return mcp::runMcpStdio(*options, WABCUT_VERSION, wabcut::ai::edit::editingCommands(),
                            editingServicesFor(options->project, false));
}

// wabcut-cli tools --format anthropic|openai|jsonschema
int runTools(int argc, char** argv) {
    if (argc != 4 || std::string_view(argv[2]) != "--format") {
        return printUsage();
    }
    return finish(wabcut::command::runToolsCli(argv[3], wabcut::ai::edit::editingCommands()));
}

// ---- render ------------------------------------------------------------------------------------------

std::atomic<bool> g_interrupted{false};

extern "C" void onInterrupt(int) {
    g_interrupted.store(true);
}

std::optional<wabcut::exporter::Container> containerFromExtension(const std::filesystem::path& file) {
    using wabcut::exporter::Container;
    std::string ext = wabcut::pathToUtf8(file.extension());
    for (char& c : ext) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (ext == ".mp4") {
        return Container::Mp4;
    }
    if (ext == ".mov") {
        return Container::Mov;
    }
    if (ext == ".mkv") {
        return Container::Mkv;
    }
    if (ext == ".wav") {
        return Container::Wav;
    }
    return std::nullopt;
}

// wabcut-cli render <project.wabcut> <out> [--preset <id>] [--range all|in-out|work-area]
//                   [--sequence <name|id>] [--encoder <name>] [--test-pattern] [--no-audio]
int runRender(int argc, char** argv) {
    namespace ex = wabcut::exporter;
    if (argc < 4) {
        return printUsage();
    }
    const std::filesystem::path projectFile = wabcut::pathFromUtf8(argv[2]);
    std::filesystem::path output = wabcut::pathFromUtf8(argv[3]);
    std::string presetId = "match-source";
    std::string range = "all";
    std::string sequenceArg;
    std::string encoder;
    bool testPattern = false;
    bool noAudio = false;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--preset" && i + 1 < argc) {
            presetId = argv[++i];
        } else if (arg == "--range" && i + 1 < argc) {
            range = argv[++i];
        } else if (arg == "--sequence" && i + 1 < argc) {
            sequenceArg = argv[++i];
        } else if (arg == "--encoder" && i + 1 < argc) {
            encoder = argv[++i];
        } else if (arg == "--test-pattern") {
            testPattern = true;
        } else if (arg == "--no-audio") {
            noAudio = true;
        } else {
            return printUsage();
        }
    }

    const auto preset = ex::findPreset(presetId);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'; presets:", presetId.c_str());
        for (const ex::Preset& p : ex::builtInPresets()) {
            std::fprintf(stderr, " %s", p.id.c_str());
        }
        std::fprintf(stderr, "\n");
        return 2;
    }
    ex::ExportSettings settings = preset->settings;
    if (range == "all") {
        settings.range = ex::RangeMode::WholeSequence;
    } else if (range == "in-out") {
        settings.range = ex::RangeMode::InOut;
    } else if (range == "work-area") {
        settings.range = ex::RangeMode::WorkArea;
    } else {
        std::fprintf(stderr, "invalid --range '%s' (all, in-out or work-area)\n", range.c_str());
        return 2;
    }
    if (!encoder.empty()) {
        settings.video.encoder = encoder;
    }
    if (noAudio) {
        settings.audio.enabled = false;
    }
    if (output.extension().empty()) {
        output += std::string(ex::fileExtension(settings.container));
    } else if (const auto container = containerFromExtension(output)) {
        settings.container = *container;
    } else {
        std::fprintf(stderr, "unknown output extension '%s' (.mp4, .mov, .mkv or .wav)\n",
                     wabcut::pathToUtf8(output.extension()).c_str());
        return 2;
    }

    auto project = wabcut::io::loadProject(projectFile);
    if (!project) {
        std::fprintf(stderr, "cannot load %s: %s\n", argv[2], project.error().message.c_str());
        return 2;
    }
    if (project->sequences.empty()) {
        std::fprintf(stderr, "the project has no sequence\n");
        return 2;
    }
    wabcut::SequencePtr sequence = project->sequences.front();
    if (!sequenceArg.empty()) {
        sequence = nullptr;
        for (const wabcut::SequencePtr& s : project->sequences) {
            if (s->name == sequenceArg || std::to_string(s->id.value) == sequenceArg) {
                sequence = s;
                break;
            }
        }
        if (!sequence) {
            std::fprintf(stderr, "no sequence named '%s'\n", sequenceArg.c_str());
            return 2;
        }
    }

    ex::ExportRequest request;
    request.sequence = sequence->id;
    request.snapshot = std::make_shared<const wabcut::ProjectState>(std::move(*project));
    request.settings = settings;
    request.output = output;
    request.projectFile = projectFile;
    if (testPattern) {
        request.videoSource = ex::syntheticVideoSourceFactory();
    }
    std::stop_source cancel;
    request.onProgress = [&cancel](const ex::ExportProgress& p) {
        if (g_interrupted.load()) {
            cancel.request_stop();
        }
        if (p.phase == ex::ExportPhase::Rendering) {
            std::fprintf(stderr, "\r%s %lld/%lld  %.1f fps  ETA %.1f s   ",
                         p.videoEncoder.empty() ? p.audioEncoder.c_str() : p.videoEncoder.c_str(),
                         static_cast<long long>(p.framesDone), static_cast<long long>(p.framesTotal), p.fps,
                         p.etaSeconds.value_or(0.0));
        }
    };
    std::signal(SIGINT, onInterrupt);

    // The rendered video source composites on the GPU (QRhi), which needs a QGuiApplication. No window is
    // ever created; without a display (Linux CI) the offscreen platform plugin is used.
    static int qtArgc = 1;
    static char* qtArgv[] = {argv[0], nullptr};
    std::unique_ptr<QGuiApplication> qtApp;
    if (settings.video.enabled && !testPattern && QGuiApplication::instance() == nullptr) {
#if defined(__linux__)
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") &&
            qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
#endif
        qtApp = std::make_unique<QGuiApplication>(qtArgc, qtArgv);
    }

    wabcut::ai::Json generatedProvenance;
    if (const auto* seq = request.snapshot->findSequence(request.sequence)) {
        const auto resolved = ex::resolve(request.settings, *seq);
        if (resolved) generatedProvenance = wabcut::ai::provenanceReport(*request.snapshot, request.sequence, resolved->range);
    }
    ex::ExportJob job(std::move(request));
    const auto result = job.run(cancel.get_token());
    std::fprintf(stderr, "\n");
    if (!result) {
        const bool cancelled = result.error().code == wabcut::ErrorCode::Cancelled;
        std::fprintf(stderr, "render %s: %s\n", cancelled ? "cancelled" : "failed",
                     result.error().message.c_str());
        return cancelled ? 130 : 2;
    }
    if (!generatedProvenance.is_null()) {
        auto sidecar = result->output; sidecar += ".provenance.json";
        if (auto wrote = wabcut::io::writeFileAtomically(sidecar, generatedProvenance.dump(2)); !wrote)
            std::fprintf(stderr, "warning: export succeeded, unsigned provenance sidecar failed\n");
    }
    std::printf("wrote %s\n", wabcut::pathToUtf8(result->output).c_str());
    if (!result->videoEncoder.empty()) {
        std::printf("video: %s, %lld frames, source: %s\n", result->videoEncoder.c_str(),
                    static_cast<long long>(result->frames), result->videoSource.c_str());
        for (const std::string& f : result->encoderFallbacks) {
            std::printf("  skipped %s\n", f.c_str());
        }
    }
    if (!result->audioEncoder.empty()) {
        std::printf("audio: %s, %lld samples\n", result->audioEncoder.c_str(),
                    static_cast<long long>(result->audioSamples));
    }
    std::printf("duration %.3f s, took %.2f s (%.2fx realtime)\n", result->duration.toSeconds(),
                result->seconds, result->realtimeFactor);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    if (argc >= 2 && std::string_view(argv[1]) == "--version") {
        return printVersion();
    }
    startAudioPlugins();
    // AI-14: matte masks (AI object masks, AI-tracked redactions) render from the analysis cache's mattes; without them
    // the masks' bounding boxes stand in.
    wabcut::ai::segment::installMatteSource();
    if (argc >= 2 && std::string_view(argv[1]) == "import") {
        return runImport(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "cmd") {
        return runCmd(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "export-timeline") {
        return runExportTimeline(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "tools") {
        return runTools(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "render") {
        return runRender(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "mcp") {
        return runMcp(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "ext") {
        const auto gui = guiFor(argv[0]); // extensions run the colour commands too
        return runExtensions(argc, argv, WABCUT_VERSION,
                             // An extension is no local caller: like an MCP client it may not name script or sidecar files.
                             [](const std::filesystem::path& project) { return editingServicesFor(project, false); });
    }
    if (argc >= 2 && std::string_view(argv[1]) == "transcript-edit") {
        return runTranscriptEdit(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "captions") {
        return runCaptions(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "subjects") {
        return runSubjects(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "analyze") {
        return runAnalyze(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "scopes") {
        return runScopes(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "transcribe") {
        return runTranscribe(argc, argv);
    }
    return printUsage();
}
