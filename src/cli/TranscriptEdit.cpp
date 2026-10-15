#include "TranscriptEdit.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/audio/AudioJobs.h>
#include <wabcut/ai/edit/CaptionBuilder.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/edit/TranscriptSources.h>
#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Cli.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <charconv>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace edit = wabcut::ai::edit;
using wabcut::command::Json;

int usage() {
    std::fprintf(stderr,
                 "usage: wabcut-cli transcript-edit <project.wabcut> [--remove-fillers] [--remove-silences <s>]\n"
                 "                                  [--min-confidence <0..1>] [--padding <s>] [--room-tone <s>]\n"
                 "                                  [--sequence <id>] [--review] [--dry-run] [--save]\n"
                 "       wabcut-cli captions <project.wabcut> [--sequence <id>] [--srt <out.srt>] [--vtt <out.vtt>]\n"
                 "                           [--max-chars <n>] [--max-lines <n>] [--max-duration <s>] [--save]\n"
                 "  both: [--cache <analysis cache dir>] [--transcript <media id or name>[:<stream>]=<file.json>]...\n");
    return 2;
}

std::optional<double> number(std::string_view text) {
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// Options both subcommands take.
struct Sources {
    std::filesystem::path cache = wabcut::ai::AnalysisCache::defaultRoot();
    std::vector<std::string> transcripts; ///< "<media>[:<stream>]=<file>"
    std::optional<std::uint64_t> sequence;
};

/// Consumes a shared option at argv[i]; false when argv[i] is not one. Sets `bad` on a malformed one.
bool sharedOption(int argc, char** argv, int& i, Sources& sources, bool& bad) {
    const std::string_view arg = argv[i];
    if (arg == "--cache" && i + 1 < argc) {
        sources.cache = argv[++i];
    } else if (arg == "--transcript" && i + 1 < argc) {
        sources.transcripts.emplace_back(argv[++i]);
    } else if (arg == "--sequence" && i + 1 < argc) {
        const auto id = number(argv[++i]);
        bad = !id || *id < 1;
        sources.sequence = bad ? 0 : static_cast<std::uint64_t>(*id);
    } else {
        return false;
    }
    return true;
}

const wabcut::MediaItem* findMedia(const wabcut::ProjectState& state, std::string_view ref) {
    if (const auto id = number(ref)) {
        if (const auto* m = state.findMedia(wabcut::MediaId{static_cast<std::uint64_t>(*id)})) {
            return m;
        }
    }
    for (const auto& m : state.media) {
        if (m->name == ref) {
            return m.get();
        }
    }
    return nullptr;
}

std::shared_ptr<edit::TranscriptServices> servicesFor(const std::filesystem::path& project,
                                                      const wabcut::ProjectState* state, const Sources& sources,
                                                      std::string& error) {
    auto chain = std::make_shared<edit::TranscriptProviderChain>();
    if (!sources.transcripts.empty()) {
        auto files = std::make_shared<edit::MapTranscriptProvider>();
        for (const std::string& spec : sources.transcripts) {
            const std::size_t eq = spec.find('=');
            if (eq == std::string::npos || !state) {
                error = "--transcript needs <media>[:<stream>]=<file>, got \"" + spec + "\"";
                return nullptr;
            }
            std::string_view ref = std::string_view(spec).substr(0, eq);
            int stream = 0;
            if (const std::size_t colon = ref.rfind(':'); colon != std::string_view::npos) {
                const auto n = number(ref.substr(colon + 1));
                if (n && *n >= 0) {
                    stream = static_cast<int>(*n);
                    ref = ref.substr(0, colon);
                }
            }
            const wabcut::MediaItem* media = findMedia(*state, ref);
            if (!media) {
                error = "no media \"" + std::string(ref) + "\" in the project";
                return nullptr;
            }
            auto transcript = edit::readTranscriptFile(spec.substr(eq + 1));
            if (!transcript) {
                error = spec.substr(eq + 1) + ": " + transcript.error().message;
                return nullptr;
            }
            files->add(media->id, stream, std::make_shared<const wabcut::ai::Transcript>(std::move(*transcript)));
        }
        chain->add(files);
    }
    const std::filesystem::path absolute = std::filesystem::absolute(project);
    chain->add(std::make_shared<edit::CachedTranscriptProvider>(sources.cache, absolute));
    // Pauses are measured in one pass per media file (AudioEnergyService), cached with the other analyses.
    wabcut::ai::AudioEnergyServiceConfig energy;
    energy.cacheRoot = sources.cache;
    return std::make_shared<edit::TranscriptServices>(
        chain, std::make_shared<edit::ServiceEnergyProbe>(std::make_shared<wabcut::ai::AudioEnergyService>(energy), absolute));
}

int finish(const wabcut::command::CliOutcome& outcome) {
    std::fwrite(outcome.out.data(), 1, outcome.out.size(), stdout);
    std::fwrite(outcome.err.data(), 1, outcome.err.size(), stderr);
    return outcome.exitCode;
}

bool writeFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

} // namespace

