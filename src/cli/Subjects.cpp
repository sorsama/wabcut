#include "Subjects.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/multicam/MediaAngleSignals.h>
#include <wabcut/ai/multicam/MulticamAiCommands.h>
#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/ai/vision/MediaSubjects.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

namespace vision = wabcut::ai::vision;

std::shared_ptr<wabcut::command::CommandServices> reframeServicesFor(const std::filesystem::path& project) {
    std::error_code ec;
    vision::MediaSubjectOptions options;
    options.projectFile = std::filesystem::absolute(project, ec);
    options.cacheRoot = wabcut::ai::AnalysisCache::defaultRoot();
    auto choice = vision::openSubjectDetector({}, {});
    auto provider = std::make_shared<vision::MediaSubjectProvider>(options, choice.detector);
    // AI-13: analysis.speakers and multicam.autoSwitch read the same faces, and each angle's microphone level.
    auto speakers = std::make_shared<wabcut::ai::multicam::SpeakerServices>(wabcut::ai::multicam::makeMediaAngleSignals(provider, options.projectFile));
    // C.16: sequence.makeVersion's Auto Reframe (versions of edits without a facecam), over the same subjects.
    auto versions = std::make_shared<wabcut::command::VersionServices>(
        [provider](const wabcut::ProjectState& state, wabcut::SequenceId source, int width, int height,
                   std::string name) -> std::expected<wabcut::ReframedSequence, wabcut::Error> {
            namespace rf = wabcut::ai::reframe;
            rf::ReframeOptions options;
            options.aspect = rf::Aspect{width, height};
            options.width = width;
            options.height = height;
            options.name = std::move(name);
            const rf::SubjectSource subjects = [provider](const wabcut::MediaItem& media, int stream) {
                return provider->subjects(media, stream, [](double) { return true; });
            };
            auto made = rf::reframeSequence(state, source, options, subjects);
            if (!made) {
                return std::unexpected(made.error());
            }
            return wabcut::ReframedSequence{std::move(made->state), made->sequence, std::move(made->warnings)};
        });
    return std::make_shared<wabcut::command::ServiceBundle>(std::vector<std::shared_ptr<wabcut::command::CommandServices>>{
        std::make_shared<wabcut::ai::reframe::ReframeServices>(provider), std::move(speakers), std::move(versions)});
}

namespace {

int usage() {
    std::fprintf(stderr, "usage: wabcut-cli subjects <media> [--device auto|gpu|cpu] [--threads <n>] [--no-cache] [--json]\n"
                         "                           [--face-model <yunet.onnx> --object-model <yolox.onnx>] [--helper <wabcut-embed>]\n");
    return 2;
}

} // namespace

int runSubjects(int argc, char** argv) {
    std::string media;
    std::string device = "auto";
    int threads = 0;
    bool cache = true;
    bool json = false;
    std::filesystem::path faceModel;
    std::filesystem::path objectModel;
    std::filesystem::path helper;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--device" && hasValue) {
            device = argv[++i];
        } else if (arg == "--threads" && hasValue) {
            threads = std::atoi(argv[++i]);
        } else if (arg == "--no-cache") {
            cache = false;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--face-model" && hasValue) {
            faceModel = wabcut::pathFromUtf8(argv[++i]);
        } else if (arg == "--object-model" && hasValue) {
            objectModel = wabcut::pathFromUtf8(argv[++i]);
        } else if (arg == "--helper" && hasValue) {
            helper = wabcut::pathFromUtf8(argv[++i]);
        } else if (!arg.starts_with("--") && media.empty()) {
            media = std::string(arg);
        } else {
            return usage();
        }
    }
    if (media.empty() || (device != "auto" && device != "gpu" && device != "cpu")) {
        return usage();
    }
    std::shared_ptr<vision::HelperDetector> detector;
    std::string reason;
    if (!faceModel.empty() || !objectModel.empty()) {
        vision::HelperDetectorOptions o;
        o.faceModel = faceModel;
        o.objectModel = objectModel;
        o.executable = helper;
        o.modelVersion = vision::subjectModelVersion();
        o.threads = threads;
        o.device = device;
        detector = std::make_shared<vision::HelperDetector>(std::move(o));
    } else {
        auto choice = vision::openSubjectDetector({}, helper, threads);
        detector = choice.detector;
        reason = choice.unavailableReason;
    }
    vision::MediaSubjectOptions options;
    options.cacheRoot = cache ? wabcut::ai::AnalysisCache::defaultRoot() : std::filesystem::path{};
    vision::SubjectStats stats;
    int lastPercent = -1;
    const auto started = std::chrono::steady_clock::now();
    auto result = vision::analyzeFileSubjects(media, -1, detector.get(), options,
                                              [&](double f) {
                                                  const int percent = static_cast<int>(f * 100.0);
                                                  if (!json && percent != lastPercent) {
                                                      lastPercent = percent;
                                                      std::fprintf(stderr, "\r  %3d%%", percent);
                                                  }
                                                  return true;
                                              },
                                              &stats);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!json) {
        std::fprintf(stderr, "\n");
    }
    if (!result) {
        std::fprintf(stderr, "subjects: %s\n", result.error().message.c_str());
        return 1;
    }
    std::size_t withFaces = 0;
    std::size_t withPeople = 0;
    for (const vision::SubjectSample& s : result->samples) {
        withFaces += s.faces.empty() ? 0 : 1;
        withPeople += s.people.empty() ? 0 : 1;
    }
    const auto [faceTracks, personTracks] = vision::countTracks(*result);
    const double analysed = stats.analysedSeconds;
    const double seconds = stats.fromCache ? 0.0 : std::max(stats.seconds, 1e-6);
    const std::string where = detector ? (stats.device.empty() ? detector->device() : stats.device) : std::string("saliency only");
    if (json) {
        nlohmann::json j{{"media", media},
                         {"samples", result->samples.size()},
                         {"shots", result->shotCount()},
                         {"seconds", analysed},
                         {"wallSeconds", wall},
                         {"analysisSeconds", stats.seconds},
                         {"fromCache", stats.fromCache},
                         {"device", where},
                         {"decoder", stats.backend},
                         {"faces", faceTracks},
                         {"people", personTracks},
                         {"samplesWithFaces", withFaces},
                         {"samplesWithPeople", withPeople},
                         {"models", result->models}};
        if (!stats.fromCache) {
            j["realtime"] = analysed / seconds;
            j["samplesPerSecond"] = static_cast<double>(result->samples.size()) / seconds;
        }
        if (!reason.empty()) {
            j["note"] = reason;
        }
        const std::string out = j.dump(2) + "\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    const double n = std::max<double>(1.0, static_cast<double>(result->samples.size()));
    std::printf("%s: %zu samples over %.1f s, %zu shot(s); faces in %.0f%% of samples, people in %.0f%% (%zu face tracks, %zu person "
                "tracks)\n",
                media.c_str(), result->samples.size(), analysed, result->shotCount(), 100.0 * static_cast<double>(withFaces) / n,
                100.0 * static_cast<double>(withPeople) / n, faceTracks, personTracks);
    if (stats.fromCache) {
        std::printf("  from the analysis cache\n");
    } else {
        std::printf("  analysed in %.1f s: %.1fx real time, %.0f samples/s, detectors on %s, decoder %s\n", stats.seconds, analysed / seconds,
                    static_cast<double>(result->samples.size()) / seconds, where.c_str(), stats.backend.c_str());
    }
    if (!reason.empty()) {
        std::printf("  note: %s\n", reason.c_str());
    }
    return 0;
}
