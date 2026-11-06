#include "MarkerImport.h"

#include <wabcut/command/Command.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace wabcut::command::detail {

namespace {

/// The longest time the import takes (31 days, the bound of every typed time).
constexpr std::int64_t kMaxSeconds = 31LL * 24 * 3600;

// ---- Text helpers ------------------------------------------------------------------------------------------------

bool isSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v'; }

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string lowerAlnum(std::string_view s) {
    std::string out;
    for (const char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return out;
}

/// Whether `text` is well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF).
bool validUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (b < 0x80) {
            ++i;
            continue;
        }
        if ((b & 0xE0) == 0xC0) {
            len = 2;
            cp = b & 0x1Fu;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
            cp = b & 0x0Fu;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4;
            cp = b & 0x07u;
        } else {
            return false;
        }
        if (i + len > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(text[i + k]);
            if ((c & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        constexpr std::array<std::uint32_t, 5> kMin{0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// Windows-1252 bytes 0x80..0x9F as Unicode (a spreadsheet's "CSV" export); the five undefined bytes map to themselves.
constexpr std::array<std::uint16_t, 32> kCp1252High{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

/// Cuts `text` to at most `limit` bytes without splitting a UTF-8 sequence.
std::string cutUtf8(std::string text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    text.resize(end);
    return text;
}

// ---- Times -------------------------------------------------------------------------------------------------------

std::optional<Ticks> ticksOfSeconds(std::int64_t whole, std::int64_t fracDigits, int digitCount) {
    if (whole < 0 || whole > kMaxSeconds) {
        return std::nullopt;
    }
    std::int64_t scale = 1;
    for (int i = 0; i < 6 - digitCount; ++i) {
        scale *= 10;
    }
    // kTicksPerSecond is a multiple of 10^6, so up to six fractional digits convert exactly.
    const std::int64_t perMicro = kTicksPerSecond / 1'000'000;
    return Ticks{whole * kTicksPerSecond + fracDigits * scale * perMicro};
}

/// "12", "12.5", "12,5" (seconds); nullopt for anything else.
std::optional<Ticks> parseSecondsText(std::string_view s) {
    if (s.empty() || s.size() > 24) {
        return std::nullopt;
    }
    std::int64_t whole = 0;
    std::size_t i = 0;
    for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
        whole = whole * 10 + (s[i] - '0');
        if (whole > kMaxSeconds) {
            return std::nullopt;
        }
    }
    if (i == 0) {
        return std::nullopt;
    }
    std::int64_t frac = 0;
    int digits = 0;
    if (i < s.size()) {
        if (s[i] != '.' && s[i] != ',') {
            return std::nullopt;
        }
        for (++i; i < s.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
                return std::nullopt;
            }
            if (digits < 6) {
                frac = frac * 10 + (s[i] - '0');
                ++digits;
            }
        }
    }
    return ticksOfSeconds(whole, frac, digits);
}

/// A time as the file writes it, or nullopt when the text is not one.
std::optional<Ticks> parseTime(std::string_view raw, Rate rate, bool dropFrame) {
    std::string_view s = trimmed(raw);
    if (!s.empty() && (s.front() == '[' || s.front() == '(')) {
        s.remove_prefix(1);
    }
    if (!s.empty() && (s.back() == ']' || s.back() == ')')) {
        s.remove_suffix(1);
    }
    s = trimmed(s);
    if (s.empty() || s.size() > 32) {
        return std::nullopt;
    }
    // "1500ms" and "12.5s".
    if (s.size() > 2 && (s.ends_with("ms") || s.ends_with("MS"))) {
        const auto millis = parseSecondsText(s.substr(0, s.size() - 2));
        return millis ? std::optional<Ticks>(Ticks{millis->value / 1000}) : std::nullopt;
    }
    if (s.size() > 1 && (s.back() == 's' || s.back() == 'S')) {
        return parseSecondsText(s.substr(0, s.size() - 1));
    }
    const std::size_t separators = static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char ch) { return ch == ':' || ch == ';'; }));
    if (separators == 0) {
        return parseSecondsText(s);
    }
    if (separators >= 4) {
        return std::nullopt;
    }
    if (separators == 3) {
        // HH:MM:SS:FF at the timebase (';' before the frames: drop-frame).
        const bool drop = dropFrame || s.find(';') != std::string_view::npos;
        const auto parsed = parseTimecode(s, rate, drop && supportsDropFrame(rate));
        if (!parsed || parsed->value < 0 || parsed->value > kMaxSeconds * kTicksPerSecond) {
            return std::nullopt;
        }
        return *parsed;
    }
    // H:MM:SS(.fff) or M:SS(.fff): a clock, as YouTube writes chapters.
    std::array<std::int64_t, 3> parts{};
    std::size_t count = 0;
    std::size_t start = 0;
    std::optional<Ticks> fraction;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] != ':' && s[i] != ';') {
            continue;
        }
        std::string_view field = s.substr(start, i - start);
        start = i + 1;
        if (field.empty() || count >= parts.size()) {
            return std::nullopt;
        }
        if (i == s.size()) { // the seconds may carry a fraction
            const auto seconds = parseSecondsText(field);
            if (!seconds || field.size() > 12) {
                return std::nullopt;
            }
            parts[count++] = seconds->value / kTicksPerSecond;
            fraction = Ticks{seconds->value % kTicksPerSecond};
            continue;
        }
        std::int64_t v = 0;
        for (const char ch : field) {
            if (!std::isdigit(static_cast<unsigned char>(ch)) || v > 100000) {
                return std::nullopt;
            }
            v = v * 10 + (ch - '0');
        }
        parts[count++] = v;
    }
    std::int64_t seconds = 0;
    if (count == 3) {
        if (parts[1] > 59 || parts[2] > 59) {
            return std::nullopt;
        }
        seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
    } else if (count == 2) {
        if (parts[1] > 59) {
            return std::nullopt;
        }
        seconds = parts[0] * 60 + parts[1];
    } else {
        return std::nullopt;
    }
    if (seconds > kMaxSeconds) {
        return std::nullopt;
    }
    return Ticks{seconds * kTicksPerSecond + (fraction ? fraction->value : 0)};
}

