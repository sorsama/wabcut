#include <wabcut/keymap/Key.h>

#include <array>
#include <format>
#include <vector>

namespace wabcut::keymap {
namespace {

constexpr char toLowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(a[i]) != toLowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Decodes `s` when it is exactly one UTF-8 code point; nullopt otherwise.
std::optional<char32_t> singleCodePoint(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    const auto b0 = static_cast<unsigned char>(s[0]);
    std::size_t len = 0;
    char32_t cp = 0;
    if (b0 < 0x80) {
        len = 1;
        cp = b0;
    } else if ((b0 & 0xe0) == 0xc0) {
        len = 2;
        cp = b0 & 0x1f;
    } else if ((b0 & 0xf0) == 0xe0) {
        len = 3;
        cp = b0 & 0x0f;
    } else if ((b0 & 0xf8) == 0xf0) {
        len = 4;
        cp = b0 & 0x07;
    } else {
        return std::nullopt;
    }
    if (s.size() != len) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < len; ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if ((b & 0xc0) != 0x80) {
            return std::nullopt;
        }
        cp = (cp << 6) | (b & 0x3f);
    }
    return cp;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

struct NamedKey {
    std::string_view name; // lower-case ASCII, or a glyph
    int key;
};

// Every key name the parser accepts. Canonical output names come from keyName().
constexpr std::array kKeyNames{
    NamedKey{"space", Key_Space},
    NamedKey{"spacebar", Key_Space},
    NamedKey{"tab", Key_Tab},
    NamedKey{"⇥", Key_Tab},
    NamedKey{"backspace", Key_Backspace},
    NamedKey{"⌫", Key_Backspace},
    NamedKey{"return", Key_Return},
    NamedKey{"enter", Key_Return},
    NamedKey{"↩", Key_Return},
    NamedKey{"numenter", Key_Enter},
    NamedKey{"num enter", Key_Enter},
    NamedKey{"keypadenter", Key_Enter},
    NamedKey{"⌤", Key_Enter},
    NamedKey{"delete", Key_Delete},
    NamedKey{"del", Key_Delete},
    NamedKey{"forward delete", Key_Delete},
    NamedKey{"forwarddelete", Key_Delete},
    NamedKey{"⌦", Key_Delete},
    NamedKey{"insert", Key_Insert},
    NamedKey{"ins", Key_Insert},
    NamedKey{"escape", Key_Escape},
    NamedKey{"esc", Key_Escape},
    NamedKey{"⎋", Key_Escape},
    NamedKey{"home", Key_Home},
    NamedKey{"↖", Key_Home},
    NamedKey{"end", Key_End},
    NamedKey{"↘", Key_End},
    NamedKey{"pageup", Key_PageUp},
    NamedKey{"page up", Key_PageUp},
    NamedKey{"pgup", Key_PageUp},
    NamedKey{"⇞", Key_PageUp},
    NamedKey{"pagedown", Key_PageDown},
    NamedKey{"page down", Key_PageDown},
    NamedKey{"pgdown", Key_PageDown},
    NamedKey{"pgdn", Key_PageDown},
    NamedKey{"⇟", Key_PageDown},
    NamedKey{"left", Key_Left},
    NamedKey{"←", Key_Left},
    NamedKey{"up", Key_Up},
    NamedKey{"↑", Key_Up},
    NamedKey{"right", Key_Right},
    NamedKey{"→", Key_Right},
    NamedKey{"down", Key_Down},
    NamedKey{"↓", Key_Down},
    NamedKey{"pause", Key_Pause},
    NamedKey{"print", Key_Print},
    NamedKey{"clear", Key_Clear},
    NamedKey{"plus", Key_Plus},
    NamedKey{"minus", Key_Minus},
    NamedKey{"equal", Key_Equal},
    NamedKey{"comma", Key_Comma},
    NamedKey{"period", Key_Period},
    NamedKey{"semicolon", Key_Semicolon},
    NamedKey{"apostrophe", Key_Apostrophe},
    NamedKey{"slash", Key_Slash},
    NamedKey{"backslash", Key_Backslash},
    NamedKey{"bracketleft", Key_BracketLeft},
    NamedKey{"bracketright", Key_BracketRight},
    NamedKey{"quoteleft", Key_QuoteLeft},
    NamedKey{"backquote", Key_QuoteLeft},
    NamedKey{"grave", Key_QuoteLeft},
};

// macOS modifier glyphs in Apple's display order ⌃⌥⇧⌘.
constexpr std::string_view kGlyphCtrl = "⌃";
constexpr std::string_view kGlyphOpt = "⌥";
constexpr std::string_view kGlyphShift = "⇧";
constexpr std::string_view kGlyphCmd = "⌘";

bool isModifierGlyph(std::string_view s) {
    return s == kGlyphCtrl || s == kGlyphOpt || s == kGlyphShift || s == kGlyphCmd;
}

Error parseError(std::string_view text, std::string_view why) {
    return {ErrorCode::ParseError, std::format("invalid key chord '{}': {}", text, why)};
}

struct KeyName {
    int key = 0;
    Modifiers extra; // Keypad for "Num 1"-style names
};

std::optional<KeyName> keyFromName(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const NamedKey& nk : kKeyNames) {
        if (iequals(nk.name, name)) {
            return KeyName{nk.key, {}};
        }
    }
    // F1..F35
    if ((name[0] == 'F' || name[0] == 'f') && name.size() >= 2 && name.size() <= 3) {
        int n = 0;
        for (char c : name.substr(1)) {
            if (c < '0' || c > '9') {
                n = -1;
                break;
            }
            n = n * 10 + (c - '0');
        }
        if (n > 0 && functionKey(n) != 0) {
            return KeyName{functionKey(n), {}};
        }
    }
    // Raw key code, used to round-trip keys that have no name.
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        const std::string_view digits = name.substr(2);
        if (digits.size() > 8) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (char c : digits) {
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
            value = value * 16 + digit; // at most 8 digits: cannot overflow 32 bits
        }
        if (value == 0 || value > 0x7fffffffu) {
            return std::nullopt;
        }
        return KeyName{static_cast<int>(value), {}};
    }
    // "Num 1" / "Num1" / "Num +": keypad keys in Windows display text.
    if (name.size() > 3 && iequals(name.substr(0, 3), "num")) {
        if (auto inner = keyFromName(trim(name.substr(3))); inner && inner->extra.empty()) {
            return KeyName{inner->key, Modifier::Keypad};
        }
    }
    if (auto cp = singleCodePoint(name)) {
        char32_t c = *cp;
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        } else if (c >= 0xe0 && c <= 0xfe && c != 0xf7) {
            c -= 0x20; // Latin-1 lower → upper, as Qt reports letter keys
        }
        if (c > 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xa0)) {
            return KeyName{static_cast<int>(c), {}};
        }
    }
    return std::nullopt;
}

