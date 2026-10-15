// AI-15: the eval on the real local tiers (label "perf": a quiet machine; never CI). Opt-in:
//   WABCUT_EVAL_MODELS  directory with the pinned GGUF files (ModelInfo::fileName; each one's SHA-256 is checked)
//   WABCUT_EVAL_LLAMA   directory with llama.cpp b11429's official builds unpacked in cpu/ and vulkan/
//   WABCUT_EVAL_TIERS   small,medium,large (default: all)      WABCUT_EVAL_BACKEND  auto (default), cpu or gpu
//   WABCUT_EVAL_MODES   constrained,native (default: constrained)  WABCUT_EVAL_ONLY  case ids or categories
// Each tier is admitted like the app admits it (Tiers.h); one that does not fit here is reported, not forced. Results:
// test-output/eval/local-eval-<tier>-<mode>.json and the table in test-output/eval/local-eval.md. Nothing leaves the
// computer: the server listens on 127.0.0.1 and the fixtures hold no media.
//
// The cloud reference runs only with the user's own key (WABCUT_LIVE_<PROVIDER>_KEY, the live tests' convention) and
// WABCUT_EVAL_CLOUD naming the model; it sends the requests and project summaries, never media.
#include "EvalHarness.h"

#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>
#include <wabcut/model/ProjectIO.h>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::eval;

namespace {

void ensureCoreApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_eval_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

std::vector<std::string> listOf(const std::optional<std::string>& text, std::vector<std::string> fallback) {
    if (!text || text->empty()) {
        return fallback;
    }
    std::vector<std::string> out;
    std::stringstream in(*text);
    for (std::string item; std::getline(in, item, ',');) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

std::optional<std::string> fileSha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
    }
    return hash.finishHex();
}

struct ProcessMemory {
    std::uint64_t peakWorkingSet = 0;
    std::uint64_t privateBytes = 0;
};

ProcessMemory memoryOf(std::uint64_t pid) {
    ProcessMemory m;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process) {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            m.peakWorkingSet = counters.PeakWorkingSetSize;
            m.privateBytes = counters.PrivateUsage;
        }
        CloseHandle(process);
    }
#else
    (void)pid;
#endif
    return m;
}

double gb(std::uint64_t bytes) { return static_cast<double>(bytes) / 1e9; }

std::string fixed(double v, int digits = 1) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

std::string percent(double v) { return fixed(100.0 * v, 0) + "%"; }

std::filesystem::path outputDir() {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "eval";
    std::filesystem::create_directories(dir);
    return dir;
}

void appendMarkdown(const std::string& row) {
    const auto file = outputDir() / "local-eval.md";
    const bool fresh = !std::filesystem::exists(file);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (fresh) {
        out << "| Tier | Model | Backend | Mode | Success | Valid tool calls | Routing recall | Median step | p90 step | "
               "First token | Decode | Peak RAM | Private | GPU memory |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    }
    out << row << "\n";
}

std::string progressLine(const CaseResult& r) {
    return r.id + (r.success ? " ok " : " FAIL ") + fixed(r.seconds) + " s" + (r.success ? "" : ": " + r.failure.substr(0, 200));
}

} // namespace

