#include <wabcut/ai/llm/SystemProbe.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#endif

namespace wabcut::ai::llm {

namespace {

[[maybe_unused]] GpuVendor vendorFromPci(unsigned id) {
    switch (id) {
    case 0x10DE:
        return GpuVendor::Nvidia;
    case 0x1002:
    case 0x1022:
        return GpuVendor::Amd;
    case 0x8086:
        return GpuVendor::Intel;
    case 0x106B:
        return GpuVendor::Apple;
    default:
        return GpuVendor::Other;
    }
}

[[maybe_unused]] constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;

std::atomic<std::uint64_t> g_preferredGpuLuid{0};

#ifdef _WIN32
using CuDriverGetVersion = int(__stdcall*)(int*);

std::string narrow(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    }
    if (!out.empty()) out.pop_back();
    return out;
}

// 0.6: DXGI's budget is what Windows would let this process have, not what other processes left: with a game holding
// 8 GB of a 12 GB card it still says about 11 GB. NVIDIA's driver library (NVML, in System32 with every NVIDIA driver,
// about 13 ms to load) reports the card's real free memory, as on Linux; the smaller of the two is used. Only for an
// NVIDIA adapter whose name matches exactly one NVML device (two identical cards keep the DXGI figure).
struct NvmlMemoryInfo {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

void refineFreeMemoryWithNvml(SystemProbe& probe) {
    if (std::none_of(probe.gpus.begin(), probe.gpus.end(), [](const GpuInfo& g) { return g.vendor == GpuVendor::Nvidia; })) {
        return;
    }
    HMODULE nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvml) {
        return;
    }
    using Init = int (*)();
    using Count = int (*)(unsigned*);
    using Handle = int (*)(unsigned, void**);
    using Memory = int (*)(void*, NvmlMemoryInfo*);
    using Name = int (*)(void*, char*, unsigned);
    auto init = reinterpret_cast<Init>(GetProcAddress(nvml, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<Init>(GetProcAddress(nvml, "nvmlShutdown"));
    auto count = reinterpret_cast<Count>(GetProcAddress(nvml, "nvmlDeviceGetCount_v2"));
    auto handle = reinterpret_cast<Handle>(GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2"));
    auto memory = reinterpret_cast<Memory>(GetProcAddress(nvml, "nvmlDeviceGetMemoryInfo"));
    auto name = reinterpret_cast<Name>(GetProcAddress(nvml, "nvmlDeviceGetName"));
    if (init && shutdown && count && handle && memory && name && init() == 0) {
        std::vector<std::pair<std::string, std::uint64_t>> devices; // name, free bytes
        unsigned n = 0;
        if (count(&n) == 0) {
            for (unsigned i = 0; i < n; ++i) {
                void* device = nullptr;
                NvmlMemoryInfo info{};
                char text[96] = {};
                if (handle(i, &device) == 0 && memory(device, &info) == 0 && name(device, text, sizeof text) == 0) {
                    devices.emplace_back(text, info.free);
                }
            }
        }
        shutdown();
        for (GpuInfo& gpu : probe.gpus) {
            if (gpu.vendor != GpuVendor::Nvidia) {
                continue;
            }
            // One NVML device of that name is one physical card: every DXGI adapter of that name is it (a virtual or
            // remote display driver lists a proxy of the real GPU as a second adapter with its own LUID).
            const auto sameName = [&](const auto& d) { return d.first == gpu.name; };
            const auto found = std::find_if(devices.begin(), devices.end(), sameName);
            if (found != devices.end() && std::count_if(devices.begin(), devices.end(), sameName) == 1) {
                gpu.freeBytes = gpu.freeBytes == 0 ? found->second : std::min(gpu.freeBytes, found->second);
            }
        }
    }
    FreeLibrary(nvml);
}

void probeGpus(SystemProbe& probe) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414) {
            GpuInfo gpu;
            gpu.name = narrow(desc.Description);
            gpu.vendor = vendorFromPci(desc.VendorId);
            gpu.dedicatedBytes = desc.DedicatedVideoMemory;
            gpu.integrated = desc.DedicatedVideoMemory < kGiB; // integrated GPUs carve out a small aperture only
            gpu.luid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                       desc.AdapterLuid.LowPart;
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                    gpu.freeBytes = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
                }
                adapter3->Release();
            }
            probe.gpus.push_back(std::move(gpu));
        }
        adapter->Release();
    }
    // Windows' high-performance order (10 1803+): on a hybrid machine the discrete GPU comes first, as for the editor's
    // own choice (media::rankGpuAdapters).
    IDXGIFactory6* factory6 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6)))) {
        int position = 0;
        for (UINT i = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                         __uuidof(IDXGIAdapter1),
                                                                         reinterpret_cast<void**>(&adapter)));
             ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                const std::uint64_t luid =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                    desc.AdapterLuid.LowPart;
                for (GpuInfo& gpu : probe.gpus) {
                    if (gpu.luid == luid && gpu.highPerformanceIndex < 0) {
                        gpu.highPerformanceIndex = position++;
                    }
                }
            }
            adapter->Release();
        }
        factory6->Release();
    }
    factory->Release();
    refineFreeMemoryWithNvml(probe);
}

