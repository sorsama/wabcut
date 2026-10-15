#include "TextUtil.h"

#include <wabcut/interchange/FileUrl.h>

#include <algorithm>

namespace wabcut::interchange {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// "C:" or "C|" (old file URLs used '|' for the drive colon).
bool isDriveSpec(std::string_view text) {
    return text.size() == 2 && isAlpha(text[0]) && (text[1] == ':' || text[1] == '|');
}

/// The URL scheme ("file", "http") or empty. A single letter before ':' is a drive, not a scheme.
std::string_view schemeOf(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon < 2 || !isAlpha(text[0])) {
        return {};
    }
    for (std::size_t i = 1; i < colon; ++i) {
        const char c = text[i];
        if (!isAlpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') {
            return {};
        }
    }
    return text.substr(0, colon);
}

} // namespace

std::expected<std::string, Error> percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '%') {
            out += c;
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::unexpected(detail::parseError("truncated percent escape"));
        }
        const int hi = hexValue(text[i + 1]);
        const int lo = hexValue(text[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::unexpected(detail::parseError("invalid percent escape"));
        }
        const auto byte = static_cast<char>(hi * 16 + lo);
        if (byte == '\0') {
            return std::unexpected(detail::parseError("percent escape decodes to NUL"));
        }
        out += byte;
        i += 2;
    }
    return out;
}

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        const std::uint32_t minimum = length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u;
        if (cp < minimum || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool isFileUrl(std::string_view text) {
    return detail::iequals(schemeOf(detail::trim(text)), "file");
}

std::expected<std::string, Error> mediaReferenceToPath(std::string_view reference) {
    reference = detail::trim(reference);
    if (reference.empty()) {
        return std::unexpected(detail::parseError("empty media reference"));
    }
    const std::string_view scheme = schemeOf(reference);
    std::string path;
    if (scheme.empty()) {
        path.assign(reference);
    } else {
        if (!detail::iequals(scheme, "file")) {
            return std::unexpected(
                detail::parseError("unsupported URL scheme '" + std::string(scheme) + "'"));
        }
        std::string_view rest = reference.substr(scheme.size() + 1);
        if (const auto cut = rest.find_first_of("?#"); cut != std::string_view::npos) {
            rest = rest.substr(0, cut);
        }
        std::string_view encoded;
        std::string prefix;
        if (rest.starts_with("//")) {
            rest.remove_prefix(2);
            const auto slash = rest.find('/');
            const std::string_view authority = rest.substr(0, slash);
            const std::string_view remainder =
                slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
            if (authority.empty() || detail::iequals(authority, "localhost")) {
                encoded = remainder;
            } else if (isDriveSpec(authority)) {
                prefix = std::string(1, authority[0]) + ":";
                encoded = remainder;
            } else {
                prefix = "//" + std::string(authority); // UNC share
                encoded = remainder;
            }
        } else {
            encoded = rest;
        }
        auto decoded = percentDecode(encoded);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        path = prefix + *decoded;
        // "/C:/..." (or the old "/C|/...") is a Windows drive path.
        if (path.size() >= 3 && path[0] == '/' && isDriveSpec(std::string_view(path).substr(1, 2)) &&
            (path.size() == 3 || path[3] == '/' || path[3] == '\\')) {
            path.erase(0, 1);
        }
        if (path.size() >= 2 && isAlpha(path[0]) && path[1] == '|') {
            path[1] = ':';
        }
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty()) {
        return std::unexpected(detail::parseError("media reference has an empty path"));
    }
    if (path.find('\0') != std::string::npos || !isValidUtf8(path)) {
        return std::unexpected(detail::parseError("media reference is not valid UTF-8"));
    }
    return path;
}

std::string pathToFileUrl(std::string_view path, FileUrlStyle style) {
    std::string p(path);
    for (char& ch : p) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    const auto encode = [](std::string_view text, bool encodeColon) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (const char ch : text) {
            const auto b = static_cast<unsigned char>(ch);
            const bool keep = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' ||
                              b == '.' || b == '_' || b == '~' || b == '/' || (b == ':' && !encodeColon);
            if (keep) {
                out += ch;
            } else {
                out += '%';
                out += kHex[b >> 4];
                out += kHex[b & 0x0F];
            }
        }
        return out;
    };
    if (p.size() >= 2 && p[0] == '/' && p[1] == '/') {
        return "file:" + encode(p, false); // UNC: file://server/share/...
    }
    const bool drive = p.size() >= 2 && p[1] == ':' &&
                       ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
    if (drive) {
        const std::string rest = encode(std::string_view(p).substr(2), false);
        const std::string letter(1, p[0]);
        return style == FileUrlStyle::Localhost ? "file://localhost/" + letter + "%3a" + rest
                                                : "file:///" + letter + ":" + rest;
    }
    if (!p.empty() && p[0] == '/') {
        return (style == FileUrlStyle::Localhost ? "file://localhost" : "file://") + encode(p, false);
    }
    return encode(p, false);
}

bool isWindowsAbsolutePath(std::string_view path) {
    if (path.size() >= 3 && isAlpha(path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        return true;
    }
    return path.size() >= 3 && ((path[0] == '/' && path[1] == '/') || (path[0] == '\\' && path[1] == '\\')) &&
           path[2] != '/' && path[2] != '\\';
}

bool isAbsoluteOnAnyPlatform(std::string_view path) {
    return (!path.empty() && (path[0] == '/' || path[0] == '\\')) || isWindowsAbsolutePath(path);
}

std::string_view fileNameOf(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace wabcut::interchange
