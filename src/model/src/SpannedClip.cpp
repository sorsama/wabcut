#include <wabcut/model/SpannedClip.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/RawSourceSettings.h>

#include <algorithm>
#include <charconv>
#include <system_error>

namespace wabcut {

namespace {

constexpr std::string_view kQuery = "?span=";

bool validName(std::string_view name) {
    if (name.empty() || name.size() > kMaxSpannedNameBytes || name == "." || name == "..") {
        return false;
    }
    return std::ranges::none_of(name, [](char c) {
        return c == '/' || c == '\\' || c == '?' || c == '|' || c == ':' || c == '*' || c == '\0';
    });
}

std::optional<std::int64_t> parseMicroseconds(std::string_view text) {
    if (text.empty() || text.size() > 12) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value <= 0 || value > kMaxSpannedPartMicroseconds) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::filesystem::path SpannedClipSpec::partFile(std::size_t i) const {
    return i < parts.size() ? pathFromUtf8(parts[i].name) : std::filesystem::path{};
}

std::int64_t SpannedClipSpec::totalMicroseconds() const {
    std::int64_t total = 0;
    for (const SpannedPart& p : parts) {
        total += p.microseconds;
    }
    return total;
}

std::string spannedClipPath(const SpannedClipSpec& spec) {
    if (spec.parts.size() < 2 || spec.parts.size() > kMaxSpannedParts) {
        return {};
    }
    // The folder as it is written, and the first part joined with the separator it uses (one style in one path).
    std::string out = pathToUtf8(spec.directory);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') {
        out += out.find('\\') != std::string::npos ? '\\' : '/';
    }
    out += spec.parts.front().name;
    out += kQuery;
    for (std::size_t i = 0; i < spec.parts.size(); ++i) {
        const SpannedPart& p = spec.parts[i];
        if (!validName(p.name) || p.microseconds <= 0 || p.microseconds > kMaxSpannedPartMicroseconds) {
            return {};
        }
        if (i > 0) {
            out += '|';
        }
        out += p.name;
        out += ':';
        out += std::to_string(p.microseconds);
    }
    return out.size() <= kMaxSpannedPathBytes ? out : std::string{};
}

std::optional<SpannedClipSpec> parseSpannedClipPath(std::string_view utf8) {
    utf8 = withoutRawSourceSettings(utf8); // 0.2 RAW Source Settings ride after the path
    if (utf8.size() > kMaxSpannedPathBytes) {
        return std::nullopt;
    }
    const std::size_t query = utf8.find(kQuery);
    if (query == std::string_view::npos || query == 0) {
        return std::nullopt;
    }
    const std::filesystem::path first = pathFromUtf8(std::string(utf8.substr(0, query)));
    SpannedClipSpec spec;
    spec.directory = first.parent_path();
    std::string_view rest = utf8.substr(query + kQuery.size());
    while (!rest.empty()) {
        if (spec.parts.size() >= kMaxSpannedParts) {
            return std::nullopt;
        }
        const std::size_t bar = rest.find('|');
        const std::string_view entry = rest.substr(0, bar);
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
        if (bar != std::string_view::npos && rest.empty()) {
            return std::nullopt; // a trailing '|'
        }
        const std::size_t colon = entry.rfind(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = entry.substr(0, colon);
        const auto micros = parseMicroseconds(entry.substr(colon + 1));
        if (!validName(name) || !micros) {
            return std::nullopt;
        }
        spec.parts.push_back({std::string(name), *micros});
    }
    if (spec.parts.size() < 2 || pathToUtf8(first.filename()) != spec.parts.front().name) {
        return std::nullopt;
    }
    return spec;
}

} // namespace wabcut