void probeDrivers(SystemProbe& probe) {
    if (HMODULE vulkan = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        probe.vulkanLoader = true;
        FreeLibrary(vulkan);
    }
    if (HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (auto getVersion = reinterpret_cast<CuDriverGetVersion>(GetProcAddress(cuda, "cuDriverGetVersion"))) {
            int version = 0;
            if (getVersion(&version) == 0) {
                probe.cudaDriverVersion = version;
            }
        }
        FreeLibrary(cuda);
    }
}

void probeMemory(SystemProbe& probe) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        probe.ramTotal = status.ullTotalPhys;
        probe.ramAvailable = status.ullAvailPhys;
    }
}
#elif defined(__APPLE__)
void probeGpus(SystemProbe& probe) {
    // Apple silicon: one GPU on unified memory; Metal lets a process use about three quarters of it.
    GpuInfo gpu;
    gpu.name = "Apple GPU";
    gpu.vendor = GpuVendor::Apple;
    gpu.integrated = true;
    gpu.freeBytes = probe.ramAvailable / 4 * 3;
    probe.gpus.push_back(gpu);
}

void probeDrivers(SystemProbe&) {}

void probeMemory(SystemProbe& probe) {
    std::uint64_t memsize = 0;
    std::size_t length = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &length, nullptr, 0) == 0) {
        probe.ramTotal = memsize;
        probe.ramAvailable = memsize / 2; // conservative: the OS keeps file cache that it would give back
    }
}
#else
struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

