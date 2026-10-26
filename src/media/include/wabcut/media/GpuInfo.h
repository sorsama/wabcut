#pragma once

// GPU adapters, and the one Wabcut previews, decodes and exports on (issue #10; docs/performance.md, "Which GPU Wabcut
// uses"). No native types in this header.
//
// This computer (Windows): DXGI lists the adapters in the system order (IDXGIFactory1::EnumAdapters1: the index Qt's
// QT_D3D_ADAPTER_INDEX and FFmpeg's d3d11va device string take), and IDXGIFactory6::EnumAdapterByGpuPreference (Windows
// 10 1803+) in the high-performance and minimum-power orders; DXCore (Windows 10 2004+, loaded at run time) says which
// adapters are integrated and which can render at all (an indirect-display proxy of a real GPU is not listed twice).
// Software adapters (WARP, "Microsoft Basic Render Driver") are never listed. Linux: the PCI display controllers' vendors;
// macOS: Apple. Those carry no index: the system picks the device.
//
// Which GPU (selectGpu(), pure): the adapter picked in Settings > Playback & performance > Graphics card when it is
// present; else Automatic: the Windows Graphics setting for wabcut.exe when the user made one (Power saving, High
// performance or a specific GPU: Windows decides then, and Wabcut follows), else the high-performance GPU. A
// QT_D3D_ADAPTER_INDEX set before Wabcut started wins over everything (a developer's override). rankGpuAdapters() orders
// a list without touching hardware, so tests feed it adapter descriptions.
//
// Process state: setGpuChoice() is the Settings choice. selectedGpu() follows it at once: export encoders (gpuVendors(),
// exporter::hardwareEncoderOrder()). previewGpu() is the selection as it was the first time it was asked (main() asks
// before any window exists): the preview window, the offscreen renderer and the hardware decoders stay on it for the
// whole run, so they share one device; a new choice reaches them at the next start.
//
// Thread-safe.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::media {

enum class GpuKind : std::uint8_t {
    Unknown,    ///< the system does not say (one GPU, older Windows, other platforms)
    Integrated, ///< shares the system memory; saves battery
    Discrete,
};

struct GpuAdapter {
    std::string name;   ///< as the driver reports it: "NVIDIA GeForce RTX 5060 Laptop GPU"
    std::string vendor; ///< "NVIDIA", "AMD", "Intel", "Microsoft", "Apple" or a hex PCI id ("0x1AB8")
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t subSysId = 0;
    std::uint32_t revision = 0;
    std::uint64_t luid = 0;            ///< Windows adapter LUID, (HighPart << 32) | LowPart; changes on every boot
    std::uint64_t dedicatedMemory = 0; ///< bytes of dedicated video memory
    std::uint64_t sharedMemory = 0;
    int systemIndex = -1;          ///< position in the system order (EnumAdapters1); -1 where there is none
    int highPerformanceIndex = -1; ///< position in the high-performance order; -1 when the system cannot say
    int minimumPowerIndex = -1;    ///< position in the minimum-power order; -1 when the system cannot say
    GpuKind kind = GpuKind::Unknown;
    bool software = false; ///< a software rasterizer (never chosen)
    /// Stable across restarts (a LUID is not): "VVVV:DDDD:SSSSSSSS:RR" from the PCI ids, with "#2", "#3" for a second and
    /// third identical card (assignGpuKeys()); the vendor name where there are no ids. What Settings stores.
    std::string key;

    bool operator==(const GpuAdapter&) const = default;
};

enum class GpuPreference : std::uint8_t {
    HighPerformance, ///< external, then discrete, then integrated GPUs (DXGI's order, else by kind and memory)
    MinimumPower,    ///< integrated first
    SystemOrder,     ///< as the system lists them (DXGI adapter 0 first)
};

/// The hardware adapters of `adapters`, best first for `preference`; software adapters are dropped. With every adapter's
/// index for that preference known, that order; otherwise discrete before unknown before integrated (the reverse for
/// MinimumPower), then more dedicated memory first (less for MinimumPower), then the system order. Stable. Pure.
std::vector<GpuAdapter> rankGpuAdapters(std::vector<GpuAdapter> adapters, GpuPreference preference);

/// Fills every adapter's `key` from its PCI ids (or its vendor), numbering identical ones in list order. Pure.
void assignGpuKeys(std::vector<GpuAdapter>& adapters);

/// Linux (sysfs): an adapter from a PCI display controller - its ids, its slot ("0000:01:00.0"), its dedicated video memory
/// (amdgpu's mem_info_vram_total, 0 when unknown) and whether the firmware drew on it (boot_vga). The kind, so discrete
/// ranks first: NVIDIA discrete; Intel integrated on bus 0, else discrete (Arc); AMD discrete with 4 GiB of video memory or
/// more, integrated below (an APU's carve-out) or, without a size, when it is the boot display. Pure (tests).
GpuAdapter describePciGpu(std::uint32_t vendorId, std::uint32_t deviceId, std::string_view slot, std::uint64_t vram,
                          bool bootVga);