// ---- Colours and types -------------------------------------------------------------------------------------------

/// Premiere's eight marker colours (colorIndex 0-7), and the extra names tools write.
std::optional<std::int32_t> colorOf(std::string_view text) {
    static const std::map<std::string, std::int32_t> kNames{
        {"green", 0}, {"red", 1}, {"purple", 2}, {"magenta", 2}, {"orange", 3}, {"yellow", 4},
        {"white", 5}, {"blue", 6}, {"cyan", 7}, {"aqua", 7}};
    const std::string key = lowerAlnum(text);
    if (const auto it = kNames.find(key); it != kNames.end()) {
        return it->second;
    }
    if (!key.empty() && key.size() <= 2 && std::all_of(key.begin(), key.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        const int index = std::stoi(key);
        if (index <= 15) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<MarkerType> typeOf(std::string_view text) {
    const std::string key = lowerAlnum(text);
    if (key == "comment" || key == "marker") {
        return MarkerType::Comment;
    }
    if (key == "chapter") {
        return MarkerType::Chapter;
    }
    if (key == "segmentation" || key == "segment") {
        return MarkerType::Segmentation;
    }
    if (key == "weblink" || key == "link") {
        return MarkerType::WebLink;
    }
    return std::nullopt;
}

// ---- The result under construction -------------------------------------------------------------------------------

struct Reader {
    ParsedMarkerList out;
    Rate rate;
    bool dropFrame = false;
    std::set<std::string> warnedColors;

    void warn(std::string message) {
        if (out.warnings.size() < kMaxMarkerImportWarnings) {
            out.warnings.push_back(std::move(message));
        }
    }
    void skip(std::size_t where, const std::string& why) {
        ++out.skipped;
        warn((out.format == "json" ? "entry " : "line ") + std::to_string(where) + ": " + why);
    }
    std::optional<Ticks> time(std::string_view text) const { return parseTime(text, rate, dropFrame); }

    /// A name: no line breaks, cut at the marker limit.
    static std::string nameText(std::string_view raw) {
        std::string s;
        for (const char ch : trimmed(raw)) {
            s += (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
        }
        return cutUtf8(std::move(s), kMaxMarkerTextBytes);
    }
    static std::string commentText(std::string_view raw) { return cutUtf8(std::string(trimmed(raw)), kMaxMarkerTextBytes); }

    void setColor(Marker& m, std::string_view text, std::size_t where) {
        if (trimmed(text).empty()) {
            return;
        }
        if (const auto index = colorOf(text)) {
            m.colorIndex = *index;
        } else if (warnedColors.insert(lowerAlnum(text)).second) {
            warn((out.format == "json" ? "entry " : "line ") + std::to_string(where) + ": unknown colour \"" +
                 std::string(trimmed(text)).substr(0, 40) + "\" (the default is used)");
        }
    }
    void setType(Marker& m, std::string_view text) {
        if (const auto type = typeOf(text)) {
            m.type = *type;
        }
    }
    void add(Marker marker, std::size_t where) {
        out.markers.push_back(ParsedMarker{std::move(marker), where});
    }
};

// ---- CSV ---------------------------------------------------------------------------------------------------------

struct CsvRow {
    std::size_t line = 0;
    std::vector<std::string> fields;
};

/// RFC 4180 fields: quotes hold the delimiter and line breaks, "" is a quote. The line is where the row began.
std::vector<CsvRow> csvRows(std::string_view text, char delimiter) {
    std::vector<CsvRow> rows;
    CsvRow row;
    std::string field;
    bool quoted = false;
    bool any = false;
    std::size_t line = 1;
    row.line = 1;
    const auto endField = [&] {
        row.fields.push_back(std::move(field));
        field.clear();
    };
    const auto endRow = [&](std::size_t nextLine) {
        endField();
        if (any || row.fields.size() > 1 || !row.fields.front().empty()) {
            rows.push_back(std::move(row));
        }
        row = CsvRow{};
        row.line = nextLine;
        any = false;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                if (ch == '\n') {
                    ++line;
                }
                field += ch;
            }
            continue;
        }
        if (ch == '"' && field.empty()) {
            quoted = true;
            any = true;
        } else if (ch == delimiter) {
            endField();
            any = true;
        } else if (ch == '\n') {
            ++line;
            endRow(line);
        } else {
            field += ch;
            any = true;
        }
    }
    endRow(line);
    return rows;
}

/// Which of ',', ';' and TAB splits the first line into the most fields (outside quotes); ',' when none does.
char guessDelimiter(std::string_view firstLine) {
    char best = ',';
    std::size_t bestCount = 0;
    for (const char candidate : {',', ';', '\t'}) {
        std::size_t count = 0;
        bool quoted = false;
        for (const char ch : firstLine) {
            if (ch == '"') {
                quoted = !quoted;
            } else if (ch == candidate && !quoted) {
                ++count;
            }
        }
        if (count > bestCount) {
            best = candidate;
            bestCount = count;
        }
    }
    return best;
}

enum class Column { None, Name, Comment, Start, End, Duration, Type, Color };

Column columnOf(std::string_view header) {
    static const std::map<std::string, Column> kColumns{
        {"markername", Column::Name}, {"name", Column::Name}, {"title", Column::Name}, {"label", Column::Name},
        {"marker", Column::Name}, {"text", Column::Name},
        {"description", Column::Comment}, {"comment", Column::Comment}, {"comments", Column::Comment},
        {"notes", Column::Comment}, {"note", Column::Comment},
        {"in", Column::Start}, {"start", Column::Start}, {"time", Column::Start}, {"timecode", Column::Start},
        {"position", Column::Start}, {"markerin", Column::Start}, {"starttime", Column::Start},
        {"out", Column::End}, {"end", Column::End}, {"markerout", Column::End}, {"endtime", Column::End},
        {"duration", Column::Duration}, {"length", Column::Duration},
        {"markertype", Column::Type}, {"type", Column::Type},
        {"color", Column::Color}, {"colour", Column::Color}};
    const auto it = kColumns.find(lowerAlnum(header));
    return it == kColumns.end() ? Column::None : it->second;
}

/// Whether the first row names columns: at least two known names, one of them a time.
bool looksLikeHeader(const CsvRow& row) {
    int known = 0;
    bool hasTime = false;
    for (const std::string& f : row.fields) {
        const Column c = columnOf(f);
        known += c != Column::None ? 1 : 0;
        hasTime = hasTime || c == Column::Start;
    }
    return known >= 2 && hasTime;
}

void readCsv(Reader& r, std::string_view text, char delimiter, bool forceHeaderless) {
    r.out.format = "csv";
    const std::vector<CsvRow> rows = csvRows(text, delimiter);
    std::map<Column, std::size_t> at; // column -> field index
    std::size_t first = 0;
    if (!rows.empty() && !forceHeaderless && looksLikeHeader(rows.front())) {
        for (std::size_t i = 0; i < rows.front().fields.size(); ++i) {
            const Column c = columnOf(rows.front().fields[i]);
            if (c != Column::None && !at.contains(c)) {
                at[c] = i;
            }
        }
        first = 1;
    } else { // no header row: time, name, comment
        at = {{Column::Start, 0}, {Column::Name, 1}, {Column::Comment, 2}};
    }
    const auto field = [&](const CsvRow& row, Column c) -> std::string_view {
        const auto it = at.find(c);
        return it != at.end() && it->second < row.fields.size() ? std::string_view(row.fields[it->second]) : std::string_view();
    };
    for (std::size_t i = first; i < rows.size(); ++i) {
        const CsvRow& row = rows[i];
        const std::string_view startText = trimmed(field(row, Column::Start));
        if (startText.empty()) {
            r.skip(row.line, "no time");
            continue;
        }
        const auto start = r.time(startText);
        if (!start) {
            r.skip(row.line, "\"" + std::string(startText).substr(0, 40) + "\" is not a time");
            continue;
        }
        Marker m;
        m.start = *start;
        m.name = Reader::nameText(field(row, Column::Name));
        m.comment = Reader::commentText(field(row, Column::Comment));
        const std::string_view durationText = trimmed(field(row, Column::Duration));
        const std::string_view endText = trimmed(field(row, Column::End));
        if (!durationText.empty()) {
            if (const auto d = r.time(durationText)) {
                m.duration = *d;
            }
        } else if (!endText.empty()) {
            if (const auto e = r.time(endText); e && e->value > start->value) {
                m.duration = Ticks{e->value - start->value};
            }
        }
        r.setType(m, field(row, Column::Type));
        r.setColor(m, field(row, Column::Color), row.line);
        r.add(std::move(m), row.line);
    }
}

// ---- Text lines --------------------------------------------------------------------------------------------------

/// The leading time token of a line: digits, ':' ';' '.' ',' and an optional bracket, ended by a space, tab, ',' ';' or '-'.
std::string_view leadingTime(std::string_view line) {
    std::size_t i = 0;
    if (i < line.size() && (line[i] == '[' || line[i] == '(')) {
        ++i;
    }
    const std::size_t from = i;
    while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i])) || line[i] == ':' || line[i] == ';' ||
                               line[i] == '.')) {
        ++i;
    }
    if (i == from) {
        return {};
    }
    // A trailing "s" / "ms" unit or the closing bracket belongs to the token.
    std::size_t end = i;
    if (end + 1 < line.size() && line[end] == 'm' && line[end + 1] == 's') {
        end += 2;
    } else if (end < line.size() && line[end] == 's') {
        ++end;
    }
    if (end < line.size() && (line[end] == ']' || line[end] == ')')) {
        ++end;
    }
    return line.substr(0, end);
}