void probeGpus(SystemProbe& probe) {
    // NVIDIA through NVML (part of the driver); other vendors' memory is unknown here (llama.cpp's own fit decides).
    void* nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!nvml) {
        return;
    }
    using Init = int (*)();
    using Count = int (*)(unsigned*);
    using Handle = int (*)(unsigned, void**);
    using Memory = int (*)(void*, NvmlMemory*);
    using Name = int (*)(void*, char*, unsigned);
    auto init = reinterpret_cast<Init>(dlsym(nvml, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<Init>(dlsym(nvml, "nvmlShutdown"));
    auto count = reinterpret_cast<Count>(dlsym(nvml, "nvmlDeviceGetCount_v2"));
    auto handle = reinterpret_cast<Handle>(dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2"));
    auto memory = reinterpret_cast<Memory>(dlsym(nvml, "nvmlDeviceGetMemoryInfo"));
    auto name = reinterpret_cast<Name>(dlsym(nvml, "nvmlDeviceGetName"));
    if (init && shutdown && count && handle && memory && name && init() == 0) {
        unsigned n = 0;
        if (count(&n) == 0) {
            for (unsigned i = 0; i < n; ++i) {
                void* device = nullptr;
                NvmlMemory info{};
                char text[96] = {};
                if (handle(i, &device) == 0 && memory(device, &info) == 0) {
                    GpuInfo gpu;
                    gpu.name = name(device, text, sizeof text) == 0 ? text : "NVIDIA GPU";
                    gpu.vendor = GpuVendor::Nvidia;
                    gpu.dedicatedBytes = info.total;
                    gpu.freeBytes = info.free;
                    probe.gpus.push_back(gpu);
                }
            }
        }
        shutdown();
    }
    dlclose(nvml);
}

void probeDrivers(SystemProbe& probe) {
    if (void* vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)) {
        probe.vulkanLoader = true;
        dlclose(vulkan);
    }
    if (void* cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL)) {
        using GetVersion = int (*)(int*);
        if (auto getVersion = reinterpret_cast<GetVersion>(dlsym(cuda, "cuDriverGetVersion"))) {
            int version = 0;
            if (getVersion(&version) == 0) {
                probe.cudaDriverVersion = version;
            }
        }
        dlclose(cuda);
    }
}

void probeMemory(SystemProbe& probe) {
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t kib = 0;
        fields >> key >> kib;
        if (key == "MemTotal:") {
            probe.ramTotal = kib * 1024;
        } else if (key == "MemAvailable:") {
            probe.ramAvailable = kib * 1024;
        }
    }
}
#endif

} // namespace

std::string_view toString(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::Nvidia:
        return "NVIDIA";
    case GpuVendor::Amd:
        return "AMD";
    case GpuVendor::Intel:
        return "Intel";
    case GpuVendor::Apple:
        return "Apple";
    case GpuVendor::Other:
        return "other";
    case GpuVendor::Unknown:
        break;
    }
    return "unknown";
}

const GpuInfo* SystemProbe::bestGpu() const {
    if (preferredGpuLuid != 0) {
        for (const GpuInfo& g : gpus) {
            if (g.luid == preferredGpuLuid) {
                return &g;
            }
        }
    }
    // a before b: discrete first, then the high-performance order (where both positions are known), then free memory.
    const auto better = [](const GpuInfo& a, const GpuInfo& b) {
        if (a.integrated != b.integrated) {
            return !a.integrated;
        }
        if (a.highPerformanceIndex >= 0 && b.highPerformanceIndex >= 0 && a.highPerformanceIndex != b.highPerformanceIndex) {
            return a.highPerformanceIndex < b.highPerformanceIndex;
        }
        return a.freeBytes > b.freeBytes;
    };
    const GpuInfo* best = nullptr;
    for (const GpuInfo& g : gpus) {
        if (!best || better(g, *best)) {
            best = &g;
        }
    }
    return best;
}

void setPreferredGpuLuid(std::uint64_t luid) { g_preferredGpuLuid.store(luid, std::memory_order_relaxed); }

std::uint64_t preferredGpuLuid() { return g_preferredGpuLuid.load(std::memory_order_relaxed); }

std::uint64_t SystemProbe::gpuFreeBytes() const {
    const GpuInfo* gpu = bestGpu();
    if (!gpu) {
        return 0;
    }
    return gpu->freeBytes;
}

SystemProbe probeSystem() {
    SystemProbe probe;
#ifdef _WIN32
    probe.os = "windows";
#elif defined(__APPLE__)
    probe.os = "macos";
#else
    probe.os = "linux";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    probe.arch = "arm64";
#else
    probe.arch = "x64";
#endif
    probe.cpuThreads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    probeMemory(probe);
    probeDrivers(probe);
    probeGpus(probe);
    probe.preferredGpuLuid = preferredGpuLuid();
    return probe;
}

std::string formatBytes(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return buf;
}

} // namespace wabcut::ai::llm
