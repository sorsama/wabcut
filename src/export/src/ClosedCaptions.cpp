#include <wabcut/model/Plural.h>
#include <wabcut/export/ClosedCaptions.h>

#include <wabcut/interchange/CaptionFiles.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace wabcut::exporter {

namespace {

// ---- CEA-608 -----------------------------------------------------------------------------------------------------

std::uint8_t oddParity(std::uint8_t b) {
    b &= 0x7F;
    return std::popcount(static_cast<unsigned>(b)) % 2 == 0 ? static_cast<std::uint8_t>(b | 0x80) : b;
}

// ---- CEA-708 codes (CEA-708-E section 7) --------------------------------------------------------------------------

constexpr std::uint8_t kCR = 0x0D;
constexpr std::uint8_t kEXT1 = 0x10;
constexpr std::uint8_t kCLW = 0x88;
constexpr std::uint8_t kDSW = 0x89;
constexpr std::uint8_t kHDW = 0x8A;
constexpr std::uint8_t kTGW = 0x8B;
constexpr std::uint8_t kDLW = 0x8C;
constexpr std::uint8_t kSPA = 0x90;
constexpr std::uint8_t kSPC = 0x91;
constexpr std::uint8_t kSPL = 0x92;
constexpr std::uint8_t kSWA = 0x97;
constexpr std::uint8_t kDF0 = 0x98;

constexpr int kMaxColumns = 32; ///< the 4:3 safe title width every decoder shows (608's too)
constexpr int kMaxPopOnRows = 4;
constexpr int kMaxBlockPayload = 31;

/// A command with its parameters, or one character: never split across service blocks.
using Unit = std::vector<std::uint8_t>;

/// G2 (after EXT1) symbols and the code points they show.
constexpr std::pair<char32_t, std::uint8_t> kG2[] = {
    {U'…', 0x25}, {U'Š', 0x2A}, {U'Œ', 0x2C}, {U'█', 0x30}, {U'‘', 0x31}, {U'’', 0x32},
    {U'“', 0x33}, {U'”', 0x34}, {U'•', 0x35}, {U'™', 0x39}, {U'š', 0x3A}, {U'œ', 0x3C},
    {U'℠', 0x3D}, {U'Ÿ', 0x3F}, {U'⅛', 0x76}, {U'⅜', 0x77}, {U'⅝', 0x78}, {U'⅞', 0x79},
    {U'│', 0x7A}, {U'┐', 0x7B}, {U'└', 0x7C}, {U'─', 0x7D}, {U'┘', 0x7E}, {U'┌', 0x7F},
};
constexpr char32_t kMusicNote = U'♪';

std::vector<char32_t> decodeUtf8(std::string_view s) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + static_cast<std::size_t>(len) > s.size()) {
            out.push_back(U'�');
            ++i;
            continue;
        }
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            ok = ok && (cc & 0xC0) == 0x80;
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.push_back(ok ? cp : U'�');
        i += ok ? static_cast<std::size_t>(len) : 1;
    }
    return out;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// The 708 code of one character; `replaced` counts the ones shown as '?'.
Unit characterUnit(char32_t cp, int& replaced) {
    if (cp == U'\t') {
        cp = U' ';
    }
    if (cp >= 0x20 && cp <= 0x7E) {
        return {static_cast<std::uint8_t>(cp)};
    }
    if (cp == kMusicNote) {
        return {0x7F};
    }
    if (cp >= 0xA0 && cp <= 0xFF) {
        return {static_cast<std::uint8_t>(cp)};
    }
    for (const auto& [code, g2] : kG2) {
        if (code == cp) {
            return {kEXT1, g2};
        }
    }
    ++replaced;
    return {static_cast<std::uint8_t>('?')};
}

/// A caption's lines as 708 characters: blank lines dropped, lines longer than 32 characters wrapped at spaces (a longer
/// word is cut), at most `maxRows` rows (0 = any).
struct Lines {
    std::vector<std::vector<Unit>> rows;
    int columns = 1;
    bool wrapped = false;
    bool cut = false;
    int replaced = 0;
};

Lines layoutText(std::string_view text, int maxRows) {
    Lines out;
    std::vector<std::vector<char32_t>> rows;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        start = end + 1;
        std::vector<char32_t> cps = decodeUtf8(line);
        while (!cps.empty() && cps.back() == U' ') {
            cps.pop_back();
        }
        while (!cps.empty() && cps.front() == U' ') {
            cps.erase(cps.begin());
        }
        if (cps.empty()) {
            continue;
        }
        // Greedy word wrap at kMaxColumns.
        while (static_cast<int>(cps.size()) > kMaxColumns) {
            out.wrapped = true;
            int cutAt = kMaxColumns;
            for (int k = kMaxColumns; k > 0; --k) {
                if (cps[static_cast<std::size_t>(k)] == U' ') {
                    cutAt = k;
                    break;
                }
            }
            rows.emplace_back(cps.begin(), cps.begin() + cutAt);
            std::size_t next = static_cast<std::size_t>(cutAt);
            while (next < cps.size() && cps[next] == U' ') {
                ++next;
            }
            cps.erase(cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(next));
        }
        if (!cps.empty()) {
            rows.push_back(std::move(cps));
        }
    }
    if (maxRows > 0 && static_cast<int>(rows.size()) > maxRows) {
        rows.resize(static_cast<std::size_t>(maxRows));
        out.cut = true;
    }
    for (const auto& row : rows) {
        std::vector<Unit> units;
        for (const char32_t cp : row) {
            units.push_back(characterUnit(cp, out.replaced));
        }
        out.columns = std::max(out.columns, static_cast<int>(row.size()));
        out.rows.push_back(std::move(units));
    }
    return out;
}

