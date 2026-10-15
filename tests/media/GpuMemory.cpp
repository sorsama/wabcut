// processGpuMemoryBytes() (TestMedia.h), in its own translation unit: the DXGI headers pull in rpcndr.h, whose
// macros (`small`, ...) must not leak into the tests.

#include "TestMedia.h"

#if defined(_WIN32)
#include <dxgi1_4.h>
#endif

namespace wabcut::media::test {

long long processGpuMemoryBytes() {
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return -1;
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
    return total;
#else
    return -1;
#endif
}

} // namespace wabcut::media::test