std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t tab = std::min(line.find('\t', start), line.size());
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return out;
}

/// The name after a time: separators " - ", "-", "–", ":" and spaces are dropped.
std::string_view afterTime(std::string_view rest) {
    rest = trimmed(rest);
    while (!rest.empty() && (rest.front() == '-' || rest.front() == ':' || rest.front() == ',' || rest.front() == ';' ||
                             rest.front() == '|')) {
        rest.remove_prefix(1);
        rest = trimmed(rest);
    }
    if (rest.starts_with("\xE2\x80\x93") || rest.starts_with("\xE2\x80\x94")) { // an en or em dash
        rest.remove_prefix(3);
        rest = trimmed(rest);
    }
    return rest;
}

void readLines(Reader& r, std::string_view text) {
    r.out.format = "txt";
    std::size_t number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = std::min(text.find('\n', start), text.size());
        const std::string_view line = trimmed(text.substr(start, nl - start));
        start = nl + 1;
        ++number;
        if (line.empty() || line.front() == '#' || line.starts_with("//")) {
            continue;
        }
        Marker m;
        // Audacity labels, and other tab-separated tables: a time, an optional end or duration, the name, a comment.
        if (line.find('\t') != std::string_view::npos) {
            const auto cells = splitTabs(line);
            const auto start0 = r.time(trimmed(cells[0]));
            if (!start0) {
                r.skip(number, "\"" + std::string(trimmed(cells[0])).substr(0, 40) + "\" is not a time");
                continue;
            }
            m.start = *start0;
            std::size_t next = 1;
            if (cells.size() >= 3) { // Audacity: start, end, label
                if (const auto end = r.time(trimmed(cells[1])); end && cells[1].find_first_not_of(" 0123456789.:,;sm") == std::string_view::npos) {
                    if (end->value > start0->value) {
                        m.duration = Ticks{end->value - start0->value};
                    }
                    next = 2;
                }
            }
            if (next < cells.size()) {
                m.name = Reader::nameText(cells[next]);
            }
            if (next + 1 < cells.size()) {
                m.comment = Reader::commentText(cells[next + 1]);
            }
            r.add(std::move(m), number);
            continue;
        }
        const std::string_view token = leadingTime(line);
        // A bare number is no time here ("10 things to know"): a clock or timecode, or a unit ("12.5s"), is.
        const bool clock = token.find_first_of(":;") != std::string_view::npos;
        const bool unit = token.size() > 1 && (token.back() == 's' || token.back() == ']' || token.back() == ')');
        const auto parsed = token.empty() || (!clock && !unit) ? std::nullopt : r.time(token);
        if (!parsed) {
            r.skip(number, "does not begin with a time");
            continue;
        }
        m.start = *parsed;
        m.name = Reader::nameText(afterTime(line.substr(token.size())));
        r.add(std::move(m), number);
    }
}