/// 708's 64-colour palette: two bits a channel.
std::uint8_t color708(const CaptionRgb& c) {
    const auto q = [](double v) { return static_cast<std::uint8_t>(std::clamp<long>(std::lround(v * 3.0), 0, 3)); };
    return static_cast<std::uint8_t>((q(c[0]) << 4) | (q(c[1]) << 2) | q(c[2]));
}

/// 0 solid, 2 translucent, 3 transparent.
std::uint8_t opacity708(double opacity) {
    return opacity >= 0.75 ? 0 : opacity >= 0.25 ? 2 : 3;
}

struct WindowLayout {
    std::uint8_t anchorPoint = 7; ///< 0-8: upper/middle/lower x left/centre/right
    std::uint8_t vertical = 90;   ///< relative, 0-99 %
    std::uint8_t horizontal = 50;
    std::uint8_t justify = 2; ///< 0 left, 1 right, 2 centre
};

WindowLayout layoutFor(const ClosedCaptionOptions& o) {
    WindowLayout l;
    const double safe = std::clamp(o.style.safeArea, 0.0, 0.4);
    const bool top = o.placement == CaptionPlacement::Top;
    const auto pct = [](double v) { return static_cast<std::uint8_t>(std::clamp<long>(std::lround(v * 100.0), 0, 99)); };
    l.vertical = top ? pct(safe) : pct(1.0 - safe);
    int column = 1;
    switch (o.alignment) {
    case CaptionAlign::Left:
        column = 0;
        l.horizontal = pct(safe);
        l.justify = 0;
        break;
    case CaptionAlign::Center:
        column = 1;
        l.horizontal = 50;
        l.justify = 2;
        break;
    case CaptionAlign::Right:
        column = 2;
        l.horizontal = pct(1.0 - safe);
        l.justify = 1;
        break;
    }
    l.anchorPoint = static_cast<std::uint8_t>((top ? 0 : 6) + column);
    return l;
}

Unit defineWindow(int id, bool visible, const WindowLayout& l, int rows, int columns) {
    return {static_cast<std::uint8_t>(kDF0 + id),
            static_cast<std::uint8_t>((visible ? 0x20 : 0) | 0x10 | 0x08), // row and column lock, priority 0
            static_cast<std::uint8_t>(0x80 | l.vertical),                 // relative positioning
            l.horizontal,
            static_cast<std::uint8_t>((l.anchorPoint << 4) | std::clamp(rows - 1, 0, 14)),
            static_cast<std::uint8_t>(std::clamp(columns - 1, 0, 41)),
            0x09}; // window style 1, pen style 1 (then set below)
}

Unit windowAttributes(const WindowLayout& l) {
    // Transparent fill (the pen background draws the box), no border, left-to-right, scrolls bottom-to-top, no word
    // wrap, snap.
    return {kSWA, 0xC0, 0x00, static_cast<std::uint8_t>((3 << 2) | l.justify), 0x00};
}

Unit penAttributes(const CaptionStyle& style) {
    const std::uint8_t edge = style.outlineWidth > 0.0 ? 3 : 0; // uniform outline
    return {kSPA, 0x05, static_cast<std::uint8_t>(edge << 3)};  // standard size, normal offset, dialog, default font
}

Unit penColor(const CaptionStyle& style) {
    const std::uint8_t fg = color708(style.fill);
    const std::uint8_t bgOpacity = style.box ? opacity708(style.boxOpacity) : 3;
    const std::uint8_t bg = style.box ? color708(style.boxColor) : 0;
    return {kSPC, fg, static_cast<std::uint8_t>((bgOpacity << 6) | bg), color708(style.outline)};
}

Unit penLocation(int row, int column) {
    return {kSPL, static_cast<std::uint8_t>(row & 0x0F), static_cast<std::uint8_t>(column & 0x3F)};
}

Unit windowsCommand(std::uint8_t command, int mask) {
    return {command, static_cast<std::uint8_t>(mask)};
}

