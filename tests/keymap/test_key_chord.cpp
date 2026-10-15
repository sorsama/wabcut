#include <wabcut/keymap/Key.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::keymap;

namespace {

KeyChord parse(std::string_view text, Platform p) {
    auto chord = parseKeyChord(text, p);
    INFO(text);
    REQUIRE(chord.has_value());
    return *chord;
}

constexpr Modifiers kCtrl = Modifier::Control;
constexpr Modifiers kAlt = Modifier::Alt;
constexpr Modifiers kShift = Modifier::Shift;
constexpr Modifiers kMeta = Modifier::Meta;

} // namespace

TEST_CASE("Key and modifier values are Qt's", "[keymap][key]") {
    // Spot checks; test_qt_compat.cpp compares every constant against <QtCore/qnamespace.h> when Qt is
    // available.
    STATIC_REQUIRE(Key_A == 0x41);
    STATIC_REQUIRE(Key_Delete == 0x01000007);
    STATIC_REQUIRE(Key_Backspace == 0x01000003);
    STATIC_REQUIRE(Key_Return == 0x01000004);
    STATIC_REQUIRE(Key_Enter == 0x01000005);
    STATIC_REQUIRE(Key_F12 == functionKey(12));
    STATIC_REQUIRE(static_cast<std::uint32_t>(Modifier::Control) == 0x04000000);
    STATIC_REQUIRE(static_cast<std::uint32_t>(Modifier::Meta) == 0x10000000);
    STATIC_REQUIRE(KeyChord::fromQt(Key_K, 0x04000000 | 0x02000000) == KeyChord{Key_K, kCtrl | kShift});
    STATIC_REQUIRE(Modifiers::fromBits(0x80000000u | 0x08000000u) == kAlt); // unknown bits dropped
}

TEST_CASE("Windows chord text parses with Windows modifier names", "[keymap][key]") {
    const Platform win = Platform::Windows;
    CHECK(parse("Ctrl+Shift+K", win) == KeyChord{Key_K, kCtrl | kShift});
    CHECK(parse("Alt+Backspace", win) == KeyChord{Key_Backspace, kAlt});
    CHECK(parse("Shift+Delete", win) == KeyChord{Key_Delete, kShift});
    CHECK(parse("Ctrl + Alt + Shift + M", win) == KeyChord{Key_M, kCtrl | kAlt | kShift}); // Adobe's spelling
    CHECK(parse("shift+ctrl+up", win) == KeyChord{Key_Up, kCtrl | kShift}); // any order and case
    CHECK(parse("Win+E", win) == KeyChord{Key_E, kMeta});
    CHECK(parse(",", win) == KeyChord{Key_Comma});
    CHECK(parse("'", win) == KeyChord{Key_Apostrophe});
    CHECK(parse("Ctrl+=", win) == KeyChord{Key_Equal, kCtrl});
    CHECK(parse("Alt+-", win) == KeyChord{Key_Minus, kAlt});
    CHECK(parse("Ctrl++", win) == KeyChord{Key_Plus, kCtrl});
    CHECK(parse("Ctrl + +", win) == KeyChord{Key_Plus, kCtrl});
    CHECK(parse("+", win) == KeyChord{Key_Plus});
    CHECK(parse("Shift+\\", win) == KeyChord{Key_Backslash, kShift});
    CHECK(parse("Shift+`", win) == KeyChord{Key_QuoteLeft, kShift});
    CHECK(parse("Ctrl+Page Up", win) == KeyChord{Key_PageUp, kCtrl});
    CHECK(parse("Enter", win) == KeyChord{Key_Return}); // Adobe's "Enter" is the main Enter/Return key
    CHECK(parse("NumEnter", win) == KeyChord{Key_Enter});
    CHECK(parse("Space", win) == KeyChord{Key_Space});
    CHECK(parse("F1", win) == KeyChord{Key_F1});
    CHECK(parse("f12", win) == KeyChord{Key_F12});
    CHECK(parse("F35", win) == KeyChord{Key_F35});
    CHECK(parse("j", win) == KeyChord{Key_J});
    CHECK(parse("Keypad+5", win) == KeyChord{Key_5, Modifier::Keypad});
    CHECK(parse("Num 5", win) == KeyChord{Key_5, Modifier::Keypad});
}

