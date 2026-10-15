#pragma once

// Target platform of a keymap. Shortcut tables are per-OS (Premiere ships separate Windows and macOS key
// sets; macOS is never derived by swapping Ctrl and Cmd). Linux uses the Windows table.

#include <cstdint>
#include <optional>
#include <string_view>

namespace wabcut::keymap {

enum class Platform : std::uint8_t { Windows, MacOS, Linux };

/// The platform this binary was built for.
constexpr Platform currentPlatform() {
#if defined(__APPLE__)
    return Platform::MacOS;
#elif defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

/// True when chord text uses macOS modifier names (Cmd/Opt/Ctrl) and Qt's macOS modifier mapping.
constexpr bool usesMacModifiers(Platform p) {
    return p == Platform::MacOS;
}

/// The platform whose default table a platform uses (Linux → Windows).
constexpr Platform tablePlatform(Platform p) {
    return p == Platform::Linux ? Platform::Windows : p;
}

/// Stable lower-case id used in JSON: "windows", "macos", "linux".
std::string_view platformId(Platform p);
std::optional<Platform> platformFromId(std::string_view id);

} // namespace wabcut::keymap
