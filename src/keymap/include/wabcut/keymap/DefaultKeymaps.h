#pragma once

// The built-in Premiere Pro default tables, compiled into the library from src/keymap/keymaps/*.json (the
// editable source of truth), so the engine works without loose files.

#include <wabcut/keymap/Platform.h>

#include <string_view>

namespace wabcut::keymap {

/// JSON text of the Premiere default table for `platform`: premiere-windows.json for Windows and Linux,
/// premiere-macos.json for macOS. The view points at static storage.
std::string_view defaultKeymapJson(Platform platform);

} // namespace wabcut::keymap