/// What Windows Settings > System > Display > Graphics says for wabcut.exe.
enum class WindowsGpuSetting : std::uint8_t {
    None,            ///< no setting, or "Let Windows decide"
    PowerSaving,
    HighPerformance,
    Specific,        ///< a GPU picked by name (Windows 11): Windows lists it first, Wabcut follows the system order
};

/// Parses the registry value Windows writes for one program (HKCU\Software\Microsoft\DirectX\UserGpuPreferences,
/// e.g. "SwapEffectUpgradeEnable=1;GpuPreference=2;"): GpuPreference 1 = PowerSaving, 2 = HighPerformance, 0 or absent =
/// None, any other value or a SpecificAdapter entry = Specific. Pure.
WindowsGpuSetting parseWindowsGpuSetting(std::string_view value);

/// Why selectGpu() chose what it did.
enum class GpuReason : std::uint8_t {
    NoGpu,                  ///< no hardware adapter
    OnlyGpu,                ///< one hardware GPU
    HighPerformance,        ///< Automatic: the high-performance GPU
    WindowsPowerSaving,     ///< Automatic, and Windows Graphics settings say Power saving for Wabcut
    WindowsHighPerformance, ///< Automatic, and Windows Graphics settings say High performance for Wabcut
    WindowsSpecific,        ///< Automatic, and Windows Graphics settings name a GPU for Wabcut
    UserChoice,             ///< picked in Settings
    UserChoiceMissing,      ///< the GPU picked in Settings is not in this computer now: Automatic instead
    Override,               ///< QT_D3D_ADAPTER_INDEX was set before Wabcut started
};

/// "noGpu", "onlyGpu", "highPerformance", "windowsPowerSaving", "windowsHighPerformance", "windowsSpecific",
/// "userChoice", "userChoiceMissing", "override".
std::string_view gpuReasonKey(GpuReason reason);
std::optional<GpuReason> gpuReasonFromKey(std::string_view key);

struct GpuSelection {
    std::optional<GpuAdapter> adapter; ///< the GPU used; empty: none (no hardware adapter)
    GpuReason reason = GpuReason::NoGpu;
    std::string missingChoice; ///< UserChoiceMissing: the key that was asked for

    bool operator==(const GpuSelection&) const = default;
};

struct GpuSelectionInput {
    std::string choice; ///< "" = Automatic, else a GpuAdapter::key
    WindowsGpuSetting windows = WindowsGpuSetting::None;
    int overrideIndex = -1; ///< QT_D3D_ADAPTER_INDEX (a systemIndex) set before start; -1 = none
};

/// The adapter to use (see the header comment for the rules). Pure.
GpuSelection selectGpu(const std::vector<GpuAdapter>& adapters, const GpuSelectionInput& input);

// ---- This computer ------------------------------------------------------------------------------------------------

/// The hardware adapters, high-performance first (enumerated once per process).
std::vector<GpuAdapter> gpuAdapters();
/// The Windows Graphics setting for this executable (read once; None elsewhere).
WindowsGpuSetting windowsGpuSetting();

/// The Settings choice: "" = Automatic, else a GpuAdapter::key. selectedGpu() and the export encoders follow at once.
void setGpuChoice(std::string key);
std::string gpuChoice();
/// The adapter for the current choice (a QT_D3D_ADAPTER_INDEX set before start wins here too, as for the preview).
GpuSelection selectedGpu();
/// The adapter the preview, the offscreen renderer and the hardware decoders use in this run: selectedGpu() as it was
/// the first time this was called (that call also reads QT_D3D_ADAPTER_INDEX), then fixed.
GpuSelection previewGpu();
/// previewGpu()'s system index (for QT_D3D_ADAPTER_INDEX and the d3d11va device), -1 = the default adapter.
int previewAdapterIndex();
/// previewGpu()'s LUID, 0 = none.
std::uint64_t previewAdapterLuid();

struct GpuTestSetup {
    std::vector<GpuAdapter> adapters; ///< as an enumerator would list them (keys are assigned, the list is ranked)
    WindowsGpuSetting windows = WindowsGpuSetting::None;
    int overrideIndex = -1;
};
/// Tests: replaces this computer's adapters and Windows setting (nullopt: the real ones again) and forgets the preview
/// selection (the next previewGpu() decides again). The choice is kept.
void setGpuAdaptersForTesting(std::optional<GpuTestSetup> setup);

} // namespace wabcut::media