/// The 708 bytes of one service, frame by frame: commands that must take effect on a given frame ("timed": show, hide,
/// clear) go first in their frame, loads (definitions, text) after them, in order. Each frame holds at most its capacity.
class ServiceSchedule {
public:
    explicit ServiceSchedule(std::vector<int> capacity)
        : m_capacity(std::move(capacity)), m_timed(m_capacity.size()), m_load(m_capacity.size()) {}

    std::int64_t frames() const { return static_cast<std::int64_t>(m_capacity.size()); }

    /// Places `unit` on frame `k`, or the first later frame with room; the frame used, or -1 past the end.
    std::int64_t timed(std::int64_t k, const Unit& unit) {
        k = std::max<std::int64_t>(k, 0);
        while (k < frames() && room(k) < static_cast<int>(unit.size())) {
            ++k;
        }
        if (k >= frames()) {
            return -1;
        }
        auto& bytes = m_timed[static_cast<std::size_t>(k)];
        bytes.insert(bytes.end(), unit.begin(), unit.end());
        return k;
    }

    /// Places `units` in order from frame `k` on; the frames of the first and last unit, or nullopt when they run past
    /// the end (what fitted stays).
    std::optional<std::pair<std::int64_t, std::int64_t>> load(std::int64_t k, const std::vector<Unit>& units) {
        k = std::max<std::int64_t>(k, 0);
        std::int64_t first = -1;
        for (const Unit& unit : units) {
            while (k < frames() && room(k) < static_cast<int>(unit.size())) {
                ++k;
            }
            if (k >= frames()) {
                return std::nullopt;
            }
            auto& bytes = m_load[static_cast<std::size_t>(k)];
            bytes.insert(bytes.end(), unit.begin(), unit.end());
            if (first < 0) {
                first = k;
            }
        }
        return std::pair{first < 0 ? k : first, k};
    }

    std::vector<std::uint8_t> payload(std::int64_t k) const {
        std::vector<std::uint8_t> out = m_timed[static_cast<std::size_t>(k)];
        const auto& more = m_load[static_cast<std::size_t>(k)];
        out.insert(out.end(), more.begin(), more.end());
        return out;
    }

private:
    int room(std::int64_t k) const {
        const auto i = static_cast<std::size_t>(k);
        return m_capacity[i] - static_cast<int>(m_timed[i].size() + m_load[i].size());
    }

    std::vector<int> m_capacity;
    std::vector<std::vector<std::uint8_t>> m_timed;
    std::vector<std::vector<std::uint8_t>> m_load;
};

struct CueFrames {
    const CaptionCue* cue = nullptr;
    std::int64_t start = 0; ///< first frame
    std::int64_t end = 0;   ///< first frame after it
};

std::vector<CueFrames> cueFrames(const std::vector<CaptionCue>& cues, TimeRange range, Rate rate, std::int64_t frames) {
    std::vector<CueFrames> out;
    for (const CaptionCue& cue : cues) {
        const Ticks start = std::max(cue.start, range.start);
        const Ticks end = std::min(cue.end(), range.end());
        if (!(start < end)) {
            continue;
        }
        const auto s = ticksToFrames(start - range.start, rate, Rounding::Nearest);
        const auto e = ticksToFrames(end - range.start, rate, Rounding::Nearest);
        if (!s || !e || *s >= frames || *e <= *s) {
            continue;
        }
        out.push_back({&cue, *s, std::min(*e, frames)});
    }
    return out;
}

struct Encoded708 {
    std::vector<std::vector<std::uint8_t>> packets; ///< per frame: a whole DTVCC packet, or empty
    int late = 0;
    std::int64_t maxLate = 0;
    int dropped = 0;
    bool wrapped = false;
    bool cut = false;
    int replaced = 0;
};

