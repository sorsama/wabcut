// CEA-608 decoding and encoding (Cea608.h). Code tables after CEA-608-E (and as FFmpeg's ccaption_dec reads them).

#include "Cea608.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace wabcut::interchange::detail::cea608 {

namespace {

/// 0x11 0x30-0x3F (data channel 1).
constexpr std::array<char32_t, 16> kSpecial = {0xAE, 0xB0, 0xBD, 0xBF, 0x2122, 0xA2, 0xA3, 0x266A,
                                                 0xE0, 0xA0, 0xE8, 0xE2, 0xEA,   0xEE, 0xF4, 0xFB};
/// 0x12 0x20-0x3F: Spanish, French and miscellaneous.
constexpr std::array<char32_t, 32> kExtendedSpanishFrench = {
    0xC1, 0xC9, 0xD3, 0xDA, 0xDC, 0xFC, 0xB4, 0xA1, '*',  0x2018, 0x2014, 0xA9, 0x2120, 0x2022, 0x201C, 0x201D,
    0xC0, 0xC2, 0xC7, 0xC8, 0xCA, 0xCB, 0xEB, 0xCE, 0xCF, 0xEF,   0xD4,   0xD9, 0xF9,   0xDB,   0xAB,   0xBB};
/// 0x13 0x20-0x3F: Portuguese, German and Danish.
constexpr std::array<char32_t, 32> kExtendedPortugueseGerman = {
    0xC3, 0xE3, 0xCD, 0xCC, 0xEC, 0xD2, 0xF2, 0xD5, 0xF5, '{',  '}',  '\\',   '^',    '_',    '|',    '~',
    0xC4, 0xE4, 0xD6, 0xF6, 0xDF, 0xA5, 0xA4, 0xA6, 0xC5, 0xE5, 0xD8, 0xF8, 0x250C, 0x2510, 0x2514, 0x2518};

/// Basic characters 608 shows differently from ASCII.
constexpr std::array<std::pair<std::uint8_t, char32_t>, 11> kBasicOverrides = {{{0x27, '\''},
                                                                                 {0x2A, 0xE1},
                                                                                 {0x5C, 0xE9},
                                                                                 {0x5E, 0xED},
                                                                                 {0x5F, 0xF3},
                                                                                 {0x60, 0xFA},
                                                                                 {0x7B, 0xE7},
                                                                                 {0x7C, 0xF7},
                                                                                 {0x7D, 0xD1},
                                                                                 {0x7E, 0xF1},
                                                                                 {0x7F, 0x2588}}};

/// A Latin letter with a diacritic -> the letter (the extended sets' fallback, and the stand-in for letters 608 lacks).
char baseLetter(char32_t ch) {
    // U+00C0..U+017F: each entry the base letter or '?'.
    static constexpr std::string_view kLatin1 = "AAAAAAACEEEEIIII" // C0-CF
                                                "DNOOOOO?OUUUUY?s" // D0-DF
                                                "aaaaaaaceeeeiiii" // E0-EF
                                                "dnooooo?ouuuuy?y"; // F0-FF
    static constexpr std::string_view kExtendedA = "AaAaAaCcCcCcCcDd" // 100-10F
                                                   "DdEeEeEeEeEeGgGg" // 110-11F
                                                   "GgGgHhHhIiIiIiIi" // 120-12F
                                                   "IiIiJjKkkLlLlLlL" // 130-13F
                                                   "lLlNnNnNnnNnOoOo" // 140-14F
                                                   "OoOoRrRrRrSsSsSs" // 150-15F
                                                   "SsTtTtTtUuUuUuUu" // 160-16F
                                                   "UuUuWwYyYZzZzZzs"; // 170-17F
    if (ch >= 0xC0 && ch <= 0xFF) {
        return kLatin1[ch - 0xC0];
    }
    if (ch >= 0x100 && ch <= 0x17F) {
        return kExtendedA[ch - 0x100];
    }
    return '?';
}

constexpr std::uint8_t kChannelBit = 0x08; // data channel 2 adds 8 to the first byte

/// Misc control codes (first byte 0x14 for CC1).
enum : std::uint8_t {
    kRCL = 0x20,
    kBS = 0x21,
    kDER = 0x24,
    kRU2 = 0x25,
    kRU3 = 0x26,
    kRU4 = 0x27,
    kRDC = 0x29,
    kTR = 0x2A,
    kRTD = 0x2B,
    kEDM = 0x2C,
    kCR = 0x2D,
    kENM = 0x2E,
    kEOC = 0x2F,
};

std::pair<std::uint8_t, std::uint8_t> misc(std::uint8_t code) { return {withParity(0x14), withParity(code)}; }

/// The PAC of `row` (1..15) at `indent` (0, 4, ... 28), white.
std::pair<std::uint8_t, std::uint8_t> pac(int row, int indent) {
    static constexpr std::uint8_t kFirst[16] = {0, 0x11, 0x11, 0x12, 0x12, 0x15, 0x15, 0x16, 0x16,
                                                0x17, 0x17, 0x10, 0x13, 0x13, 0x14, 0x14};
    const bool second = row == 2 || row == 4 || row == 6 || row == 8 || row == 10 || row == 13 || row == 15;
    const std::uint8_t attr = indent > 0 ? static_cast<std::uint8_t>(8 + indent / 4) : 0;
    return {withParity(kFirst[row]), withParity(static_cast<std::uint8_t>(0x40 | (second ? 0x20 : 0) | (attr << 1)))};
}

std::string utf8(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

/// UTF-8 -> code points (invalid bytes become U+FFFD).
std::u32string codePoints(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        char32_t cp = 0xFFFD;
        if (len == 1) {
            cp = c;
        } else if (len > 1 && i + len <= text.size()) {
            cp = c & (0x7F >> len);
            for (std::size_t k = 1; k < len; ++k) {
                const auto cc = static_cast<unsigned char>(text[i + k]);
                if ((cc & 0xC0) != 0x80) {
                    cp = 0xFFFD;
                    len = 1;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
        } else {
            len = 1;
        }
        out.push_back(cp);
        i += std::max<std::size_t>(len, 1);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Characters

char32_t decodeBasic(std::uint8_t b) {
    b &= 0x7F;
    for (const auto& [code, ch] : kBasicOverrides) {
        if (code == b) {
            return ch;
        }
    }
    return b;
}

std::optional<char32_t> decodeTwoByte(std::uint8_t first, std::uint8_t second) {
    first = static_cast<std::uint8_t>(first & 0x77); // channel bit off
    second &= 0x7F;
    if (first == 0x11 && second >= 0x30 && second <= 0x3F) {
        return kSpecial[second - 0x30];
    }
    if (first == 0x12 && second >= 0x20 && second <= 0x3F) {
        return kExtendedSpanishFrench[second - 0x20];
    }
    if (first == 0x13 && second >= 0x20 && second <= 0x3F) {
        return kExtendedPortugueseGerman[second - 0x20];
    }
    return std::nullopt;
}

std::optional<Encoded> encodeCharacter(char32_t ch) {
    static const std::set<char32_t> kNotBasic = {0x2A, 0x5C, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};
    if (ch >= 0x20 && ch <= 0x7E && !kNotBasic.contains(ch)) {
        return Encoded{static_cast<std::uint8_t>(ch), 0, false, 0};
    }
    if (ch == 0x2019 || ch == 0x60) {
        return Encoded{0x27, 0, false, 0}; // typographic apostrophe, grave accent
    }
    for (const auto& [code, c] : kBasicOverrides) {
        if (c == ch && code != 0x27) {
            return Encoded{code, 0, false, 0};
        }
    }
    for (std::size_t i = 0; i < kSpecial.size(); ++i) {
        if (kSpecial[i] == ch) {
            return Encoded{0x11, static_cast<std::uint8_t>(0x30 + i), true, 0};
        }
    }
    auto fallbackOf = [](char32_t c) -> char {
        if (c == '{') {
            return '(';
        }
        if (c == '}') {
            return ')';
        }
        if (c == '\\') {
            return '/';
        }
        if (c == 0x2018 || c == 0xB4) {
            return '\'';
        }
        if (c == 0x201C || c == 0x201D || c == 0xAB || c == 0xBB) {
            return '"';
        }
        if (c == 0x2014 || c == '_' || c == '~' || c == '^') {
            return '-';
        }
        if (c == '|' || c == 0xA6) {
            return 'I';
        }
        if (c == '*' || c == 0x2022) {
            return '.';
        }
        if (c == 0xA1) {
            return '!';
        }
        if (c == 0xA9) {
            return 'c';
        }
        const char base = baseLetter(c);
        return base == '?' ? ' ' : base;
    };
    for (std::size_t i = 0; i < kExtendedSpanishFrench.size(); ++i) {
        if (kExtendedSpanishFrench[i] == ch) {
            return Encoded{0x12, static_cast<std::uint8_t>(0x20 + i), true, fallbackOf(ch)};
        }
    }
    for (std::size_t i = 0; i < kExtendedPortugueseGerman.size(); ++i) {
        if (kExtendedPortugueseGerman[i] == ch) {
            return Encoded{0x13, static_cast<std::uint8_t>(0x20 + i), true, fallbackOf(ch)};
        }
    }
    // Typographic stand-ins with an exact basic equivalent.
    if (ch == 0x2013) {
        return Encoded{'-', 0, false, 0};
    }
    if (ch == 0x2026) {
        return std::nullopt; // the caller writes "..."
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------------
// Decoder

Decoder::Decoder()
    : m_displayed(kRows, std::vector<Cell>(kColumns)), m_hidden(kRows, std::vector<Cell>(kColumns)) {}

bool Decoder::empty(const Grid& g) const {
    for (const auto& row : g) {
        for (const Cell& c : row) {
            if (c.ch != 0 && c.ch != ' ' && c.ch != 0xA0) {
                return false;
            }
        }
    }
    return true;
}

Caption Decoder::snapshot(const Grid& g, int onlyRow) const {
    Caption c;
    c.mode = m_mode;
    c.rollUpRows = m_rollRows;
    int top = -1;
    int left = kColumns;
    for (int r = 0; r < kRows; ++r) {
        if (onlyRow >= 0 && r != onlyRow) {
            continue;
        }
        std::string line;
        int first = -1;
        int last = -1;
        for (int col = 0; col < kColumns; ++col) {
            const Cell& cell = g[static_cast<std::size_t>(r)][static_cast<std::size_t>(col)];
            if (cell.ch != 0 && cell.ch != ' ' && cell.ch != 0xA0) {
                first = first < 0 ? col : first;
                last = col;
            }
        }
        if (first < 0) {
            continue;
        }
        for (int col = first; col <= last; ++col) {
            const Cell& cell = g[static_cast<std::size_t>(r)][static_cast<std::size_t>(col)];
            line += cell.ch == 0 || cell.ch == 0xA0 ? std::string(" ") : utf8(cell.ch);
            c.italic = c.italic || cell.italic;
            c.underline = c.underline || cell.underline;
            c.colour = c.colour || cell.colour;
        }
        top = top < 0 ? r : top;
        left = std::min(left, first);
        c.rows.push_back(std::move(line));
    }
    c.topRow = top < 0 ? kRows : top + 1;
    c.column = left == kColumns ? 0 : left;
    return c;
}

void Decoder::put(char32_t ch) {
    if (m_row < 0 || m_row >= kRows) {
        return;
    }
    Grid& g = target();
    Cell& cell = g[static_cast<std::size_t>(m_row)][static_cast<std::size_t>(m_col)];
    cell = {ch, m_italic, m_underline, m_colour};
    m_col = std::min(m_col + 1, kColumns - 1);
}

void Decoder::backspace() {
    if (m_col > 0) {
        --m_col;
        target()[static_cast<std::size_t>(m_row)][static_cast<std::size_t>(m_col)] = Cell{};
    }
}

void Decoder::close(std::int64_t frame) {
    if (m_onScreen && frame > m_onScreen->startFrame && !m_onScreen->rows.empty()) {
        m_onScreen->endFrame = frame;
        m_out.push_back(std::move(*m_onScreen));
    }
    m_onScreen.reset();
}

void Decoder::changed(std::int64_t frame) {
    if (m_mode == Mode::PopOn) {
        return; // pop-on text is loaded off screen
    }
    // Roll-up: one caption a line, from its first character to the carriage return that rolls it up. Paint-on:
    // what is on screen, from the first character painted to the erase.
    Caption c = snapshot(m_displayed, m_mode == Mode::RollUp ? m_row : -1);
    if (c.rows.empty()) {
        close(frame);
        return;
    }
    if (m_onScreen) {
        c.startFrame = m_onScreen->startFrame;
    } else {
        c.startFrame = frame;
        if (m_mode == Mode::RollUp) {
            ++m_stats.rollUp;
        } else {
            ++m_stats.paintOn;
        }
    }
    m_onScreen = std::move(c);
}

void Decoder::control(std::int64_t frame, std::uint8_t a, std::uint8_t b) {
    // Preamble address codes.
    if ((a == 0x10 && b >= 0x40 && b <= 0x5F) || (a >= 0x11 && a <= 0x17 && b >= 0x40 && b <= 0x7F)) {
        static constexpr int kBaseRow[8] = {11, 1, 3, 12, 14, 5, 7, 9};
        const int row = kBaseRow[a - 0x10] + ((b & 0x20) && a != 0x10 ? 1 : 0);
        const int attr = (b & 0x1E) >> 1;
        m_underline = (b & 1) != 0;
        m_italic = attr == 7;
        m_colour = attr > 0 && attr < 7;
        m_col = attr >= 8 ? (attr - 8) * 4 : 0;
        if (m_mode == Mode::RollUp && row - 1 != m_row) {
            // The roll-up window moves to the new base row, its rows with it.
            Grid moved(kRows, std::vector<Cell>(kColumns));
            for (int k = 0; k < m_rollRows; ++k) {
                const int from = m_row - k;
                const int to = row - 1 - k;
                if (from >= 0 && to >= 0) {
                    moved[static_cast<std::size_t>(to)] = m_displayed[static_cast<std::size_t>(from)];
                }
            }
            m_displayed = std::move(moved);
        }
        m_row = row - 1;
        return;
    }
    // Mid-row codes: attributes, and a space.
    if (a == 0x11 && b >= 0x20 && b <= 0x2F) {
        const int attr = (b & 0x0E) >> 1;
        m_italic = attr == 7;
        m_colour = attr > 0 && attr < 7;
        m_underline = (b & 1) != 0;
        put(' ');
        changed(frame);
        return;
    }
    if (auto ch = decodeTwoByte(a, b)) {
        if (a == 0x12 || a == 0x13) {
            backspace(); // an extended character replaces the basic one sent before it
        }
        put(*ch);
        changed(frame);
        return;
    }
    if (a == 0x17 && b >= 0x21 && b <= 0x23) { // tab offsets
        m_col = std::min(m_col + (b - 0x20), kColumns - 1);
        return;
    }
    if (a == 0x10 && b >= 0x20 && b <= 0x2F) { // background attributes: a space
        put(' ');
        changed(frame);
        return;
    }
    if (!(a == 0x14 || a == 0x15) || b < 0x20 || b > 0x2F) {
        ++m_stats.unknownCodes;
        return;
    }
    auto clear = [](Grid& g) {
        for (auto& row : g) {
            std::fill(row.begin(), row.end(), Cell{});
        }
    };
    switch (b) {
    case kRCL:
        if (m_mode != Mode::PopOn) {
            close(frame);
        }
        m_mode = Mode::PopOn;
        break;
    case kBS:
        backspace();
        changed(frame);
        break;
    case kDER:
        for (int col = m_col; col < kColumns; ++col) {
            target()[static_cast<std::size_t>(m_row)][static_cast<std::size_t>(col)] = Cell{};
        }
        changed(frame);
        break;
    case kRU2:
    case kRU3:
    case kRU4:
        if (m_mode != Mode::RollUp) {
            close(frame);
            clear(m_displayed);
            clear(m_hidden);
            m_row = kRows - 1;
        }
        m_mode = Mode::RollUp;
        m_rollRows = b - kRU2 + 2;
        m_col = 0;
        break;
    case kRDC:
        if (m_mode == Mode::RollUp) {
            close(frame);
        }
        m_mode = Mode::PaintOn;
        break;
    case kTR:
    case kRTD:
        break; // text service: not captions
    case kEDM:
        close(frame);
        clear(m_displayed);
        break;
    case kCR:
        if (m_mode == Mode::RollUp) {
            close(frame);
            for (int k = m_rollRows - 1; k > 0; --k) {
                const int to = m_row - k;
                if (to >= 0) {
                    m_displayed[static_cast<std::size_t>(to)] = m_displayed[static_cast<std::size_t>(to + 1)];
                }
            }
            for (int r = 0; r <= m_row - m_rollRows; ++r) {
                std::fill(m_displayed[static_cast<std::size_t>(r)].begin(),
                          m_displayed[static_cast<std::size_t>(r)].end(), Cell{});
            }
            std::fill(m_displayed[static_cast<std::size_t>(m_row)].begin(),
                      m_displayed[static_cast<std::size_t>(m_row)].end(), Cell{});
            m_col = 0;
        }
        break;
    case kENM:
        clear(m_hidden);
        break;
    case kEOC:
        close(frame);
        std::swap(m_displayed, m_hidden);
        m_mode = Mode::PopOn;
        if (!empty(m_displayed)) {
            m_onScreen = snapshot(m_displayed, -1);
            m_onScreen->startFrame = frame;
        }
        break;
    default:
        ++m_stats.unknownCodes;
        break;
    }
}

void Decoder::push(std::int64_t frame, std::uint8_t b1, std::uint8_t b2) {
    if (!parityOk(b1) || !parityOk(b2)) {
        ++m_stats.parityErrors;
        m_previousControl.reset();
        return;
    }
    const auto a = static_cast<std::uint8_t>(b1 & 0x7F);
    const auto b = static_cast<std::uint8_t>(b2 & 0x7F);
    if (a == 0 && b == 0) {
        return; // padding
    }
    if (a >= 0x10 && a <= 0x1F) {
        if (a & kChannelBit) {
            ++m_stats.otherChannel;
            m_previousControl.reset();
            return;
        }
        if (m_previousControl && *m_previousControl == std::pair{a, b}) {
            m_previousControl.reset(); // the redundant second transmission
            return;
        }
        m_previousControl = std::pair{a, b};
        control(frame, a, b);
        return;
    }
    m_previousControl.reset();
    if (a < 0x20) {
        return; // extended data services
    }
    put(decodeBasic(a));
    if (b >= 0x20) {
        put(decodeBasic(b));
    }
    changed(frame);
}

std::vector<Caption> Decoder::finish(std::int64_t frame) {
    close(frame);
    return std::move(m_out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoder

namespace {

struct Words {
    std::vector<std::pair<std::uint8_t, std::uint8_t>> list;
    std::size_t replaced = 0;
    std::size_t clipped = 0;

    void control(std::pair<std::uint8_t, std::uint8_t> code) {
        list.push_back(code);
        list.push_back(code); // sent twice
    }

    /// One line of text at the current position: basic characters two to a pair, special and extended codes on
    /// their own.
    void text(std::string_view line) {
        std::optional<std::uint8_t> pending;
        auto flush = [&]() {
            if (pending) {
                list.emplace_back(withParity(*pending), withParity(0));
                pending.reset();
            }
        };
        auto basic = [&](std::uint8_t c) {
            if (pending) {
                list.emplace_back(withParity(*pending), withParity(c));
                pending.reset();
            } else {
                pending = c;
            }
        };
        std::u32string cps = codePoints(line);
        std::size_t shown = 0;
        for (const char32_t ch : cps) {
            if (shown >= static_cast<std::size_t>(kColumns)) {
                ++clipped;
                break;
            }
            if (ch == 0x2026) { // an ellipsis: three dots (three cells)
                for (int k = 0; k < 3 && shown < static_cast<std::size_t>(kColumns); ++k, ++shown) {
                    basic('.');
                }
                continue;
            }
            const auto e = encodeCharacter(ch);
            ++shown;
            if (!e) {
                ++replaced;
                const char base = baseLetter(ch);
                basic(static_cast<std::uint8_t>(base == '?' && ch < 0x80 ? '?' : base));
                continue;
            }
            if (!e->twoByte) {
                basic(e->first);
                continue;
            }
            if (e->fallback != 0) {
                basic(static_cast<std::uint8_t>(e->fallback));
            }
            flush();
            list.emplace_back(withParity(e->first), withParity(e->second));
        }
        flush();
    }
};

std::size_t displayLength(std::string_view line) {
    std::size_t n = 0;
    for (const char32_t ch : codePoints(line)) {
        n += ch == 0x2026 ? 3 : 1;
    }
    return std::min<std::size_t>(n, kColumns);
}

/// PAC and tab offset for a line of `length` cells on `row`, aligned left / centre / right.
void position(Words& w, int row, std::size_t length, int align) {
    const int col = align == 0 ? 0 : align == 2 ? kColumns - static_cast<int>(length) : (kColumns - static_cast<int>(length)) / 2;
    w.control(pac(row, (col / 4) * 4));
    if (col % 4 != 0) {
        w.control({withParity(0x17), withParity(static_cast<std::uint8_t>(0x20 + col % 4))});
    }
}

} // namespace

EncodeResult encode(std::span<const Cue> cues, Mode mode, int rollUpRows) {
    EncodeResult r;
    rollUpRows = std::clamp(rollUpRows, 2, 4);
    std::int64_t lastUsed = std::numeric_limits<std::int64_t>::min(); // last frame given a pair
    auto place = [&](std::int64_t at, std::pair<std::uint8_t, std::uint8_t> pair) {
        while (r.pairs.contains(at)) {
            ++at;
        }
        r.pairs[at] = pair;
        lastUsed = std::max(lastUsed, at);
        return at;
    };
    /// Schedules `words` so that word `anchor` lands on `at` (or as soon after it as there is room), every word after
    /// the last frame used. Returns the frame the anchor word got.
    auto schedule = [&](const std::vector<std::pair<std::uint8_t, std::uint8_t>>& words, std::size_t anchor,
                        std::int64_t at) {
        std::int64_t first = at - static_cast<std::int64_t>(anchor);
        if (lastUsed != std::numeric_limits<std::int64_t>::min()) {
            first = std::max(first, lastUsed + 1);
        }
        first = std::max<std::int64_t>(first, 0);
        std::int64_t anchorAt = first;
        std::int64_t frame = first;
        for (std::size_t i = 0; i < words.size(); ++i) {
            frame = place(frame, words[i]);
            if (i == anchor) {
                anchorAt = frame;
            }
            ++frame;
        }
        return anchorAt;
    };

    for (std::size_t i = 0; i < cues.size(); ++i) {
        const Cue& cue = cues[i];
        const bool nextTouches = i + 1 < cues.size() && cues[i + 1].startFrame <= cue.endFrame;
        std::vector<std::string> lines;
        for (const std::string& l : cue.lines) {
            if (!l.empty() && lines.size() < 4) {
                lines.push_back(l);
            }
        }
        Words w;
        std::size_t anchor = 0;
        if (mode == Mode::PopOn) {
            w.control(misc(kRCL));
            w.control(misc(kENM));
            const int n = static_cast<int>(lines.size());
            for (int k = 0; k < n; ++k) {
                const int row = cue.top ? 1 + k : kRows - n + 1 + k;
                position(w, row, displayLength(lines[static_cast<std::size_t>(k)]), cue.align);
                w.text(lines[static_cast<std::size_t>(k)]);
            }
            anchor = w.list.size();
            w.control(misc(kEOC));
        } else if (mode == Mode::RollUp) {
            if (i == 0) {
                w.control(misc(static_cast<std::uint8_t>(kRU2 + rollUpRows - 2)));
            }
            for (const std::string& line : lines) {
                w.control(misc(kCR));
                w.control(pac(kRows, 0));
                w.text(line);
            }
        } else {
            w.control(misc(kRDC));
            w.control(misc(kEDM));
            const int n = static_cast<int>(lines.size());
            for (int k = 0; k < n; ++k) {
                const int row = cue.top ? 1 + k : kRows - n + 1 + k;
                position(w, row, displayLength(lines[static_cast<std::size_t>(k)]), cue.align);
                w.text(lines[static_cast<std::size_t>(k)]);
            }
        }
        r.replacedCharacters += w.replaced;
        r.clippedLines += w.clipped;
        const std::int64_t shownAt = schedule(w.list, anchor, cue.startFrame);
        if (shownAt > cue.startFrame) {
            ++r.lateCaptions;
            r.maxLateFrames = std::max(r.maxLateFrames, shownAt - cue.startFrame);
        }
        if (!nextTouches) {
            // Erase at the end (the next caption's End of Caption replaces it when they touch).
            std::vector<std::pair<std::uint8_t, std::uint8_t>> erase;
            erase.push_back(misc(kEDM));
            erase.push_back(misc(kEDM));
            schedule(erase, 0, std::max(cue.endFrame, shownAt + 1));
        }
    }
    return r;
}

} // namespace wabcut::interchange::detail::cea608