enum class ModToken { Unknown, MacOnly, NonMacOnly };

std::optional<Modifier> modifierFromName(std::string_view tok, Platform p, ModToken& why) {
    const bool mac = usesMacModifiers(p);
    why = ModToken::Unknown;
    if (iequals(tok, "shift") || tok == kGlyphShift) {
        return Modifier::Shift;
    }
    if (iequals(tok, "keypad") || iequals(tok, "num")) {
        return Modifier::Keypad;
    }
    if (iequals(tok, "ctrl") || iequals(tok, "control")) {
        return mac ? Modifier::Meta : Modifier::Control;
    }
    if (iequals(tok, "alt")) {
        return Modifier::Alt;
    }
    if (iequals(tok, "meta")) {
        return Modifier::Meta;
    }
    if (iequals(tok, "cmd") || iequals(tok, "command") || tok == kGlyphCmd) {
        if (!mac) {
            why = ModToken::MacOnly;
            return std::nullopt;
        }
        return Modifier::Control;
    }
    if (iequals(tok, "opt") || iequals(tok, "option") || tok == kGlyphOpt) {
        if (!mac) {
            why = ModToken::MacOnly;
            return std::nullopt;
        }
        return Modifier::Alt;
    }
    if (tok == kGlyphCtrl) {
        if (!mac) {
            why = ModToken::MacOnly;
            return std::nullopt;
        }
        return Modifier::Meta;
    }
    if (iequals(tok, "win") || iequals(tok, "windows") || iequals(tok, "super")) {
        if (mac) {
            why = ModToken::NonMacOnly;
            return std::nullopt;
        }
        return Modifier::Meta;
    }
    return std::nullopt;
}

std::expected<void, Error> addModifier(Modifiers& mods, std::string_view tok, std::string_view text,
                                       Platform p) {
    ModToken why{};
    const auto m = modifierFromName(tok, p, why);
    if (!m) {
        if (why == ModToken::MacOnly) {
            return std::unexpected(parseError(text, std::format("'{}' is a macOS modifier", tok)));
        }
        if (why == ModToken::NonMacOnly) {
            return std::unexpected(parseError(text, std::format("'{}' does not exist on macOS", tok)));
        }
        return std::unexpected(parseError(text, std::format("unknown modifier '{}'", tok)));
    }
    if (mods.has(*m)) {
        return std::unexpected(parseError(text, std::format("modifier '{}' repeated", tok)));
    }
    mods |= *m;
    return {};
}

