// GPU adapters and the one Wabcut uses (issue #10; GpuInfo.h), and the vendor list the hardware policies follow
// (gpuVendors(), VideoDecoder.h). No FFmpeg, no native types in the headers.

#include <wabcut/media/GpuInfo.h>
#include <wabcut/media/Log.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#if __has_include(<dxcore_interface.h>)
#include <dxcore_interface.h>
#define WABCUT_HAVE_DXCORE 1
#endif
#elif defined(__linux__)
#include <filesystem>
#include <fstream>
#endif

namespace wabcut::media {

namespace detail { // Log.cpp (FfmpegUtil.h would bring FFmpeg in)
bool logEnabled(LogLevel level);
void logMessage(LogLevel level, std::string_view message);
} // namespace detail

namespace {

std::string vendorName(std::uint32_t id) {
    switch (id) {
    case 0x10DE:
        return "NVIDIA";
    case 0x1002:
    case 0x1022:
        return "AMD";
    case 0x8086:
        return "Intel";
    case 0x1414:
        return "Microsoft";
    case 0x106B:
        return "Apple";
    default:
        return std::format("0x{:04X}", id);
    }
}

bool isSoftwareAdapter(const GpuAdapter& a) {
    return a.software || (a.vendorId == 0x1414 && a.deviceId == 0x8C); // WARP: "Microsoft Basic Render Driver"
}

/// 0 best .. 2 worst for the preference (unknown kinds sit between).
int kindRank(GpuKind kind, bool highPerformance) {
    switch (kind) {
    case GpuKind::Discrete:
        return highPerformance ? 0 : 2;
    case GpuKind::Integrated:
        return highPerformance ? 2 : 0;
    case GpuKind::Unknown:
        break;
    }
    return 1;
}

int systemRank(const GpuAdapter& a) {
    return a.systemIndex >= 0 ? a.systemIndex : 1'000'000;
}

std::optional<std::string> environment(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string text(value);
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

int parseIndex(const std::optional<std::string>& text) {
    if (!text || text->empty()) {
        return -1;
    }
    int value = -1;
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), value);
    return ec == std::errc{} && end == text->data() + text->size() && value >= 0 ? value : -1;
}

#if defined(_WIN32)
std::string narrow(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

std::uint64_t luidOf(const LUID& luid) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}

struct DxcoreInfo {
    std::uint64_t luid = 0;
    bool integrated = false;
    bool hardware = false;
};

/// The adapters DXCore says can render (D3D11 graphics), with their kind; empty without DXCore (before Windows 10 2004).
std::vector<DxcoreInfo> dxcoreGraphicsAdapters() {
    std::vector<DxcoreInfo> out;
#if defined(WABCUT_HAVE_DXCORE)
    HMODULE dll = LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        return out;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dll, "DXCoreCreateAdapterFactory")));
    IDXCoreAdapterFactory* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXCoreAdapterFactory), reinterpret_cast<void**>(&factory)))) {
        // DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS (dxcore_interface.h; defined here so no INITGUID unit is needed).
        static constexpr GUID kD3D11Graphics{0x8c47866b, 0x7583, 0x450d, {0xf0, 0xf0, 0x6b, 0xad, 0xa8, 0x95, 0xaf, 0x4b}};
        IDXCoreAdapterList* list = nullptr;
        if (SUCCEEDED(factory->CreateAdapterList(1, &kD3D11Graphics, __uuidof(IDXCoreAdapterList),
                                                 reinterpret_cast<void**>(&list)))) {
            for (std::uint32_t i = 0; i < list->GetAdapterCount(); ++i) {
                IDXCoreAdapter* adapter = nullptr;
                if (FAILED(list->GetAdapter(i, __uuidof(IDXCoreAdapter), reinterpret_cast<void**>(&adapter)))) {
                    continue;
                }
                LUID luid{};
                bool integrated = false;
                bool hardware = false;
                if (SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(luid), &luid))) {
                    (void)adapter->GetProperty(DXCoreAdapterProperty::IsIntegrated, sizeof(integrated), &integrated);
                    (void)adapter->GetProperty(DXCoreAdapterProperty::IsHardware, sizeof(hardware), &hardware);
                    out.push_back({luidOf(luid), integrated, hardware});
                }
                adapter->Release();
            }
            list->Release();
        }
        factory->Release();
    }
    // dxcore.dll stays loaded: it is a system library, and unloading it under adapters still in use is not worth the
    // risk for the few kilobytes.
