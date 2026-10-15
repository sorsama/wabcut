// wabcut-cli transcribe: ai::TranscribeService (what the app and transcript.transcribe use) on one media file. Local
// transcription needs the wabcut-infer helper (WABCUT_WITH_LOCAL_AI builds); cloud transcription goes through the AI
// platform's guards with the key from WABCUT_<PROVIDER>_API_KEY and consent only from --allow-cloud; every build
// reads the analysis cache.

#include "Transcribe.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/Captions.h>
#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/TranscribeService.h>
#include <wabcut/ai/TranscriptJson.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#ifdef WABCUT_WITH_LOCAL_AI
#include <wabcut/ai_local/HttpDownload.h>
#endif

#include <QCoreApplication>

#include <charconv>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

namespace ai = wabcut::ai;

int usage() {
    std::fprintf(stderr,
                 "usage: wabcut-cli transcribe <media> [--provider auto|local|cloud:<model>] [--allow-cloud]\n"
                 "                             [--model base|large-v3-turbo|tiny] [--language th|en|auto]\n"
                 "                             [--json|--srt|--vtt] [--cpu] [--no-vad] [--beam <n>] [--threads <n>]\n"
                 "                             [--force | --no-cache] [--no-download] [--stream <index>] [--providers]\n"
                 "\n"
                 "Transcribes the audio of <media> and prints the transcript: text with timestamps by default, or JSON\n"
                 "(wabcut.transcript), SubRip or WebVTT. A transcript already in the analysis cache (from any provider,\n"
                 "e.g. the app) is printed without transcribing again. Local transcription runs whisper.cpp on this\n"
                 "computer (nothing is uploaded); models download into the user cache on first use (SHA-256 verified).\n"
                 "Cloud transcription uploads the audio (16 kHz mono) to the provider with the key in\n"
                 "WABCUT_<PROVIDER>_API_KEY, only with --allow-cloud. Timing goes to stderr.\n"
                 "  --provider <id>   auto (default: local when built), local, or cloud:<model id> (--providers lists them)\n"
                 "  --allow-cloud     consent to upload this file's audio to the cloud provider\n"
                 "  --model <id>      base (default, CPU-friendly), large-v3-turbo (best, wants a GPU), tiny\n"
                 "  --language <xx>   ISO 639-1 code or auto (default)\n"
                 "  --cpu             do not use a GPU\n"
                 "  --no-vad          transcribe everything, silence included\n"
                 "  --beam <n>        beam size (default 5; 1 = greedy, faster)\n"
                 "  --force           transcribe again and replace the cached transcript\n"
                 "  --no-cache        neither read nor write the analysis cache\n"
                 "  --no-download     fail instead of downloading a missing model\n"
                 "  --providers       list the providers and whether they are available, then exit\n");
    return 1;
}

