// The GPU parts of the perf harness (the machine's adapter, this process's GPU memory), in their own translation unit:
// the DXGI headers pull in rpcndr.h, whose macros (`small`, ...) must not leak into the benchmarks.

#include <string>

#if defined(_WIN32)
#include <dxgi1_4.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace wabcut::perf {

/// The first hardware adapter's name ("NVIDIA GeForce RTX 4070"); "no GPU" when there is none.
std::string gpuAdapterName() {
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return "no GPU";
    }
    std::string name = "no GPU";
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        const bool hardware = SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        adapter->Release();
        if (hardware) {
            char utf8[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof utf8 - 1, nullptr, nullptr);
            name = utf8;
            break;
        }
    }
    factory->Release();
    return name;
#else
    return "unknown GPU";
#endif
}

/// This process's GPU memory, local + non-local over all adapters, MB; -1 when unknown.
double gpuProcessMemoryMb() {
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return -1.0;
    }
    long long total = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            for (const DXGI_MEMORY_SEGMENT_GROUP group :
                 {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, group, &info))) {
                    total += static_cast<long long>(info.CurrentUsage);
                }
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return static_cast<double>(total) / (1024.0 * 1024.0);
#else
    return -1.0;
#endif
}

} // namespace wabcut::perf
