#include "Analyze.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/analysis/AnalysisJson.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace an = wabcut::ai::analysis;
using nlohmann::json;

int usage() {
    std::fprintf(stderr,
                 "usage: wabcut-cli analyze <media> [--shots] [--beats] [--json]\n"
                 "                          [--detector combined|content|adaptive|threshold] [--threshold <n>]\n"
                 "                          [--min-bpm <n>] [--max-bpm <n>] [--cache <dir> | --no-cache]\n");
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

std::string seconds(wabcut::Ticks t) {
    char text[32];
    std::snprintf(text, sizeof text, "%.3f", t.toSeconds());
    return text;
}

json statsJson(const an::MediaAnalyzer::Stats& s) {
    const double media = s.mediaDuration.toSeconds();
    json out{{"fromCache", s.fromCache}, {"seconds", s.seconds}, {"mediaSeconds", media}};
    if (!s.backend.empty()) {
        out["decoder"] = s.backend;
    }
    if (!s.fromCache && s.seconds > 0.0) {
        out["realtimeFactor"] = media / s.seconds;
    }
    return out;
}

std::string speedText(const an::MediaAnalyzer::Stats& s) {
    if (s.fromCache) {
        return "from the cache";
    }
    char text[96];
    std::snprintf(text, sizeof text, "%.2f s for %.2f s of media = %.1fx realtime", s.seconds, s.mediaDuration.toSeconds(),
                  s.seconds > 0.0 ? s.mediaDuration.toSeconds() / s.seconds : 0.0);
    return text;
}

} // namespace

std::shared_ptr<wabcut::command::CommandServices> analysisServicesFor(const std::filesystem::path& project) {
    an::MediaAnalyzerOptions options;
    options.cacheRoot = wabcut::ai::AnalysisCache::defaultRoot();
    return std::make_shared<an::AnalysisServices>(
        std::make_shared<an::MediaAnalysisProvider>(options, std::filesystem::absolute(project)));
}

// wabcut-cli analyze <media> --shots --beats [--json]
int runAnalyze(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string path = argv[2];
    bool shots = false;
    bool beats = false;
    bool asJson = false;
    an::ShotOptions shotOptions;
    an::BeatOptions beatOptions;
    an::MediaAnalyzerOptions options;
    options.cacheRoot = wabcut::ai::AnalysisCache::defaultRoot();
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--shots") {
            shots = true;
        } else if (arg == "--beats") {
            beats = true;
        } else if (arg == "--json") {
            asJson = true;
        } else if (arg == "--no-cache") {
            options.cacheRoot.clear();
        } else if (arg == "--cache" && i + 1 < argc) {
            options.cacheRoot = argv[++i];
        } else if (arg == "--detector" && i + 1 < argc) {
            const auto detector = an::shotDetectorFromString(argv[++i]);
            if (!detector) {
                return usage();
            }
            shotOptions.detector = *detector;
        } else if ((arg == "--threshold" || arg == "--min-bpm" || arg == "--max-bpm") && i + 1 < argc) {
            const auto value = number(argv[++i]);
            if (!value || *value <= 0.0) {
                return usage();
            }
            (arg == "--threshold" ? shotOptions.contentThreshold : arg == "--min-bpm" ? beatOptions.minBpm : beatOptions.maxBpm) = *value;
        } else {
            return usage();
        }
    }
    if (!shots && !beats) {
        shots = beats = true;
    }
    an::MediaAnalyzer analyzer(options);
    json out{{"media", path}};
    std::string text = path + "\n";
    int failures = 0;
    if (shots) {
        an::MediaAnalyzer::Stats stats;
        const auto result = analyzer.shots(path, -1, shotOptions, {}, &stats);
        if (!result) {
            std::fprintf(stderr, "shots: %s\n", result.error().message.c_str());
            out["shots"] = json{{"error", result.error().message}};
            ++failures;
        } else {
            json payload = json::parse(an::toJson(*result));
            payload["stats"] = statsJson(stats);
            out["shots"] = std::move(payload);
            text += "shots (" + std::string(an::toString(shotOptions.detector)) + "): " + std::to_string(result->cuts.size()) +
                    " cuts in " + std::to_string(result->frames) + " frames; " + speedText(stats) +
                    (stats.backend.empty() ? "" : ", decoder " + stats.backend) + "\n";
            for (const an::ShotCut& c : result->cuts) {
                char line[96];
                std::snprintf(line, sizeof line, "  %10s s  frame %-7lld %-8s %.2f\n", seconds(c.time).c_str(),
                              static_cast<long long>(c.frame), std::string(an::toString(c.kind)).c_str(), c.confidence);
                text += line;
            }
        }
    }
    if (beats) {
        an::MediaAnalyzer::Stats stats;
        const auto result = analyzer.beats(path, -1, beatOptions, {}, &stats);
        if (!result) {
            std::fprintf(stderr, "beats: %s\n", result.error().message.c_str());
            out["beats"] = json{{"error", result.error().message}};
            ++failures;
        } else {
            json payload = json::parse(an::toJson(*result));
            payload["stats"] = statsJson(stats);
            out["beats"] = std::move(payload);
            char line[160];
            std::snprintf(line, sizeof line, "beats: %.2f BPM (confidence %.2f), %zu beats, %zu downbeats; %s\n", result->bpm,
                          result->confidence, result->beats.size(), result->downbeats.size(), speedText(stats).c_str());
            text += line;
            text += "  first beats (s):";
            for (std::size_t i = 0; i < result->beats.size() && i < 8; ++i) {
                text += " " + seconds(result->beats[i]);
            }
            text += "\n";
        }
    }
    const std::string printed = asJson ? out.dump(2) + "\n" : text;
    std::fwrite(printed.data(), 1, printed.size(), stdout);
    return failures == 0 ? 0 : 1;
}