std::optional<int> parseInt(std::string_view s) {
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

std::string clock(wabcut::Ticks t) {
    const auto ms = wabcut::mulDiv(t.value, 1000, wabcut::kTicksPerSecond, wabcut::Rounding::Nearest).value_or(0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld.%03lld", static_cast<long long>(ms / 3'600'000),
                  static_cast<long long>(ms / 60'000 % 60), static_cast<long long>(ms / 1000 % 60),
                  static_cast<long long>(ms % 1000));
    return buf;
}

std::string plainText(const ai::Transcript& t) {
    std::string out;
    for (const ai::TranscriptSegment& s : t.segments) {
        out += "[" + clock(s.t0) + " --> " + clock(s.t1) + "] " + ai::segmentWordsText(t, s) + "\n";
    }
    return out;
}

#ifdef WABCUT_WITH_LOCAL_AI
/// Prints the license card and progress of a first-use model download to stderr.
class ConsoleDownload final : public ai::IDownloadTransport {
public:
    explicit ConsoleDownload(std::unique_ptr<ai::IDownloadTransport> inner) : m_inner(std::move(inner)) {}
    std::expected<Response, wabcut::Error> get(const std::string& url, std::uint64_t offset,
                                               const std::function<bool(const Response&)>& onResponse,
                                               const std::function<bool(std::span<const std::byte>)>& onData,
                                               std::stop_token stop) override {
        for (const ai::ModelInfo& m : ai::builtinModels()) {
            if (m.url == url) {
                std::fprintf(stderr, "downloading model '%s' (%.0f MB)%s\n  %s\n", m.id.c_str(),
                             static_cast<double>(m.size) / 1e6, offset > 0 ? ", resuming" : "", m.licenseCard.c_str());
            }
        }
        std::uint64_t received = offset;
        int lastPercent = -1;
        auto result = m_inner->get(url, offset, onResponse,
                                   [&](std::span<const std::byte> data) {
                                       received += data.size();
                                       for (const ai::ModelInfo& m : ai::builtinModels()) {
                                           const int percent = m.url == url && m.size > 0
                                                                   ? static_cast<int>(received * 100 / m.size)
                                                                   : lastPercent;
                                           if (m.url == url && percent != lastPercent) {
                                               lastPercent = percent;
                                               std::fprintf(stderr, "\r  %3d%%", percent);
                                           }
                                       }
                                       return onData(data);
                                   },
                                   stop);
        std::fprintf(stderr, "\n");
        return result;
    }

private:
    std::unique_ptr<ai::IDownloadTransport> m_inner;
};
#endif

/// Cloud transcription for one run: the built-in model registry and adapters, keys from the environment only,
/// consent from --allow-cloud only, no spend cap, Qt's network stack (a QCoreApplication must exist).
struct CloudPlatform {
    ai::ProviderRegistry providers{ai::ModelRegistry::builtin()};
    ai::MemorySecretStore store;
    ai::KeyRing keys{store};
    ai::ConsentLedger consent;
    ai::SpendGuard spend;
    ai::RequestLog log{100};
    ai::net::NetworkStack network{log};
    ai::AiPlatform platform{providers, keys, consent, spend, log, network.transport()};

    CloudPlatform() { ai::net::registerCloudAdapters(providers); }
};

#ifdef _WIN32
/// Review P2-14: the arguments as UTF-8 (argv is in the ANSI code page, which cannot hold every file name).
std::vector<std::string> utf8Arguments() {
    std::vector<std::string> out;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return out;
    }
    for (int i = 0; i < count; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(std::max(bytes, 1)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), bytes, nullptr, nullptr);
        s.resize(static_cast<std::size_t>(std::max(bytes - 1, 0)));
        out.push_back(std::move(s));
    }
    LocalFree(wide);
    return out;
}
#endif

int transcribe(int argc, char** argv);

} // namespace

int runTranscribe(int argc, char** argv) {
#ifdef _WIN32
    std::vector<std::string> utf8 = utf8Arguments();
    std::vector<char*> pointers;
    if (static_cast<int>(utf8.size()) == argc) {
        for (std::string& a : utf8) {
            pointers.push_back(a.data());
        }
        pointers.push_back(nullptr);
        argv = pointers.data();
    }
#endif
    try {
        return transcribe(argc, argv);
    } catch (const std::exception& e) { // a path the OS cannot represent, say: an error, not an abort
        std::fprintf(stderr, "transcription failed: %s\n", e.what());
        return 2;
    }
}

namespace {

int transcribe(int argc, char** argv) {
    ai::TranscribeSource source;
    ai::TranscribeJobOptions options;
    enum class Format { Text, Json, Srt, Vtt } format = Format::Text;
    bool download = true;
    bool cache = true;
    bool listProviders = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--provider" && hasValue) {
            options.provider = argv[++i];
        } else if (arg == "--allow-cloud") {
            options.allowOnce = true;
        } else if (arg == "--model" && hasValue) {
            options.local.model = argv[++i];
        } else if (arg == "--language" && hasValue) {
            options.language = argv[++i];
        } else if (arg == "--json") {
            format = Format::Json;
        } else if (arg == "--srt") {
            format = Format::Srt;
        } else if (arg == "--vtt") {
            format = Format::Vtt;
        } else if (arg == "--cpu") {
            options.local.useGpu = false;
        } else if (arg == "--no-vad") {
            options.local.vad = false;
        } else if (arg == "--chunk-vad") {
            options.local.vadInsideChunks = true;
        } else if (arg == "--force") {
            options.useCache = false;
        } else if (arg == "--no-cache") {
            cache = false;
        } else if (arg == "--no-download") {
            download = false;
        } else if (arg == "--providers") {
            listProviders = true;
        } else if ((arg == "--beam" || arg == "--threads" || arg == "--stream") && hasValue) {
            const auto value = parseInt(argv[++i]);
            if (!value) {
                return usage();
            }
            (arg == "--beam" ? options.local.beamSize : arg == "--threads" ? options.local.threads : source.containerStream) = *value;
        } else if (!arg.starts_with("--") && source.mediaPath.empty()) {
            source.mediaPath = std::string(arg);
        } else {
            return usage();
        }
    }
    if (source.mediaPath.empty() && !listProviders) {
        return usage();
    }
    if (ai::findModel(options.local.model) == nullptr) {
        std::fprintf(stderr, "unknown model '%s'; available:", options.local.model.c_str());
        for (const ai::ModelInfo& m : ai::builtinModels()) {
            if (m.kind == ai::ModelKind::Whisper) {
                std::fprintf(stderr, " %s", m.id.c_str());
            }
        }
        std::fprintf(stderr, "\n");
        return 2;
    }

    ai::TranscribeServiceConfig config;
    if (!cache) {
        config.cacheRoot.clear();
    }
    // The cloud providers only when asked for (they need Qt's network stack).
    std::unique_ptr<QCoreApplication> qtApp;
    std::unique_ptr<CloudPlatform> cloud;
    if (listProviders || options.provider.starts_with("cloud:")) {
        if (QCoreApplication::instance() == nullptr) {
            static int qtArgc = 1;
            static char qtName[] = "wabcut-cli";
            static char* qtArgv[] = {qtName, nullptr};
            qtApp = std::make_unique<QCoreApplication>(qtArgc, qtArgv);
        }
        cloud = std::make_unique<CloudPlatform>();
        config.platform = &cloud->platform;
    }
