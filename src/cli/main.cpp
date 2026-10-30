#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/autozoom/MediaAutoZoom.h>
#include <wabcut/ai/c2pa/ContentCredentials.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/vision/ColorFaces.h>
#include <wabcut/ai/highlights/MediaHighlights.h>
#include <wabcut/ai/roughcut/MediaTakeProbe.h>
#include <wabcut/ai/roughcut/RoughCutCommands.h>
#include <wabcut/ai/segment/MediaSegment.h>
#include <wabcut/ai/translate/LocalTranslation.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/command/Cli.h>
#include <wabcut/command/EditingServices.h>
#include <wabcut/command/ImageSequenceServices.h>
#include <wabcut/command/SpellingServices.h>
#include <wabcut/editor/MediaFormats.h>
#include <wabcut/command/mcp/McpCli.h>
#include <wabcut/editor/Activity.h>
#include <wabcut/editor/ColorFrames.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/SystemSpellChecker.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/render/GraphicRaster.h>
#include <wabcut/export/AudioPostHost.h>
#include <wabcut/export/ExportCommandServices.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ReviewFrames.h>
#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/export/WatchFolders.h>
#include <wabcut/export/StillExport.h>
#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/media/FfmpegInfo.h>
#include <wabcut/media/GpuInfo.h>
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
#include <QDir>
#include <QStandardPaths>

#include <algorithm>
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
#include <thread>
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
    std::printf("       wabcut-cli export-timeline <project.wabcut> <out> [--format fcp7-xml|fcpxml|otio|edl|aaf|omf] "
                "[--sequence <name|id>]\n");
    std::printf("                             [--fcpxml-version 1.10|1.11] [--json] [--carried] [AAF / OMF options]\n");
    std::printf("       wabcut-cli cmd <project.wabcut> (--json '<request or batch>' | --json-file <file>) "
                "[--dry-run] [--save]\n");
    std::printf("       wabcut-cli tools --format anthropic|openai|jsonschema\n");
    std::printf("       wabcut-cli render <project.wabcut> <out> [--preset <id>] "
                "[--range all|in-out|work-area]\n");
    std::printf("                             [--sequence <name|id>] [--encoder <name>] [--test-pattern] "
                "[--no-audio] [--dynamic-range auto|sdr|pq|hlg]\n");
    std::printf("                             [--content-credentials[=off|ai|always]] [--c2pa-cert <chain.pem> "
                "--c2pa-key <key.pem>]\n");
    std::printf("                             [--c2pa-producer <name>] [--c2pa-no-thumbnail] [--c2pa-do-not-train]\n");
    std::printf("                             [--preset-file <preset.json>] [--set '<overrides json>']\n");
    std::printf("       wabcut-cli watch <folder> --preset <id> [--output <folder>] [--keep-sources] [--once] "
                "[--test-pattern]\n");
    std::printf("       wabcut-cli credentials verify <file.mp4|.mov> [--trust <root.pem>] [--json]\n");
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
    std::printf("  AAF / OMF (audio post; the audio is rendered to new mono files, as Premiere's Export AAF / OMF):\n");
    std::printf("  --sample-rate 48000|96000, --bits 16|24          the new audio files (default 48000, 24)\n");
    std::printf("  --separate-audio         audio files in \"<out name> Audio\" next to the file (default: embedded)\n");
    std::printf("  --audio-format wav|aiff  default wav (AAF: Broadcast WAV)\n");
    std::printf("  --copy-complete          whole media files (default: trim to the clips, --handles <frames>, 24)\n");
    std::printf("  --no-breakout            AAF: link the original media instead of rendering mono files\n");
    std::printf("  --render-effects         render the clips' audio effects (--clean-copies: also No FX tracks)\n");
    std::printf("  --mixdown-video          AAF: render the video to \"<out name> Reference.mov\"\n");
    std::printf("  --omf-title <t>          OMF: the composition's name; --no-pan: OMF without pan\n");
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
    std::printf("  --dynamic-range <r>      auto (default: the sequence's colour setup), sdr, pq or hlg (HDR: an HDR or "
                "Wide Gamut colour setup, 10-bit HEVC, AV1 or ProRes)\n");
    std::printf("  --preset <id>            also a user preset (\"user-...\", the app's Save Preset; folder: "
                "WABCUT_EXPORT_PRESETS)\n");
    std::printf("  --preset-file <file>     a preset file (.json, Import Preset's format)\n");
    std::printf("  --set '<json>'           overrides on top of the preset (docs/export.md: effects, metadata, "
                "subtitles, render, ...)\n");
    std::printf("  The container follows the extension of <out> (.mp4 .mov .mkv .wav .mp3 .m4a .aif); without one, the "
                "preset's. Image presets write numbered stills.\n");
    std::printf("watch:  Media Encoder's watch folder: exports every media file dropped into <folder> with the preset "
                "into <folder>/Output\n");
    std::printf("        (and moves it into <folder>/Source); --once takes what is there now and exits.\n");
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