// ---- JSON --------------------------------------------------------------------------------------------------------

const Json* member(const Json& object, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (const auto it = object.find(name); it != object.end() && !it->is_null()) {
            return &*it;
        }
    }
    return nullptr;
}

std::optional<Ticks> jsonTime(const Reader& r, const Json& value) {
    if (value.is_number_integer() || value.is_number_unsigned()) {
        const auto seconds = value.get<std::int64_t>();
        return seconds >= 0 && seconds <= kMaxSeconds ? std::optional<Ticks>(Ticks{seconds * kTicksPerSecond}) : std::nullopt;
    }
    if (value.is_number_float()) {
        const double seconds = value.get<double>();
        if (!std::isfinite(seconds) || seconds < 0 || seconds > static_cast<double>(kMaxSeconds)) {
            return std::nullopt;
        }
        return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
    }
    if (value.is_string()) {
        return r.time(value.get<std::string>());
    }
    if (value.is_object()) { // marker.list's { "ticks": ..., "timecode": ... }
        if (const auto it = value.find("ticks"); it != value.end() && it->is_number_integer()) {
            const auto ticks = it->get<std::int64_t>();
            return ticks >= 0 && ticks <= kMaxSeconds * kTicksPerSecond ? std::optional<Ticks>(Ticks{ticks}) : std::nullopt;
        }
        if (const auto it = value.find("timecode"); it != value.end() && it->is_string()) {
            return r.time(it->get<std::string>());
        }
    }
    return std::nullopt;
}