std::expected<KeyChord, Error> finishChord(Modifiers mods, std::string_view keyText, std::string_view text) {
    if (keyText.empty()) {
        return std::unexpected(parseError(text, "missing key"));
    }
    const auto key = keyFromName(keyText);
    if (!key) {
        return std::unexpected(parseError(text, std::format("unknown key '{}'", keyText)));
    }
    if (mods.has(Modifier::Keypad) && key->extra.has(Modifier::Keypad)) {
        return std::unexpected(parseError(text, "keypad given twice"));
    }
    KeyChord chord{key->key, mods | key->extra};
    if (!chord.isValid()) {
        return std::unexpected(parseError(text, "a modifier key cannot be the chord's key"));
    }
    return chord;
}

/// "⌥⌘I" style: leading modifier glyphs followed by a key name. nullopt when `text` has no glyph prefix.
std::optional<std::expected<KeyChord, Error>> parseGlyphPrefix(std::string_view text, Platform p) {
    std::string_view rest = text;
    Modifiers mods;
    bool any = false;
    while (true) {
        std::string_view glyph;
        for (std::string_view g : {kGlyphCtrl, kGlyphOpt, kGlyphShift, kGlyphCmd}) {
            if (rest.starts_with(g)) {
                glyph = g;
            }
        }
        if (glyph.empty()) {
            break;
        }
        if (auto r = addModifier(mods, glyph, text, p); !r) {
            return std::expected<KeyChord, Error>(std::unexpected(r.error()));
        }
        rest.remove_prefix(glyph.size());
        any = true;
    }
    if (!any) {
        return std::nullopt;
    }
    return finishChord(mods, trim(rest), text);
}

std::string keyName(int key, Platform p, ChordFormat format) {
    const bool mac = usesMacModifiers(p);
    const bool native = format == ChordFormat::Native;
    switch (key) {
    case Key_Return:
        return native && mac ? "↩" : (mac ? "Return" : "Enter");
    case Key_Enter:
        return native ? (mac ? "⌤" : "Num Enter") : "NumEnter";
    case Key_Backspace:
        return native && mac ? "⌫" : "Backspace";
    case Key_Delete:
        return native && mac ? "⌦" : "Delete";
    case Key_Space:
        return "Space";
    case Key_Tab:
        return native && mac ? "⇥" : "Tab";
    case Key_Escape:
        return native ? (mac ? "⎋" : "Esc") : "Escape";
    case Key_Insert:
        return "Insert";
    case Key_Home:
        return native && mac ? "↖" : "Home";
    case Key_End:
        return native && mac ? "↘" : "End";
    case Key_PageUp:
        return native ? (mac ? "⇞" : "Page Up") : "PageUp";
    case Key_PageDown:
        return native ? (mac ? "⇟" : "Page Down") : "PageDown";
    case Key_Left:
        return native && mac ? "←" : "Left";
    case Key_Up:
        return native && mac ? "↑" : "Up";
    case Key_Right:
        return native && mac ? "→" : "Right";
    case Key_Down:
        return native && mac ? "↓" : "Down";
    case Key_Pause:
        return "Pause";
    case Key_Print:
        return "Print";
    case Key_Clear:
        return "Clear";
    default:
        break;
    }
    if (key >= Key_F1 && key <= Key_F35) {
        return std::format("F{}", key - Key_F1 + 1);
    }
    if ((key > 0x20 && key < 0x7f) || (key >= 0xa0 && key <= 0x10ffff && (key < 0xd800 || key > 0xdfff))) {
        std::string s;
        appendUtf8(s, static_cast<char32_t>(key));
        // A character that reads back as something else (a glyph name such as ⌫, a modifier glyph such as ⌘,
        // or a lower-case letter, which parses as the upper-case key) is written as its code instead.
        const auto back = keyFromName(s);
        if (back && back->key == key && back->extra.empty() && !isModifierGlyph(s)) {
            return s;
        }
    }
    return std::format("0x{:X}", static_cast<std::uint32_t>(key));
}

} // namespace

std::optional<KeyChord> unshiftedUsLayout(KeyChord chord) {
    if (!chord.mods.has(Modifier::Shift)) {
        return std::nullopt;
    }
    int base = 0;
    switch (chord.key) {
    case Key_Exclam:
        base = Key_1;
        break;
    case Key_At:
        base = Key_2;
        break;
    case Key_NumberSign:
        base = Key_3;
        break;
    case Key_Dollar:
        base = Key_4;
        break;
    case Key_Percent:
        base = Key_5;
        break;
    case Key_AsciiCircum:
        base = Key_6;
        break;
    case Key_Ampersand:
        base = Key_7;
        break;
    case Key_Asterisk:
        base = Key_8;
        break;
    case Key_ParenLeft:
        base = Key_9;
        break;
    case Key_ParenRight:
        base = Key_0;
        break;
    case Key_Underscore:
        base = Key_Minus;
        break;
    case Key_Plus:
        base = Key_Equal;
        break;
    case Key_BraceLeft:
        base = Key_BracketLeft;
        break;
    case Key_BraceRight:
        base = Key_BracketRight;
        break;
    case Key_Bar:
        base = Key_Backslash;
        break;
    case Key_Colon:
        base = Key_Semicolon;
        break;
    case Key_QuoteDbl:
        base = Key_Apostrophe;
        break;
    case Key_Less:
        base = Key_Comma;
        break;
    case Key_Greater:
        base = Key_Period;
        break;
    case Key_Question:
        base = Key_Slash;
        break;
    case Key_AsciiTilde:
        base = Key_QuoteLeft;
        break;
    default:
        return std::nullopt;
    }
    return KeyChord{base, chord.mods};
}