TEST_CASE("macOS chord text maps Cmd to ControlModifier and Ctrl to MetaModifier", "[keymap][key]") {
    const Platform mac = Platform::MacOS;
    CHECK(parse("Cmd+Opt+I", mac) == KeyChord{Key_I, kCtrl | kAlt});
    CHECK(parse("Shift + Cmd + W", mac) == KeyChord{Key_W, kShift | kCtrl});
    CHECK(parse("Opt+Delete", mac) == KeyChord{Key_Delete, kAlt});
    CHECK(parse("Ctrl+Space", mac) == KeyChord{Key_Space, kMeta}); // the physical Control key
    CHECK(parse("Ctrl+Shift+I", mac) == KeyChord{Key_I, kMeta | kShift});
    CHECK(parse("Option+Command+K", mac) == KeyChord{Key_K, kAlt | kCtrl});
    CHECK(parse("Alt+K", mac) == KeyChord{Key_K, kAlt});
    CHECK(parse("⌥⌘I", mac) == KeyChord{Key_I, kAlt | kCtrl});
    CHECK(parse("⌃⇧I", mac) == KeyChord{Key_I, kMeta | kShift});
    CHECK(parse("⌘⌫", mac) == KeyChord{Key_Backspace, kCtrl});
    CHECK(parse("⌘+", mac) == KeyChord{Key_Plus, kCtrl});
    CHECK(parse("Return", mac) == KeyChord{Key_Return});
}

TEST_CASE("Invalid chord text is rejected with a ParseError", "[keymap][key]") {
    for (const char* bad : {"0x800000041",
                            "Ctrl+0x800000041",
                            "0x80000000",
                            "0x0",
                            "0xG1",
                            "0x",
                            "",
                            "   ",
                            "Ctrl+",
                            "Ctrl+Shift",
                            "Ctrl+Ctrl+K",
                            "Hyper+K",
                            "Ctrl++Shift",
                            "K+Ctrl",
                            "Ctrl+Foo",
                            "F0",
                            "F36",
                            "Shift+Num",
                            "Ctrl+Shift+",
                            "+K",
                            "Ctrl"}) {
        INFO(bad);
        const auto r = parseKeyChord(bad, Platform::Windows);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::ParseError);
    }
    // macOS-only names are refused on Windows/Linux and vice versa, so a table cannot mix them up.
    CHECK_FALSE(parseKeyChord("Cmd+K", Platform::Windows));
    CHECK_FALSE(parseKeyChord("Opt+K", Platform::Linux));
    CHECK_FALSE(parseKeyChord("⌘K", Platform::Windows));
    CHECK_FALSE(parseKeyChord("Win+K", Platform::MacOS));
}

TEST_CASE("Chords format canonically per platform", "[keymap][key]") {
    const KeyChord ctrlShiftK{Key_K, kCtrl | kShift};
    CHECK(formatKeyChord(ctrlShiftK, Platform::Windows) == "Ctrl+Shift+K");
    CHECK(formatKeyChord(ctrlShiftK, Platform::MacOS) == "Shift+Cmd+K");
    CHECK(formatKeyChord(ctrlShiftK, Platform::MacOS, ChordFormat::Native) == "⇧⌘K");
    CHECK(formatKeyChord({Key_I, kCtrl | kAlt}, Platform::MacOS) == "Opt+Cmd+I");
    CHECK(formatKeyChord({Key_M, kCtrl | kAlt | kShift}, Platform::Windows) == "Ctrl+Alt+Shift+M");
    CHECK(formatKeyChord({Key_Space, kMeta}, Platform::MacOS) == "Ctrl+Space");
    CHECK(formatKeyChord({Key_Space, kMeta}, Platform::MacOS, ChordFormat::Native) == "⌃Space");
    CHECK(formatKeyChord({Key_E, kMeta}, Platform::Windows, ChordFormat::Native) == "Win+E");
    CHECK(formatKeyChord({Key_E, kMeta}, Platform::Linux, ChordFormat::Native) == "Meta+E");
    CHECK(formatKeyChord({Key_Return}, Platform::Windows) == "Enter");
    CHECK(formatKeyChord({Key_Return}, Platform::MacOS) == "Return");
    CHECK(formatKeyChord({Key_Return}, Platform::MacOS, ChordFormat::Native) == "↩");
    CHECK(formatKeyChord({Key_Backspace, kAlt}, Platform::MacOS, ChordFormat::Native) == "⌥⌫");
    CHECK(formatKeyChord({Key_Delete, kShift}, Platform::MacOS, ChordFormat::Native) == "⇧⌦");
    CHECK(formatKeyChord({Key_PageUp, kCtrl}, Platform::Windows, ChordFormat::Native) == "Ctrl+Page Up");
    CHECK(formatKeyChord({Key_Plus, kCtrl}, Platform::Windows) == "Ctrl++");
    CHECK(formatKeyChord({Key_5, Modifier::Keypad}, Platform::Windows) == "Keypad+5");
    CHECK(formatKeyChord({Key_5, Modifier::Keypad}, Platform::Windows, ChordFormat::Native) == "Num 5");
    CHECK(formatKeyChord({0x01001234}, Platform::Windows) == "0x1001234");
}

