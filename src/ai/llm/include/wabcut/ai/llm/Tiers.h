#pragma once

// Local assistant tiers (AI-15, docs/ai.md "Local assistant tiers"): allow-listed (Apache-2.0 / MIT) instruct models in
// GGUF at three sizes, each admitted by the memory it needs on THIS computer, now:
//
//   small   Qwen3 1.7B (Q4_K_M, 1.1 GB)          runs in about 3 GB: on a busy machine, or on the CPU
//   medium  Qwen3 4B Instruct 2507 (Q4_K_M, 2.5 GB)
//   large   Qwen3 8B (Q4_K_M, 5.0 GB)
//
// Admission: the weights, the KV cache of the tier's context (layers x 2 x KV heads x head size x 2 bytes x tokens) and
// llama.cpp's compute buffers, on the GPU (the weights and cache in its free memory; a little RAM for the process) or
// on the CPU (all of it in free RAM), plus a reserve for the editor. Unknown memory fails closed. The recommendation is
// the largest tier that fits on the GPU; without a usable GPU, the largest that fits in RAM and still answers at a
// usable speed on the CPU threads there are (the large tier never runs on the CPU by default).

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/llm/SystemProbe.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::llm {

struct TierSpec {
    std::string_view id;       ///< "small", "medium", "large"
    std::string_view name;     ///< "Small: Qwen3 1.7B"
    std::string_view model;    ///< ModelStore / registry id
    double paramsB = 0.0;      ///< billions of parameters
    int layers = 0;            ///< transformer blocks
    int kvHeads = 0;           ///< key / value heads (grouped-query attention)
    int headDim = 0;
    int context = 8192;        ///< tokens the tier runs with
    bool hybridThinking = false; ///< Qwen3 hybrid model: llama-server runs it with --reasoning off
};

/// The tiers, smallest first.
std::span<const TierSpec> tiers();
const TierSpec* findTier(std::string_view id);
/// The tier of a model id (nullptr for models that are not a tier, e.g. Phi-4 mini).
const TierSpec* tierForModel(std::string_view modelId);

/// The KV cache of `tier` at its context, f16 keys and values.
std::uint64_t kvCacheBytes(const TierSpec& tier, int context = 0);

struct AdmissionOptions {
    bool cpuFallback = true;      ///< run on the CPU when the GPU cannot take the tier
    bool cudaAccepted = false;    ///< the user accepted NVIDIA's CUDA runtime terms (else Vulkan on NVIDIA)
    std::uint64_t reserveBytes = 1ull << 30; ///< free RAM left for the editor (the runtime pauses for playback/export)
    bool allowLargeOnCpu = false; ///< the large tier on the CPU (slow; off unless asked)
};

/// What one tier would need here, and whether it fits.
struct Admission {
    const TierSpec* tier = nullptr;
    bool fits = false;
    bool gpu = false;
    std::string runtimeId;        ///< llama-server build (ModelStore id)
    std::uint64_t weightsBytes = 0;
    std::uint64_t kvBytes = 0;
    std::uint64_t ramNeeded = 0;  ///< free RAM it takes
    std::uint64_t vramNeeded = 0; ///< free GPU memory it takes (0 on the CPU)
    int context = 0;
    int threads = 1;
    std::string reason;           ///< why it fits where it does, or why not
};

Admission admit(const TierSpec& tier, const ModelInfo& model, const SystemProbe& probe, const AdmissionOptions& options = {});

struct Recommendation {
    std::optional<std::string> tier; ///< nullopt: nothing fits
    std::vector<Admission> tiers;    ///< every tier, smallest first
    std::string reason;
};

/// The tier to suggest on this computer now (models need not be downloaded: sizes come from the manifest).
Recommendation recommend(const SystemProbe& probe, const AdmissionOptions& options = {});
/// The model to use when none is named (the app's default chat model, llm.local.start without a model): the recommended
/// tier's, else the small tier's - it fits the most machines.
std::string_view defaultModel(const SystemProbe& probe, const AdmissionOptions& options = {});

} // namespace wabcut::ai::llm