#endif
    return out;
}

std::vector<GpuAdapter> enumerateAdapters() {
    std::vector<GpuAdapter> adapters;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return adapters;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            GpuAdapter a;
            a.name = narrow(desc.Description);
            a.vendorId = desc.VendorId;
            a.deviceId = desc.DeviceId;
            a.subSysId = desc.SubSysId;
            a.revision = desc.Revision;
            a.vendor = vendorName(desc.VendorId);
            a.luid = luidOf(desc.AdapterLuid);
            a.dedicatedMemory = desc.DedicatedVideoMemory;
            a.sharedMemory = desc.SharedSystemMemory;
            a.systemIndex = static_cast<int>(i);
            a.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            adapters.push_back(std::move(a));
        }
        adapter->Release();
    }
    // The preference orders (Windows 10 1803+): an adapter's position in each.
    IDXGIFactory6* factory6 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6)))) {
        for (const DXGI_GPU_PREFERENCE preference : {DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, DXGI_GPU_PREFERENCE_MINIMUM_POWER}) {
            IDXGIAdapter1* ranked = nullptr;
            for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i, preference, __uuidof(IDXGIAdapter1),
                                                                  reinterpret_cast<void**>(&ranked)) != DXGI_ERROR_NOT_FOUND;
                 ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                if (ranked && SUCCEEDED(ranked->GetDesc1(&desc))) {
                    const std::uint64_t luid = luidOf(desc.AdapterLuid);
                    for (GpuAdapter& a : adapters) {
                        if (a.luid == luid) {
                            (preference == DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE ? a.highPerformanceIndex
                                                                                : a.minimumPowerIndex) = static_cast<int>(i);
                        }
                    }
                }
                if (ranked) {
                    ranked->Release();
                    ranked = nullptr;
                }
            }
        }
        factory6->Release();
    }
    factory->Release();

    // DXCore: integrated or discrete, and which adapters render at all. An adapter it does not list (an indirect display
    // driver's proxy of a real GPU, listed by DXGI a second time after the software adapter) is not a choice.
    const std::vector<DxcoreInfo> dxcore = dxcoreGraphicsAdapters();
    if (!dxcore.empty()) {
        std::erase_if(adapters, [&](const GpuAdapter& a) {
            return !a.software && std::ranges::none_of(dxcore, [&](const DxcoreInfo& d) { return d.luid == a.luid; });
        });
        for (GpuAdapter& a : adapters) {
            for (const DxcoreInfo& d : dxcore) {
                if (d.luid == a.luid && d.hardware) {
                    a.kind = d.integrated ? GpuKind::Integrated : GpuKind::Discrete;
                }
            }
        }
    }
    return adapters;
}

WindowsGpuSetting readWindowsSetting() {
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe)) {
        return WindowsGpuSetting::None;
    }
    wchar_t value[512] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", exe, RRF_RT_REG_SZ, nullptr,
                     value, &size) != ERROR_SUCCESS) {
        return WindowsGpuSetting::None;
    }
    return parseWindowsGpuSetting(narrow(value));
}
#else
#if defined(__linux__)
/// A hex sysfs value ("0x10de"), 0 when unreadable.
std::uint32_t readHex(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string text;
    if (!(in >> text)) {
        return 0;
    }
    if (text.rfind("0x", 0) == 0) {
        text.erase(0, 2);
    }
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return ec == std::errc{} ? value : 0;
}
#endif