#ifdef WABCUT_WITH_LOCAL_AI
    std::unique_ptr<ai::IDownloadTransport> transport;
    if (download) {
        if (auto http = wabcut::ai_local::makeHttpDownloadTransport()) {
            transport = std::make_unique<ConsoleDownload>(std::move(http));
        }
    }
    config.local.download = transport.get();
#else
    (void)download;
#endif
    ai::TranscribeService service(std::move(config));

    if (listProviders) {
        for (const ai::TranscribeProviderInfo& p : service.providers()) {
            std::printf("%-24s %-10s %s%s%s\n", p.id.c_str(), p.available ? "available" : "-", p.label.c_str(),
                        p.reason.empty() ? "" : ": ", p.reason.c_str());
        }
        return 0;
    }

    int lastPercent = -1;
    auto outcome = service.run(source, options, {}, [&](float p) {
        const int percent = static_cast<int>(p * 100.0f);
        if (percent != lastPercent) {
            lastPercent = percent;
            std::fprintf(stderr, "\rtranscribing %3d%%", percent);
        }
    });
    std::fprintf(stderr, "\n");
    if (!outcome) {
        std::fprintf(stderr, "transcription failed: %s\n", outcome.error().message.c_str());
        if (outcome.error().message.find("ConsentRequired") != std::string::npos) {
            std::fprintf(stderr, "add --allow-cloud to upload the audio of %s to the provider\n", source.mediaPath.c_str());
        }
        return 2;
    }
    const ai::Transcript& transcript = outcome->transcript;

    std::string out;
    switch (format) {
    case Format::Text:
        out = plainText(transcript);
        break;
    case Format::Json:
        out = ai::toJson(transcript, 2) + "\n";
        break;
    case Format::Srt:
        out = ai::toSrt(transcript);
        break;
    case Format::Vtt:
        out = ai::toVtt(transcript);
        break;
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);

    const ai::TranscriptionStats& stats = outcome->localStats;
    if (outcome->fromCache || stats.fromCache) {
        std::fprintf(stderr, "from the analysis cache (made by %s; language %s, model %s)\n", outcome->provider.c_str(),
                     transcript.language.c_str(), transcript.provenance.modelId.c_str());
    } else if (outcome->provider == "local") {
        const double total = stats.totalMs / 1000.0;
        std::fprintf(stderr,
                     "transcribed %.1f s of audio (%.1f s of speech) in %.2f s = %.1fx real time; whisper %.2f s = %.1fx "
                     "real time on the speech\n"
                     "  model %s on %s, language %s; extract %.2f s, load %.2f s, VAD %.2f s\n",
                     stats.mediaSeconds, stats.speechSeconds, total, total > 0 ? stats.mediaSeconds / total : 0.0,
                     stats.recognizeMs / 1000.0, stats.recognizeMs > 0 ? stats.speechSeconds * 1000.0 / stats.recognizeMs : 0.0,
                     transcript.provenance.modelId.c_str(), stats.device.c_str(), transcript.language.c_str(),
                     stats.extractMs / 1000.0, stats.loadMs / 1000.0, stats.vadMs / 1000.0);
    } else {
        std::fprintf(stderr, "transcribed by %s (language %s)\n", outcome->provider.c_str(), transcript.language.c_str());
    }
    if (!outcome->note.empty()) {
        std::fprintf(stderr, "note: %s\n", outcome->note.c_str());
    }
    return 0;
}

} // namespace
