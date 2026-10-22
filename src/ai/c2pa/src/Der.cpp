#include "Der.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace wabcut::ai::c2pa::der {

namespace {

void appendLength(Bytes& out, std::size_t n) {
    if (n < 0x80) {
        out.push_back(static_cast<std::uint8_t>(n));
        return;
    }
    std::uint8_t digits[8];
    int count = 0;
    while (n > 0) {
        digits[count++] = static_cast<std::uint8_t>(n & 0xFF);
        n >>= 8;
    }
    out.push_back(static_cast<std::uint8_t>(0x80 | count));
    while (count > 0) {
        out.push_back(digits[--count]);
    }
}

Bytes join(std::uint8_t t, const auto& items) {
    Bytes content;
    for (const Bytes& item : items) {
        content.insert(content.end(), item.begin(), item.end());
    }
    return tlv(t, content);
}

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

Bytes tlv(std::uint8_t t, std::span<const std::uint8_t> content) {
    Bytes out;
    out.reserve(content.size() + 6);
    out.push_back(t);
    appendLength(out, content.size());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

Bytes sequence(std::initializer_list<Bytes> items) { return join(tag::Sequence, items); }
Bytes sequence(const std::vector<Bytes>& items) { return join(tag::Sequence, items); }
Bytes set(std::initializer_list<Bytes> items) { return join(tag::Set, items); }

Bytes integer(std::span<const std::uint8_t> bigEndian) {
    std::size_t first = 0;
    while (first + 1 < bigEndian.size() && bigEndian[first] == 0) {
        ++first;
    }
    Bytes content;
    if (bigEndian.empty()) {
        content.push_back(0);
    } else {
        if (bigEndian[first] & 0x80) {
            content.push_back(0);
        }
        content.insert(content.end(), bigEndian.begin() + static_cast<std::ptrdiff_t>(first), bigEndian.end());
    }
    return tlv(tag::Integer, content);
}

Bytes integer(std::int64_t value) {
    Bytes be;
    for (int shift = 56; shift >= 0; shift -= 8) {
        be.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> shift) & 0xFF));
    }
    return integer(be);
}

