#pragma once

// What this computer can run (AI-10, docs/ai.md "Local assistant"): GPUs with their memory, RAM, and whether a
// Vulkan loader and an NVIDIA CUDA driver are present. "Probe, don't assume": the resource governor plans every
// llama-server launch from this, and tests pass a hand-made probe. Nothing here initializes a GPU API in the
// editor's process: Windows reads DXGI adapter descriptions and memory budgets; drivers are only checked for being
// loadable (cuDriverGetVersion needs no cuInit).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::llm {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel, Apple, Other };

std::string_view toString(GpuVendor vendor);

struct GpuInfo {
    std::string name;
    GpuVendor vendor = GpuVendor::Unknown;
    std::uint64_t dedicatedBytes = 0; ///< device memory; 0 = unknown or shared
    /// What the OS budget leaves for a new process now (Windows: DXGI budget minus usage); 0 = unknown.
    std::uint64_t freeBytes = 0;
    bool integrated = false;          ///< shares system memory
};

struct SystemProbe {
    std::string os;   ///< "windows", "linux", "macos"
    std::string arch; ///< "x64", "arm64"
    std::vector<GpuInfo> gpus;
    std::uint64_t ramTotal = 0;
    std::uint64_t ramAvailable = 0;
    int cpuThreads = 0;
    bool vulkanLoader = false;  ///< vulkan-1.dll / libvulkan.so.1 can be loaded
    int cudaDriverVersion = 0;  ///< cuDriverGetVersion(), e.g. 13040 for CUDA 13.4; 0 = no NVIDIA driver

    /// The GPU a model would run on: the discrete one with the most free memory (else the largest), or null.
    const GpuInfo* bestGpu() const;
    /// Free memory of bestGpu(); 0 when the free amount is unknown or no GPU is available.
    std::uint64_t gpuFreeBytes() const;
};

/// Probes this computer (a few milliseconds; call it per launch, memory changes).
SystemProbe probeSystem();

/// "8.0 GB" (decimal, one decimal place).
std::string formatBytes(std::uint64_t bytes);

} // namespace wabcut::ai::llm
