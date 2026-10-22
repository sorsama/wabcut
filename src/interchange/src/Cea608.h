#pragma once

// CEA-608 line 21 captions (the data inside SCC and MCC files): byte pairs with odd parity, one pair per frame of
// field 1, carrying control codes (pop-on loading, roll-up, paint-on, erase, carriage return, end of caption), preamble
// address codes (row 1-15, indent, colour, italics, underline), mid-row codes, tab offsets and characters (the basic
// North American set, its special characters and the two extended Western European sets) on a 15 x 32 character grid.
//
//  * Decoder turns a stream of pairs into captions with their display times, the way a set-top decoder shows them
//    (pop-on: what End of Caption displays until it is erased or replaced; roll-up and paint-on: what is on screen at
//    each carriage return or erase). Redundant control codes (sent twice) count once.
//  * encode() turns timed captions into pairs: pop-on (load off screen, End of Caption at the start frame, Erase
//    Displayed Memory at the end frame), roll-up (2-4 rows) or paint-on, every control code sent twice.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::interchange::detail::cea608 {

inline constexpr int kRows = 15;
inline constexpr int kColumns = 32;

/// `b` (7 bits) with bit 7 set so the byte has an odd number of ones.
constexpr std::uint8_t withParity(std::uint8_t b) {
    b &= 0x7F;
    int ones = 0;
    for (int i = 0; i < 7; ++i) {
        ones += (b >> i) & 1;
    }
    return static_cast<std::uint8_t>(ones % 2 == 0 ? b | 0x80 : b);
}

/// Whether `b` has odd parity.
constexpr bool parityOk(std::uint8_t b) {
    int ones = 0;
    for (int i = 0; i < 8; ++i) {
        ones += (b >> i) & 1;
    }
    return ones % 2 == 1;
}

enum class Mode : std::uint8_t { PopOn, RollUp, PaintOn };

/// A caption as decoded: its text rows (UTF-8, top to bottom, trimmed) and where and how it showed.
struct Caption {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;  ///< exclusive
    std::vector<std::string> rows;
    int topRow = 15;            ///< 1..15
    int column = 0;             ///< leftmost column used, 0..31
    Mode mode = Mode::PopOn;
    int rollUpRows = 2;         ///< roll-up captions: the window's depth
    bool italic = false;
    bool underline = false;
    bool colour = false;        ///< a colour other than white
};

struct DecodeStats {
    std::size_t parityErrors = 0;
    std::size_t unknownCodes = 0;
    std::size_t rollUp = 0;     ///< captions that rolled up
    std::size_t paintOn = 0;
    std::size_t otherChannel = 0; ///< pairs of data channel 2 (CC2 / CC4), skipped
};

class Decoder {
public:
    /// Decodes data channel 1 of field 1 (CC1).
    Decoder();
    /// One byte pair (as transmitted, with parity) at `frame`. Frames increase.
    void push(std::int64_t frame, std::uint8_t b1, std::uint8_t b2);
    /// Ends what is on screen at `frame` and returns every caption, in order.
    std::vector<Caption> finish(std::int64_t frame);
    const DecodeStats& stats() const { return m_stats; }

private:
    struct Cell {
        char32_t ch = 0;
        bool italic = false;
        bool underline = false;
        bool colour = false;
    };
    using Grid = std::vector<std::vector<Cell>>; // [row][column]

    void control(std::int64_t frame, std::uint8_t a, std::uint8_t b);
    void put(char32_t ch);
    void backspace();
    /// Ends the caption on screen at `frame`.
    void close(std::int64_t frame);
    /// Roll-up and paint-on: the displayed memory changed at `frame`.
    void changed(std::int64_t frame);
    Grid& target() { return m_mode == Mode::PopOn ? m_hidden : m_displayed; }
    bool empty(const Grid& g) const;
    /// The text of `g` (or of its row `onlyRow` when >= 0).
    Caption snapshot(const Grid& g, int onlyRow) const;

    Grid m_displayed;
    Grid m_hidden;
    Mode m_mode = Mode::PopOn;
    int m_rollRows = 2;
    int m_row = 14;   ///< 0-based
    int m_col = 0;
    bool m_italic = false;
    bool m_underline = false;
    bool m_colour = false;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> m_previousControl;
    std::optional<Caption> m_onScreen; ///< what is displayed since its startFrame
    std::vector<Caption> m_out;
    DecodeStats m_stats;
};

/// A caption to encode: its lines (at most four of at most 32 characters, already wrapped) and placement.
struct Cue {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;
    std::vector<std::string> lines;
    bool top = false;          ///< rows from the top (else ending on row 15)
    int align = 1;             ///< 0 left, 1 centre, 2 right
};

struct EncodeResult {
    std::map<std::int64_t, std::pair<std::uint8_t, std::uint8_t>> pairs; ///< frame -> byte pair (with parity)
    std::size_t lateCaptions = 0;      ///< captions shown later than their start (no room to load them)
    std::int64_t maxLateFrames = 0;
    std::size_t replacedCharacters = 0; ///< characters 608 cannot show, sent as their closest letter or '?'
    std::size_t clippedLines = 0;      ///< lines longer than 32 characters, cut
};

/// Pop-on (or roll-up with `rollUpRows` 2-4, or paint-on) captions as frame-indexed byte pairs for CC1.
EncodeResult encode(std::span<const Cue> cues, Mode mode = Mode::PopOn, int rollUpRows = 2);

/// The 608 bytes of one Unicode character: a basic character (one byte), or a two-byte special / extended code
/// (`fallback` is then the basic character a decoder without the extended set shows). nullopt when 608 has none.
struct Encoded {
    std::uint8_t first = 0;   ///< basic: the character; two-byte: 0x11..0x13
    std::uint8_t second = 0;  ///< two-byte codes only
    bool twoByte = false;
    char fallback = 0;        ///< extended characters: the basic character sent before the code
};
std::optional<Encoded> encodeCharacter(char32_t ch);
/// The character a basic byte (0x20-0x7F) or a special / extended code shows.
char32_t decodeBasic(std::uint8_t b);
std::optional<char32_t> decodeTwoByte(std::uint8_t first, std::uint8_t second);

} // namespace wabcut::interchange::detail::cea608
