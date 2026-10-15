#pragma once

// Key primitives: key codes, modifiers and key chords, with no Qt dependency.
//
// Key codes and modifier bits are numerically identical to Qt::Key and Qt::KeyboardModifier, so the app
// passes QKeyEvent::key() and QKeyEvent::modifiers().toInt() straight through (see KeyChord::fromQt).
// The constants deliberately keep Qt's spelling (Key_A, Key_Delete, ...) so call sites read the same.
//
// macOS quirk (inherited from Qt): Qt reports the Command key (⌘) as ControlModifier and the physical
// Control key (⌃) as MetaModifier. So Modifier::Control means "Ctrl on Windows/Linux, ⌘ on macOS" and
// Modifier::Meta means "Windows/Super key on Windows/Linux, ⌃ on macOS". Chord text is parsed and
// formatted per platform with that mapping: "Cmd+K" on macOS and "Ctrl+K" on Windows are the same chord
// (Control | Key_K), while "Ctrl+Space" on macOS is Meta | Key_Space.
//
// Other Qt behaviours the keymap relies on (see KeyChord::normalized and KeymapManager::resolve):
// - Shift+Tab arrives as Key_Backtab.
// - Shift+<symbol> arrives as the shifted symbol (Shift+; is Key_Colon on a US layout).
// - Keypad keys carry KeypadModifier; on macOS Qt also sets it for the arrow keys.

