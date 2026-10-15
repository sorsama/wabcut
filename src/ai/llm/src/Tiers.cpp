#include <wabcut/ai/llm/Tiers.h>

#include <algorithm>
#include <array>

namespace wabcut::ai::llm {
namespace {

constexpr std::uint64_t MiB = std::uint64_t{1} << 20;

// Shapes from the GGUF headers (block_count, attention.head_count_kv, attention.key_length); the contexts are what the
// tiers run with, not the models' maximum (the assistant offers 8 tools at 6k, 12 at 8k: agent/ToolRouter.h).
constexpr std::array kTiers{
    TierSpec{"small", "Small: Qwen3 1.7B", "qwen3-1.7b", 1.7, 28, 8, 128, 6144, true},
    TierSpec{"medium", "Medium: Qwen3 4B Instruct", "qwen3-4b-instruct-2507", 4.0, 36, 8, 128, 8192, false},
    TierSpec{"large", "Large: Qwen3 8B", "qwen3-8b", 8.2, 36, 8, 128, 8192, true},
};

// llama.cpp's compute buffers at the default 512-token micro-batch (measured: 0.3-0.5 GB for these models), and what
// llama-server itself takes in RAM besides the weights (the program, its backends, the tokenizer).
constexpr std::uint64_t kComputeBytes = 512 * MiB;
constexpr std::uint64_t kProcessBytes = 512 * MiB;
// Free GPU memory left for the editor's own textures and the driver after the model is loaded.
constexpr std::uint64_t kVramMargin = 512 * MiB;

std::string gb(std::uint64_t bytes) { return formatBytes(bytes); }

} // namespace

std::span<const TierSpec> tiers() { return kTiers; }

const TierSpec* findTier(std::string_view id) {
    for (const auto& t : kTiers)
        if (t.id == id) return &t;
    return nullptr;
}

const TierSpec* tierForModel(std::string_view modelId) {
    for (const auto& t : kTiers)
        if (t.model == modelId) return &t;
    return nullptr;
}

std::uint64_t kvCacheBytes(const TierSpec& tier, int context) {
    const auto tokens = static_cast<std::uint64_t>(context > 0 ? context : tier.context);
    return static_cast<std::uint64_t>(tier.layers) * 2u * static_cast<std::uint64_t>(tier.kvHeads) *
           static_cast<std::uint64_t>(tier.headDim) * 2u * tokens;
}

Admission admit(const TierSpec& tier, const ModelInfo& model, const SystemProbe& probe, const AdmissionOptions& options) {
    Admission a;
    a.tier = &tier;
    a.context = tier.context;
    a.weightsBytes = model.size;
    a.kvBytes = kvCacheBytes(tier);
    if (model.kind != ModelKind::Llm || (model.license != "MIT" && model.license != "Apache-2.0")) {
        a.reason = "only MIT or Apache-2.0 GGUF models are allowed";
        return a;
    }
    if (probe.os != "windows" || probe.arch != "x64") {
        a.reason = "managed llama-server currently supports Windows x64";
        return a;
    }
    const std::uint64_t onDevice = a.weightsBytes + a.kvBytes + kComputeBytes;

    // The GPU: the weights, cache and buffers in its free memory, the program in RAM. Multi-GPU selection is ambiguous:
    // do not assume llama's device zero is the adapter we measured.
    const auto* gpu = probe.bestGpu();
    const std::uint64_t gpuRam = kProcessBytes + options.reserveBytes;
    if (probe.gpus.size() == 1 && gpu && !gpu->integrated && probe.gpuFreeBytes() >= onDevice + kVramMargin &&
        probe.ramAvailable >= gpuRam) {
        if (gpu->vendor == GpuVendor::Nvidia && probe.cudaDriverVersion >= 12040 && options.cudaAccepted)
            a.runtimeId = "llama-server-win-cuda-12.4-x64";
        else if (probe.vulkanLoader)
            a.runtimeId = "llama-server-win-vulkan-x64";
        if (!a.runtimeId.empty()) {
            a.fits = a.gpu = true;
            a.vramNeeded = onDevice + kVramMargin;
            a.ramNeeded = gpuRam;
            a.threads = std::clamp(probe.cpuThreads / 4, 1, 4);
            a.reason = "on the GPU (" + gb(a.vramNeeded) + " of " + gb(probe.gpuFreeBytes()) + " free)";
            return a;
        }
    }

    // The CPU: everything in free RAM, with the editor's reserve on top.
    a.ramNeeded = onDevice + kProcessBytes + options.reserveBytes;
    a.vramNeeded = 0;
    a.threads = std::clamp(probe.cpuThreads / 2, 1, 8);
    const std::string gpuNote = gpu && probe.gpus.size() == 1 && !gpu->integrated
        ? "the GPU has " + gb(probe.gpuFreeBytes()) + " free, " + gb(onDevice + kVramMargin) + " needed"
        : "no single discrete GPU with a known free memory";
    if (!options.cpuFallback) {
        a.reason = "insufficient verified GPU memory (" + gpuNote + "); CPU fallback disabled";
        return a;
    }
    if (tier.id == "large" && !options.allowLargeOnCpu) {
        a.reason = "the large tier runs on a GPU only (" + gpuNote + ")";
        return a;
    }
    if (probe.ramAvailable < a.ramNeeded) {
        a.reason = "not enough free RAM: " + gb(a.ramNeeded) + " needed on the CPU, " + gb(probe.ramAvailable) + " free";
        return a;
    }
    a.fits = true;
    a.runtimeId = "llama-server-win-cpu-x64";
    a.reason = "on the CPU, " + std::to_string(a.threads) + " threads (" + gpuNote + ")";
    return a;
}

Recommendation recommend(const SystemProbe& probe, const AdmissionOptions& options) {
    Recommendation r;
    const Admission* best = nullptr;
    for (const auto& tier : kTiers) {
        const ModelInfo* model = findModel(tier.model);
        if (!model) continue;
        r.tiers.push_back(admit(tier, *model, probe, options));
    }
    // The largest tier on the GPU; on the CPU, the medium tier only with enough threads to answer at a usable speed
    // (eight or more: about 8 tokens a second on a desktop CPU), else the small one.
    for (const auto& a : r.tiers) {
        if (!a.fits) continue;
        if (!a.gpu && a.tier->id == "medium" && probe.cpuThreads < 8) continue;
        if (!best || a.gpu || !best->gpu) best = &a;
    }
    if (!best) {
        r.reason = r.tiers.empty() ? "no local assistant model in the catalog" : r.tiers.front().reason;
        return r;
    }
    r.tier = std::string(best->tier->id);
    r.reason = std::string(best->tier->name) + " " + best->reason;
    return r;
}

std::string_view defaultModel(const SystemProbe& probe, const AdmissionOptions& options) {
    const Recommendation r = recommend(probe, options);
    const TierSpec* tier = r.tier ? findTier(*r.tier) : nullptr;
    return tier ? tier->model : kTiers.front().model;
}

} // namespace wabcut::ai::llm