std::vector<GpuAdapter> enumerateAdapters() {
    std::vector<GpuAdapter> adapters;
#if defined(__linux__)
    // One adapter per PCI display controller (class 0x03xxxx), in slot order, with what sysfs tells about its kind:
    // amdgpu's dedicated memory (mem_info_vram_total) and the firmware's display GPU (boot_vga).
    std::vector<std::filesystem::path> devices;
    std::error_code ec;
    for (const auto& dev : std::filesystem::directory_iterator("/sys/bus/pci/devices", ec)) {
        devices.push_back(dev.path());
    }
    std::ranges::sort(devices);
    for (const std::filesystem::path& dev : devices) {
        std::ifstream cls(dev / "class");
        std::string c;
        if (!(cls >> c) || c.rfind("0x03", 0) != 0) {
            continue;
        }
        std::uint64_t vram = 0;
        if (std::ifstream mem(dev / "mem_info_vram_total"); mem) {
            mem >> vram;
        }
        std::string boot;
        if (std::ifstream vga(dev / "boot_vga"); vga) {
            vga >> boot;
        }
        adapters.push_back(describePciGpu(readHex(dev / "vendor"), readHex(dev / "device"), dev.filename().string(), vram,
                                          boot == "1"));
    }
#elif defined(__APPLE__)
    GpuAdapter apple;
    apple.name = "Apple";
    apple.vendor = "Apple";
    apple.vendorId = 0x106B;
    apple.kind = GpuKind::Integrated;
    adapters.push_back(std::move(apple));
#endif
    return adapters;
}

WindowsGpuSetting readWindowsSetting() {
    return WindowsGpuSetting::None;
}
#endif

/// The process's GPU state.
struct State {
    std::mutex mutex;
    bool enumerated = false;
    std::vector<GpuAdapter> real; ///< ranked, keyed
    WindowsGpuSetting realWindows = WindowsGpuSetting::None;
    std::optional<GpuTestSetup> test; ///< ranked, keyed
    std::string choice;
    std::optional<GpuSelection> preview;
    bool overrideRead = false;
    int overrideIndex = -1; ///< QT_D3D_ADAPTER_INDEX as it was the first time a selection was made (before main() sets it)

    /// Under the lock.
    const std::vector<GpuAdapter>& adapters() {
        if (test) {
            return test->adapters;
        }
        if (!enumerated) {
            enumerated = true;
            std::vector<GpuAdapter> listed = enumerateAdapters();
            assignGpuKeys(listed);
            real = rankGpuAdapters(std::move(listed), GpuPreference::HighPerformance);
            realWindows = readWindowsSetting();
        }
        return real;
    }
    WindowsGpuSetting windows() {
        (void)adapters();
        return test ? test->windows : realWindows;
    }
    /// The developer's QT_D3D_ADAPTER_INDEX: read once, so main() setting it later for the preview does not count.
    int overrideAdapter() {
        if (test) {
            return test->overrideIndex;
        }
        if (!overrideRead) {
            overrideRead = true;
            overrideIndex = parseIndex(environment("QT_D3D_ADAPTER_INDEX"));
        }
        return overrideIndex;
    }
    GpuSelection select() {
        const std::vector<GpuAdapter>& list = adapters();
        return selectGpu(list, {choice, windows(), overrideAdapter()});
    }
};

State& state() {
    static auto* s = new State(); // never destroyed: decoder and render threads may ask during static destruction
    return *s;
}

void logSelection(const GpuSelection& s) {
    if (!detail::logEnabled(LogLevel::Info)) {
        return;
    }
    detail::logMessage(LogLevel::Info, s.adapter ? std::format("GPU for preview, decoding and export: {} ({})",
                                                               s.adapter->name, gpuReasonKey(s.reason))
                                                 : std::string("GPU: no hardware adapter found"));
}

} // namespace

std::vector<GpuAdapter> rankGpuAdapters(std::vector<GpuAdapter> adapters, GpuPreference preference) {
    std::erase_if(adapters, isSoftwareAdapter);
    const bool high = preference == GpuPreference::HighPerformance;
    const bool known = std::ranges::all_of(adapters, [&](const GpuAdapter& a) {
        return (high ? a.highPerformanceIndex : a.minimumPowerIndex) >= 0;
    });
    std::ranges::stable_sort(adapters, [&](const GpuAdapter& a, const GpuAdapter& b) {
        if (preference == GpuPreference::SystemOrder) {
            return systemRank(a) < systemRank(b);
        }
        if (known) {
            return high ? a.highPerformanceIndex < b.highPerformanceIndex : a.minimumPowerIndex < b.minimumPowerIndex;
        }
        if (const int ka = kindRank(a.kind, high), kb = kindRank(b.kind, high); ka != kb) {
            return ka < kb;
        }
        if (a.dedicatedMemory != b.dedicatedMemory) {
            return high ? a.dedicatedMemory > b.dedicatedMemory : a.dedicatedMemory < b.dedicatedMemory;
        }
        return systemRank(a) < systemRank(b);
    });
    return adapters;
}