TEST_CASE("Local assistant eval on the real tiers", "[ai][eval][perf][live]") {
    const auto models = systemEnv("WABCUT_EVAL_MODELS");
    const auto llama = systemEnv("WABCUT_EVAL_LLAMA");
    if (systemEnv("CI") || !models || !llama) {
        SKIP("set WABCUT_EVAL_MODELS and WABCUT_EVAL_LLAMA to run the local eval (never CI)");
    }
    ensureCoreApp();
    const auto tiers = listOf(systemEnv("WABCUT_EVAL_TIERS"), {"small", "medium", "large"});
    const auto modes = listOf(systemEnv("WABCUT_EVAL_MODES"), {"constrained"});
    const std::string backend = systemEnv("WABCUT_EVAL_BACKEND").value_or("auto");
    RunOptions runOptions;
    runOptions.only = listOf(systemEnv("WABCUT_EVAL_ONLY"), {});
    runOptions.progress = [](const CaseResult& r) {
        std::fprintf(stderr, "  %s\n", progressLine(r).c_str());
        std::fflush(stderr);
    };
    int ran = 0;
    for (const std::string& tierId : tiers) {
        const llm::TierSpec* tier = llm::findTier(tierId);
        REQUIRE(tier != nullptr);
        const ModelInfo& info = *findModel(tier->model);
        const auto weights = std::filesystem::path(*models) / info.fileName;
        if (!std::filesystem::exists(weights)) {
            UNSCOPED_INFO(tierId << ": " << weights.string() << " not downloaded");
            continue;
        }
        REQUIRE(fileSha256(weights) == info.sha256); // only the pinned weights are measured
        llm::AdmissionOptions admission;
        admission.cpuFallback = backend != "gpu";
        admission.allowLargeOnCpu = backend == "cpu";
        llm::SystemProbe probe = llm::probeSystem();
        if (backend == "cpu") {
            probe.gpus.clear(); // measure the CPU path even with a GPU
        }
        const llm::Admission a = llm::admit(*tier, info, probe, admission);
        if (!a.fits) {
            UNSCOPED_INFO(tierId << " does not fit here: " << a.reason);
            appendMarkdown("| " + tierId + " | " + info.id + " | - | - | not run: " + a.reason + " | | | | | | | | | |");
            continue;
        }
        llm::LaunchPlan plan;
        plan.runtimeId = a.runtimeId;
        plan.ramNeeded = a.ramNeeded;
        plan.vramNeeded = a.vramNeeded;
        plan.threads = a.threads;
        plan.gpu = a.gpu;
        plan.reason = a.reason;
        plan.context = a.context;
        plan.reasoningOff = tier->hybridThinking;
        const auto executable = std::filesystem::path(*llama) / (a.gpu ? "vulkan" : "cpu") / "llama-server.exe";
        REQUIRE(std::filesystem::exists(executable));

        net::QtHttpTransport http;
        llm::LocalRuntime runtime(http);
        llm::LocalRuntime::Options options;
        options.startupTimeout = std::chrono::minutes(5);
        options.idleTimeout = std::chrono::hours(2);
        const std::uint64_t gpuFreeBefore = llm::probeSystem().gpuFreeBytes();
        const auto loadStart = std::chrono::steady_clock::now();
        auto started = runtime.start(executable, weights, info, plan, {}, options);
        INFO((started ? std::string("started") : started.error().message));
        REQUIRE(started);
        const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count();
        const std::uint64_t gpuFreeAfter = llm::probeSystem().gpuFreeBytes();
        const ModelRegistry& registry = ModelRegistry::builtin();
        const ModelSpec* spec = registry.model(info.id);
        REQUIRE(spec != nullptr);
        auto adapter = net::makeChatCompletionsChat({*registry.provider(spec->provider), *spec, runtime.status().baseUrl, runtime.key(), http});
        runtime.setBusy(true);
        for (const std::string& mode : modes) {
            std::fprintf(stderr, "%s (%s, %s, %s):\n", tierId.c_str(), info.id.c_str(), a.gpu ? "GPU" : "CPU", mode.c_str());
            RunOptions o = runOptions;
            o.constrained = mode == "constrained";
            const EvalSummary s = runEval([&](const EvalCase&, const Fixture&) -> IChatModel& { return *adapter; }, o);
            const ProcessMemory memory = memoryOf(runtime.processId());
            const double gpuUsed = gpuFreeBefore > gpuFreeAfter ? gb(gpuFreeBefore - gpuFreeAfter) : 0.0;
            Json report = s.toJson();
            report["tier"] = tierId;
            report["sha256"] = info.sha256;
            report["runtime"] = "llama.cpp b11429 " + plan.runtimeId;
            report["backend"] = a.gpu ? "gpu" : "cpu";
            report["threads"] = plan.threads;
            report["context"] = plan.context;
            report["admission"] = {{"ramNeededGB", gb(a.ramNeeded)}, {"vramNeededGB", gb(a.vramNeeded)}, {"reason", a.reason}};
            report["loadSeconds"] = loadSeconds;
            report["peakWorkingSetGB"] = gb(memory.peakWorkingSet);
            report["privateGB"] = gb(memory.privateBytes);
            report["gpuMemoryUsedGB"] = gpuUsed;
            report["machine"] = {{"cpuThreads", probe.cpuThreads}, {"ramTotalGB", gb(probe.ramTotal)}, {"ramFreeGB", gb(probe.ramAvailable)},
                                 {"gpu", probe.bestGpu() ? probe.bestGpu()->name : std::string()}};
            REQUIRE(io::writeFileAtomically(outputDir() / ("local-eval-" + tierId + "-" + mode + ".json"), report.dump(2)));
            appendMarkdown("| " + tierId + " | " + info.id + " | " + (a.gpu ? "GPU (Vulkan)" : "CPU, " + std::to_string(plan.threads) + " threads") +
                           " | " + mode + " | " + percent(s.successRate()) + " (" + std::to_string(s.passed()) + "/" +
                           std::to_string(s.cases.size()) + ") | " + percent(s.validToolCallRate()) + " | " + percent(s.routingRecall()) +
                           " | " + fixed(s.medianStepSeconds()) + " s | " + fixed(s.p90StepSeconds()) + " s | " +
                           fixed(s.medianFirstTokenSeconds(), 2) + " s | " + fixed(s.decodeTokensPerSecond()) + " tok/s | " +
                           fixed(gb(memory.peakWorkingSet)) + " GB | " + fixed(gb(memory.privateBytes)) + " GB | " +
                           (a.gpu ? fixed(gpuUsed) + " GB" : std::string("-")) + " |");
            ++ran;
            if (mode == "constrained" && !s.cases.empty()) {
                CHECK(s.validToolCallRate() >= 0.9); // the grammar makes every call parse and name an offered tool
            }
        }
        runtime.setBusy(false);
        runtime.stop();
    }
    if (ran == 0) {
        SKIP("no tier was downloaded or fits here");
    }
}