std::string jsonText(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.is_number() || value.is_boolean() ? value.dump() : std::string();
}

void readJson(Reader& r, std::string_view text) {
    r.out.format = "json";
    Json doc = Json::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded()) {
        return;
    }
    const Json* list = &doc;
    if (doc.is_object()) {
        list = member(doc, {"markers", "Markers", "items"});
    }
    if (!list || !list->is_array()) {
        return;
    }
    std::size_t number = 0;
    for (const Json& entry : *list) {
        ++number;
        if (!entry.is_object()) {
            r.skip(number, "not an object");
            continue;
        }
        const Json* startValue = member(entry, {"time", "start", "in", "position", "timecode", "Start", "In"});
        if (!startValue) {
            r.skip(number, "no time");
            continue;
        }
        const auto start = jsonTime(r, *startValue);
        if (!start) {
            r.skip(number, "the time is not valid");
            continue;
        }
        Marker m;
        m.start = *start;
        if (const Json* v = member(entry, {"name", "title", "label", "Name"})) {
            m.name = Reader::nameText(jsonText(*v));
        }
        if (const Json* v = member(entry, {"comment", "description", "notes", "note", "Comment"})) {
            m.comment = Reader::commentText(jsonText(*v));
        }
        if (const Json* v = member(entry, {"duration", "length", "Duration"})) {
            if (const auto d = jsonTime(r, *v)) {
                m.duration = *d;
            }
        } else if (const Json* v2 = member(entry, {"out", "end", "Out"})) {
            if (const auto e = jsonTime(r, *v2); e && e->value > start->value) {
                m.duration = Ticks{e->value - start->value};
            }
        }
        if (const Json* v = member(entry, {"type", "markerType", "Type"})) {
            r.setType(m, jsonText(*v));
        }
        if (const Json* v = member(entry, {"colorIndex", "color", "colour", "Color"})) {
            r.setColor(m, jsonText(*v), number);
        }
        r.add(std::move(m), number);
    }
}

} // namespace