void assignGpuKeys(std::vector<GpuAdapter>& adapters) {
    std::vector<std::string> seen;
    for (GpuAdapter& a : adapters) {
        std::string base = a.vendorId != 0 || a.deviceId != 0
                               ? std::format("{:04X}:{:04X}:{:08X}:{:02X}", a.vendorId, a.deviceId, a.subSysId, a.revision)
                               : a.vendor;
        const auto count = std::ranges::count(seen, base);
        seen.push_back(base);
        a.key = count == 0 ? base : std::format("{}#{}", base, count + 1);
    }
}

WindowsGpuSetting parseWindowsGpuSetting(std::string_view value) {
    WindowsGpuSetting setting = WindowsGpuSetting::None;
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t end = std::min(value.find(';', start), value.size());
        const std::string_view item = value.substr(start, end - start);
        start = end + 1;
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view name = item.substr(0, eq);
        const std::string_view text = item.substr(eq + 1);
        if (name == "SpecificAdapter" && !text.empty()) {
            return WindowsGpuSetting::Specific;
        }
        if (name != "GpuPreference") {
            continue;
        }
        long long number = 0;
        const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec != std::errc{} || p != text.data() + text.size()) {
            continue;
        }
        setting = number == 0   ? WindowsGpuSetting::None
                  : number == 1 ? WindowsGpuSetting::PowerSaving
                  : number == 2 ? WindowsGpuSetting::HighPerformance
                                : WindowsGpuSetting::Specific;
    }
    return setting;
}

std::string_view gpuReasonKey(GpuReason reason) {
    switch (reason) {
    case GpuReason::NoGpu:
        return "noGpu";
    case GpuReason::OnlyGpu:
        return "onlyGpu";
    case GpuReason::HighPerformance:
        return "highPerformance";
    case GpuReason::WindowsPowerSaving:
        return "windowsPowerSaving";
    case GpuReason::WindowsHighPerformance:
        return "windowsHighPerformance";
    case GpuReason::WindowsSpecific:
        return "windowsSpecific";
    case GpuReason::UserChoice:
        return "userChoice";
    case GpuReason::UserChoiceMissing:
        return "userChoiceMissing";
    case GpuReason::Override:
        return "override";
    }
    return "noGpu";
}

std::optional<GpuReason> gpuReasonFromKey(std::string_view key) {
    for (const GpuReason r : {GpuReason::NoGpu, GpuReason::OnlyGpu, GpuReason::HighPerformance, GpuReason::WindowsPowerSaving,
                              GpuReason::WindowsHighPerformance, GpuReason::WindowsSpecific, GpuReason::UserChoice,
                              GpuReason::UserChoiceMissing, GpuReason::Override}) {
        if (gpuReasonKey(r) == key) {
            return r;
        }
    }
    return std::nullopt;
}

GpuSelection selectGpu(const std::vector<GpuAdapter>& adapters, const GpuSelectionInput& input) {
    GpuSelection out;
    const std::vector<GpuAdapter> hardware = rankGpuAdapters(adapters, GpuPreference::HighPerformance);
    if (hardware.empty()) {
        return out;
    }
    if (input.overrideIndex >= 0) {
        if (const auto it = std::ranges::find(hardware, input.overrideIndex, &GpuAdapter::systemIndex); it != hardware.end()) {
            out.adapter = *it;
            out.reason = GpuReason::Override;
            return out;
        }
    }
    if (!input.choice.empty()) {
        if (const auto it = std::ranges::find(hardware, input.choice, &GpuAdapter::key); it != hardware.end()) {
            out.adapter = *it;
            out.reason = GpuReason::UserChoice;
            return out;
        }
        out.missingChoice = input.choice;
    }
    // Automatic.
    GpuPreference order = GpuPreference::HighPerformance;
    GpuReason reason = GpuReason::HighPerformance;
    switch (input.windows) {
    case WindowsGpuSetting::PowerSaving:
        order = GpuPreference::MinimumPower;
        reason = GpuReason::WindowsPowerSaving;
        break;
    case WindowsGpuSetting::HighPerformance:
        reason = GpuReason::WindowsHighPerformance;
        break;
    case WindowsGpuSetting::Specific:
        order = GpuPreference::SystemOrder; // Windows lists the GPU it was told to use first
        reason = GpuReason::WindowsSpecific;
        break;
    case WindowsGpuSetting::None:
        break;
    }
    out.adapter = rankGpuAdapters(hardware, order).front();
    out.reason = !out.missingChoice.empty() ? GpuReason::UserChoiceMissing
                 : hardware.size() == 1      ? GpuReason::OnlyGpu
                                             : reason;
    return out;
}