/// `preload`: how many frames before its start a pop-on caption may be loaded into its hidden window at the earliest
/// (a decoder that starts or seeks in between still gets it; more than enough bandwidth for 4 rows of 32).
Encoded708 encode708(const std::vector<CueFrames>& cues, const std::vector<int>& slots708, const ClosedCaptionOptions& o,
                     std::int64_t preload) {
    Encoded708 out;
    const auto frames = static_cast<std::int64_t>(slots708.size());
    const int header = o.service708 < 7 ? 1 : 2; // service block header (+ the extended service number)
    std::vector<int> capacity(slots708.size());
    for (std::size_t k = 0; k < slots708.size(); ++k) {
        capacity[k] = std::clamp(slots708[k] * 2 - 1 - header, 0, kMaxBlockPayload); // DTVCC packet header byte
    }
    ServiceSchedule schedule(std::move(capacity));
    const WindowLayout layout = layoutFor(o);
    const std::uint8_t display = std::min<std::uint8_t>(o.display, 4);
    const bool popOn = display == 0;
    const bool rollUp = display >= 1 && display <= 3;
    const int rollRows = display + 1;
    const auto attributes = [&] {
        return std::vector<Unit>{windowAttributes(layout), penAttributes(o.style), penColor(o.style)};
    };
    const auto noteLate = [&](std::int64_t shown, std::int64_t wanted) {
        if (shown > wanted) {
            ++out.late;
            out.maxLate = std::max(out.maxLate, shown - wanted);
        }
    };

    // A cue with nothing to show (empty, or only spaces and blank lines) is no caption: it is left out before anything
    // is scheduled, so the caption before it hides at its own end (a gap) and the two pop-on windows alternate between
    // captions that are shown. (Kept in, it used to leave the caption before it waiting to be replaced, its window
    // never free again: every later caption was dropped.)
    struct Visible {
        const CueFrames* cue = nullptr;
        Lines lines;
    };
    std::vector<Visible> visible;
    visible.reserve(cues.size());
    for (const CueFrames& c : cues) {
        Lines lines = layoutText(c.cue->text, rollUp ? 0 : kMaxPopOnRows);
        out.wrapped = out.wrapped || lines.wrapped;
        out.cut = out.cut || lines.cut;
        out.replaced += lines.replaced;
        if (!lines.rows.empty()) {
            visible.push_back({&c, std::move(lines)});
        }
    }

    std::int64_t cursor = 0;                                // loads go out in order: after the previous one
    std::vector<std::int64_t> hideFrame(visible.size(), 0); // pop-on: when each caption's window is free again
    const std::int64_t preloadFrames = std::max<std::int64_t>(preload, 1);
    bool rollDefined = false;
    bool rollCleared = false; // roll-up: the window was cleared after a gap; the pen goes back to its bottom row
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const CueFrames& c = *visible[i].cue;
        Lines& lines = visible[i].lines;
        const bool last = i + 1 == visible.size();
        const bool gapAfter = last || c.end < visible[i + 1].cue->start;
        const int rows = static_cast<int>(lines.rows.size());
        if (popOn) {
            const int window = static_cast<int>(i % 2);
            std::vector<Unit> units{defineWindow(window, false, layout, rows, lines.columns), windowsCommand(kCLW, 1 << window)};
            for (Unit& u : attributes()) {
                units.push_back(std::move(u));
            }
            for (int r = 0; r < rows; ++r) {
                units.push_back(penLocation(r, 0));
                for (Unit& ch : lines.rows[static_cast<std::size_t>(r)]) {
                    units.push_back(std::move(ch));
                }
            }
            // The window is free once the caption two before is hidden; the load goes out at most `preload` frames
            // before the caption starts.
            const std::int64_t from = std::max({cursor, i >= 2 ? hideFrame[i - 2] : std::int64_t{0}, c.start - preloadFrames});
            const auto placed = schedule.load(from, units);
            if (!placed) {
                out.dropped += static_cast<int>(visible.size() - i);
                break;
            }
            cursor = placed->second;
            std::int64_t shown = schedule.timed(std::max(c.start, placed->second + 1), windowsCommand(kDSW, 1 << window));
            if (shown < 0) {
                out.dropped += static_cast<int>(visible.size() - i);
                break;
            }
            if (i >= 1 && visible[i - 1].cue->end >= c.start) { // no gap: this caption replaces the one before
                schedule.timed(shown, windowsCommand(kHDW, 1 << (1 - window)));
                hideFrame[i - 1] = shown;
            }
            noteLate(shown, c.start);
            if (gapAfter) {
                const std::int64_t hidden = schedule.timed(std::max(c.end, shown + 1), windowsCommand(kHDW, 1 << window));
                hideFrame[i] = hidden < 0 ? frames : hidden;
            } else {
                hideFrame[i] = frames; // set when the next caption shows (every caption in `visible` has text)
            }
        } else {
            std::vector<Unit> units;
            if (rollUp) {
                if (!rollDefined) {
                    units.push_back(defineWindow(0, true, layout, rollRows, kMaxColumns));
                    for (Unit& u : attributes()) {
                        units.push_back(std::move(u));
                    }
                    units.push_back(penLocation(rollRows - 1, 0));
                    rollDefined = true;
                } else if (rollCleared) {
                    // ClearWindows emptied it after a gap, and a decoder may home its pen: back to the bottom row.
                    units.push_back(penLocation(rollRows - 1, 0));
                }
                rollCleared = false;
                for (auto& row : lines.rows) {
                    units.push_back({kCR});
                    for (Unit& ch : row) {
                        units.push_back(std::move(ch));
                    }
                }
            } else { // paint-on: written into the visible window as it arrives
                units.push_back(defineWindow(0, true, layout, rows, lines.columns));
                units.push_back(windowsCommand(kCLW, 1));
                for (Unit& u : attributes()) {
                    units.push_back(std::move(u));
                }
                for (int r = 0; r < rows; ++r) {
                    units.push_back(penLocation(r, 0));
                    for (Unit& ch : lines.rows[static_cast<std::size_t>(r)]) {
                        units.push_back(std::move(ch));
                    }
                }
            }
            const auto placed = schedule.load(std::max(cursor, c.start), units);
            if (!placed) {
                out.dropped += static_cast<int>(visible.size() - i);
                break;
            }
            cursor = placed->second;
            noteLate(placed->first, c.start);
            if (gapAfter) {
                schedule.timed(std::max(c.end, placed->second + 1), windowsCommand(kCLW, 1));
                rollCleared = true;
            }
        }
    }

    // One DTVCC packet a frame: header (sequence number, size code), one service block, a null block to make it even.
    out.packets.resize(static_cast<std::size_t>(frames));
    int sequence = 0;
    for (std::int64_t k = 0; k < frames; ++k) {
        std::vector<std::uint8_t> payload = schedule.payload(k);
        if (payload.empty()) {
            continue;
        }
        std::vector<std::uint8_t> packet{0};
        if (o.service708 < 7) {
            packet.push_back(static_cast<std::uint8_t>((o.service708 << 5) | payload.size()));
        } else {
            packet.push_back(static_cast<std::uint8_t>((7 << 5) | payload.size()));
            packet.push_back(static_cast<std::uint8_t>(o.service708 & 0x3F));
        }
        packet.insert(packet.end(), payload.begin(), payload.end());
        if (packet.size() % 2 != 0) {
            packet.push_back(0x00); // null service block header: the rest of the packet is padding
        }
        packet[0] = static_cast<std::uint8_t>((sequence << 6) | (packet.size() / 2));
        sequence = (sequence + 1) & 3;
        out.packets[static_cast<std::size_t>(k)] = std::move(packet);
    }
    return out;
}

} // namespace