std::shared_ptr<wabcut::command::CommandServices> audioServicesFor(const std::filesystem::path& project) {
    const Sources sources;
    auto transcripts = std::make_shared<edit::TranscriptProviderChain>();
    transcripts->add(std::make_shared<edit::CachedTranscriptProvider>(sources.cache, std::filesystem::absolute(project)));
    auto services = wabcut::audio::edit::makeMediaAudioServices(transcripts, std::make_shared<wabcut::ai::AnalysisCache>(sources.cache));
    // A.4: Enhance Speech and stem separation run on this computer (wabcut-embed next to the CLI). A headless request
    // downloads nothing: the models come from the model store (the app downloads them on first use).
    services->enhancer = wabcut::ai::audio::makeSpeechEnhancer({});
    services->separator = wabcut::ai::audio::makeStemSeparator({});
    // A.5 / C.8: multicam.create's "audio" sync, on the analysis audio.syncClips uses.
    auto multicam = std::make_shared<wabcut::command::MulticamServices>(std::make_shared<wabcut::audio::edit::MulticamAudioSync>(services->sync));
    return std::make_shared<wabcut::command::ServiceBundle>(
        std::vector<std::shared_ptr<wabcut::command::CommandServices>>{std::move(services), std::move(multicam)});
}

std::shared_ptr<wabcut::command::CommandServices> transcriptServicesFor(const std::filesystem::path& project) {
    std::string error;
    return servicesFor(project, nullptr, Sources{}, error);
}

std::shared_ptr<wabcut::command::CommandServices> transcribeServicesFor(const std::filesystem::path& project) {
    // Local transcription and the cache only: a headless request (cmd, an MCP agent) cannot answer a cloud consent
    // prompt, so cloud transcription is for `wabcut-cli transcribe --allow-cloud` and the app.
    auto service = std::make_shared<wabcut::ai::TranscribeService>(wabcut::ai::TranscribeServiceConfig{});
    return std::make_shared<edit::TranscribeServices>(
        std::make_shared<edit::ServiceMediaTranscriber>(std::move(service), std::filesystem::absolute(project)));
}

// wabcut-cli transcript-edit <project> --remove-fillers --remove-silences 0.8 [--dry-run] [--save]
int runTranscriptEdit(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::filesystem::path project = argv[2];
    Sources sources;
    bool fillers = false;
    std::optional<double> silences;
    bool review = false;
    bool dryRun = false;
    bool save = false;
    Json params = Json::object();
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        bool bad = false;
        if (sharedOption(argc, argv, i, sources, bad)) {
            if (bad) {
                return usage();
            }
        } else if (arg == "--remove-fillers") {
            fillers = true;
        } else if (arg == "--remove-silences" && i + 1 < argc) {
            silences = number(argv[++i]);
            if (!silences || *silences <= 0.0) {
                return usage();
            }
        } else if ((arg == "--min-confidence" || arg == "--padding" || arg == "--room-tone") && i + 1 < argc) {
            const auto value = number(argv[++i]);
            if (!value || *value < 0.0) {
                return usage();
            }
            params[arg == "--min-confidence" ? "minConfidence" : arg == "--padding" ? "padding" : "roomTone"] = *value;
        } else if (arg == "--review") {
            review = true;
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--save") {
            save = true;
        } else {
            return usage();
        }
    }
    if (!review && !fillers && !silences) {
        std::fprintf(stderr, "transcript-edit: say what to remove (--remove-fillers, --remove-silences <s>) or --review\n");
        return usage();
    }
    auto loaded = wabcut::io::loadProject(project);
    if (!loaded) {
        std::fprintf(stderr, "cannot load %s: %s\n", wabcut::pathToUtf8(project).c_str(), loaded.error().message.c_str());
        return 2;
    }
    std::string error;
    auto services = servicesFor(project, &*loaded, sources, error);
    if (!services) {
        std::fprintf(stderr, "transcript-edit: %s\n", error.c_str());
        return 2;
    }
    if (sources.sequence) {
        params["sequence"] = *sources.sequence;
    }
    params["fillers"] = review || fillers;
    params["silences"] = review && !silences ? 0.8 : silences.value_or(0.0);
    if (review) {
        params.erase("minConfidence");
        params.erase("padding");
        params.erase("roomTone");
    }
    const Json request{{"command", review ? "transcript.review" : "transcript.cleanup"}, {"params", params}};
    return finish(wabcut::command::runCommandCli(project, request.dump(), dryRun, save && !review, edit::editingCommands(),
                                                 services));
}

