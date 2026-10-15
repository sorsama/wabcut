#include <wabcut/keymap/KysImport.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <pugixml.hpp>
#include <span>
#include <utility>

namespace wabcut::keymap {
namespace {

// Premiere's codes for named keys. Each was verified by matching a command's default shortcut in Adobe's
// documentation to its <virtualkey> in a real key set (docs/keymap.md). Codes 19-30 (probably F13-F24), 5
// (probably keypad Enter) and others are not verified and are rejected.
constexpr std::pair<std::uint32_t, int> kNamedKeys[] = {
    {1, Key_Space},  {2, Key_Backspace}, {3, Key_Tab},     {4, Key_Return},    {35, Key_Delete},
    {36, Key_Home},  {37, Key_End},      {38, Key_PageUp}, {39, Key_PageDown}, {42, Key_Left},
    {43, Key_Right}, {44, Key_Up},       {45, Key_Down},
};
constexpr std::uint32_t kFirstFunctionKey = 7; // 7..18 = F1..F12
constexpr std::uint32_t kCharacterFlag = 0x80000000u;
constexpr std::uint32_t kKeypadFlag = 0x40000000u; // with kCharacterFlag: 0xC0000000 | character

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

std::optional<std::uint32_t> parseUnsigned(std::string_view text) {
    std::uint64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end || value > 0xffffffffu) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

struct ModifierElement {
    std::string_view name;
    Modifier modifier;
};
constexpr ModifierElement kWindowsModifiers[] = {
    {"modifier.ctrl", Modifier::Control},
    {"modifier.alt", Modifier::Alt},
    {"modifier.shift", Modifier::Shift},
};
// On macOS Premiere writes Command as modifier.command and the physical Control key as modifier.ctrl, which
// Qt reports as ControlModifier and MetaModifier respectively.
constexpr ModifierElement kMacModifiers[] = {
    {"modifier.command", Modifier::Control},
    {"modifier.opt", Modifier::Alt},
    {"modifier.shift", Modifier::Shift},
    {"modifier.ctrl", Modifier::Meta},
};

/// The path as UTF-8 for messages. path::string() would throw on Windows for characters outside the ANSI code
/// page.
std::string utf8Path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct KysCommand {
    std::vector<KeyChord> chords;
    bool undecodable = false; ///< some shortcut could not be decoded: leave the command as it is
};

} // namespace

std::optional<KeyChord> decodeKysVirtualKey(std::uint32_t virtualKey) {
    if ((virtualKey & kCharacterFlag) != 0) {
        const bool keypad = (virtualKey & kKeypadFlag) != 0;
        auto c = static_cast<int>(virtualKey & ~(kCharacterFlag | kKeypadFlag));
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        // Only US-ASCII characters are verified; key sets saved with other layouts use their own characters.
        if (c <= 0x20 || c >= 0x7f) {
            return std::nullopt;
        }
        return KeyChord{c, keypad ? Modifiers(Modifier::Keypad) : Modifiers()};
    }
    if (virtualKey >= kFirstFunctionKey && virtualKey < kFirstFunctionKey + 12) {
        return KeyChord{functionKey(static_cast<int>(virtualKey - kFirstFunctionKey) + 1)};
    }
    for (const auto& [code, key] : kNamedKeys) {
        if (code == virtualKey) {
            return KeyChord{key};
        }
    }
    return std::nullopt;
}

std::expected<KysImport, Error> parseKys(std::string_view xml, const CommandRegistry& registry) {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        return std::unexpected(
            Error{ErrorCode::ParseError, std::format("not a valid .kys file: {} at offset {}",
                                                     parsed.description(), parsed.offset)});
    }
    const pugi::xml_node shortcuts = doc.child("PremiereData").child("shortcuts");
    if (!shortcuts) {
        return std::unexpected(
            Error{ErrorCode::ParseError, "not a Premiere keyboard shortcut set (no PremiereData/shortcuts)"});
    }

    KysImport result;
    const std::string_view platformText = shortcuts.child_value("platform");
    if (platformText == "windows") {
        result.platform = Platform::Windows;
    } else if (platformText == "macintosh") {
        result.platform = Platform::MacOS;
    } else {
        return std::unexpected(
            Error{ErrorCode::Unsupported, std::format("unsupported .kys platform '{}'", platformText)});
    }
    const std::span<const ModifierElement> modifierElements =
        result.platform == Platform::MacOS ? std::span<const ModifierElement>(kMacModifiers)
                                           : std::span<const ModifierElement>(kWindowsModifiers);