int ccCountForRate(Rate rate) {
    if (!rate.isValid()) {
        return 20;
    }
    return std::clamp(static_cast<int>(std::lround(600.0 / rate.toDouble())), 1, 31);
}

std::array<std::uint8_t, 2> cea608PairForChannel(std::array<std::uint8_t, 2> cc1, int channel) {
    std::uint8_t a = cc1[0] & 0x7F;
    const std::uint8_t b = cc1[1] & 0x7F;
    if (a >= 0x10 && a <= 0x17) { // a control code (PAC, mid-row, special / extended character, command, tab)
        if (channel == 2 || channel == 4) {
            a = static_cast<std::uint8_t>(a | 0x08);
        }
        if ((channel == 3 || channel == 4) && (a & 0x77) == 0x14 && b >= 0x20 && b <= 0x2F) {
            a = static_cast<std::uint8_t>(a + 1); // field 2's miscellaneous control codes: 15h / 1Dh
        }
        return {oddParity(a), cc1[1]};
    }
    return cc1;
}

std::span<const std::uint8_t> ClosedCaptionData::frame(std::int64_t k) const {
    if (k < 0 || k >= frames || ccCount <= 0) {
        return {};
    }
    const auto size = static_cast<std::size_t>(ccCount) * 3;
    return std::span<const std::uint8_t>(bytes).subspan(static_cast<std::size_t>(k) * size, size);
}

