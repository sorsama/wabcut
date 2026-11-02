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
    /// Windows adapter LUID, (HighPart << 32) | LowPart (media::GpuAdapter::luid); 0 = unknown.
    std::uint64_t luid = 0;
    /// Position in Windows' high-performance order (IDXGIFactory6::EnumAdapterByGpuPreference); -1 = unknown.
    int highPerformanceIndex = -1;
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
    /// 0.6: the adapter the editor uses (Settings > Playback & performance > Graphics card, else Automatic: the
    /// high-performance GPU; media::selectedGpu()), by LUID; 0 = none known. probeSystem() fills it from
    /// setPreferredGpuLuid().
    std::uint64_t preferredGpuLuid = 0;

    /// The GPU a model would run on (0.6, hybrid graphics): the adapter the editor uses when it is listed (even an
    /// integrated one: the user's choice, and the model then runs on the CPU); else discrete before integrated, then
    /// Windows' high-performance order, then the most free memory, then the listing order. Null without a GPU. Pure.
    const GpuInfo* bestGpu() const;
    /// Free memory of bestGpu(); 0 when the free amount is unknown or no GPU is available.
    std::uint64_t gpuFreeBytes() const;
};

/// Probes this computer (a few milliseconds; call it per launch, memory changes).
SystemProbe probeSystem();

/// Process-wide: the LUID of the adapter the editor uses (the app sets it from media::selectedGpu() at start and when
/// Settings > Graphics card changes; 0 = Automatic by Windows' high-performance order). Thread-safe.
void setPreferredGpuLuid(std::uint64_t luid);
std::uint64_t preferredGpuLuid();

/// "8.0 GB" (decimal, one decimal place).
std::string formatBytes(std::uint64_t bytes);

} // namespace wabcut::ai::llm
