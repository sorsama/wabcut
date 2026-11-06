#include <wabcut/cloud/Common.h>

#include <wabcut/ai/Sha256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace wabcut::cloud {

Error toError(const StoreError& error) {
    ErrorCode code = ErrorCode::IoError;
    switch (error.kind) {
    case StoreFailure::NotFound:
        code = ErrorCode::NotFound;
        break;
    case StoreFailure::Invalid:
        code = ErrorCode::InvalidArgument;
        break;
    case StoreFailure::Cancelled:
        code = ErrorCode::Cancelled;
        break;
    default:
        break;
    }
    return Error{code, error.message};
}

StoreError fromTransport(const ai::AiError& error) {
    StoreFailure kind = StoreFailure::Other;
    switch (error.code) {
    case ai::AiErrorCode::Unauthorized:
    case ai::AiErrorCode::NoCredentials:
        kind = StoreFailure::Unauthorized;
        break;
    case ai::AiErrorCode::Offline:
        kind = StoreFailure::Offline;
        break;
    case ai::AiErrorCode::Cancelled:
        kind = StoreFailure::Cancelled;
        break;
    case ai::AiErrorCode::PayloadTooLarge:
        kind = StoreFailure::TooLarge;
        break;
    case ai::AiErrorCode::InvalidRequest:
        kind = StoreFailure::Invalid;
        break;
    case ai::AiErrorCode::Network:
    case ai::AiErrorCode::RateLimited:
    case ai::AiErrorCode::ProviderError:
        kind = StoreFailure::Network;
        break;
    default:
        break;
    }
    return StoreError{kind, error.message, error.httpStatus};
}

StoreError fromStatus(int status, std::string_view what) {
    StoreFailure kind = StoreFailure::Other;
    switch (status) {
    case 401:
    case 403:
        kind = StoreFailure::Unauthorized;
        break;
    case 404:
    case 410:
        kind = StoreFailure::NotFound;
        break;
    case 409:
    case 412:
        kind = StoreFailure::PreconditionFailed;
        break;
    case 413:
    case 507:
        kind = StoreFailure::TooLarge;
        break;
    default:
        break;
    }
    return StoreError{kind, std::string(what) + " failed (HTTP " + std::to_string(status) + ")", status};
}

// ---- hashes and encodings ----

std::string sha256Hex(std::string_view data) { return ai::Sha256::hex(data); }

std::string hmacSha256(std::string_view key, std::string_view data) {
    constexpr std::size_t kBlock = 64;
    std::array<std::uint8_t, kBlock> k{};
    if (key.size() > kBlock) {
        ai::Sha256 h;
        h.update(key);
        const auto digest = h.finish();
        std::copy(digest.begin(), digest.end(), k.begin());
    } else {
        std::copy(key.begin(), key.end(), k.begin());
    }
    std::string inner(kBlock, '\0');
    std::string outer(kBlock, '\0');
    for (std::size_t i = 0; i < kBlock; ++i) {
        inner[i] = static_cast<char>(k[i] ^ 0x36u);
        outer[i] = static_cast<char>(k[i] ^ 0x5cu);
    }
    ai::Sha256 a;
    a.update(inner);
    a.update(data);
    const auto innerDigest = a.finish();
    ai::Sha256 b;
    b.update(outer);
    b.update(std::string_view(reinterpret_cast<const char*>(innerDigest.data()), innerDigest.size()));
    const auto digest = b.finish();
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string toHex(std::string_view raw) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out += kDigits[b >> 4];
        out += kDigits[b & 15u];
    }
    return out;
}

std::string base64Encode(std::string_view data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const auto n = (std::uint32_t{static_cast<unsigned char>(data[i])} << 16) |
                       (std::uint32_t{static_cast<unsigned char>(data[i + 1])} << 8) |
                       std::uint32_t{static_cast<unsigned char>(data[i + 2])};
        out += kTable[(n >> 18) & 63u];
        out += kTable[(n >> 12) & 63u];
        out += kTable[(n >> 6) & 63u];
        out += kTable[n & 63u];
    }
    if (i < data.size()) {
        std::uint32_t n = std::uint32_t{static_cast<unsigned char>(data[i])} << 16;
        if (i + 1 < data.size()) {
            n |= std::uint32_t{static_cast<unsigned char>(data[i + 1])} << 8;
        }
        out += kTable[(n >> 18) & 63u];
        out += kTable[(n >> 12) & 63u];
        out += i + 1 < data.size() ? kTable[(n >> 6) & 63u] : '=';
        out += '=';
    }
    return out;
}

std::string base32Lower(std::string_view raw) {
    static constexpr char kTable[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : raw) {
        buffer = (buffer << 8) | static_cast<unsigned char>(c);
        bits += 8;
        while (bits >= 5) {
            out += kTable[(buffer >> (bits - 5)) & 31u];
            bits -= 5;
        }
    }
    if (bits > 0) {
        out += kTable[(buffer << (5 - bits)) & 31u];
    }
    return out;
}