ClosedCaptionData buildClosedCaptions(const std::vector<CaptionCue>& cues, TimeRange range, Rate rate, std::int64_t frames,
                                      const ClosedCaptionOptions& o) {
    ClosedCaptionData out;
    out.ccCount = ccCountForRate(rate);
    out.frames = std::max<std::int64_t>(frames, 0);
    const auto count = static_cast<std::size_t>(out.frames);
    const std::vector<CueFrames> timed = cueFrames(cues, range, rate, out.frames);
    out.cues = static_cast<int>(timed.size());
    const bool lowRate = rate.toDouble() < 31.0; // a 608 slot on each field in every frame

    // CEA-608: the SCC writer's CC1 pairs, moved to the channel's field and data channel.
    std::map<std::int64_t, std::vector<std::array<std::uint8_t, 2>>> pairs;
    const int field = o.channel608 <= 2 ? 1 : 2;
    if (o.cea608 && !timed.empty()) {
        interchange::CaptionWriteOptions w;
        w.language = o.language;
        w.placement = o.placement == CaptionPlacement::Top ? CaptionPlacement::Top : CaptionPlacement::Bottom;
        w.alignment = o.alignment;
        w.display = static_cast<interchange::Cea608Display>(std::min<int>(o.display, 4));
        interchange::ImportReport report;
        pairs = interchange::cea608ForVideo(cues, range, rate, w, &report);
        for (auto& [frame, list] : pairs) {
            for (auto& p : list) {
                p = cea608PairForChannel(p, o.channel608);
            }
        }
        std::set<std::string> seen;
        for (const interchange::ReportEntry& e : report.entries()) {
            if (e.outcome != interchange::ReportOutcome::Carried && seen.insert(e.reason).second) {
                out.notes.push_back("608: " + e.reason);
            }
        }
    }
    const auto pairsAt = [&](std::int64_t k) -> const std::vector<std::array<std::uint8_t, 2>>* {
        const auto it = pairs.find(k);
        return it == pairs.end() ? nullptr : &it->second;
    };
    // The 608 triplets of each frame, then what is left for 708.
    std::vector<int> slots708(count, 0);
    std::vector<int> slots608(count, 0);
    for (std::size_t k = 0; k < count; ++k) {
        const auto* p = pairsAt(static_cast<std::int64_t>(k));
        const int n = p ? static_cast<int>(std::min<std::size_t>(p->size(), 4)) : 0;
        const int f1 = field == 1 ? n : 0;
        const int f2 = field == 2 ? n : 0;
        const int used = lowRate ? std::max(1, f1) + std::max(1, f2) : std::max(1, f1 + f2);
        slots608[k] = std::min(used, out.ccCount);
        slots708[k] = o.cea708 ? out.ccCount - slots608[k] : 0;
    }
    Encoded708 cc708;
    if (o.cea708 && !timed.empty()) {
        // A pop-on caption is loaded at most 2 seconds before it shows (ample for 4 rows of 32 characters).
        cc708 = encode708(timed, slots708, o, static_cast<std::int64_t>(std::ceil(rate.toDouble() * 2.0)));
    }

    out.bytes.reserve(count * static_cast<std::size_t>(out.ccCount) * 3);
    for (std::size_t k = 0; k < count; ++k) {
        std::vector<std::uint8_t> f;
        const auto put = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c) {
            f.push_back(a);
            f.push_back(b);
            f.push_back(c);
        };
        const auto* p = pairsAt(static_cast<std::int64_t>(k));
        const std::uint8_t valid = o.cea608 ? 0xFC : 0xF8; // 708 only: the 608 slots say "not valid"
        const auto field608 = [&](int which) {
            const bool mine = field == which && p && !p->empty();
            if (mine) {
                for (std::size_t i = 0; i < std::min<std::size_t>(p->size(), 4); ++i) {
                    put(static_cast<std::uint8_t>(0xFC | (which - 1)), (*p)[i][0], (*p)[i][1]);
                }
            }
            return mine;
        };
        if (lowRate) {
            if (!field608(1)) {
                put(valid, 0x80, 0x80);
            }
            if (!field608(2)) {
                put(static_cast<std::uint8_t>(valid | 1), 0x80, 0x80);
            }
        } else {
            const bool a = field608(1);
            const bool b = field608(2);
            if (!a && !b) {
                put(static_cast<std::uint8_t>(valid | (k & 1)), 0x80, 0x80);
            }
        }
        if (k < cc708.packets.size()) {
            const auto& packet = cc708.packets[k];
            for (std::size_t i = 0; i + 1 < packet.size(); i += 2) {
                put(i == 0 ? 0xFF : 0xFE, packet[i], packet[i + 1]);
            }
        }
        f.resize(std::min(f.size(), static_cast<std::size_t>(out.ccCount) * 3));
        while (f.size() < static_cast<std::size_t>(out.ccCount) * 3) {
            put(0xFA, 0x00, 0x00); // 708 padding
        }
        out.bytes.insert(out.bytes.end(), f.begin(), f.end());
    }

    if (o.cea708) {
        out.late708 = cc708.late;
        out.maxLateFrames = cc708.maxLate;
        if (cc708.late > 0) {
            out.notes.push_back(std::format("708: {} up to {} late (not enough caption bandwidth "
                                            "to load them before they start)",
                                            counted(static_cast<std::int64_t>(cc708.late), "caption"),
                                            counted(static_cast<std::int64_t>(cc708.maxLate), "frame")));
        }
        if (cc708.dropped > 0) {
            out.notes.push_back(std::format("708: {} at the very end did not fit before the export ends",
                                            counted(static_cast<std::int64_t>(cc708.dropped), "caption")));
        }
        if (cc708.wrapped || cc708.cut) {
            out.notes.push_back(std::format("708: captions rewrapped to 32 columns{}",
                                            cc708.cut ? " and cut to 4 rows" : ""));
        }
        if (cc708.replaced > 0) {
            out.notes.push_back(std::format("708: replaced by '?' because 708 cannot show them: {}",
                                            counted(static_cast<std::int64_t>(cc708.replaced), "character")));
        }
    }
    return out;
}

// ---- Reading 708 back ----------------------------------------------------------------------------------------------

namespace {

struct Window {
    bool defined = false;
    bool visible = false;
    int rows = 1;
    int columns = 1;
    int penRow = 0;
    int penColumn = 0;
    std::vector<std::vector<char32_t>> grid;

