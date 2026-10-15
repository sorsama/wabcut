// Explicitly opt-in, never CI: WABCUT_LIVE_LOCAL_LLM=1 and WABCUT_CACHE_DIR containing pinned verified assets.
#include "TimelineFixture.h"
#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>
#include <wabcut/model/ProjectIO.h>
#include <QCoreApplication>
#include <catch2/catch_test_macros.hpp>
using namespace wabcut;
using namespace wabcut::ai;
TEST_CASE("Real managed Qwen local agent fixture-task evaluation", "[ai][llm][live][eval]") {
    if (systemEnv("CI") || systemEnv("WABCUT_LIVE_LOCAL_LLM") != std::optional<std::string>("1")) {
        SKIP("Opt-in local model evaluation disabled (never CI)");
    }
    if (!QCoreApplication::instance()) {
        static int argc = 1; static char name[] = "wabcut_local_eval"; static char* argv[]{name, nullptr};
        static QCoreApplication app(argc, argv);
    }
    const auto* info = findModel("qwen3-4b-instruct-2507"); REQUIRE(info);
    ModelStore store(ModelStore::defaultRoot());
    REQUIRE(store.isInstalled(*info)); REQUIRE(store.verify(*info));
    auto plan = llm::planLaunch(*info, llm::probeSystem()); INFO((plan ? "memory fits" : plan.error().message)); REQUIRE(plan);
    // Evaluate the cached official CPU binary so the GPU remains available for the editor's preview.
    plan->runtimeId = "llama-server-win-cpu-x64"; plan->gpu = false;
    auto root = systemEnv("SystemRoot"); REQUIRE(root);
    auto executable = llm::installRuntime(store, *plan, nullptr, std::filesystem::path(*root) / "System32" / "tar.exe", {});
    INFO((executable ? "installed" : executable.error().message)); REQUIRE(executable);
    net::QtHttpTransport http; llm::LocalRuntime runtime(http);
    auto started = runtime.start(*executable, store.pathFor(*info), *info, *plan, {}, {});
    INFO((started ? "started" : started.error().message)); REQUIRE(started);
    const auto& registry = ModelRegistry::builtin(); const auto* model = registry.model(info->id); REQUIRE(model);
    auto adapter = net::makeChatCompletionsChat({*registry.provider(model->provider), *model, runtime.status().baseUrl, runtime.key(), http});
    Json rows = Json::array(); int passed = 0; int calls = 0; int invalid = 0;
    const std::vector<std::string> tasks{
        "Add exactly one marker named Intro at 00:00:00:05. Read the sequence first, then make the edit.",
        "Add exactly one marker named Ending at 00:00:00:12. Do not alter any clip.",
        "Use sequence_summary to inspect this sequence, then finish without editing anything."};
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        wabcut::test::Timeline tl("V1 tgt |Aaaaaaaaaaaaaaaaaaaa|\nA1 tgt |Aaaaaaaaaaaaaaaaaaaa|");
        agent::AgentRequest request; request.prompt = tasks[i]; request.sequence = tl.seq;
        request.base = std::make_shared<const ProjectState>(tl.state);
        agent::AgentOptions options; options.limits.maxSteps = 5; options.limits.timeout = std::chrono::minutes(4);
        const auto begin = std::chrono::steady_clock::now();
        runtime.setBusy(true); auto result = agent::EditAgent(options).run(*adapter, request, {}, {}); runtime.setBusy(false);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const auto* seq = result.staged ? result.staged->findSequence(tl.seq) : nullptr;
        bool success = seq && result.toolCalls > 0;
        if (seq) success = success && seq->videoTracks == tl.sequence().videoTracks && seq->audioTracks == tl.sequence().audioTracks;
        bool readSummary = false; bool readBeforeEdit = false; Json toolResults = Json::array();
        for (const auto& step : result.steps) {
            if (step.tool == "sequence.summary" && step.ok) readSummary = true;
            if (step.tool == "marker.add" && step.ok) readBeforeEdit = readSummary;
            if (!step.tool.empty()) toolResults.push_back({{"tool", step.tool}, {"ok", step.ok}});
        }
        if (i < 2) success = success && result.status == agent::AgentStatus::Staged && seq->markers.size() == 1 &&
            seq->markers[0].start == wabcut::test::f(i == 0 ? 5 : 12) && seq->markers[0].name == (i == 0 ? "Intro" : "Ending");
        else success = success && readSummary && result.status == agent::AgentStatus::NoChanges && seq->markers.empty();
        if (i == 0) success = success && readBeforeEdit;
        int errors = 0; for (const auto& step : result.steps) errors += step.kind == agent::AgentStep::Kind::Error ? 1 : 0;
        passed += success ? 1 : 0; calls += result.toolCalls; invalid += errors;
        rows.push_back({{"task", i + 1}, {"passed", success}, {"status", agent::toString(result.status)},
            {"toolCalls", result.toolCalls}, {"toolErrors", errors}, {"tools", toolResults}, {"modelCalls", result.modelCalls}, {"seconds", elapsed}, {"error", result.error}});
        UNSCOPED_INFO(rows.back().dump());
    }
    runtime.stop();
    Json report{{"model", info->id}, {"sha256", info->sha256}, {"runtime", "b11429 CPU"}, {"context", 8192},
        {"tasks", rows}, {"passed", passed}, {"total", tasks.size()}, {"toolCalls", calls}, {"toolErrors", invalid},
        {"cloud", "not measured: paid provider calls unauthorized"}};
    const auto reportPath = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-local-eval.json";
    REQUIRE(io::writeFileAtomically(reportPath, report.dump(2)));
    CHECK(passed == static_cast<int>(tasks.size()));
}
