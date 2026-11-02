// AI-15: local assistant tiers - the allow-listed models, admission by memory on this computer, the recommendation,
// and the launch they lead to (context, thinking off, no RAM prompt cache).
#include "FakeTransport.h"

#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
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
    SECTION("an integrated GPU only or no Vulkan loader: the CPU (two GPUs: see the hybrid-graphics test)") {
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

// 0.6 fix round 2 (D6): a desktop or laptop with an integrated GPU next to its discrete one is the common case. The
// admission runs on the adapter the editor uses (Settings > Graphics card, else the high-performance one), and the
// launch pins llama-server to that card. Fake adapter lists only: no GPU needed.
namespace {

constexpr std::uint64_t MiB = std::uint64_t{1} << 20;

llm::GpuInfo integratedGpu(std::uint64_t luid = 0x11, int highPerformance = 1) {
    return {"Intel(R) UHD Graphics 770", llm::GpuVendor::Intel, 128 * MiB, 2 * GiB, true, luid, highPerformance};
}
llm::GpuInfo rtx3060(std::uint64_t free = 11 * GiB, std::uint64_t luid = 0x22, int highPerformance = 0) {
    return {"NVIDIA GeForce RTX 3060", llm::GpuVendor::Nvidia, 12 * GiB, free, false, luid, highPerformance};
}
llm::GpuInfo rtx4090(std::uint64_t free = 20 * GiB, std::uint64_t luid = 0x33, int highPerformance = 0) {
    return {"NVIDIA GeForce RTX 4090", llm::GpuVendor::Nvidia, 24 * GiB, free, false, luid, highPerformance};
}
llm::SystemProbe machine(std::vector<llm::GpuInfo> gpus) {
    auto p = desktop(16 * GiB, 0);
    p.gpus = std::move(gpus);
    return p;
}
llm::Admission smallOn(const llm::SystemProbe& probe, const llm::AdmissionOptions& options = {}) {
    return llm::admit(*llm::findTier("small"), *findModel("qwen3-1.7b"), probe, options);
}

} // namespace

TEST_CASE("Tier admission on hybrid graphics picks the discrete or chosen GPU and reads its free memory",
          "[ai][llm][tiers][gpu]") {
    SECTION("integrated GPU listed first and a discrete RTX 3060: the 3060 takes every tier") {
        for (const bool orderKnown : {true, false}) {
            auto probe = machine({integratedGpu(0x11, orderKnown ? 1 : -1), rtx3060(11 * GiB, 0x22, orderKnown ? 0 : -1)});
            REQUIRE(probe.bestGpu() != nullptr);
            CHECK(probe.bestGpu()->name == "NVIDIA GeForce RTX 3060");
            CHECK(probe.gpuFreeBytes() == 11 * GiB);
            const auto a = smallOn(probe);
            INFO(a.reason);
            CHECK(a.fits);
            CHECK(a.gpu);
            CHECK(a.gpuName == "NVIDIA GeForce RTX 3060");
            CHECK(a.gpuFreeBytes == 11 * GiB);
            CHECK(a.runtimeId == "llama-server-win-vulkan-x64");
            CHECK(a.reason.find("NVIDIA GeForce RTX 3060") != std::string::npos);
            const auto r = llm::recommend(probe);
            REQUIRE(r.tier);
            CHECK(*r.tier == "large");
            CHECK(tierIn(r, "medium").gpu);
        }
    }
    SECTION("a discrete GPU only: the GPU") {
        const auto a = smallOn(machine({rtx3060()}));
        CHECK(a.gpu);
        CHECK(a.gpuName == "NVIDIA GeForce RTX 3060");
    }
    SECTION("an integrated GPU only: the CPU, and the reason says so") {
        const auto a = smallOn(machine({integratedGpu()}));
        INFO(a.reason);
        CHECK(a.fits);
        CHECK_FALSE(a.gpu);
        CHECK(a.gpuName.empty());
        CHECK(a.reason.find("only an integrated GPU") != std::string::npos);
    }
    SECTION("two discrete GPUs: the high-performance one, else the one with the most free memory") {
        auto probe = machine({rtx3060(3 * GiB, 0x22, 1), rtx4090(20 * GiB, 0x33, 0)});
        CHECK(smallOn(probe).gpuName == "NVIDIA GeForce RTX 4090");
        CHECK(llm::recommend(probe).tier == "large");
        // Windows' order wins over free memory where it is known.
        probe = machine({rtx3060(11 * GiB, 0x22, 0), rtx4090(20 * GiB, 0x33, 1)});
        CHECK(smallOn(probe).gpuName == "NVIDIA GeForce RTX 3060");
        // Without an order: the most free memory.
        probe = machine({rtx3060(11 * GiB, 0x22, -1), rtx4090(20 * GiB, 0x33, -1)});
        CHECK(smallOn(probe).gpuName == "NVIDIA GeForce RTX 4090");
        // Two identical cards: one of them, with its own free memory (the launch tells them apart by it).
        probe = machine({rtx3060(4 * GiB, 0x22, -1), rtx3060(10 * GiB, 0x44, -1)});
        const auto a = smallOn(probe);
        CHECK(a.gpu);
        CHECK(a.gpuFreeBytes == 10 * GiB);
    }
    SECTION("the GPU chosen in Settings: followed, also when it cannot take the tier") {
        auto probe = machine({rtx3060(4 * GiB, 0x22, 1), rtx4090(20 * GiB, 0x33, 0)});
        probe.preferredGpuLuid = 0x22;
        CHECK(probe.bestGpu()->name == "NVIDIA GeForce RTX 3060");
        CHECK(smallOn(probe).gpuName == "NVIDIA GeForce RTX 3060"); // about 2.9 GB fits in 4 GB, the large tier does not
        const auto large = llm::admit(*llm::findTier("large"), *findModel("qwen3-8b"), probe);
        INFO(large.reason);
        CHECK_FALSE(large.fits);
        CHECK(large.reason.find("NVIDIA GeForce RTX 3060 has") != std::string::npos);
        // The integrated GPU chosen for Wabcut on a hybrid machine: the model runs on the CPU, and says why.
        probe = machine({integratedGpu(0x11, 1), rtx3060(11 * GiB, 0x22, 0)});
        probe.preferredGpuLuid = 0x11;
        const auto onIntegrated = smallOn(probe);
        INFO(onIntegrated.reason);
        CHECK_FALSE(onIntegrated.gpu);
        CHECK(onIntegrated.reason.find("chosen for Wabcut") != std::string::npos);
        // A choice that is not in this computer now (a LUID from an earlier boot): the high-performance GPU.
        probe.preferredGpuLuid = 0x99;
        CHECK(smallOn(probe).gpuName == "NVIDIA GeForce RTX 3060");
    }
    SECTION("unknown free memory on the discrete GPU fails closed to the CPU") {
        auto probe = machine({integratedGpu(), rtx3060(0)});
        const auto a = smallOn(probe);
        INFO(a.reason);
        CHECK(a.fits);
        CHECK_FALSE(a.gpu);
        CHECK(a.reason.find("free memory of NVIDIA GeForce RTX 3060 is unknown") != std::string::npos);
        llm::AdmissionOptions noFallback;
        noFallback.cpuFallback = false;
        CHECK_FALSE(smallOn(probe, noFallback).fits);
    }
}

TEST_CASE("The llama.cpp device for the admitted GPU is found by name in its device list", "[ai][llm][tiers][gpu]") {
    const std::string hybrid = "Available devices:\n"
                               "  Vulkan0: Intel(R) UHD Graphics 770 (16270 MiB, 15502 MiB free)\n"
                               "  Vulkan1: NVIDIA GeForce RTX 3060 (12329 MiB, 11551 MiB free)\n";
    CHECK(llm::pickLlamaDevice(hybrid, "NVIDIA GeForce RTX 3060", 11 * GiB) == "Vulkan1");
    CHECK(llm::pickLlamaDevice(hybrid, "Intel(R) UHD Graphics 770", 0) == "Vulkan0");
    CHECK(llm::pickLlamaDevice(hybrid, "intel  uhd graphics 770", 0) == "Vulkan0"); // case, (R) and spacing aside
    CHECK_FALSE(llm::pickLlamaDevice(hybrid, "AMD Radeon RX 7800 XT", 0));
    CHECK_FALSE(llm::pickLlamaDevice(hybrid, "", 0));
    CHECK_FALSE(llm::pickLlamaDevice("Available devices:\n  (none)\n", "NVIDIA GeForce RTX 3060", 0));
    CHECK_FALSE(llm::pickLlamaDevice("", "NVIDIA GeForce RTX 3060", 0));
    // Windows line endings, CUDA names, and two identical cards told apart by their free memory.
    const std::string twin = "Available devices:\r\n"
                             "  CUDA0: NVIDIA GeForce RTX 3060 (12287 MiB, 2048 MiB free)\r\n"
                             "  CUDA1: NVIDIA GeForce RTX 3060 (12287 MiB, 11000 MiB free)\r\n";
    CHECK(llm::pickLlamaDevice(twin, "NVIDIA GeForce RTX 3060", 10 * GiB) == "CUDA1");
    CHECK(llm::pickLlamaDevice(twin, "NVIDIA GeForce RTX 3060", 2 * GiB) == "CUDA0");
}

TEST_CASE("llama-server on a hybrid-graphics machine is pinned to the discrete GPU", "[ai][llm][tiers][gpu][process]") {
    const auto model = *findModel("qwen3-1.7b");
    auto run = [&](const llm::LaunchPlan& plan, std::string& device) {
        FakeTransport fake;
        llm::LocalRuntime runtime(fake);
        const auto weights = uniqueTempPath("record-args-weights", ".gguf");
        std::ofstream(weights) << "fixture";
        fake.pushJson(200, Json{{"data", Json::array({{{"id", model.id}}})}}.dump());
        llm::LocalRuntime::Options options;
        options.startupTimeout = std::chrono::seconds(10);
        const auto started = runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options);
        INFO((started ? std::string() : started.error().message));
        REQUIRE(started);
        device = runtime.status().device;
        runtime.stop();
        std::vector<std::string> args;
        {
            std::ifstream in(weights.string() + ".args");
            for (std::string line; std::getline(in, line);) {
                args.push_back(line);
            }
        }
        std::error_code ec;
        std::filesystem::remove(weights, ec);
        std::filesystem::remove(weights.string() + ".args", ec);
        return args;
    };
    auto valueOf = [](const std::vector<std::string>& args, std::string_view flag) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == flag) {
                return args[i + 1];
            }
        }
        return std::string("(missing)");
    };
    // The fake lists the integrated GPU as Vulkan0 and the RTX 3060 as Vulkan1 (llama.cpp's order, not DXGI's).
    const auto gpuPlan = llm::planLaunch(model, machine({integratedGpu(), rtx3060()}));
    REQUIRE(gpuPlan);
    REQUIRE(gpuPlan->gpu);
    CHECK(gpuPlan->gpuName == "NVIDIA GeForce RTX 3060");
    std::string device;
    const auto gpuArgs = run(*gpuPlan, device);
    CHECK(valueOf(gpuArgs, "--device") == "Vulkan1");
    CHECK(valueOf(gpuArgs, "--gpu-layers") == "99");
    CHECK(device == "Vulkan1");
    // The CPU: no device listing, no --device.
    const auto cpuPlan = llm::planLaunch(model, machine({integratedGpu()}));
    REQUIRE(cpuPlan);
    REQUIRE_FALSE(cpuPlan->gpu);
    const auto cpuArgs = run(*cpuPlan, device);
    CHECK(valueOf(cpuArgs, "--device") == "(missing)");
    CHECK(valueOf(cpuArgs, "--gpu-layers") == "0");
    CHECK(device.empty());
}

TEST_CASE("Diagnostic: this computer's GPUs as the local assistant sees them", "[.][probe]") {
    const llm::SystemProbe probe = llm::probeSystem();
    for (const llm::GpuInfo& g : probe.gpus) {
        std::fprintf(stderr, "%s | %s | dedicated %s | free %s | integrated %d | luid %llx | high-performance #%d\n",
                     g.name.c_str(), std::string(toString(g.vendor)).c_str(), llm::formatBytes(g.dedicatedBytes).c_str(),
                     llm::formatBytes(g.freeBytes).c_str(), g.integrated ? 1 : 0, static_cast<unsigned long long>(g.luid),
                     g.highPerformanceIndex);
    }
    const llm::GpuInfo* best = probe.bestGpu();
    std::fprintf(stderr, "best: %s\n", best ? best->name.c_str() : "none");
    SUCCEED();
}
