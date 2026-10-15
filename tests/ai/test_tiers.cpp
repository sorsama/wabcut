// AI-15: local assistant tiers - the allow-listed models, admission by memory on this computer, the recommendation,
// and the launch they lead to (context, thinking off, no RAM prompt cache).
#include "FakeTransport.h"

#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <thread>

using namespace wabcut::ai;
using namespace wabcut::ai::test;

namespace {

constexpr std::uint64_t GiB = std::uint64_t{1} << 30;

llm::SystemProbe desktop(std::uint64_t ramFree, std::uint64_t gpuFree, int threads = 16) {
    llm::SystemProbe p;
    p.os = "windows";
    p.arch = "x64";
    p.cpuThreads = threads;
    p.ramTotal = 32 * GiB;
    p.ramAvailable = ramFree;
    p.vulkanLoader = true;
    p.cudaDriverVersion = 13000;
    p.gpus.push_back({"NVIDIA GeForce RTX 3060", llm::GpuVendor::Nvidia, 12 * GiB, gpuFree, false});
    return p;
}

const llm::Admission& tierIn(const llm::Recommendation& r, std::string_view id) {
    for (const auto& a : r.tiers) {
        if (a.tier->id == id) {
            return a;
        }
    }
    FAIL("no tier " << id);
    return r.tiers.front();
}

} // namespace

TEST_CASE("Tiers are allow-listed GGUF models of the catalog smallest first", "[ai][llm][tiers]") {
    REQUIRE(llm::tiers().size() == 3);
    const ModelRegistry& registry = ModelRegistry::builtin();
    double last = 0.0;
    for (const llm::TierSpec& t : llm::tiers()) {
        INFO(t.id);
        const ModelInfo* m = findModel(t.model);
        REQUIRE(m != nullptr);
        CHECK(m->kind == ModelKind::Llm);
        CHECK((m->license == "Apache-2.0" || m->license == "MIT"));
        CHECK(isAllowedModelLicense(m->license));
        CHECK(m->sha256.size() == 64);
        CHECK(m->url.starts_with("https://huggingface.co/"));
        CHECK(m->url.find("/resolve/") != std::string::npos); // pinned to a revision, not "main"
        CHECK(m->url.find("/main/") == std::string::npos);
        CHECK(t.paramsB > last);
        last = t.paramsB;
        CHECK(llm::tierForModel(t.model) == &t);
        CHECK(llm::findTier(t.id) == &t);
        const ModelSpec* spec = registry.model(std::string(t.model));
        REQUIRE(spec != nullptr);
        CHECK(spec->provider == "local-llm");
        CHECK(spec->caps.toolsOnChat);
        CHECK(spec->caps.constrainedTools);
        CHECK(spec->contextTokens == t.context);
    }
    CHECK(llm::tierForModel("phi-4-mini-instruct") == nullptr);
    CHECK(llm::findTier("huge") == nullptr);
    // Shapes from the GGUF headers: 28 blocks x 8 KV heads x 128 for the 1.7B model, f16, both keys and values.
    CHECK(llm::kvCacheBytes(*llm::findTier("small")) == 28ull * 2 * 8 * 128 * 2 * 6144);
    CHECK(llm::kvCacheBytes(*llm::findTier("large"), 4096) == 36ull * 2 * 8 * 128 * 2 * 4096);
}