TEST_CASE("Cloud reference on the same eval set (the user's own key only)", "[ai][eval][perf][live][cloud]") {
    const auto modelId = systemEnv("WABCUT_EVAL_CLOUD");
    if (systemEnv("CI") || !modelId) {
        SKIP("WABCUT_EVAL_CLOUD is not set: no cloud reference");
    }
    const ModelSpec* spec = ModelRegistry::builtin().model(*modelId);
    REQUIRE(spec != nullptr);
    std::string variable = "WABCUT_LIVE_" + spec->provider + "_KEY";
    for (char& c : variable) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const auto key = systemEnv(variable.c_str());
    if (!key) {
        SKIP(variable + " is not set: the cloud reference runs only with the user's own key");
    }
    ensureCoreApp();
    RequestLog log;
    net::NetworkStack stack(log);
    const ProviderSpec& provider = *ModelRegistry::builtin().provider(spec->provider);
    auto context = [&] { return ProviderContext{provider, *spec, provider.baseUrl, Secret(*key), stack.transport()}; };
    const std::string api = spec->api.empty() ? provider.api : spec->api;
    std::unique_ptr<IChatModel> chat;
    if (api == "anthropic-messages") {
        chat = net::makeAnthropicChat(context());
    } else if (api == "openai-responses") {
        chat = net::makeOpenAiResponsesChat(context());
    } else if (api == "gemini-interactions") {
        chat = net::makeGeminiInteractionsChat(context());
    } else {
        chat = net::makeChatCompletionsChat(context());
    }
    RunOptions o;
    o.only = listOf(systemEnv("WABCUT_EVAL_ONLY"), {});
    o.progress = [](const CaseResult& r) { std::fprintf(stderr, "  %s\n", progressLine(r).c_str()); };
    const EvalSummary s = runEval([&](const EvalCase&, const Fixture&) -> IChatModel& { return *chat; }, o);
    Json report = s.toJson();
    report["cloud"] = true;
    REQUIRE(io::writeFileAtomically(outputDir() / ("cloud-eval-" + *modelId + ".json"), report.dump(2)));
    appendMarkdown("| cloud | " + *modelId + " | " + provider.id + " | " + s.mode + " | " + percent(s.successRate()) + " (" +
                   std::to_string(s.passed()) + "/" + std::to_string(s.cases.size()) + ") | " + percent(s.validToolCallRate()) + " | " +
                   percent(s.routingRecall()) + " | " + fixed(s.medianStepSeconds()) + " s | " + fixed(s.p90StepSeconds()) + " s | " +
                   fixed(s.medianFirstTokenSeconds(), 2) + " s | - | - | - | - |");
}