Bytes oid(std::string_view dotted) {
    std::vector<std::uint64_t> arcs;
    std::uint64_t current = 0;
    for (char c : dotted) {
        if (c == '.') {
            arcs.push_back(current);
            current = 0;
        } else {
            current = current * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
    arcs.push_back(current);
    Bytes content;
    auto base128 = [&](std::uint64_t v) {
        std::uint8_t digits[10];
        int n = 0;
        do {
            digits[n++] = static_cast<std::uint8_t>(v & 0x7F);
            v >>= 7;
        } while (v > 0);
        while (n > 0) {
            --n;
            content.push_back(static_cast<std::uint8_t>(digits[n] | (n > 0 ? 0x80 : 0)));
        }
    };
    base128(arcs[0] * 40 + (arcs.size() > 1 ? arcs[1] : 0));
    for (std::size_t i = 2; i < arcs.size(); ++i) {
        base128(arcs[i]);
    }
    return tlv(tag::Oid, content);
}

Bytes octetString(std::span<const std::uint8_t> bytes) { return tlv(tag::OctetString, bytes); }

Bytes bitString(std::span<const std::uint8_t> bytes, std::uint8_t unusedBits) {
    Bytes content{unusedBits};
    content.insert(content.end(), bytes.begin(), bytes.end());
    return tlv(tag::BitString, content);
}

Bytes boolean(bool value) {
    const std::uint8_t v = value ? 0xFF : 0x00;
    return tlv(tag::Boolean, std::span(&v, 1));
}

Bytes utf8(std::string_view text) {
    return tlv(tag::Utf8String, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Bytes time(std::int64_t unixSeconds) {
    using namespace std::chrono;
    const sys_seconds t{seconds{unixSeconds}};
    const auto day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss hms{t - day};
    const int y = static_cast<int>(ymd.year());
    char buf[32];
    if (y < 2050) {
        std::snprintf(buf, sizeof buf, "%02d%02u%02u%02d%02d%02dZ", y % 100, static_cast<unsigned>(ymd.month()),
                      static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                      static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()));
        return tlv(tag::UtcTime, std::span(reinterpret_cast<const std::uint8_t*>(buf), 13));
    }
    std::snprintf(buf, sizeof buf, "%04d%02u%02u%02d%02d%02dZ", y, static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                  static_cast<int>(hms.seconds().count()));
    return tlv(tag::GeneralizedTime, std::span(reinterpret_cast<const std::uint8_t*>(buf), 15));
}

Bytes explicitTag(int n, const Bytes& content) { return tlv(tag::context(n), content); }

std::optional<Element> read(std::span<const std::uint8_t> data) {
    if (data.size() < 2) {
        return std::nullopt;
    }
    Element e;
    e.tag = data[0];
    std::size_t length = data[1];
    std::size_t header = 2;
    if (length & 0x80) {
        const std::size_t count = length & 0x7F;
        if (count == 0 || count > 4 || data.size() < 2 + count) {
            return std::nullopt;
        }
        length = 0;
        for (std::size_t i = 0; i < count; ++i) {
            length = (length << 8) | data[2 + i];
        }
        header += count;
    }
    if (data.size() - header < length) {
        return std::nullopt;
    }
    e.content = data.subspan(header, length);
    e.encoded = data.subspan(0, header + length);
    return e;
}

std::optional<std::vector<Element>> children(std::span<const std::uint8_t> content) {
    std::vector<Element> out;
    while (!content.empty()) {
        auto e = read(content);
        if (!e) {
            return std::nullopt;
        }
        out.push_back(*e);
        content = content.subspan(e->encoded.size());
    }
    return out;
}

std::string oidText(std::span<const std::uint8_t> content) {
    std::string out;
    std::uint64_t value = 0;
    bool first = true;
    for (std::uint8_t b : content) {
        value = (value << 7) | (b & 0x7F);
        if (b & 0x80) {
            continue;
        }
        if (first) {
            const std::uint64_t a = value < 80 ? value / 40 : 2;
            out = std::to_string(a) + "." + std::to_string(value - a * 40);
            first = false;
        } else {
            out += "." + std::to_string(value);
        }
        value = 0;
    }
    return out;
}

std::optional<std::int64_t> timeValue(const Element& e) {
    const std::string text(e.content.begin(), e.content.end());
    int y = 0;
    unsigned mo = 0;
    unsigned d = 0;
    int h = 0;
    int mi = 0;
    int s = 0;
    if (e.tag == tag::UtcTime && text.size() == 13) {
        if (std::sscanf(text.c_str(), "%2d%2u%2u%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
            return std::nullopt;
        }
        y += y < 50 ? 2000 : 1900;
    } else if (e.tag == tag::GeneralizedTime && text.size() == 15) {
        if (std::sscanf(text.c_str(), "%4d%2u%2u%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    using namespace std::chrono;
    const year_month_day ymd{year{y}, month{mo}, day{d}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return (sys_days{ymd}.time_since_epoch() + hours{h} + minutes{mi} + seconds{s}) / seconds{1};
}

std::string base64(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0u) |
                                (i + 2 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 2]) : 0u);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
    }
    return out;
}

std::optional<Bytes> unbase64(std::string_view text) {
    Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        const char* p = std::find(kAlphabet, kAlphabet + 64, c);
        if (p == kAlphabet + 64) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(p - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    wipe(std::span(reinterpret_cast<std::uint8_t*>(&buffer), sizeof buffer));
    return out;
}

std::string pem(std::string_view label, std::span<const std::uint8_t> der) {
    std::string body = base64(der);
    std::string out = "-----BEGIN " + std::string(label) + "-----\n";
    for (std::size_t i = 0; i < body.size(); i += 64) {
        out.append(body, i, 64);
        out.push_back('\n');
    }
    out += "-----END " + std::string(label) + "-----\n";
    wipe(body);
    return out;
}

std::vector<Bytes> pemBlocks(std::string_view text, std::string_view label) {
    std::vector<Bytes> out;
    const std::string begin = "-----BEGIN " + std::string(label) + "-----";
    const std::string end = "-----END " + std::string(label) + "-----";
    std::size_t at = 0;
    while ((at = text.find(begin, at)) != std::string_view::npos) {
        const std::size_t from = at + begin.size();
        const std::size_t to = text.find(end, from);
        if (to == std::string_view::npos) {
            break;
        }
        if (auto der = unbase64(text.substr(from, to - from))) {
            out.push_back(std::move(*der));
        }
        at = to + end.size();
    }
    return out;
}

void wipe(std::span<std::uint8_t> bytes) {
    volatile std::uint8_t* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        p[i] = 0;
    }
}

void wipe(std::string& text) {
    wipe(std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size()));
    text.clear();
}

} // namespace wabcut::ai::c2pa::der