TEST_CASE("Tier admission: the GPU when its free memory holds the tier or else the CPU within free RAM", "[ai][llm][tiers]") {
    SECTION("an idle 12 GB GPU takes every tier and the large one is recommended") {
        const auto r = llm::recommend(desktop(5 * GiB, 10 * GiB));
        REQUIRE(r.tier);
        CHECK(*r.tier == "large");
        for (const auto& a : r.tiers) {
            INFO(a.tier->id << ": " << a.reason);
            CHECK(a.fits);
            CHECK(a.gpu);
            CHECK(a.runtimeId == "llama-server-win-vulkan-x64"); // CUDA's terms not accepted
            CHECK(a.vramNeeded > a.weightsBytes + a.kvBytes);
            CHECK(a.ramNeeded < 2 * GiB); // the weights live on the GPU
        }
    }
    SECTION("a busy machine (GPU full, 4-5 GB of RAM free while building) still runs the small tier on the CPU") {
        for (const std::uint64_t ram : {4 * GiB, 5 * GiB}) {
            const auto r = llm::recommend(desktop(ram, 1 * GiB));
            INFO(r.reason);
            REQUIRE(r.tier);
            CHECK(*r.tier == "small");
            const auto& small = tierIn(r, "small");
            CHECK(small.fits);
            CHECK_FALSE(small.gpu);
            CHECK(small.runtimeId == "llama-server-win-cpu-x64");
            CHECK(small.ramNeeded <= ram);
            CHECK(small.threads == 8);
            CHECK_FALSE(tierIn(r, "large").fits); // never on the CPU by default
        }
        CHECK_FALSE(tierIn(llm::recommend(desktop(5 * GiB, 1 * GiB)), "medium").fits);
        // From about 5.5 GB free the medium tier fits on the CPU too, and sixteen threads answer at a usable speed.
        CHECK(llm::recommend(desktop(7 * GiB, 1 * GiB)).tier == "medium");
    }
    SECTION("without a GPU the medium tier needs eight CPU threads to be recommended") {
        auto probe = desktop(24 * GiB, 0);
        probe.gpus.clear();
        CHECK(llm::recommend(probe).tier == "medium");
        probe.cpuThreads = 4;
        CHECK(llm::recommend(probe).tier == "small");
        llm::AdmissionOptions options;
        options.allowLargeOnCpu = true;
        const auto large = llm::admit(*llm::findTier("large"), *findModel("qwen3-8b"), probe, options);
        CHECK(large.fits);
        CHECK_FALSE(large.gpu);
    }
    SECTION("unknown or missing memory fails closed") {
        auto probe = desktop(0, 0);
        const auto r = llm::recommend(probe);
        CHECK_FALSE(r.tier);
        CHECK(r.reason.find("RAM") != std::string::npos);
        llm::AdmissionOptions noFallback;
        noFallback.cpuFallback = false;
        const auto a = llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), desktop(16 * GiB, 0), noFallback);
        CHECK_FALSE(a.fits);
        CHECK(a.reason.find("CPU fallback disabled") != std::string::npos);
    }
    SECTION("two GPUs, an integrated GPU or no Vulkan loader: the CPU") {
        auto two = desktop(16 * GiB, 10 * GiB);
        two.gpus.push_back(two.gpus.front());
        CHECK_FALSE(llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), two).gpu);
        auto integrated = desktop(16 * GiB, 10 * GiB);
        integrated.gpus.front().integrated = true;
        CHECK_FALSE(llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), integrated).gpu);
        auto noVulkan = desktop(16 * GiB, 10 * GiB);
        noVulkan.vulkanLoader = false;
        CHECK_FALSE(llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), noVulkan).gpu);
        llm::AdmissionOptions cuda;
        cuda.cudaAccepted = true;
        noVulkan.cudaDriverVersion = 12040;
        CHECK(llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), noVulkan, cuda).runtimeId == "llama-server-win-cuda-12.4-x64");
    }
    SECTION("licences outside the allow-list and other platforms are refused") {
        auto bad = *findModel("qwen3-1.7b");
        bad.license = "LicenseRef-Qwen-Research";
        CHECK_FALSE(llm::admit(*llm::findTier("small"), bad, desktop(16 * GiB, 10 * GiB)).fits);
        auto mac = desktop(16 * GiB, 10 * GiB);
        mac.os = "macos";
        CHECK_FALSE(llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), mac).fits);
    }
}

TEST_CASE("Launch plans carry the tier context and turn the hybrid models thinking off", "[ai][llm][tiers]") {
    const auto probe = desktop(16 * GiB, 10 * GiB);
    const auto small = llm::planLaunch(*findModel("qwen3-1.7b"), probe);
    REQUIRE(small);
    CHECK(small->context == 6144);
    CHECK(small->reasoningOff);
    CHECK(small->gpu);
    const auto medium = llm::planLaunch(*findModel("qwen3-4b-instruct-2507"), probe);
    REQUIRE(medium);
    CHECK(medium->context == 8192);
    CHECK_FALSE(medium->reasoningOff); // an instruct model without a thinking mode
    const auto large = llm::planLaunch(*findModel("qwen3-8b"), probe);
    REQUIRE(large);
    CHECK(large->reasoningOff);
    // The user picked the model: a large one may still run on the CPU when RAM allows (slowly).
    auto cpuOnly = desktop(24 * GiB, 0);
    const auto largeCpu = llm::planLaunch(*findModel("qwen3-8b"), cpuOnly);
    REQUIRE(largeCpu);
    CHECK_FALSE(largeCpu->gpu);
    const auto phi = llm::planLaunch(*findModel("phi-4-mini-instruct"), probe);
    REQUIRE(phi);
    CHECK(phi->context == 8192);
    CHECK_FALSE(phi->reasoningOff);
}

TEST_CASE("llama-server starts with the tier context and no RAM prompt cache and thinking off", "[ai][llm][tiers][process]") {
    FakeTransport fake;
    llm::LocalRuntime runtime(fake);
    const auto model = *findModel("qwen3-1.7b");
    auto plan = llm::planLaunch(model, desktop(4 * GiB, 0));
    REQUIRE(plan);
    const auto weights = uniqueTempPath("record-args-weights", ".gguf");
    std::ofstream(weights) << "fixture";
    fake.pushJson(200, Json{{"data", Json::array({{{"id", model.id}}})}}.dump());
    llm::LocalRuntime::Options options;
    options.startupTimeout = std::chrono::seconds(10);
    const auto started = runtime.start(WABCUT_FAKE_LLAMA, weights, model, *plan, {}, options);
    INFO((started ? std::string() : started.error().message));
    REQUIRE(started);
    runtime.stop();
    std::vector<std::string> args;
    {
        std::ifstream in(weights.string() + ".args");
        for (std::string line; std::getline(in, line);) {
            args.push_back(line);
        }
    }
    auto valueOf = [&](std::string_view flag) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == flag) {
                return args[i + 1];
            }
        }
        return std::string("(missing)");
    };
    CHECK(valueOf("--ctx-size") == "6144");
    CHECK(valueOf("--reasoning") == "off");
    CHECK(valueOf("--cache-ram") == "0");
    CHECK(valueOf("--gpu-layers") == "0");
    CHECK(valueOf("--threads") == "8");
    CHECK(valueOf("--host") == "127.0.0.1");
    CHECK(valueOf("--api-key") == "<key>");
    std::error_code ec;
    std::filesystem::remove(weights, ec);
    std::filesystem::remove(weights.string() + ".args", ec);
}