std::string markerFileText(std::string bytes) {
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
    }
    if (!validUtf8(bytes)) {
        std::string converted;
        converted.reserve(bytes.size() + bytes.size() / 8);
        for (const char c : bytes) {
            const auto b = static_cast<unsigned char>(c);
            appendUtf8(converted, b >= 0x80 && b < 0xA0 ? kCp1252High[b - 0x80] : b);
        }
        bytes = std::move(converted);
    }
    std::erase(bytes, '\r');
    return bytes;
}

std::expected<ParsedMarkerList, std::string> parseMarkerList(std::string_view text, std::string_view format, Rate rate,
                                                             bool dropFrame) {
    if (text.size() > kMaxMarkerImportBytes) {
        return std::unexpected("the marker file is larger than 16 MiB");
    }
    Reader r;
    r.rate = rate;
    r.dropFrame = dropFrame;
    const std::string_view body = trimmed(text);
    std::string detected(format);
    if (detected.empty() || detected == "auto") {
        if (!body.empty() && (body.front() == '[' || body.front() == '{') && Json::accept(body.begin(), body.end())) {
            detected = "json";
        } else {
            const std::size_t nl = std::min(body.find('\n'), body.size());
            const std::string_view first = body.substr(0, nl);
            const char delimiter = guessDelimiter(first);
            const std::vector<CsvRow> head = csvRows(first, delimiter);
            if (!head.empty() && looksLikeHeader(head.front())) {
                detected = "csv";
            } else {
                // A line that opens with a time and a comma or semicolon is a table without a header row.
                const std::string_view token = leadingTime(first);
                const bool tabular = !token.empty() && token.size() < first.size() &&
                                     (first[token.size()] == ',' || first[token.size()] == ';') && r.time(token).has_value();
                detected = tabular ? "csv" : "txt";
            }
        }
    }
    if (detected == "json") {
        readJson(r, body);
    } else if (detected == "csv") {
        const std::size_t nl = std::min(body.find('\n'), body.size());
        const char delimiter = guessDelimiter(body.substr(0, nl));
        readCsv(r, body, delimiter, false);
    } else if (detected == "txt") {
        readLines(r, body);
    } else {
        return std::unexpected("unknown marker file format \"" + detected + "\" (csv, txt or json)");
    }
    if (r.out.markers.empty()) {
        std::string why = "no marker found in the file";
        if (!r.out.warnings.empty()) {
            why += " (" + r.out.warnings.front() + ")";
        }
        return std::unexpected(std::move(why));
    }
    return std::move(r.out);
}

} // namespace wabcut::command::detail