std::expected<KeyChord, Error> parseKeyChord(std::string_view text, Platform platform) {
    const std::string_view s = trim(text);
    if (s.empty()) {
        return std::unexpected(parseError(text, "empty"));
    }
    if (auto glyph = parseGlyphPrefix(s, platform)) {
        return *glyph;
    }
    if (s == "+") {
        return KeyChord{Key_Plus};
    }

    std::string_view keyText;
    std::string_view modsText;
    if (s.back() == '+') {
        // "Ctrl++" / "Ctrl + +": the key is '+', and what precedes it must end with a '+' separator.
        const std::string_view head = trim(s.substr(0, s.size() - 1));
        if (head.empty() || head.back() != '+') {
            return std::unexpected(parseError(text, "missing key"));
        }
        keyText = "+";
        modsText = trim(head.substr(0, head.size() - 1));
        if (modsText.empty()) {
            return std::unexpected(parseError(text, "empty modifier"));
        }
    } else {
        const auto plus = s.rfind('+');
        if (plus == std::string_view::npos) {
            keyText = s;
        } else {
            keyText = trim(s.substr(plus + 1));
            modsText = trim(s.substr(0, plus));
            if (modsText.empty()) {
                return std::unexpected(parseError(text, "empty modifier"));
            }
        }
    }

    Modifiers mods;
    while (!modsText.empty()) {
        const auto plus = modsText.find('+');
        const std::string_view tok = trim(modsText.substr(0, plus));
        if (tok.empty()) {
            return std::unexpected(parseError(text, "empty modifier"));
        }
        if (auto r = addModifier(mods, tok, text, platform); !r) {
            return std::unexpected(r.error());
        }
        if (plus == std::string_view::npos) {
            break;
        }
        modsText = modsText.substr(plus + 1);
        if (trim(modsText).empty()) {
            return std::unexpected(parseError(text, "empty modifier"));
        }
    }
    return finishChord(mods, keyText, text);
}

std::string formatKeyChord(KeyChord chord, Platform platform, ChordFormat format) {
    const bool mac = usesMacModifiers(platform);
    const Modifiers m = chord.mods;
    std::string out;

    if (format == ChordFormat::Native && mac) {
        // Apple order ⌃⌥⇧⌘, no separators.
        if (m.has(Modifier::Meta)) {
            out += kGlyphCtrl;
        }
        if (m.has(Modifier::Alt)) {
            out += kGlyphOpt;
        }
        if (m.has(Modifier::Shift)) {
            out += kGlyphShift;
        }
        if (m.has(Modifier::Control)) {
            out += kGlyphCmd;
        }
        out += keyName(chord.key, platform, format);
        return out;
    }

    std::vector<std::string_view> parts;
    if (mac) {
        if (m.has(Modifier::Meta)) {
            parts.push_back("Ctrl");
        }
        if (m.has(Modifier::Alt)) {
            parts.push_back("Opt");
        }
        if (m.has(Modifier::Shift)) {
            parts.push_back("Shift");
        }
        if (m.has(Modifier::Control)) {
            parts.push_back("Cmd");
        }
    } else {
        if (m.has(Modifier::Control)) {
            parts.push_back("Ctrl");
        }
        if (m.has(Modifier::Alt)) {
            parts.push_back("Alt");
        }
        if (m.has(Modifier::Shift)) {
            parts.push_back("Shift");
        }
        if (m.has(Modifier::Meta)) {
            parts.push_back(format == ChordFormat::Native && platform == Platform::Windows ? "Win" : "Meta");
        }
    }
    std::string key = keyName(chord.key, platform, format);
    if (m.has(Modifier::Keypad)) {
        if (format == ChordFormat::Text) {
            parts.push_back("Keypad");
        } else if (chord.key != Key_Enter) {
            key = "Num " + key;
        }
    }
    for (std::string_view part : parts) {
        out += part;
        out += '+';
    }
    out += key;
    return out;
}

} // namespace wabcut::keymap
