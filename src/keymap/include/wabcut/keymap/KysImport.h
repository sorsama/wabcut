#pragma once

// Import of Adobe Premiere Pro keyboard shortcut sets (.kys) as Wabcut user overrides.
//
// The format was verified against real Premiere key sets for both OSes (docs/keymap.md lists the sources):
//
//   <PremiereData Version="3">
//     <shortcuts Version="5">                      (4 in older files)
//       <platform>windows</platform>               ("macintosh" on macOS; first or last child)
//       <context.global Version="1">               (one element per shortcut context: global, timeline, ...)
//         <itemcount>924</itemcount>
//         <item.806 Version="1">
//           <virtualkey>1</virtualkey>             (absent = command has no shortcut)
//           <modifier.ctrl>true</modifier.ctrl>    (Windows: ctrl, alt, shift)
//           <modifier.alt>false</modifier.alt>     (macOS: command, opt, shift, ctrl = the physical Control
//           key) <modifier.shift>true</modifier.shift> <commandname>cmd.transport.playintoout</commandname>
//         </item.806>
//
// A key set lists every command of the Premiere version that wrote it; a command with several shortcuts has
// several items with the same command name in the same context. <virtualkey> is 0x80000000 | the key's
// character code, 0xC0000000 | the character for keypad keys, or a small code for named keys (see
// decodeKysVirtualKey). Child element order varies between versions and is not relied on.

#include <wabcut/keymap/CommandRegistry.h>
#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/KeymapManager.h>
#include <wabcut/keymap/Platform.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::keymap {

struct KysImport {
    Platform platform = Platform::Windows; ///< Windows or MacOS, from <platform>
    /// Every Wabcut command the key set mentions (via CommandInfo::kysContext/kysName), with its shortcuts.
    /// An empty chord list means the key set leaves the command unbound. Commands the key set does not
    /// mention are absent, so they keep their current bindings. Registry order.
    std::vector<CommandOverride> overrides;
    /// Premiere commands that have shortcuts in the key set but no Wabcut equivalent
    /// ("context.global/cmd.x").
    std::vector<std::string> unmappedCommands;
    /// Shortcuts that could not be decoded (unverified virtual key codes, unknown modifier elements). The
    /// affected commands are left out of `overrides`, so they keep their current bindings.
    std::vector<std::string> warnings;
};

/// Decodes a .kys <virtualkey> value: the Qt-compatible key, with Modifier::Keypad for keypad keys. Only
/// codes verified against real key sets are accepted; anything else is nullopt.
std::optional<KeyChord> decodeKysVirtualKey(std::uint32_t virtualKey);

/// Parses .kys XML. Errors: ParseError (not XML / not a Premiere key set), Unsupported (unknown <platform> or
/// layout).
std::expected<KysImport, Error> parseKys(std::string_view xml,
                                         const CommandRegistry& registry = CommandRegistry::builtin());

/// Reads and parses a .kys file. Errors: IoError, plus those of parseKys().
std::expected<KysImport, Error> importKys(const std::filesystem::path& path,
                                          const CommandRegistry& registry = CommandRegistry::builtin());

/// Applies an import to `keymap` as user overrides (overrides equal to the defaults are dropped). Premiere
/// key sets are per-OS, so a macOS key set cannot be applied to a Windows/Linux keymap or vice versa
/// (InvalidArgument).
std::expected<void, Error> applyKysImport(KeymapManager& keymap, const KysImport& import);

} // namespace wabcut::keymap