    std::map<std::pair<std::string, std::string>, KysCommand> commands; // (context, command name) → shortcuts
    bool anyContext = false;
    for (const pugi::xml_node context : shortcuts.children()) {
        const std::string_view contextName = context.name();
        if (!startsWith(contextName, "context.")) {
            continue;
        }
        anyContext = true;
        for (const pugi::xml_node item : context.children()) {
            if (!startsWith(item.name(), "item.")) {
                continue;
            }
            const std::string_view name = item.child_value("commandname");
            if (name.empty()) {
                continue;
            }
            KysCommand& command = commands[{std::string(contextName), std::string(name)}];

            const pugi::xml_node keyNode = item.child("virtualkey");
            if (!keyNode) {
                continue; // listed without a shortcut
            }
            const auto virtualKey = parseUnsigned(keyNode.child_value());
            if (virtualKey && *virtualKey == 0) {
                continue; // Premiere writes 0 for "no key" on some window commands
            }
            const auto chord = virtualKey ? decodeKysVirtualKey(*virtualKey) : std::nullopt;
            if (!chord) {
                result.warnings.push_back(std::format("{}/{}: unsupported virtual key '{}'; shortcuts kept",
                                                      contextName, name, keyNode.child_value()));
                command.undecodable = true;
                continue;
            }
            Modifiers mods = chord->mods;
            bool decoded = true;
            for (const pugi::xml_node child : item.children()) {
                const std::string_view tag = child.name();
                if (!startsWith(tag, "modifier.")) {
                    continue;
                }
                const auto known = std::ranges::find(modifierElements, tag, &ModifierElement::name);
                if (known == modifierElements.end()) {
                    result.warnings.push_back(
                        std::format("{}/{}: unknown modifier '{}'; shortcuts kept", contextName, name, tag));
                    decoded = false;
                    break;
                }
                if (std::string_view(child.child_value()) == "true") {
                    mods |= known->modifier;
                }
            }
            const KeyChord full{chord->key, mods};
            if (!decoded) {
                command.undecodable = true;
            } else if (std::ranges::find(command.chords, full) == command.chords.end()) {
                command.chords.push_back(full);
            }
        }
    }
    if (!anyContext) {
        return std::unexpected(
            Error{ErrorCode::Unsupported, "unsupported .kys layout: no shortcut contexts under <shortcuts>"});
    }

    std::map<const CommandInfo*, std::vector<KeyChord>> mapped;
    for (const auto& [key, command] : commands) {
        const CommandInfo* info = registry.findKys(key.first, key.second);
        if (info == nullptr) {
            if (!command.chords.empty() || command.undecodable) {
                result.unmappedCommands.push_back(key.first + "/" + key.second);
            }
            continue;
        }
        if (command.undecodable) {
            continue;
        }
        auto& chords = mapped[info];
        for (const KeyChord& c : command.chords) {
            if (std::ranges::find(chords, c) == chords.end()) {
                chords.push_back(c);
            }
        }
    }
    for (const CommandInfo& info : registry.all()) {
        if (const auto it = mapped.find(&info); it != mapped.end()) {
            result.overrides.push_back({CommandId(info.id), it->second});
        }
    }
    return result;
}

std::expected<KysImport, Error> importKys(const std::filesystem::path& path,
                                          const CommandRegistry& registry) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(Error{ErrorCode::IoError, std::format("cannot open '{}'", utf8Path(path))});
    }
    const std::string xml{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) {
        return std::unexpected(Error{ErrorCode::IoError, std::format("cannot read '{}'", utf8Path(path))});
    }
    return parseKys(xml, registry);
}

std::expected<void, Error> applyKysImport(KeymapManager& keymap, const KysImport& import) {
    if (usesMacModifiers(import.platform) != usesMacModifiers(keymap.platform())) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument,
                  std::format("a {} key set cannot be applied to a {} keymap", platformId(import.platform),
                              platformId(keymap.platform()))});
    }
    return keymap.applyOverrides(import.overrides);
}

} // namespace wabcut::keymap
