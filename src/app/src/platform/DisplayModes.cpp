// Display modes and Match Refresh Rate (DisplayModes.h, 0.7 card 110).

#include "DisplayModes.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wabcut::app::platform {

namespace {

#ifdef _WIN32

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

/// Windows reports whole hertz: 59 for 59.94 Hz, 23 for 23.976, 29 for 29.97 and so on (the NTSC family 24, 30, 48, 60, 72,
/// 96, 120, 144 divided by 1.001, shown one lower). The rate they mean.
double broadcastRate(unsigned hz) {
    switch (hz) {
    case 23:
    case 29:
    case 47:
    case 59:
    case 71:
    case 95:
    case 119:
    case 143:
        return (hz + 1) * 1000.0 / 1001.0;
    default:
        return static_cast<double>(hz);
    }
}

/// The whole hertz a rate is listed as: the inverse of broadcastRate.
unsigned systemRate(double hz) {
    const double scaled = hz * 1.001;
    if (hz > 0 && std::abs(scaled - std::round(scaled)) < 0.01 && std::abs(hz - std::round(hz)) > 0.01) {
        return static_cast<unsigned>(std::lround(scaled)) - 1;
    }
    return static_cast<unsigned>(std::lround(hz));
}

#endif

} // namespace

std::vector<videoout::DisplayMode> displayModes(const std::string& deviceName) {
    std::vector<videoout::DisplayMode> out;
#ifdef _WIN32
    const std::wstring device = widen(deviceName);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsExW(device.c_str(), i, &mode, 0) && out.size() < 4096; ++i) {
        if (mode.dmBitsPerPel < 24 || mode.dmDisplayFrequency < 2) { // 0 and 1 mean "the hardware default"
            continue;
        }
        const videoout::DisplayMode found{static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight),
                                          broadcastRate(mode.dmDisplayFrequency)};
        if (std::find(out.begin(), out.end(), found) == out.end()) {
            out.push_back(found);
        }
    }
#else
    (void)deviceName;
#endif
    return out;
}

bool setDisplayMode(const std::string& deviceName, const videoout::DisplayMode& wanted) {
#ifdef _WIN32
    const std::wstring device = widen(deviceName);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
        return false;
    }
    mode.dmPelsWidth = static_cast<DWORD>(wanted.width);
    mode.dmPelsHeight = static_cast<DWORD>(wanted.height);
    mode.dmDisplayFrequency = systemRate(wanted.refreshHz);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    // Tested first, so a mode the display refuses never blanks it.
    if (ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        return false;
    }
    return ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
#else
    (void)deviceName;
    (void)wanted;
    return false;
#endif
}

void restoreDisplayMode(const std::string& deviceName) {
#ifdef _WIN32
    const std::wstring device = widen(deviceName);
    ChangeDisplaySettingsExW(device.c_str(), nullptr, nullptr, 0, nullptr);
#else
    (void)deviceName;
#endif
}

} // namespace wabcut::app::platform