TEST_CASE("Text format round-trips every chord on every platform", "[keymap][key]") {
    const Platform platform = GENERATE(Platform::Windows, Platform::MacOS, Platform::Linux);
    std::vector<int> keys;
    for (int k = 0x21; k < 0x7f; ++k) {
        keys.push_back(k); // lower-case codes are not Qt keys, but still round-trip (as hex)
    }
    for (int n = 1; n <= kMaxFunctionKey; ++n) {
        keys.push_back(functionKey(n));
    }
    for (int k : {Key_Space,      Key_Escape,
                  Key_Tab,        Key_Backspace,
                  Key_Return,     Key_Enter,
                  Key_Insert,     Key_Delete,
                  Key_Pause,      Key_Print,
                  Key_Clear,      Key_Home,
                  Key_End,        Key_Left,
                  Key_Up,         Key_Right,
                  Key_Down,       Key_PageUp,
                  Key_PageDown,   0xc4 /* Ä */,
                  0x0416 /* Ж */, 0x01001234,
                  0x7fffffff,     0x2318 /* ⌘ as a key */,
                  0x21e7 /* ⇧ */, 0x232b /* ⌫ as a character */,
                  0x2190 /* ← */}) {
        keys.push_back(k);
    }
    for (std::uint32_t bits = 0; bits < 32; ++bits) {
        Modifiers mods;
        if (bits & 1)
            mods |= Modifier::Shift;
        if (bits & 2)
            mods |= Modifier::Control;
        if (bits & 4)
            mods |= Modifier::Alt;
        if (bits & 8)
            mods |= Modifier::Meta;
        if (bits & 16)
            mods |= Modifier::Keypad;
        for (int key : keys) {
            const KeyChord chord{key, mods};
            const std::string text = formatKeyChord(chord, platform, ChordFormat::Text);
            const auto back = parseKeyChord(text, platform);
            INFO(text);
            REQUIRE(back.has_value());
            CHECK(*back == chord);
            // Native text round-trips too, except keypad chords (displayed like their main-keyboard twins).
            if (!mods.has(Modifier::Keypad)) {
                const std::string native = formatKeyChord(chord, platform, ChordFormat::Native);
                const auto nativeBack = parseKeyChord(native, platform);
                INFO(native);
                REQUIRE(nativeBack.has_value());
                CHECK(*nativeBack == chord);
            }
        }
    }
}

TEST_CASE("Parsing then formatting normalises spelling", "[keymap][key]") {
    CHECK(formatKeyChord(parse("shift + ctrl + k", Platform::Windows), Platform::Windows) == "Ctrl+Shift+K");
    CHECK(formatKeyChord(parse("Cmd+Opt+I", Platform::MacOS), Platform::MacOS) == "Opt+Cmd+I");
    CHECK(formatKeyChord(parse("Cmd + Forward Delete", Platform::MacOS), Platform::MacOS) == "Cmd+Delete");
    CHECK(formatKeyChord(parse("Ctrl + Page Down", Platform::Windows), Platform::Windows) == "Ctrl+PageDown");
}

TEST_CASE("Qt event quirks normalise to the bound chord", "[keymap][key]") {
    // Shift+Tab arrives as Key_Backtab.
    CHECK(KeyChord{Key_Backtab, kShift}.normalized() == KeyChord{Key_Tab, kShift});
    CHECK(KeyChord{Key_Backtab}.normalized() == KeyChord{Key_Tab, kShift});
    // GroupSwitch is dropped; Keypad is kept for keypad-specific bindings.
    CHECK(KeyChord{Key_A, Modifiers(Modifier::GroupSwitch) | kCtrl}.normalized() == KeyChord{Key_A, kCtrl});
    CHECK(KeyChord{Key_5, Modifier::Keypad}.normalized() == KeyChord{Key_5, Modifier::Keypad});
    // Shift+<symbol> arrives as the shifted symbol on a US layout.
    CHECK(unshiftedUsLayout({Key_Colon, kShift}) == KeyChord{Key_Semicolon, kShift});
    CHECK(unshiftedUsLayout({Key_Colon, kCtrl | kShift}) == KeyChord{Key_Semicolon, kCtrl | kShift});
    CHECK(unshiftedUsLayout({Key_Exclam, kShift}) == KeyChord{Key_1, kShift});
    CHECK(unshiftedUsLayout({Key_Less, kAlt | kShift}) == KeyChord{Key_Comma, kAlt | kShift});
    CHECK(unshiftedUsLayout({Key_Bar, kShift}) == KeyChord{Key_Backslash, kShift});
    CHECK(unshiftedUsLayout({Key_AsciiTilde, kShift}) == KeyChord{Key_QuoteLeft, kShift});
    CHECK_FALSE(unshiftedUsLayout({Key_Colon}));     // no Shift: not a shifted symbol
    CHECK_FALSE(unshiftedUsLayout({Key_K, kShift})); // letters are reported unshifted already
    // Bare modifier keys never form a chord.
    CHECK_FALSE(KeyChord{Key_Shift, kShift}.isValid());
    CHECK_FALSE(KeyChord{Key_Control}.isValid());
    CHECK_FALSE(KeyChord{0}.isValid());
    CHECK_FALSE(KeyChord{Key_unknown}.isValid());
}