// wabcut-cli captions <project> --sequence <id> --srt out.srt
int runCaptions(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::filesystem::path project = argv[2];
    Sources sources;
    std::optional<std::filesystem::path> srt;
    std::optional<std::filesystem::path> vtt;
    bool save = false;
    Json params = Json::object();
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        bool bad = false;
        if (sharedOption(argc, argv, i, sources, bad)) {
            if (bad) {
                return usage();
            }
        } else if (arg == "--srt" && i + 1 < argc) {
            srt = argv[++i];
        } else if (arg == "--vtt" && i + 1 < argc) {
            vtt = argv[++i];
        } else if ((arg == "--max-chars" || arg == "--max-lines" || arg == "--max-duration") && i + 1 < argc) {
            const auto value = number(argv[++i]);
            if (!value || *value <= 0.0) {
                return usage();
            }
            if (arg == "--max-duration") {
                params["maxDuration"] = *value;
            } else {
                params[arg == "--max-chars" ? "maxCharsPerLine" : "maxLines"] = static_cast<std::int64_t>(*value);
            }
        } else if (arg == "--save") {
            save = true;
        } else {
            return usage();
        }
    }
    if (!srt && !vtt && !save) {
        std::fprintf(stderr, "captions: say where the captions go (--srt, --vtt and/or --save)\n");
        return usage();
    }
    auto loaded = wabcut::io::loadProject(project);
    if (!loaded) {
        std::fprintf(stderr, "cannot load %s: %s\n", wabcut::pathToUtf8(project).c_str(), loaded.error().message.c_str());
        return 2;
    }
    std::string error;
    auto services = servicesFor(project, &*loaded, sources, error);
    if (!services) {
        std::fprintf(stderr, "captions: %s\n", error.c_str());
        return 2;
    }
    if (sources.sequence) {
        params["sequence"] = *sources.sequence;
    }
    // captions.generate on a scratch session makes the track (with the same validation as the tool); the files
    // are written from it, and --save stores it in the project too.
    const Json request{{"command", "captions.generate"}, {"params", params}};
    const auto outcome = wabcut::command::runCommandCli(project, request.dump(), false, save, edit::editingCommands(), services);
    if (outcome.exitCode != 0) {
        return finish(outcome);
    }
    const Json result = Json::parse(outcome.out);
    const wabcut::SequenceId sequenceId{sources.sequence.value_or(
        loaded->sequences.size() == 1 ? loaded->sequences.front()->id.value : 0)};
    const auto transcript = edit::SequenceTranscript::build(*loaded, sequenceId, *services->transcripts);
    edit::CaptionOptions options;
    options.maxCharsPerLine = static_cast<int>(params.value("maxCharsPerLine", std::int64_t{42}));
    options.maxLines = static_cast<int>(params.value("maxLines", std::int64_t{2}));
    if (params.contains("maxDuration")) {
        options.maxDuration = wabcut::Ticks{static_cast<std::int64_t>(params["maxDuration"].get<double>() *
                                                                      static_cast<double>(wabcut::kTicksPerSecond))};
    }
    const wabcut::CaptionTrack track = edit::buildCaptions(transcript, options);
    Json summary{{"cues", track.cues.size()}, {"language", track.language}, {"words", transcript.size()}};
    if (save) {
        summary["captionTrack"] = result["data"]["captionTrack"];
        summary["saved"] = result.value("saved", false);
    }
    Json written = Json::array();
    if (srt) {
        if (!writeFile(*srt, edit::toSrt(track))) {
            std::fprintf(stderr, "cannot write %s\n", wabcut::pathToUtf8(*srt).c_str());
            return 2;
        }
        written.push_back(wabcut::pathToUtf8(*srt));
    }
    if (vtt) {
        if (!writeFile(*vtt, edit::toVtt(track))) {
            std::fprintf(stderr, "cannot write %s\n", wabcut::pathToUtf8(*vtt).c_str());
            return 2;
        }
        written.push_back(wabcut::pathToUtf8(*vtt));
    }
    summary["written"] = std::move(written);
    const std::string out = summary.dump(2) + "\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
