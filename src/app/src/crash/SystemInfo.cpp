#include "SystemInfo.h"

#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#else
#include <sys/utsname.h>
#endif

namespace wabcut::crash {

namespace {

#ifdef _WIN32

std::string utf8(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/// The real Windows version (GetVersionEx lies to unmanifested processes): RtlGetVersion from ntdll.
std::string windowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            get(&info);
        }
    }
    if (info.dwMajorVersion == 0) {
        return "Windows";
    }
    const char* name = info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    return std::string(name) + " (" + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
           std::to_string(info.dwBuildNumber) + ")";
}

/// The primary (first, non-software) adapter and its user-mode driver version.
void primaryGpu(std::map<std::string, std::string>& out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi) {
        return;
    }
    auto create = reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            const bool software = FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            if (!software) {
                out["gpu"] = utf8(desc.Description);
                LARGE_INTEGER umd{};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                    const auto v = static_cast<unsigned long long>(umd.QuadPart);
                    out["driver"] = std::to_string(v >> 48) + "." + std::to_string((v >> 32) & 0xFFFF) + "." +
                                    std::to_string((v >> 16) & 0xFFFF) + "." + std::to_string(v & 0xFFFF);
                }
                adapter->Release();
                break;
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
}

#endif

} // namespace

std::map<std::string, std::string> systemDiagnostics() {
    std::map<std::string, std::string> out;
#ifdef _WIN32
    out["os"] = windowsVersion();
    primaryGpu(out);
#else
    utsname name{};
    if (uname(&name) == 0) {
#ifdef __APPLE__
        out["os"] = std::string("macOS (Darwin ") + name.release + ", " + name.machine + ")";
#else
        out["os"] = std::string(name.sysname) + " " + name.release + " (" + name.machine + ")";
#endif
    }
#endif
    if (const unsigned threads = std::thread::hardware_concurrency()) {
        out["cpu"] = std::to_string(threads) + " threads";
    }
    return out;
}

} // namespace wabcut::crash
