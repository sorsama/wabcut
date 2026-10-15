#include <wabcut/render/reference/Image.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace wabcut::render::reference {

namespace {

constexpr std::string_view kMagic = "WRI1";
constexpr int kMaxSide = 4096;
constexpr std::uintmax_t kMaxFileBytes = 256u * 1024u * 1024u;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// The next whitespace-separated token of `text` from `pos` (empty at the end).
std::string_view nextToken(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && isSpace(text[pos])) {
        ++pos;
    }
    const std::size_t start = pos;
    while (pos < text.size() && !isSpace(text[pos])) {
        ++pos;
    }
    return text.substr(start, pos - start);
}

std::unexpected<Error> parseError(std::string message) {
    return std::unexpected(Error{ErrorCode::ParseError, "golden image: " + std::move(message)});
}

bool parseInt(std::string_view token, int& out) {
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), out);
    return ec == std::errc() && ptr == token.data() + token.size();
}

void appendValue(std::string& out, float v) {
    char buffer[32];
    const float value = v == 0.0f ? 0.0f : v; // -0 is written 0
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general, 6);
    out.append(buffer, result.ptr);
}

} // namespace

std::string goldenText(const Image& image) {
    std::string out;
    out.reserve(image.pixelCount() * 32 + 32);
    out += kMagic;
    out += ' ';
    out += std::to_string(image.width);
    out += ' ';
    out += std::to_string(image.height);
    out += '\n';
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (c > 0) {
                out += ' ';
            }
            appendValue(out, image.rgba[i + c]);
        }
        out += '\n';
    }
    return out;
}

std::expected<Image, Error> parseGolden(std::string_view text) {
    std::size_t pos = 0;
    if (nextToken(text, pos) != kMagic) {
        return parseError("the first line must start with WRI1");
    }
    int width = 0;
    int height = 0;
    if (!parseInt(nextToken(text, pos), width) || !parseInt(nextToken(text, pos), height) || width < 1 ||
        height < 1 || width > kMaxSide || height > kMaxSide) {
        return parseError("the size must be two integers in 1.." + std::to_string(kMaxSide));
    }
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    std::vector<float> values;
    values.reserve(std::min(count, text.size() / 2 + 1)); // never more than the text can hold
    for (;;) {
        const std::string_view token = nextToken(text, pos);
        if (token.empty()) {
            break;
        }
        if (values.size() == count) {
            return parseError("more values than " + std::to_string(width) + " x " + std::to_string(height) + " x 4");
        }
        float v = 0.0f;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), v, std::chars_format::general);
        if (ec != std::errc() || ptr != token.data() + token.size() || !std::isfinite(v)) {
            return parseError("value " + std::to_string(values.size()) + " is not a finite number");
        }
        values.push_back(v);
    }
    if (values.size() != count) {
        return parseError("expected " + std::to_string(count) + " values, found " + std::to_string(values.size()));
    }
    Image out;
    out.width = width;
    out.height = height;
    out.rgba = std::move(values);
    return out;
}

std::expected<Image, Error> readGolden(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "golden image not found: " + file.string()});
    }
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size > kMaxFileBytes) {
        return std::unexpected(Error{ErrorCode::IoError, "golden image unreadable or too large: " + file.string()});
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(text.data(), static_cast<std::streamsize>(size))) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read golden image: " + file.string()});
    }
    return parseGolden(text);
}

bool writeGolden(const std::filesystem::path& file, const Image& image) {
    if (image.empty() || image.rgba.size() != image.pixelCount() * 4 || image.width > kMaxSide ||
        image.height > kMaxSide) {
        return false;
    }
    for (const float v : image.rgba) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    const std::string text = goldenText(image);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

} // namespace wabcut::render::reference
