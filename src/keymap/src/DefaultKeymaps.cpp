#include "EmbeddedKeymaps.h"

#include <wabcut/keymap/DefaultKeymaps.h>

namespace wabcut::keymap {

std::string_view defaultKeymapJson(Platform platform) {
    return tablePlatform(platform) == Platform::MacOS ? detail::premiereMacosJson()
                                                      : detail::premiereWindowsJson();
}

} // namespace wabcut::keymap