#include <wabcut/keymap/Platform.h>
#include <wabcut/model/Error.h>

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::keymap {

// ---- Key codes (values of Qt::Key) ----------------------------------------------------------------------
// Printable keys use the ASCII code of the unshifted character; letters use the upper-case code.

inline constexpr int Key_Space = 0x20;
inline constexpr int Key_Exclam = 0x21;
inline constexpr int Key_QuoteDbl = 0x22;
inline constexpr int Key_NumberSign = 0x23;
inline constexpr int Key_Dollar = 0x24;
inline constexpr int Key_Percent = 0x25;
inline constexpr int Key_Ampersand = 0x26;
inline constexpr int Key_Apostrophe = 0x27;
inline constexpr int Key_ParenLeft = 0x28;
inline constexpr int Key_ParenRight = 0x29;
inline constexpr int Key_Asterisk = 0x2a;
inline constexpr int Key_Plus = 0x2b;
inline constexpr int Key_Comma = 0x2c;
inline constexpr int Key_Minus = 0x2d;
inline constexpr int Key_Period = 0x2e;
inline constexpr int Key_Slash = 0x2f;
inline constexpr int Key_0 = 0x30;
inline constexpr int Key_1 = 0x31;
inline constexpr int Key_2 = 0x32;
inline constexpr int Key_3 = 0x33;
inline constexpr int Key_4 = 0x34;
inline constexpr int Key_5 = 0x35;
inline constexpr int Key_6 = 0x36;
inline constexpr int Key_7 = 0x37;
inline constexpr int Key_8 = 0x38;
inline constexpr int Key_9 = 0x39;
inline constexpr int Key_Colon = 0x3a;
inline constexpr int Key_Semicolon = 0x3b;
inline constexpr int Key_Less = 0x3c;
inline constexpr int Key_Equal = 0x3d;
inline constexpr int Key_Greater = 0x3e;
inline constexpr int Key_Question = 0x3f;
inline constexpr int Key_At = 0x40;
inline constexpr int Key_A = 0x41;
inline constexpr int Key_B = 0x42;
inline constexpr int Key_C = 0x43;
inline constexpr int Key_D = 0x44;
inline constexpr int Key_E = 0x45;
inline constexpr int Key_F = 0x46;
inline constexpr int Key_G = 0x47;
inline constexpr int Key_H = 0x48;
inline constexpr int Key_I = 0x49;
inline constexpr int Key_J = 0x4a;
inline constexpr int Key_K = 0x4b;
inline constexpr int Key_L = 0x4c;
inline constexpr int Key_M = 0x4d;
inline constexpr int Key_N = 0x4e;
inline constexpr int Key_O = 0x4f;
inline constexpr int Key_P = 0x50;
inline constexpr int Key_Q = 0x51;
inline constexpr int Key_R = 0x52;
inline constexpr int Key_S = 0x53;
inline constexpr int Key_T = 0x54;
inline constexpr int Key_U = 0x55;
inline constexpr int Key_V = 0x56;
inline constexpr int Key_W = 0x57;
inline constexpr int Key_X = 0x58;
inline constexpr int Key_Y = 0x59;
inline constexpr int Key_Z = 0x5a;
inline constexpr int Key_BracketLeft = 0x5b;
inline constexpr int Key_Backslash = 0x5c;
inline constexpr int Key_BracketRight = 0x5d;
inline constexpr int Key_AsciiCircum = 0x5e;
inline constexpr int Key_Underscore = 0x5f;
inline constexpr int Key_QuoteLeft = 0x60;
inline constexpr int Key_BraceLeft = 0x7b;
inline constexpr int Key_Bar = 0x7c;
inline constexpr int Key_BraceRight = 0x7d;
inline constexpr int Key_AsciiTilde = 0x7e;

inline constexpr int Key_Escape = 0x01000000;
inline constexpr int Key_Tab = 0x01000001;
inline constexpr int Key_Backtab = 0x01000002;
inline constexpr int Key_Backspace = 0x01000003; // macOS keyboards label this key "delete" (⌫)
inline constexpr int Key_Return = 0x01000004;    // main Enter/Return key
inline constexpr int Key_Enter = 0x01000005;     // keypad Enter (⌤)
inline constexpr int Key_Insert = 0x01000006;
inline constexpr int Key_Delete =
    0x01000007; // forward delete (⌦); Adobe's macOS tables call it "Forward Delete"
inline constexpr int Key_Pause = 0x01000008;
inline constexpr int Key_Print = 0x01000009;
inline constexpr int Key_Clear = 0x0100000b;
inline constexpr int Key_Home = 0x01000010;
inline constexpr int Key_End = 0x01000011;
inline constexpr int Key_Left = 0x01000012;
inline constexpr int Key_Up = 0x01000013;
inline constexpr int Key_Right = 0x01000014;
inline constexpr int Key_Down = 0x01000015;
inline constexpr int Key_PageUp = 0x01000016;
inline constexpr int Key_PageDown = 0x01000017;
inline constexpr int Key_Shift = 0x01000020;
inline constexpr int Key_Control = 0x01000021;
inline constexpr int Key_Meta = 0x01000022;
inline constexpr int Key_Alt = 0x01000023;
inline constexpr int Key_CapsLock = 0x01000024;
inline constexpr int Key_NumLock = 0x01000025;
inline constexpr int Key_ScrollLock = 0x01000026;
inline constexpr int Key_AltGr = 0x01001103;
inline constexpr int Key_F1 = 0x01000030; // F1..F35 are contiguous, see functionKey()
inline constexpr int Key_F2 = 0x01000031;
inline constexpr int Key_F3 = 0x01000032;
inline constexpr int Key_F4 = 0x01000033;
inline constexpr int Key_F5 = 0x01000034;
inline constexpr int Key_F6 = 0x01000035;
inline constexpr int Key_F7 = 0x01000036;
inline constexpr int Key_F8 = 0x01000037;
inline constexpr int Key_F9 = 0x01000038;
inline constexpr int Key_F10 = 0x01000039;
inline constexpr int Key_F11 = 0x0100003a;
inline constexpr int Key_F12 = 0x0100003b;
inline constexpr int Key_F35 = 0x01000052;
inline constexpr int Key_unknown = 0x01ffffff;

inline constexpr int kMaxFunctionKey = 35;

/// Qt key code of function key F`n` (1..35); 0 when out of range.
constexpr int functionKey(int n) {
    return n >= 1 && n <= kMaxFunctionKey ? Key_F1 + (n - 1) : 0;
}

/// True for keys that are modifiers themselves (Shift, Ctrl, Alt, Meta, AltGr, lock keys). Pressing one
/// alone never triggers a command.
constexpr bool isModifierKey(int key) {
    return key == Key_Shift || key == Key_Control || key == Key_Meta || key == Key_Alt || key == Key_AltGr ||
           key == Key_CapsLock || key == Key_NumLock || key == Key_ScrollLock;
}

// ---- Modifiers (values of Qt::KeyboardModifier) -------------------------------------------------------

enum class Modifier : std::uint32_t {
    None = 0,
    Shift = 0x02000000,
    Control = 0x04000000, ///< Ctrl on Windows/Linux, ⌘ Command on macOS
    Alt = 0x08000000,     ///< Alt on Windows/Linux, ⌥ Option on macOS
    Meta = 0x10000000,    ///< Windows/Super key on Windows/Linux, ⌃ Control on macOS
    Keypad = 0x20000000,  ///< key is on the numeric keypad (Qt also sets it for macOS arrow keys)
    GroupSwitch = 0x40000000,
};

/// A set of Modifier flags. Bit-compatible with Qt::KeyboardModifiers.
class Modifiers {
public:
    static constexpr std::uint32_t kMask = 0x7e000000; ///< every Modifier bit

    constexpr Modifiers() = default;
    constexpr Modifiers(Modifier m) : m_bits(static_cast<std::uint32_t>(m)) {} // NOLINT: implicit by design

    /// Takes Qt::KeyboardModifiers bits (e.g. QKeyEvent::modifiers().toInt()); unknown bits are dropped.
    static constexpr Modifiers fromBits(std::uint32_t bits) {
        Modifiers m;
        m.m_bits = bits & kMask;
        return m;
    }

    constexpr std::uint32_t bits() const { return m_bits; }
    constexpr bool empty() const { return m_bits == 0; }
    constexpr bool has(Modifier m) const { return (m_bits & static_cast<std::uint32_t>(m)) != 0; }
    constexpr Modifiers with(Modifiers o) const { return fromBits(m_bits | o.m_bits); }
    constexpr Modifiers without(Modifiers o) const { return fromBits(m_bits & ~o.m_bits); }
    constexpr Modifiers operator|(Modifiers o) const { return with(o); }
    constexpr Modifiers& operator|=(Modifiers o) {
        m_bits |= o.m_bits;
        return *this;
    }

    constexpr auto operator<=>(const Modifiers&) const = default;

private:
    std::uint32_t m_bits = 0;
};

constexpr Modifiers operator|(Modifier a, Modifier b) {
    return Modifiers(a) | Modifiers(b);
}

// ---- Key chords ---------------------------------------------------------------------------------------

/// One key plus the modifiers held with it, e.g. Control|Key_K. Ordered and hashable by value.
struct KeyChord {
    int key = 0;
    Modifiers mods;

    constexpr KeyChord() = default;
    constexpr KeyChord(int k, Modifiers m = {}) : key(k), mods(m) {} // NOLINT: implicit from a key code

    /// Builds a chord from a Qt key event: KeyChord::fromQt(e->key(), e->modifiers().toInt()).
    static constexpr KeyChord fromQt(int qtKey, std::uint32_t qtModifiers) {
        return {qtKey, Modifiers::fromBits(qtModifiers)};
    }

    /// False for "no key", Key_unknown and bare modifier keys.
    constexpr bool isValid() const { return key > 0 && key != Key_unknown && !isModifierKey(key); }

    /// Folds Qt event quirks into the form bindings use: drops GroupSwitch and turns Key_Backtab into
    /// Shift+Key_Tab. Keypad is kept (a binding may target the keypad); resolve() also retries without it.
    constexpr KeyChord normalized() const {
        KeyChord c{key, mods.without(Modifier::GroupSwitch)};
        if (c.key == Key_Backtab) {
            c.key = Key_Tab;
            c.mods |= Modifier::Shift;
        }
        return c;
    }

    constexpr auto operator<=>(const KeyChord&) const = default;
};

/// For Shift+<symbol> chords, the chord with the US-layout base key (Shift+Key_Colon → Shift+Key_Semicolon,
/// Shift+Key_Exclam → Shift+Key_1). Qt reports the shifted symbol as the key, while Premiere tables name the
/// physical key. Returns nullopt when the chord has no Shift or the key is not a shifted US symbol.
std::optional<KeyChord> unshiftedUsLayout(KeyChord chord);

/// How a chord is rendered as text.
enum class ChordFormat {
    /// Storage/interchange text that parseKeyChord() reads back for every valid chord:
    /// "Ctrl+Alt+Shift+K" on Windows/Linux, "Ctrl+Opt+Shift+Cmd+K" order on macOS.
    Text,
    /// Display text for menus and tooltips: "Ctrl+Shift+K" on Windows/Linux, "⇧⌘K" on macOS.
    /// parseKeyChord() also reads it back, except for keypad chords.
    Native,
};

/// Parses chord text such as "Ctrl+Shift+K", "Cmd+Opt+I", "Alt+Backspace", "Shift+Delete", "Ctrl + =" or
/// "⌥⌘I". Modifier names are interpreted for `platform` (see the macOS note at the top of this header):
///   Windows/Linux: Ctrl|Control, Alt, Shift, Meta|Win|Super, Keypad|Num
///   macOS:         Cmd|Command|⌘ (Control), Opt|Option|Alt|⌥ (Alt), Ctrl|Control|⌃ (Meta), Shift|⇧, Keypad
/// Cmd/Opt are rejected on Windows/Linux so a mac name cannot slip into a Windows table. Names are
/// case-insensitive, any modifier order is accepted, and spaces around '+' are ignored. The key is the last
/// token: a printable character, F1..F35, or a name (Space, Tab, Backspace, Delete|Forward Delete,
/// Return|Enter (both mean the main key), NumEnter (keypad Enter), Insert, Escape, Home, End, PageUp|Page Up,
/// PageDown, Left, Right, Up, Down, Pause, Print, Clear, Plus, Minus, ...). "Ctrl++" binds the '+' key.
std::expected<KeyChord, Error> parseKeyChord(std::string_view text, Platform platform);

/// Formats a chord for `platform`. Text output is canonical (fixed modifier order and key names), so
/// formatKeyChord(parseKeyChord(s)) normalises any accepted spelling.
std::string formatKeyChord(KeyChord chord, Platform platform, ChordFormat format = ChordFormat::Text);

} // namespace wabcut::keymap
