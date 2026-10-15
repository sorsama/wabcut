#include <wabcut/keymap/KysImport.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

using namespace wabcut;
using namespace wabcut::keymap;

namespace {

const std::filesystem::path kDataDir = WABCUT_KEYMAP_TEST_DATA_DIR;

const CommandOverride* findOverride(const KysImport& import, std::string_view command) {
    const auto it = std::ranges::find(import.overrides, CommandId(command), &CommandOverride::command);
    return it == import.overrides.end() ? nullptr : &*it;
}

bool contains(const std::vector<std::string>& items, std::string_view needle) {
    return std::ranges::any_of(items,
                               [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("Verified .kys virtual key codes decode to Qt keys", "[keymap][kys]") {
    CHECK(decodeKysVirtualKey(1) == KeyChord{Key_Space});
    CHECK(decodeKysVirtualKey(2) == KeyChord{Key_Backspace});
    CHECK(decodeKysVirtualKey(3) == KeyChord{Key_Tab});
    CHECK(decodeKysVirtualKey(4) == KeyChord{Key_Return});
    CHECK(decodeKysVirtualKey(7) == KeyChord{Key_F1});
    CHECK(decodeKysVirtualKey(18) == KeyChord{Key_F12});
    CHECK(decodeKysVirtualKey(35) == KeyChord{Key_Delete});
    CHECK(decodeKysVirtualKey(36) == KeyChord{Key_Home});
    CHECK(decodeKysVirtualKey(37) == KeyChord{Key_End});
    CHECK(decodeKysVirtualKey(38) == KeyChord{Key_PageUp});
    CHECK(decodeKysVirtualKey(39) == KeyChord{Key_PageDown});
    CHECK(decodeKysVirtualKey(42) == KeyChord{Key_Left});
    CHECK(decodeKysVirtualKey(43) == KeyChord{Key_Right});
    CHECK(decodeKysVirtualKey(44) == KeyChord{Key_Up});
    CHECK(decodeKysVirtualKey(45) == KeyChord{Key_Down});
    CHECK(decodeKysVirtualKey(0x80000000u | 'K') == KeyChord{Key_K});
    CHECK(decodeKysVirtualKey(0x80000000u | ';') == KeyChord{Key_Semicolon});
    CHECK(decodeKysVirtualKey(0x80000000u | '\'') == KeyChord{Key_Apostrophe});
    CHECK(decodeKysVirtualKey(0x80000000u | '\\') == KeyChord{Key_Backslash});
    CHECK(decodeKysVirtualKey(0x80000000u | 'k') == KeyChord{Key_K});
    CHECK(decodeKysVirtualKey(0xC0000000u | '5') == KeyChord{Key_5, Modifier::Keypad});
    CHECK(decodeKysVirtualKey(0xC0000000u | '+') == KeyChord{Key_Plus, Modifier::Keypad});
    // Unverified codes are refused rather than guessed.
    for (std::uint32_t unverified : {0u, 5u, 6u, 19u, 30u, 40u, 41u, 46u, 0x80000000u | 0xC7u, 0x80000020u}) {
        INFO(unverified);
        CHECK_FALSE(decodeKysVirtualKey(unverified));
    }
}

TEST_CASE("A Windows .kys imports as overrides", "[keymap][kys]") {
    const auto import = importKys(kDataDir / "windows-custom.kys");
    REQUIRE(import.has_value());
    CHECK(import->platform == Platform::Windows);

    // Rebound, multi-shortcut, unbound, keypad and F-key items.
    REQUIRE(findOverride(*import, "tool.razor"));
    CHECK(findOverride(*import, "tool.razor")->chords == std::vector<KeyChord>{KeyChord{Key_X}});
    CHECK(findOverride(*import, "marker.markClip")->chords.empty());
    CHECK(findOverride(*import, "edit.undo")->chords ==
          std::vector<KeyChord>{{Key_Z, Modifier::Control}, {Key_Backspace, Modifier::Alt}});
    CHECK(findOverride(*import, "transport.shuttleStop")->chords ==
          std::vector<KeyChord>{KeyChord{Key_K}, KeyChord{Key_5, Modifier::Keypad}});
    CHECK(findOverride(*import, "sequence.renderInToOut")->chords ==
          std::vector<KeyChord>{KeyChord{Key_F12}});
    CHECK(findOverride(*import, "transport.playStop") == nullptr);   // one shortcut undecodable: left as is
    CHECK(findOverride(*import, "window.timeline")->chords.empty()); // virtual key 0 = no key
    // Same internal name in two contexts maps to two different commands.
    CHECK(findOverride(*import, "timeline.rippleDelete")->chords ==
          std::vector<KeyChord>{{Key_Backspace, Modifier::Control}});
    CHECK(findOverride(*import, "timeline.clearSelection")->chords ==
          std::vector<KeyChord>{KeyChord{Key_Backspace}});
    CHECK(findOverride(*import, "edit.clear") == nullptr); // not in the file
    // Registry order.
    CHECK(std::ranges::is_sorted(import->overrides, {}, [](const CommandOverride& o) {
        const auto all = CommandRegistry::builtin().all();
        return std::ranges::find(all, o.command, &CommandInfo::id) - all.begin();
    }));

    CHECK(import->unmappedCommands == std::vector<std::string>{"context.global/cmd.export.frame"});
    REQUIRE(import->warnings.size() == 1);
    CHECK(contains(import->warnings, "cmd.transport.toggleplay"));
    CHECK(contains(import->warnings, "'19'"));
}

TEST_CASE("Applying a Windows .kys rebinds the keymap", "[keymap][kys]") {
    auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap.has_value());
    const auto import = importKys(kDataDir / "windows-custom.kys");
    REQUIRE(import.has_value());
    REQUIRE(applyKysImport(*keymap, *import));

    CHECK(keymap->resolve(Scope::Application, {Key_X}) == "tool.razor");
    CHECK_FALSE(keymap->resolve(Scope::Application, {Key_C}));
    CHECK(keymap->resolve(Scope::Timeline, {Key_Backspace, Modifier::Control}) == "timeline.rippleDelete");
    CHECK(keymap->resolve(Scope::Timeline, {Key_F12}) == "sequence.renderInToOut");
    CHECK(keymap->resolve(Scope::Timeline, {Key_5, Modifier::Keypad}) == "transport.shuttleStop");
    CHECK(keymap->conflicts().empty()); // Mark Clip was unbound by the same key set
    // Entries equal to the defaults do not become overrides; undecodable ones are left alone.
    CHECK(keymap->resolve(Scope::Application, {Key_Space}) == "transport.playStop");
    CHECK_FALSE(keymap->isOverridden("transport.playStop"));
    CHECK_FALSE(keymap->isOverridden("timeline.clearSelection"));
    CHECK(keymap->isOverridden("edit.undo"));
    // A Windows key set cannot be applied to a macOS keymap.
    auto mac = KeymapManager::withDefaults(Platform::MacOS);
    REQUIRE(mac.has_value());
    CHECK(applyKysImport(*mac, *import).error().code == ErrorCode::InvalidArgument);
    // Linux shares the Windows key set format.
    auto linuxKeymap = KeymapManager::withDefaults(Platform::Linux);
    REQUIRE(linuxKeymap.has_value());
    CHECK(applyKysImport(*linuxKeymap, *import).has_value());
}

TEST_CASE("A macOS .kys maps command/opt/ctrl to Qt's macOS modifiers", "[keymap][kys]") {
    const auto import = importKys(kDataDir / "macos-custom.kys");
    REQUIRE(import.has_value());
    CHECK(import->platform == Platform::MacOS);
    CHECK(findOverride(*import, "edit.undo")->chords == std::vector<KeyChord>{{Key_Z, Modifier::Control}});
    CHECK(findOverride(*import, "marker.clearIn")->chords ==
          std::vector<KeyChord>{{Key_I, Modifier::Meta | Modifier::Alt}}); // ⌃⌥I
    CHECK(findOverride(*import, "edit.rippleDelete")->chords ==
          std::vector<KeyChord>{{Key_Delete, Modifier::Shift}});
    CHECK(findOverride(*import, "timeline.rippleDelete")->chords ==
          std::vector<KeyChord>{{Key_Backspace, Modifier::Alt}});
    CHECK(findOverride(*import, "sequence.snap") == nullptr); // undecodable shortcut: left as is
    CHECK(contains(import->warnings, "modifier.alt"));

    auto keymap = KeymapManager::withDefaults(Platform::MacOS);
    REQUIRE(keymap.has_value());
    REQUIRE(applyKysImport(*keymap, *import));
    CHECK(keymap->shortcutText("marker.clearIn") == "⌃⌥I");
    CHECK(keymap->shortcutText("sequence.snap") == "S");
    CHECK_FALSE(keymap->isOverridden("edit.undo"));             // same as the default Cmd+Z
    CHECK_FALSE(keymap->isOverridden("timeline.rippleDelete")); // same as the default Opt+Delete
    auto windows = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(windows.has_value());
    CHECK(applyKysImport(*windows, *import).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Malformed or foreign files are rejected", "[keymap][kys]") {
    CHECK(parseKys("not xml at all <").error().code == ErrorCode::ParseError);
    CHECK(parseKys("<html><body/></html>").error().code == ErrorCode::ParseError);
    CHECK(parseKys("<PremiereData><shortcuts><platform>amiga</platform></shortcuts></PremiereData>")
              .error()
              .code == ErrorCode::Unsupported);
    CHECK(parseKys("<PremiereData><shortcuts><platform>windows</platform></shortcuts></PremiereData>")
              .error()
              .code == ErrorCode::Unsupported); // no contexts: unknown layout
    CHECK(importKys(kDataDir / "does-not-exist.kys").error().code == ErrorCode::IoError);
    // A path outside the ANSI code page must not throw while building the error message.
    const auto unicodePath = kDataDir / std::filesystem::path(u8"กข-missing") / "x.kys";
    const auto missing = importKys(unicodePath);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::IoError);
    CHECK(missing.error().message.find("x.kys") != std::string::npos);

    const auto minimal = parseKys(R"(<PremiereData Version="3"><shortcuts Version="4">
        <context.global Version="1"><itemcount>1</itemcount>
          <item.0 Version="1"><commandname>cmd.tools.06razor</commandname></item.0>
        </context.global><platform>windows</platform></shortcuts></PremiereData>)");
    REQUIRE(minimal.has_value());
    REQUIRE(minimal->overrides.size() == 1);
    CHECK(minimal->overrides[0] == CommandOverride{"tool.razor", {}});
}
