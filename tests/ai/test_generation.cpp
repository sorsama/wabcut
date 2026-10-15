#include "FakeTransport.h"
#include <wabcut/ai/Generation.h>
#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/model/ProjectIO.h>
#include <catch2/catch_test_macros.hpp>
using namespace wabcut;
using namespace wabcut::ai;
TEST_CASE("Generation prices are explicit and duration requests cannot underbill", "[ai][generation]") {
    const auto registry = ModelRegistry::builtin();
    const auto* model = registry.model("veo-3.1-fast-generate-preview");
    REQUIRE(model);
    MediaGenerationRequest r;
    r.prompt = "aerial city"; r.duration = Ticks::fromSeconds(5);
    REQUIRE(checkGenerationRequest(*model, r));
    CHECK(generatedSeconds(model->generation, r.duration) == 6);
    auto cost = estimateGenerationCost(*model, r, todayUtc());
    CHECK(cost.priced); CHECK(cost.ceiling == 600000);
    r.duration = Ticks::fromSeconds(9); CHECK_FALSE(checkGenerationRequest(*model, r));
    r.duration = Ticks::fromSeconds(4); r.extra = {{"outputFormat", "prores"}};
    CHECK_FALSE(checkGenerationRequest(*model, r));
}
TEST_CASE("Local resource governor preserves playback reserves and refuses unknown memory", "[ai][llm]") {
    const auto* model = findModel("qwen3-4b-instruct-2507"); REQUIRE(model);
    llm::SystemProbe probe; probe.os = "windows"; probe.arch = "x64"; probe.cpuThreads = 16;
    probe.ramAvailable = 16ULL << 30; probe.vulkanLoader = true;
    probe.gpus.push_back({"GPU", llm::GpuVendor::Nvidia, 8ULL << 30, 8ULL << 30, false});
    auto plan = llm::planLaunch(*model, probe); REQUIRE(plan); CHECK(plan->gpu); CHECK(plan->threads == 4);
    probe.gpus[0].freeBytes = 0;
    CHECK(probe.gpuFreeBytes() == 0); CHECK_FALSE(llm::planLaunch(*model, probe, false));
    plan = llm::planLaunch(*model, probe); REQUIRE(plan); CHECK_FALSE(plan->gpu);
    probe.ramAvailable = 1ULL << 30; CHECK_FALSE(llm::planLaunch(*model, probe));
    auto bad = *model; bad.license = "NonCommercial"; CHECK_FALSE(llm::planLaunch(bad, probe));
}
TEST_CASE("Generation provenance retains only a prompt hash and escapes XML", "[ai][provenance]") {
    GeneratedProvenance p;
    p.provider = "a&b"; p.model = "model'one"; p.promptHash = promptHash("private prompt");
    p.createdAt = "2026-10-11T12:00:00Z"; p.kind = "video"; p.operation = "extend-tail";
    const auto xml = provenanceXmp(p);
    CHECK(xml.find("private prompt") == std::string::npos);
    CHECK(xml.find("a&amp;b") != std::string::npos);
    CHECK(xml.find("model&apos;one") != std::string::npos);
    CHECK(xml.find("trainedAlgorithmicMedia") != std::string::npos);
    CHECK(isIso8601Utc(p.createdAt)); CHECK_FALSE(isIso8601Utc("2026-02-30T12:00:00Z"));
    ProjectState state;
    const auto report = provenanceReport(state, {});
    CHECK(report["wabcut:unsigned"] == true);
    CHECK(report["assertions"][1]["data"].empty());
}
#include "TimelineFixture.h"
#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/model/EditSession.h>
TEST_CASE("Generated extensions preserve links, round trip provenance, and undo once", "[ai][generation][provenance]") {
    using namespace wabcut::test;
    Timeline timeline("V1 tgt |....Aaaa....|\nA1 tgt |....Aaaa....|"); timeline.linkMatching();
    const auto originalJson = io::toJson(timeline.state); REQUIRE(originalJson);
    CHECK(originalJson->find("generated") == std::string::npos);
    MediaItem asset = *timeline.state.findMedia(timeline.media('A')); asset.id = {};
    asset.generated = GeneratedProvenance{};
    auto& p = *asset.generated; p.provider = "runway"; p.model = "gen4.5";
    p.promptHash = promptHash("aerial city"); p.createdAt = "2026-10-11T12:00:00Z";
    p.kind = "video"; p.operation = "text-to-video";
    EditSession live(timeline.state);
    auto staged = generation::stageAsset(live.stateForSave(), timeline.seq, asset, f(3), {}, timeline.clip("V1@4"));
    REQUIRE(staged);
    CHECK(links(staged->state, timeline.seq) == "V1@4+V1@8+A1@4");
    auto json = io::toJson(staged->state); REQUIRE(json);
    auto loaded = io::fromJson(*json); REQUIRE(loaded);
    const auto* generated = loaded->findMedia(MediaId{staged->createdIds.front()}); REQUIRE(generated); REQUIRE(generated->generated);
    CHECK(generated->generated->promptHash == p.promptHash);
    CHECK(generated->generated->source == timeline.media('A'));
    CHECK(generated->generated->operation == "extend-tail");
    const auto report = provenanceReport(*loaded, timeline.seq, TimeRange{f(9), f(2)});
    REQUIRE(report["assertions"][1]["data"].size() == 1);
    CHECK(report["assertions"][1]["data"][0]["startTicks"] == 0);
    CHECK(report["assertions"][1]["data"][0]["durationTicks"] == f(2).value);
    REQUIRE(live.commit("Generate extension", staged->state));
    CHECK(live.snapshot()->media.size() == timeline.state.media.size() + 1);
    REQUIRE(live.undo());
    CHECK(live.snapshot()->media.size() == timeline.state.media.size());
    auto invalid = Json::parse(*json); invalid["media"].back()["generated"]["promptHash"] = "bad";
    CHECK_FALSE(io::fromJson(invalid.dump()));
    invalid = Json::parse(*json); invalid["media"].back()["generated"]["provider"] = std::string(129, 'x');
    CHECK_FALSE(io::fromJson(invalid.dump()));
    CHECK_FALSE(generation::stageAsset(timeline.state, timeline.seq, asset, f(5), {}, timeline.clip("V1@4"), true));
    auto occupied = generation::stageAsset(staged->state, timeline.seq, asset, f(3), {}, timeline.clip("V1@4"));
    CHECK_FALSE(occupied);
    const auto output = wabcut::ai::test::uniqueTempPath("provenance-test", ".mp4");
    REQUIRE(writeExportProvenance(output, *loaded, timeline.seq));
    CHECK(std::filesystem::file_size(output.string() + ".provenance.json") > 100);
    REQUIRE(writeProvenanceSidecars(output, *generated->generated));
    CHECK(std::filesystem::file_size(output.string() + ".xmp") > 100);
    std::filesystem::remove(output.string() + ".provenance.json"); std::filesystem::remove(output.string() + ".xmp");
}
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
TEST_CASE("Headless generation explicitly refuses missing approval host with no edits", "[ai][generation][commands]") {
    wabcut::test::Timeline timeline("V1 tgt |Aaaaaaaa|");
    EditSession session(timeline.state);
    command::CommandEngine engine(session, ai::edit::editingCommands());
    const auto before = session.snapshot();
    for (const auto& call : std::vector<Json>{
        {{"command", "gen.broll"}, {"params", {{"prompt", "aerial city"}, {"duration", 4}, {"aspect", "16:9"}}}},
        {{"command", "gen.image"}, {"params", {{"prompt", "paper texture"}, {"size", "720p"}}}},
        {{"command", "gen.extend"}, {"params", {{"clip", timeline.clip("V1@0").value}, {"side", "tail"}, {"seconds", 4}}}}
    }) {
        auto result = engine.executeJson(call);
        REQUIRE_FALSE(result.ok()); REQUIRE(result.error);
        CHECK(result.error->message.find("approve") != std::string::npos);
        CHECK(session.snapshot() == before);
    }
    CHECK(ai::edit::editingCommands().find("gen.broll")->description.find("Standalone CLI/MCP refuses") != std::string::npos);
}
