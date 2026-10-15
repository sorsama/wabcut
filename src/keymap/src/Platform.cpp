#include <wabcut/keymap/Platform.h>

namespace wabcut::keymap {

std::string_view platformId(Platform p) {
    switch (p) {
    case Platform::Windows:
        return "windows";
    case Platform::MacOS:
        return "macos";
    case Platform::Linux:
        return "linux";
    }
    return "windows";
}

std::optional<Platform> platformFromId(std::string_view id) {
    if (id == "windows") {
        return Platform::Windows;
    }
    if (id == "macos") {
        return Platform::MacOS;
    }
    if (id == "linux") {
        return Platform::Linux;
    }
    return std::nullopt;
}

} // namespace wabcut::keymap