void startAudioPlugins(); // below (A.6)

// wabcut-cli export-timeline <project.wabcut> <out> [--format fcp7-xml|fcpxml|otio|edl|aaf|omf] [--sequence <name|id>]
//                            [--fcpxml-version 1.10|1.11] [--json] [--carried] [AAF / OMF options (P.2)]
int runExportTimeline(int argc, char** argv) {
    namespace ic = wabcut::interchange;
    if (argc < 4) {
        return printUsage();
    }
    const std::filesystem::path projectFile = wabcut::pathFromUtf8(argv[2]);
    const std::filesystem::path output = wabcut::pathFromUtf8(argv[3]);
    std::optional<ic::Format> format;
    std::string audioPostFormat; // "aaf" / "omf"
    std::string sequenceArg;
    ic::ExportOptions options;
    ic::SummaryOptions summary;
    ic::AafExportOptions aaf;
    ic::OmfExportOptions omf;
    ic::AudioPostAudio audio;
    bool json = false;
    auto number = [](const char* text, int& out) {
        const std::string_view s = text;
        return std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{};
    };
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--format" && i + 1 < argc) {
            const std::string_view name = argv[++i];
            if (name == "aaf" || name == "omf") {
                audioPostFormat = std::string(name);
                continue;
            }
            format = ic::exportFormatFromName(name);
            if (!format) {
                std::fprintf(stderr, "unknown format '%s' (fcp7-xml, fcpxml, otio, edl, aaf or omf)\n", argv[i]);
                return 2;
            }
        } else if (arg == "--sample-rate" && i + 1 < argc) {
            if (!number(argv[++i], audio.sampleRate)) {
                return printUsage();
            }
        } else if (arg == "--bits" && i + 1 < argc) {
            if (!number(argv[++i], audio.bitsPerSample)) {
                return printUsage();
            }
        } else if (arg == "--handles" && i + 1 < argc) {
            if (!number(argv[++i], audio.handleFrames)) {
                return printUsage();
            }
        } else if (arg == "--separate-audio") {
            audio.files = ic::AudioPostFiles::Separate;
        } else if (arg == "--audio-format" && i + 1 < argc) {
            const auto f = ic::audioPostFormatFromString(argv[++i]);
            if (!f) {
                std::fprintf(stderr, "unknown audio format '%s' (wav or aiff)\n", argv[i]);
                return 2;
            }
            audio.format = *f;
        } else if (arg == "--copy-complete") {
            audio.render = ic::AudioPostRender::CopyCompleteAudioFiles;
        } else if (arg == "--no-breakout") {
            aaf.breakoutToMono = false;
        } else if (arg == "--render-effects") {
            aaf.renderAudioClipEffects = true;
        } else if (arg == "--clean-copies") {
            aaf.includeClipCopiesWithoutEffects = true;
        } else if (arg == "--mixdown-video") {
            aaf.mixdownVideo = true;
        } else if (arg == "--omf-title" && i + 1 < argc) {
            omf.title = argv[++i];
        } else if (arg == "--no-pan") {
            omf.includePan = false;
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
    if (!format && audioPostFormat.empty()) {
        std::string extension = wabcut::pathToUtf8(output.extension()).substr(output.extension().empty() ? 0 : 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
        if (extension == "aaf" || extension == "omf") {
            audioPostFormat = extension;
        } else {
            format = ic::exportFormatFromName(extension);
        }
        if (!format && audioPostFormat.empty()) {
            std::fprintf(stderr,
                         "cannot tell the format from '%s': pass --format fcp7-xml, fcpxml, otio, edl, aaf or omf\n",
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
    if (!audioPostFormat.empty()) {
        // P.2: the clips' audio through the same playback path renders use (and Mixdown Video through an export).
        startAudioPlugins();
        wabcut::exporter::ClipAudioRenderer renderer(projectFile);
        wabcut::exporter::ReferenceVideoRenderer referenceVideo(projectFile);
        ic::AudioPostHost host;
        host.audio = &renderer;
        host.video = &referenceVideo;
        int lastPercent = -1;
        host.progress = [&](double p) {
            const int percent = static_cast<int>(p * 100.0);
            if (percent != lastPercent) {
                lastPercent = percent;
                std::fprintf(stderr, "\r%3d%%", percent);
            }
            return true;
        };
        std::expected<ic::AudioPostResult, wabcut::Error> exported;
        if (audioPostFormat == "aaf") {
            aaf.audio = audio;
            aaf.projectFile = projectFile;
            exported = ic::exportAafToFile(*project, sequence->id, output, aaf, host);
        } else {
            omf.audio = audio;
            omf.projectFile = projectFile;
            exported = ic::exportOmfToFile(*project, sequence->id, output, omf, host);
        }
        std::fprintf(stderr, "\n");
        if (!exported) {
            std::fprintf(stderr, "export failed: %s\n", exported.error().message.c_str());
            return 2;
        }
        const std::string label = audioPostFormat == "aaf" ? "AAF" : "OMF";
        const std::string out = json ? ic::toJson(*exported, label) : ic::formatSummary(*exported, label, summary);
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
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

/// 0.2 graphics: spell check through the system's checker (Windows' spell checking API) and the user's dictionary: the
/// WABCUT_SPELLING_DICTIONARY file, else the app's (spelling-dictionary.txt in its data folder), so words added in the
/// app count here too.
std::shared_ptr<wabcut::command::CommandServices> spellingServices() {
    QString file = qEnvironmentVariable("WABCUT_SPELLING_DICTIONARY");
    if (file.isEmpty()) {
        QCoreApplication::setOrganizationName(QStringLiteral("Wabcut"));
        QCoreApplication::setApplicationName(QStringLiteral("Wabcut"));
        const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!data.isEmpty()) {
            file = QDir(data).filePath(QStringLiteral("spelling-dictionary.txt"));
        }
    }
    return std::make_shared<wabcut::command::SpellingServices>(std::make_shared<wabcut::PersonalSpelling>(
        wabcut::editor::systemSpellChecker(), file.isEmpty() ? std::filesystem::path() : std::filesystem::path(file.toStdWString())));
}

/// 0.2: the user's export presets (the app's Save Preset): WABCUT_EXPORT_PRESETS, else "Export Presets" in the app's
/// config folder.
std::filesystem::path userPresetFolder() {
    const QString env = qEnvironmentVariable("WABCUT_EXPORT_PRESETS");
    if (!env.isEmpty()) {
        return std::filesystem::path(env.toStdWString());
    }
    QCoreApplication::setOrganizationName(QStringLiteral("Wabcut"));
    QCoreApplication::setApplicationName(QStringLiteral("Wabcut"));
    const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return config.isEmpty() ? std::filesystem::path() : std::filesystem::path(config.toStdWString()) / "Export Presets";
}

/// 0.2: the export commands' services of this process (cmd and mcp): one queue; cmd waits for its jobs before exiting.
std::shared_ptr<wabcut::exporter::ExportCommandServices> cliExportServices(const std::filesystem::path& project) {
    static std::shared_ptr<wabcut::exporter::ExportCommandServices> services;
    if (!services) {
        std::error_code ec;
        services = wabcut::exporter::ExportCommandServices::standalone(std::filesystem::absolute(project, ec), userPresetFolder());
    }
    return services;
}

/// Color Match's face detector (0.2), when the subject detectors are downloaded; a null service is skipped.
std::shared_ptr<wabcut::command::CommandServices> cliFaceFinder() {
    static std::shared_ptr<wabcut::color::FaceFinder> finder = wabcut::ai::vision::installedFaceFinder();
    return finder;
}

/// What the editing catalog's commands need: the transcripts, the media analysis, the audio analysis, the smart
/// search of the project's media, the colour tools' frames and the audio plugins.
std::shared_ptr<wabcut::command::CommandServices> editingServicesFor(const std::filesystem::path& project, bool localCaller) {
    std::vector<std::shared_ptr<wabcut::command::CommandServices>> creator = creatorServicesFor(project, localCaller);
    return std::make_shared<wabcut::command::ServiceBundle>(std::vector<std::shared_ptr<wabcut::command::CommandServices>>{
        transcriptServicesFor(project), transcribeServicesFor(project), analysisServicesFor(project), audioServicesFor(project), searchServicesFor(project),
        reframeServicesFor(project), localLlmServices(), creator[0], creator[1], translateServices(),
        std::make_shared<wabcut::editor::ColorFrameService>(project),   // color.auto/match, scopes.read (I.5)
        cliFaceFinder(),                                                 // color.match "faceDetection" (0.2)
        std::make_shared<wabcut::editor::MediaTrackingService>(project), // track.mask, track.transform (I.6)
        objectMaskServicesFor(project),                                  // mask.objectSelect / refine / propagate (AI-14)
        highlightServicesFor(project),                                   // highlights.find / toReel / toShort (G.2)
        pluginServices(),                                                // plugins.list, plugins.scan (A.6)
        graphicsServices(),                                              // the user's template library (C.13)
        spellingServices(),                                              // captions / graphic.checkSpelling (0.2)
        std::make_shared<wabcut::command::MediaProbeServices>(           // media.replaceFootage (P.2)
            [project](const std::filesystem::path& media) {
                std::error_code ec;
                return wabcut::editor::describeMediaFile(media, std::filesystem::absolute(project, ec));
            }),
        wabcut::exporter::interchangeServices(project),                  // AAF / OMF audio, caption files (P.2)
        cliExportServices(project),                                      // export.* (0.2): presets, queue, watch folders
        wabcut::exporter::stillExportServices([project](const std::filesystem::path& file) { // Export Frame (P.2)
            return wabcut::editor::describeMediaFile(file, project);
        }),
        std::make_shared<wabcut::command::ImageSequenceServices>(        // media.importImageSequence (0.2)
            [project](const std::filesystem::path& firstFrame, wabcut::Rate rate) {
                std::error_code ec;
                return wabcut::editor::describeImageSequence(firstFrame, rate, std::filesystem::absolute(project, ec));
            }),
        wabcut::editor::mediaFormatServices([&project] { // layered files, the Media Browser (0.2)
            std::error_code ec;
            return project.empty() ? project : std::filesystem::absolute(project, ec);
        }()),
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
    const int code = finish(wabcut::command::runCommandCli(argv[2], *json, dryRun, save, wabcut::ai::edit::editingCommands(),
                                                           editingServicesFor(argv[2], true)));
    // 0.2: exports the request queued (export.quick, export.queue) finish before the process does.
    const auto jobs = cliExportServices(argv[2])->waitForJobs();
    if (!jobs.empty()) {
        std::fprintf(stderr, "exports: %s\n", jobs.dump(-1, ' ', false, wabcut::exporter::Json::error_handler_t::replace).c_str());
    }
    // The command succeeded but an export it queued did not: the run failed (exit 2, like render).
    for (const auto& job : jobs) {
        if (code == 0 && job.value("state", std::string()) != "done") {
            return 2;
        }
    }
    return code;
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
    if (ext == ".mp3") { // 0.2
        return Container::Mp3;
    }
    if (ext == ".m4a") {
        return Container::M4a;
    }
    if (ext == ".aif" || ext == ".aiff") {
        return Container::Aiff;
    }
    return std::nullopt;
}

// wabcut-cli render <project.wabcut> <out> [--preset <id>] [--range all|in-out|work-area]
//                   [--sequence <name|id>] [--encoder <name>] [--test-pattern] [--no-audio]
//                   [--dynamic-range auto|sdr|pq|hlg]
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
    std::string dynamicRange;
    std::string credentials = "ai"; // AI-15: sign exports with AI-generated material by default, like the app
    std::filesystem::path c2paCert;
    std::filesystem::path c2paKey;
    std::string c2paProducer;  // 0.2: Premiere's Content Credentials identity, the CreativeWork author
    bool c2paThumbnail = true; // 0.2: a thumbnail of the export in the manifest
    bool c2paDoNotTrain = false;
    std::filesystem::path presetFile;                                    // 0.2: Import Preset's format
    wabcut::exporter::Json overrides = wabcut::exporter::Json::object(); // 0.2: --set
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--dynamic-range" && i + 1 < argc) {
            dynamicRange = argv[++i];
        } else if (arg == "--content-credentials") {
            credentials = "always";
        } else if (arg.starts_with("--content-credentials=")) {
            credentials = std::string(arg.substr(std::string_view("--content-credentials=").size()));
            if (!wabcut::ai::c2pa::credentialsModeFromString(credentials)) {
                std::fprintf(stderr, "invalid --content-credentials '%s' (off, ai or always)\n", credentials.c_str());
                return 2;
            }
        } else if (arg == "--c2pa-producer" && i + 1 < argc) {
            c2paProducer = argv[++i];
        } else if (arg == "--c2pa-no-thumbnail") {
            c2paThumbnail = false;
        } else if (arg == "--c2pa-do-not-train") {
            c2paDoNotTrain = true;
        } else if (arg == "--c2pa-cert" && i + 1 < argc) {
            c2paCert = wabcut::pathFromUtf8(argv[++i]);
        } else if (arg == "--c2pa-key" && i + 1 < argc) {
            c2paKey = wabcut::pathFromUtf8(argv[++i]);
        } else if (arg == "--preset" && i + 1 < argc) {
            presetId = argv[++i];
        } else if (arg == "--preset-file" && i + 1 < argc) {
            presetFile = wabcut::pathFromUtf8(argv[++i]);
        } else if (arg == "--set" && i + 1 < argc) {
            try {
                overrides = wabcut::exporter::Json::parse(argv[++i]);
            } catch (const std::exception&) {
                std::fprintf(stderr, "--set takes a JSON object\n");
                return 2;
            }
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

    std::optional<ex::Preset> preset;
    if (!presetFile.empty()) {
        std::ifstream in(presetFile, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto read = ex::presetFromJson(text);
        if (!read) {
            std::fprintf(stderr, "cannot use %s: %s\n", wabcut::pathToUtf8(presetFile).c_str(), read.error().message.c_str());
            return 2;
        }
        preset = std::move(*read);
    } else {
        preset = ex::PresetStore(userPresetFolder()).findAny(presetId);
    }
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'; presets:", presetId.c_str());
        for (const ex::Preset& p : ex::builtInPresets()) {
            std::fprintf(stderr, " %s", p.id.c_str());
        }
        std::fprintf(stderr, "\n");
        return 2;
    }
    ex::ExportSettings settings = preset->settings;
    if (auto ok = ex::applyOverrides(settings, overrides); !ok) {
        std::fprintf(stderr, "--set: %s\n", ok.error().message.c_str());
        return 2;
    }
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
    if (!dynamicRange.empty()) { // 0.2 (I.9): SDR, or an HDR master of an HDR / Wide Gamut sequence
        if (dynamicRange == "auto") {
            settings.video.dynamicRange = ex::DynamicRange::Auto;
        } else if (dynamicRange == "sdr") {
            settings.video.dynamicRange = ex::DynamicRange::Sdr;
        } else if (dynamicRange == "pq" || dynamicRange == "hlg") {
            settings.video.dynamicRange = dynamicRange == "pq" ? ex::DynamicRange::Pq : ex::DynamicRange::Hlg;
            if (settings.video.pixelFormat == ex::OutputPixelFormat::Yuv420p &&
                (settings.video.codec == ex::VideoCodec::Hevc || settings.video.codec == ex::VideoCodec::Av1)) {
                settings.video.pixelFormat = ex::OutputPixelFormat::Yuv420p10; // PQ and HLG are 10-bit
            }
        } else {
            std::fprintf(stderr, "invalid --dynamic-range '%s' (auto, sdr, pq or hlg)\n", dynamicRange.c_str());
            return 2;
        }
    }
    if (output.extension().empty()) {
        output += std::string(ex::outputExtension(settings));
    } else if (settings.container == ex::Container::Image) {
        output.replace_extension(std::string(ex::outputExtension(settings))); // the still format decides
    } else if (const auto container = containerFromExtension(output)) {
        settings.container = *container;
        if (ex::isAudioOnly(*container)) {
            settings.video.enabled = false;
            settings.audio.codec = *container == ex::Container::Mp3                ? ex::AudioCodec::Mp3
                                   : *container == ex::Container::M4a              ? ex::AudioCodec::Aac
                                   : settings.audio.codec == ex::AudioCodec::Pcm16 ? ex::AudioCodec::Pcm16
                                                                                   : ex::AudioCodec::Pcm24;
        }
    } else {
        std::fprintf(stderr, "unknown output extension '%s' (.mp4, .mov, .mkv, .wav, .mp3, .m4a or .aif; image presets "
                             "write stills)\n",
                     wabcut::pathToUtf8(output.extension()).c_str());
        return 2;
    }
    if (auto ok = ex::validate(settings); !ok) {
        std::fprintf(stderr, "invalid export settings: %s\n", ok.error().message.c_str());
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
    if (settings.video.enabled && (!testPattern || settings.effects.anyVideo()) && QGuiApplication::instance() == nullptr) {
#if defined(__linux__)
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") &&
            qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
#endif
        qtApp = std::make_unique<QGuiApplication>(qtArgc, qtArgv);
    }

    wabcut::ai::Json generatedProvenance;
    std::optional<wabcut::TimeRange> exported;
    if (const auto* seq = request.snapshot->findSequence(request.sequence)) {
        const auto resolved = ex::resolve(request.settings, *seq, request.snapshot->colorManagement);
        if (resolved) {
            generatedProvenance = wabcut::ai::provenanceReport(*request.snapshot, request.sequence, resolved->range);
            exported = resolved->range;
        }
    }
    // AI-15: signed C2PA Content Credentials in MP4 / MOV (docs/ai.md "Content Credentials"): with --c2pa-cert / --c2pa-key
    // the user's certificate, else the per-user local identity (its key in the OS keychain, shared with the app).
    std::shared_ptr<std::atomic<bool>> signedFlag;
    {
        namespace c2pa = wabcut::ai::c2pa;
        const auto mode = c2pa::credentialsModeFromString(credentials).value_or(c2pa::CredentialsMode::Ai);
        const std::string format = c2pa::formatForPath(request.output);
        const bool wanted = mode == c2pa::CredentialsMode::Always ||
                            (mode == c2pa::CredentialsMode::Ai && c2pa::containsGenerated(*request.snapshot, request.sequence, exported));
        if (wanted && format.empty()) {
            std::fprintf(stderr, "note: Content Credentials go into MP4 and MOV files only: %s is not signed\n",
                         wabcut::pathToUtf8(request.output.filename()).c_str());
        } else if (wanted && !c2pa::available()) {
            std::fprintf(stderr, "note: this build cannot sign Content Credentials: the unsigned sidecar is written\n");
        } else if (wanted) {
            c2pa::ManifestOptions options;
            options.title = wabcut::pathToUtf8(request.output.filename());
            options.format = format;
            options.softwareVersion = WABCUT_VERSION;
            options.range = exported;
            options.projectFile = request.projectFile;
            options.producer = c2paProducer;
            options.doNotTrain = c2paDoNotTrain;
            if (c2paThumbnail) {
                // The export's own picture in the middle of its range (redactions included), SDR JPEG, 320 wide.
                const wabcut::Ticks at = exported ? exported->start + wabcut::Ticks{exported->duration.value / 2} : wabcut::Ticks{};
                options.thumbnail = [snapshot = request.snapshot, sequence = request.sequence, at](const std::filesystem::path&) {
                    ex::ReviewFrameOptions frameOptions;
                    frameOptions.color = wabcut::render::colorPipelineFor(snapshot->colorManagement);
                    ex::RenderedReviewFrames frames(frameOptions);
                    auto frame = frames.frame(*snapshot, sequence, at, 320);
                    return frame && frame->mime == "image/jpeg" ? frame->bytes : std::string();
                };
            }
            c2pa::IdentityProvider identity = [c2paCert, c2paKey]() -> std::expected<c2pa::SigningIdentity, wabcut::ai::AiError> {
                if (!c2paCert.empty() || !c2paKey.empty()) {
                    std::ifstream cert(c2paCert, std::ios::binary);
                    std::ifstream key(c2paKey, std::ios::binary);
                    if (!cert || !key) {
                        return std::unexpected(wabcut::ai::AiError(wabcut::ai::AiErrorCode::InvalidRequest,
                                                                   "--c2pa-cert and --c2pa-key name readable PEM files"));
                    }
                    std::string chain(std::istreambuf_iterator<char>(cert), {});
                    std::string pem(std::istreambuf_iterator<char>(key), {});
                    return c2pa::identityFromPem(std::move(chain), wabcut::ai::Secret(std::move(pem)));
                }
                auto keychain = c2pa::osKeychain();
                if (!keychain) {
                    return std::unexpected(wabcut::ai::AiError(wabcut::ai::AiErrorCode::Unsupported,
                                                               "there is no OS keychain for the signing key here"));
                }
                return c2pa::localIdentity(*keychain, c2pa::defaultIdentityDir(), true);
            };
            signedFlag = std::make_shared<std::atomic<bool>>(false);
            request.finalize = c2pa::exportFinalizer(request.snapshot, request.sequence, std::move(options), std::move(identity), signedFlag);
        }
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
    for (const std::string& warning : result->warnings) {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }
    if (signedFlag && signedFlag->load()) {
        // The manifest in the file says it: no unsigned sidecar next to it, nor one left from an earlier export.
        auto sidecar = result->output; sidecar += ".provenance.json";
        std::error_code ec;
        std::filesystem::remove(sidecar, ec);
        std::printf("signed Content Credentials (C2PA) into %s\n", wabcut::pathToUtf8(result->output.filename()).c_str());
    } else if (!generatedProvenance.is_null()) {
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
    // 0.2: what else the export wrote.
    if (result->imageFiles > 0) {
        std::printf("stills: %lld files from %s\n", static_cast<long long>(result->imageFiles),
                    wabcut::pathToUtf8(result->output.filename()).c_str());
    }
    if (result->chapters > 0) {
        std::printf("chapters: %d\n", result->chapters);
    }
    if (!result->startTimecode.empty()) {
        std::printf("start timecode: %s\n", result->startTimecode.c_str());
    }
    if (result->subtitleCues > 0) {
        std::printf("subtitle track: %d cues\n", result->subtitleCues);
    }
    if (result->xmpEmbedded || !result->xmp.empty()) {
        std::printf("XMP: %s\n", result->xmpEmbedded ? "embedded" : wabcut::pathToUtf8(result->xmp).c_str());
    }
    if (result->loudnessLufs) {
        std::printf("loudness: measured %.1f LUFS, gain %+.2f dB\n", *result->loudnessLufs, result->loudnessGainDb);
    }
    if (result->copiedFrames > 0) {
        std::printf("smart rendering: %lld frames copied\n", static_cast<long long>(result->copiedFrames));
    }
    return 0;
}

// wabcut-cli watch <folder> --preset <id> [--output <folder>] [--keep-sources] [--once] [--test-pattern]
int runWatch(int argc, char** argv) {
    namespace ex = wabcut::exporter;
    if (argc < 3) {
        return printUsage();
    }
    ex::WatchFolder folder;
    folder.folder = wabcut::pathFromUtf8(argv[2]);
    std::string presetId;
    bool once = false;
    bool testPattern = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--preset" && i + 1 < argc) {
            presetId = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            folder.output = wabcut::pathFromUtf8(argv[++i]);
        } else if (arg == "--keep-sources") {
            folder.moveSources = false;
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "--test-pattern") {
            testPattern = true;
        } else {
            return printUsage();
        }
    }
    const auto preset = ex::PresetStore(userPresetFolder()).findAny(presetId);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", presetId.c_str());
        return 2;
    }
    folder.presetId = preset->id;
    folder.settings = preset->settings;
    const auto gui = testPattern ? nullptr : guiFor(argv[0]);
    // The queue's listener can see a finished job more than once; each job is reported once, below.
    ex::ExportQueue queue([](const ex::ExportJobStatus&) {});
    ex::WatchFolderService::Options options;
    options.interval = std::chrono::milliseconds(0); // scanned below
    options.settleScans = once ? 0 : 1;
    if (testPattern) {
        options.videoSource = ex::syntheticVideoSourceFactory();
    }
    ex::WatchFolderService watch(queue, options);
    if (auto added = watch.add(folder); !added) {
        std::fprintf(stderr, "cannot watch %s: %s\n", argv[2], added.error().message.c_str());
        return 2;
    }
    std::signal(SIGINT, onInterrupt);
    int failed = 0;
    do {
        for (const ex::ExportJobId id : watch.scanNow()) {
            const auto status = queue.wait(id);
            const bool done = status && status->progress.phase == ex::ExportPhase::Done;
            if (!done) {
                ++failed;
            }
            if (status) {
                std::fprintf(stderr, "%s %s%s%s\n", done ? "exported" : "failed", wabcut::pathToUtf8(status->output).c_str(),
                             status->error ? ": " : "", status->error ? status->error->message.c_str() : "");
            }
        }
        if (!once) {
            for (int t = 0; t < 20 && !g_interrupted.load(); ++t) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } while (!once && !g_interrupted.load());
    for (const ex::WatchFolderStatus& s : watch.folders()) {
        std::printf("%s: %d exported, %d failed%s%s\n", wabcut::pathToUtf8(s.folder.folder).c_str(), s.done, s.failed,
                    s.lastError.empty() ? "" : "; ", s.lastError.c_str());
    }
    return failed > 0 ? 2 : 0;
}

} // namespace

// AI-15: `credentials verify <file>`: reads and checks an export's C2PA Content Credentials with the library that signed
// it (no network). The local identity's root (public) and any --trust roots are trusted. Exit 0: valid; 1: missing or
// invalid; 2: cannot check.
int runCredentials(int argc, char** argv) {
    namespace c2pa = wabcut::ai::c2pa;
    if (argc < 4 || std::string_view(argv[2]) != "verify") {
        return printUsage();
    }
    const std::filesystem::path file = wabcut::pathFromUtf8(argv[3]);
    std::string anchors;
    bool json = false;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--trust" && i + 1 < argc) {
            std::ifstream in(wabcut::pathFromUtf8(argv[++i]), std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            anchors += std::string(std::istreambuf_iterator<char>(in), {}) + "\n";
        } else if (arg == "--json") {
            json = true;
        } else {
            return printUsage();
        }
    }
    if (!c2pa::available()) {
        std::fprintf(stderr, "this build has no C2PA library\n");
        return 2;
    }
    if (std::ifstream root(c2pa::defaultIdentityDir() / "root-certificate.pem", std::ios::binary); root) {
        anchors += std::string(std::istreambuf_iterator<char>(root), {});
    }
    const auto checked = c2pa::verifyFile(file, anchors);
    if (!checked) {
        std::fprintf(stderr, "cannot check %s: %s\n", argv[3], checked.error().message.c_str());
        return 2;
    }
    if (json) {
        const wabcut::ai::Json out{{"hasManifest", checked->hasManifest}, {"valid", checked->valid}, {"trusted", checked->trusted},
                                   {"signer", checked->signer},          {"failures", checked->failures}, {"manifest", checked->manifest}};
        std::printf("%s\n", out.dump(2, ' ', false, wabcut::ai::Json::error_handler_t::replace).c_str());
    } else if (!checked->hasManifest) {
        std::printf("no Content Credentials in %s\n", argv[3]);
    } else {
        std::printf("Content Credentials: %s, signed by %s (%s)\n", checked->valid ? "valid" : "INVALID", checked->signer.c_str(),
                    checked->trusted ? "trusted here" : "not on a trust list here");
        for (const std::string& failure : checked->failures) {
            std::printf("  %s\n", failure.c_str());
        }
    }
    return checked->hasManifest && checked->valid ? 0 : 1;
}

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    wabcut::media::useSystemVaapiDrivers(); // Linux: hardware decoding and encoding with the distribution's VA-API drivers
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
    if (argc >= 2 && std::string_view(argv[1]) == "credentials") {
        return runCredentials(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "render") {
        return runRender(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "watch") {
        return runWatch(argc, argv);
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