std::string uriEncode(std::string_view text, bool encodeSlash) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto b = static_cast<unsigned char>(c);
        const bool plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '.' ||
                           b == '_' || b == '~' || (b == '/' && !encodeSlash);
        if (plain) {
            out += c;
        } else {
            out += '%';
            out += kDigits[b >> 4];
            out += kDigits[b & 15u];
        }
    }
    return out;
}

std::optional<std::string> uriDecode(std::string_view text) {
    const auto digit = [](char c) -> int {
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
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::nullopt;
        }
        const int hi = digit(text[i + 1]);
        const int lo = digit(text[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        const char decoded = static_cast<char>(hi * 16 + lo);
        if (decoded == '\0') {
            return std::nullopt;
        }
        out += decoded;
        i += 2;
    }
    return out;
}

// ---- randomness ----

RandomBytes systemRandom() {
    return [](std::size_t count) {
        static std::mutex mutex;
        static std::random_device device;
        std::lock_guard lock(mutex);
        std::string out(count, '\0');
        for (std::size_t i = 0; i < count; i += 4) {
            const std::uint32_t word = device();
            for (std::size_t j = 0; j < 4 && i + j < count; ++j) {
                out[i + j] = static_cast<char>((word >> (8 * j)) & 0xffu);
            }
        }
        return out;
    };
}

std::string newToken(const RandomBytes& random, std::size_t bytes) { return base32Lower(random(bytes)); }

// ---- names ----

bool validSegment(std::string_view segment) {
    if (segment.empty() || segment.size() > 128 || segment.front() == '.') {
        return false;
    }
    return std::all_of(segment.begin(), segment.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

bool validPath(std::string_view path) {
    if (path.empty()) {
        return false;
    }
    int segments = 0;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!validSegment(part) || ++segments > 8) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

std::string joinPath(std::string_view a, std::string_view b) {
    if (a.empty()) {
        return std::string(b);
    }
    if (b.empty()) {
        return std::string(a);
    }
    return std::string(a) + "/" + std::string(b);
}

std::string_view baseName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view parentOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

// ---- time ----

UnixClock systemClock() {
    return [] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
}

namespace {

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

bool digitsAt(std::string_view text, std::size_t at, std::size_t count, int& value) {
    if (at + count > text.size()) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    return true;
}

} // namespace

std::string formatIsoUtc(std::int64_t unixSeconds) {
    std::int64_t days = unixSeconds / 86400;
    std::int64_t rest = unixSeconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    std::int64_t y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(days, y, m, d);
    char text[40];
    std::snprintf(text, sizeof text, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(y), m, d,
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest % 3600 / 60), static_cast<long long>(rest % 60));
    return text;
}

std::optional<std::int64_t> parseIsoUtc(std::string_view text) {
    int y = 0;
    int mo = 0;
    int d = 0;
    if (!digitsAt(text, 0, 4, y) || text.size() < 10 || text[4] != '-' || !digitsAt(text, 5, 2, mo) || text[7] != '-' ||
        !digitsAt(text, 8, 2, d) || y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return std::nullopt;
    }
    std::int64_t seconds = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400;
    if (text.size() == 10) {
        return seconds;
    }
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if ((text[10] != 'T' && text[10] != ' ') || !digitsAt(text, 11, 2, hh) || text.size() < 16 || text[13] != ':' ||
        !digitsAt(text, 14, 2, mm) || hh > 23 || mm > 59) {
        return std::nullopt;
    }
    if (text.size() >= 19) {
        if (text[16] != ':' || !digitsAt(text, 17, 2, ss) || ss > 60) {
            return std::nullopt;
        }
    }
    const std::string_view tail = text.size() >= 19 ? text.substr(19) : text.substr(16);
    if (!tail.empty() && tail != "Z") {
        return std::nullopt;
    }
    seconds += hh * 3600 + mm * 60 + ss;
    return seconds;
}

namespace {

std::string environmentText(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string cut(std::string text) {
    if (text.size() > 128) {
        text.resize(128);
    }
    return text;
}

} // namespace

Identity currentIdentity(const RandomBytes& random) {
    Identity id;
#ifdef _WIN32
    id.user = environmentText("USERNAME");
    id.host = environmentText("COMPUTERNAME");
#else
    id.user = environmentText("USER");
    char buffer[256] = {};
    if (gethostname(buffer, sizeof buffer - 1) == 0) {
        id.host = buffer;
    }
#endif
    if (id.user.empty()) {
        id.user = "someone";
    }
    if (id.host.empty()) {
        id.host = "a computer";
    }
    id.user = cut(std::move(id.user));
    id.host = cut(std::move(id.host));
    id.session = toHex(random(8));
    return id;
}

} // namespace wabcut::cloud