    void resize(int r, int c) {
        rows = std::clamp(r, 1, 15);
        columns = std::clamp(c, 1, 42);
        grid.resize(static_cast<std::size_t>(rows));
        for (auto& row : grid) {
            row.resize(static_cast<std::size_t>(columns), U' ');
        }
        penRow = std::min(penRow, rows - 1);
        penColumn = std::min(penColumn, columns - 1);
    }
    void clear() {
        for (auto& row : grid) {
            std::fill(row.begin(), row.end(), U' ');
        }
        penRow = 0;
        penColumn = 0;
    }
    void put(char32_t cp) {
        if (penRow < rows && penColumn < columns) {
            grid[static_cast<std::size_t>(penRow)][static_cast<std::size_t>(penColumn)] = cp;
        }
        penColumn = std::min(penColumn + 1, columns);
    }
    void carriageReturn() {
        if (penRow + 1 >= rows) {
            grid.erase(grid.begin());
            grid.emplace_back(static_cast<std::size_t>(columns), U' ');
        } else {
            ++penRow;
        }
        penColumn = 0;
    }
    std::string text() const {
        std::string out;
        for (const auto& row : grid) {
            std::size_t end = row.size();
            while (end > 0 && row[end - 1] == U' ') {
                --end;
            }
            std::size_t begin = 0;
            while (begin < end && row[begin] == U' ') {
                ++begin;
            }
            if (begin == end) {
                continue;
            }
            if (!out.empty()) {
                out += '\n';
            }
            for (std::size_t i = begin; i < end; ++i) {
                appendUtf8(out, row[i]);
            }
        }
        return out;
    }
};

class Reader708 {
public:
    explicit Reader708(int service) : m_service(service) {}

    void packet(const std::vector<std::uint8_t>& p, Cea708Reading& reading) {
        ++reading.packets;
        std::size_t pos = 1;
        while (pos < p.size()) {
            const std::uint8_t header = p[pos++];
            int service = header >> 5;
            const std::size_t size = header & 0x1F;
            if (service == 0 && size == 0) {
                break; // a null block: padding to the end
            }
            if (service == 7) {
                if (pos >= p.size()) {
                    ++reading.errors;
                    break;
                }
                service = p[pos++] & 0x3F;
            }
            if (pos + size > p.size()) {
                ++reading.errors;
                break;
            }
            if (std::find(reading.services.begin(), reading.services.end(), service) == reading.services.end()) {
                reading.services.push_back(service);
            }
            if (service == m_service) {
                block(std::span<const std::uint8_t>(p).subspan(pos, size), reading);
            }
            pos += size;
        }
    }

    std::pair<std::string, std::vector<int>> screen() const {
        std::string text;
        std::vector<int> shown;
        for (int w = 0; w < 8; ++w) {
            const Window& win = m_windows[static_cast<std::size_t>(w)];
            if (!win.defined || !win.visible) {
                continue;
            }
            const std::string t = win.text();
            if (t.empty()) {
                continue;
            }
            if (!text.empty()) {
                text += '\n';
            }
            text += t;
            shown.push_back(w);
        }
        return {text, shown};
    }

private:
    Window* current() {
        Window& w = m_windows[static_cast<std::size_t>(m_current)];
        return w.defined ? &w : nullptr;
    }