std::vector<GpuAdapter> gpuAdapters() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.adapters();
}

WindowsGpuSetting windowsGpuSetting() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.windows();
}

void setGpuChoice(std::string key) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.choice = std::move(key);
}

std::string gpuChoice() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.choice;
}

GpuSelection selectedGpu() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.select(); // the override wins here too: encoders, the check and the Settings row agree with the preview
}

GpuSelection previewGpu() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.preview) {
        s.preview = s.select();
        logSelection(*s.preview);
    }
    return *s.preview;
}

int previewAdapterIndex() {
    const GpuSelection s = previewGpu();
    return s.adapter ? s.adapter->systemIndex : -1;
}

std::uint64_t previewAdapterLuid() {
    const GpuSelection s = previewGpu();
    return s.adapter ? s.adapter->luid : 0;
}

void setGpuAdaptersForTesting(std::optional<GpuTestSetup> setup) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (setup) {
        assignGpuKeys(setup->adapters);
        setup->adapters = rankGpuAdapters(std::move(setup->adapters), GpuPreference::HighPerformance);
    }
    s.test = std::move(setup);
    s.preview.reset();
    s.overrideRead = false; // real adapters again: the environment is read again
}

GpuAdapter describePciGpu(std::uint32_t vendorId, std::uint32_t deviceId, std::string_view slot, std::uint64_t vram,
                          bool bootVga) {
    GpuAdapter a;
    a.vendorId = vendorId;
    a.deviceId = deviceId;
    a.vendor = vendorName(vendorId);
    a.name = slot.empty() ? a.vendor : std::format("{} GPU ({})", a.vendor, slot);
    a.dedicatedMemory = vram;
    // "DDDD:BB:DD.F": the bus. Intel's integrated GPU is 00:02.0; a discrete Arc card sits behind a PCIe bridge.
    unsigned bus = 0xFFFF;
    if (const std::size_t colon = slot.find(':'); colon != std::string_view::npos && slot.size() >= colon + 3) {
        const std::string_view text = slot.substr(colon + 1, 2);
        (void)std::from_chars(text.data(), text.data() + text.size(), bus, 16);
    }
    constexpr std::uint64_t kDiscreteVram = std::uint64_t{4} << 30; // an APU's carve-out is smaller by default
    switch (vendorId) {
    case 0x10DE: // NVIDIA: discrete on PCs (Jetson-class GPUs do not sit on PCI)
        a.kind = GpuKind::Discrete;
        break;
    case 0x8086:
        a.kind = bus == 0 ? GpuKind::Integrated : GpuKind::Discrete;
        break;
    case 0x1002:
    case 0x1022:
        a.kind = vram >= kDiscreteVram ? GpuKind::Discrete
                 : vram > 0           ? GpuKind::Integrated
                 : bootVga            ? GpuKind::Integrated // the firmware's display GPU on a laptop without amdgpu's sizes
                                      : GpuKind::Unknown;
        break;
    default:
        break;
    }
    return a;
}

std::vector<std::string> gpuVendors() {
    const GpuSelection selection = selectedGpu();
    std::vector<std::string> vendors;
    const auto add = [&](const std::string& v) {
        if (std::ranges::find(vendors, v) == vendors.end()) {
            vendors.push_back(v);
        }
    };
    if (selection.adapter) {
        add(selection.adapter->vendor); // the chosen GPU's encoder first
    }
    for (const GpuAdapter& a : gpuAdapters()) {
        add(a.vendor);
    }
    return vendors;
}

} // namespace wabcut::media
