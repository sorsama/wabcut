#include "TextUtil.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace wabcut::interchange::detail {

namespace {

constexpr bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
constexpr char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string_view trim(std::string_view text) {
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

bool istartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

bool iendsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && iequals(text.substr(text.size() - suffix.size()), suffix);
}

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lowerAscii);
    return out;
}

std::optional<std::int64_t> parseInt64(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseDouble(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parseBool(std::string_view text) {
    text = trim(text);
    if (iequals(text, "true") || text == "1" || iequals(text, "yes")) {
        return true;
    }
    if (iequals(text, "false") || text == "0" || iequals(text, "no")) {
        return false;
    }
    return std::nullopt;
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }
    return lines;
}

std::vector<std::string_view> splitWords(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') {
            ++i;
        }
        if (i > start) {
            words.push_back(text.substr(start, i - start));
        }
    }
    return words;
}

namespace {

/// Length of the well-formed UTF-8 sequence at `i`, or 0.
std::size_t utf8SequenceAt(std::string_view s, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        n = 2;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        n = 3;
        cp = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        n = 4;
        cp = lead & 0x07u;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        if ((byte(i + k) & 0xC0u) != 0x80u) {
            return 0;
        }
        cp = (cp << 6) | (byte(i + k) & 0x3Fu);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
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

// Windows-1252 0x80-0x9F (0xFFFD: unassigned).
constexpr std::uint16_t kCp1252High[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

} // namespace

bool isValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t n = utf8SequenceAt(text, i);
        if (n == 0) {
            return false;
        }
        i += n;
    }
    return true;
}

std::string toUtf8Lenient(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    for (std::size_t i = 0; i < text.size();) {
        if (const std::size_t n = utf8SequenceAt(text, i); n != 0) {
            out.append(text.substr(i, n));
            i += n;
            continue;
        }
        const auto byte = static_cast<unsigned char>(text[i]);
        appendUtf8(out, byte >= 0x80 && byte <= 0x9F ? kCp1252High[byte - 0x80] : byte);
        ++i;
    }
    return out;
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    std::u8string text(utf8.size(), u8'\0');
    std::transform(utf8.begin(), utf8.end(), text.begin(), [](char c) { return static_cast<char8_t>(c); });
    return std::filesystem::path(text);
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

namespace {

std::expected<std::string, Error> readFileImpl(const std::filesystem::path& path, std::size_t maxBytes,
                                               bool truncate) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        return std::unexpected(Error{ErrorCode::NotFound, "file not found: " + utf8FromPath(path)});
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(Error{ErrorCode::IoError, "not a regular file: " + utf8FromPath(path)});
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read the size of " + utf8FromPath(path)});
    }
    if (size > maxBytes && !truncate) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "file is too large to be a timeline (" +
                                                                     std::to_string(size) + " bytes)"});
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot open " + utf8FromPath(path)});
    }
    const auto toRead = static_cast<std::size_t>(std::min<std::uintmax_t>(size, maxBytes));
    std::string data(toRead, '\0');
    in.read(data.data(), static_cast<std::streamsize>(toRead));
    data.resize(static_cast<std::size_t>(in.gcount()));
    if (in.bad()) {
        return std::unexpected(Error{ErrorCode::IoError, "read error on " + utf8FromPath(path)});
    }
    return data;
}

} // namespace

std::expected<std::string, Error> readFileLimited(const std::filesystem::path& path, std::size_t maxBytes) {
    return readFileImpl(path, maxBytes, false);
}

std::expected<std::string, Error> readFilePrefix(const std::filesystem::path& path, std::size_t maxBytes) {
    return readFileImpl(path, maxBytes, true);
}

std::string timecodeLabel(Ticks t, Rate rate, bool dropFrame) {
    if (!rate.isExact()) {
        return std::to_string(t.value) + " ticks";
    }
    return ticksToTimecode(t, rate, dropFrame);
}

std::string framesLabel(Ticks duration, Rate rate) {
    if (!rate.isExact()) {
        return std::to_string(duration.value) + " ticks";
    }
    const std::int64_t perFrame = rate.ticksPerUnit();
    if (duration.value % perFrame == 0) {
        const std::int64_t frames = duration.value / perFrame;
        return std::to_string(frames) + (frames == 1 ? " frame" : " frames");
    }
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.2f frames",
                  static_cast<double>(duration.value) / static_cast<double>(perFrame));
    return buffer;
}

Error parseError(std::string message) {
    return Error{ErrorCode::ParseError, std::move(message)};
}
Error unsupported(std::string message) {
    return Error{ErrorCode::Unsupported, std::move(message)};
}

} // namespace wabcut::interchange::detail