    void block(std::span<const std::uint8_t> b, Cea708Reading& reading) {
        std::size_t i = 0;
        const auto need = [&](std::size_t n) {
            if (i + n > b.size()) {
                ++reading.errors;
                return false;
            }
            return true;
        };
        while (i < b.size()) {
            const std::uint8_t c = b[i++];
            if (c == kEXT1) {
                if (!need(1)) {
                    return;
                }
                const std::uint8_t e = b[i++];
                if (e >= 0x20 && e <= 0x7F) {
                    char32_t cp = U'?';
                    if (e == 0x20 || e == 0x21) {
                        cp = U' ';
                    }
                    for (const auto& [code, g2] : kG2) {
                        if (g2 == e) {
                            cp = code;
                        }
                    }
                    if (Window* w = current()) {
                        w->put(cp);
                    }
                } else if (e >= 0x80 && e <= 0x87) {
                    i += 5; // C3 with fixed parameters
                } else if (e >= 0x88 && e <= 0x8F) {
                    i += 6;
                } else if (e >= 0xA0) {
                    if (Window* w = current()) {
                        w->put(U'?'); // G3
                    }
                } else if (e >= 0x08 && e <= 0x0F) {
                    i += 1;
                } else if (e >= 0x10 && e <= 0x17) {
                    i += 2;
                } else if (e >= 0x18 && e <= 0x1F) {
                    i += 3;
                }
                continue;
            }
            if (c < 0x20) { // C0
                Window* w = current();
                switch (c) {
                case 0x08: // BS
                    if (w && w->penColumn > 0) {
                        --w->penColumn;
                        w->grid[static_cast<std::size_t>(w->penRow)][static_cast<std::size_t>(w->penColumn)] = U' ';
                    }
                    break;
                case 0x0C: // FF
                    if (w) {
                        w->clear();
                    }
                    break;
                case kCR:
                    if (w) {
                        w->carriageReturn();
                    }
                    break;
                case 0x0E: // HCR
                    if (w) {
                        std::fill(w->grid[static_cast<std::size_t>(w->penRow)].begin(),
                                  w->grid[static_cast<std::size_t>(w->penRow)].end(), U' ');
                        w->penColumn = 0;
                    }
                    break;
                default:
                    if (c >= 0x11 && c <= 0x17) {
                        i += 1;
                    } else if (c >= 0x18) {
                        i += 2;
                    }
                    break;
                }
                continue;
            }
            if (c <= 0x7F || c >= 0xA0) { // G0 / G1
                if (Window* w = current()) {
                    w->put(c == 0x7F ? kMusicNote : static_cast<char32_t>(c));
                }
                continue;
            }
            // C1
            if (c <= 0x87) {
                m_current = c - 0x80;
                continue;
            }
            if (c >= kDF0) {
                if (!need(6)) {
                    return;
                }
                const int id = c - kDF0;
                Window& w = m_windows[static_cast<std::size_t>(id)];
                const bool fresh = !w.defined;
                w.defined = true;
                w.visible = (b[i] & 0x20) != 0;
                w.resize((b[i + 3] & 0x0F) + 1, (b[i + 4] & 0x3F) + 1);
                if (fresh) {
                    w.clear();
                }
                m_current = id;
                if (std::find(reading.windowsDefined.begin(), reading.windowsDefined.end(), id) == reading.windowsDefined.end()) {
                    reading.windowsDefined.push_back(id);
                }
                i += 6;
                continue;
            }
            switch (c) {
            case kCLW:
            case kDSW:
            case kHDW:
            case kTGW:
            case kDLW: {
                if (!need(1)) {
                    return;
                }
                const std::uint8_t mask = b[i++];
                for (int w = 0; w < 8; ++w) {
                    if (!(mask & (1 << w))) {
                        continue;
                    }
                    Window& win = m_windows[static_cast<std::size_t>(w)];
                    if (c == kCLW) {
                        win.clear();
                    } else if (c == kDSW) {
                        win.visible = true;
                    } else if (c == kHDW) {
                        win.visible = false;
                    } else if (c == kTGW) {
                        win.visible = !win.visible;
                    } else {
                        win = Window{};
                    }
                }
                break;
            }
            case 0x8D: // DLY
                i += 1;
                break;
            case 0x8E: // DLC
                break;
            case 0x8F: // RST
                m_windows = {};
                break;
            case kSPA:
                i += 2;
                break;
            case kSPC:
                i += 3;
                break;
            case kSPL:
                if (!need(2)) {
                    return;
                }
                if (Window* w = current()) {
                    w->penRow = std::min<int>(b[i] & 0x0F, w->rows - 1);
                    w->penColumn = std::min<int>(b[i + 1] & 0x3F, w->columns - 1);
                }
                i += 2;
                break;
            case kSWA:
                i += 4;
                break;
            default: // 93h-96h: reserved, no parameters
                break;
            }
            if (i > b.size()) {
                ++reading.errors;
                return;
            }
        }
    }

    int m_service = 1;
    int m_current = 0;
    std::array<Window, 8> m_windows{};
};

} // namespace

Cea708Reading readCea708(const std::vector<std::vector<std::uint8_t>>& ccData, int service) {
    Cea708Reading reading;
    Reader708 reader(service);
    std::vector<std::uint8_t> packet;
    std::size_t expected = 0;
    std::optional<Cea708Caption> open;
    const auto frames = static_cast<std::int64_t>(ccData.size());
    for (std::int64_t k = 0; k < frames; ++k) {
        const auto& triplets = ccData[static_cast<std::size_t>(k)];
        for (std::size_t i = 0; i + 2 < triplets.size(); i += 3) {
            const std::uint8_t head = triplets[i];
            if (!(head & 0x04)) {
                continue; // not valid (padding)
            }
            const int type = head & 0x03;
            if (type == 3) {
                if (!packet.empty() && packet.size() < expected) {
                    ++reading.errors; // a packet cut short
                }
                packet = {triplets[i + 1], triplets[i + 2]};
                const int code = triplets[i + 1] & 0x3F;
                expected = code == 0 ? 128 : static_cast<std::size_t>(code) * 2;
            } else if (type == 2) {
                if (packet.empty()) {
                    continue;
                }
                packet.push_back(triplets[i + 1]);
                packet.push_back(triplets[i + 2]);
            } else {
                continue; // 608
            }
            if (!packet.empty() && packet.size() >= expected) {
                packet.resize(expected);
                reader.packet(packet, reading);
                packet.clear();
            }
        }
        auto [text, windows] = reader.screen();
        if (open && open->text != text) {
            open->endFrame = k;
            reading.captions.push_back(*open);
            open.reset();
        }
        if (!open && !text.empty()) {
            open = Cea708Caption{k, k, text, windows};
        }
    }
    if (open) {
        open->endFrame = frames;
        reading.captions.push_back(*open);
    }
    std::sort(reading.windowsDefined.begin(), reading.windowsDefined.end());
    std::sort(reading.services.begin(), reading.services.end());
    return reading;
}

} // namespace wabcut::exporter
