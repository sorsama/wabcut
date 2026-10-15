// GPU vendor detection for the hardware-backend policy (no FFmpeg, no native types in headers).

#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <dxgi.h>
#elif defined(__linux__)
#include <filesystem>
#include <fstream>
#endif

namespace wabcut::media {

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

std::vector<std::string> detectVendors() {
    std::vector<std::string> vendors;
    const auto add = [&](std::string v) {
        if (std::find(vendors.begin(), vendors.end(), v) == vendors.end()) {
            vendors.push_back(std::move(v));
        }
    };
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                add(vendorName(desc.VendorId)); // adapter 0 (the primary) first
            }
            adapter->Release();
        }
        factory->Release();
    }
#elif defined(__linux__)
    std::error_code ec;
    for (const auto& dev : std::filesystem::directory_iterator("/sys/bus/pci/devices", ec)) {
        std::ifstream cls(dev.path() / "class");
        std::ifstream ven(dev.path() / "vendor");
        std::string c;
        std::string v;
        if (cls >> c && ven >> v && c.rfind("0x03", 0) == 0) { // display controllers
            add(vendorName(static_cast<std::uint32_t>(std::stoul(v, nullptr, 16))));
        }
    }
#elif defined(__APPLE__)
    add("Apple");
#endif
    return vendors;
}

} // namespace

std::vector<std::string> gpuVendors() {
    static const std::vector<std::string> vendors = detectVendors(); // hardware does not change at runtime
    return vendors;
}

} // namespace wabcut::media
