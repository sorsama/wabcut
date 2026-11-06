#include <wabcut/media/WavMetadata.h>

#include <wabcut/model/TitleOps.h>

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace wabcut::media {

namespace {

constexpr int kMaxTracksRead = 4096;   // TRACK elements looked at in one document
constexpr int kMaxChunks = 4096;       // RIFF chunks walked in one file
constexpr std::size_t kBextDescriptionBytes = 256;

bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/// A name as the Library shows it: control characters (and the C1 range) become spaces, runs of white space one space,
/// the ends are trimmed, and it is cut to kMaxChannelNameBytes on a character boundary. Empty when `raw` is not UTF-8.
std::string cleanName(std::string_view raw) {
    if (!edit::isValidUtf8(raw)) {
        return {};
    }
    std::string out;
    out.reserve(std::min<std::size_t>(raw.size(), kMaxChannelNameBytes + 4));
    bool space = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto b = static_cast<unsigned char>(raw[i]);
        const bool c1 = b == 0xC2 && i + 1 < raw.size() && static_cast<unsigned char>(raw[i + 1]) >= 0x80 &&
                        static_cast<unsigned char>(raw[i + 1]) <= 0x9F;
        if (b < 0x20 || b == 0x7F || c1 || b == ' ') {
            space = !out.empty();
            if (c1) {
                ++i;
            }
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        out += static_cast<char>(b);
    }
    if (out.size() > kMaxChannelNameBytes) {
        std::size_t end = kMaxChannelNameBytes;
        while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0U) == 0x80U) {
            --end; // out[end] continues a character: cut before that character
        }
        out.resize(end);
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
    }
    return out;
}

std::optional<int> wholeNumber(std::string_view text) {
    while (!text.empty() && isAsciiSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isAsciiSpace(text.back())) {
        text.remove_suffix(1);
    }
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// The XML declaration's encoding when it is one pugixml's auto-detection would not pick (an 8-bit code page).
pugi::xml_encoding declaredEncoding(std::string_view xml) {
    const std::string_view head = xml.substr(0, 200);
    std::string lower(head);
    for (char& c : lower) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const auto at = lower.find("encoding");
    if (lower.rfind("<?xml", 0) != 0 || at == std::string::npos) {
        return pugi::encoding_auto;
    }
    for (const char* name : {"iso-8859-1", "iso8859-1", "latin1", "latin-1", "windows-1252", "cp1252"}) {
        if (lower.find(name, at) != std::string::npos) {
            return pugi::encoding_latin1;
        }
    }
    return pugi::encoding_auto;
}

} // namespace

std::vector<std::string> parseIxmlTrackNames(std::string_view xml, int channels) {
    if (channels <= 0 || xml.empty() || xml.size() > kMaxIxmlBytes) {
        return {};
    }
    // The chunk is padded to an even size (and some writers pad further) with NULs.
    while (!xml.empty() && (xml.back() == '\0' || isAsciiSpace(xml.back()))) {
        xml.remove_suffix(1);
    }
    // Parsed without a DOCTYPE or declaration node: pugixml never expands entities that a DTD defines or reads an
    // external one, and the buffer is copied, so nothing here refers to the file again.
    pugi::xml_document doc;
    const pugi::xml_parse_result result =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default & ~pugi::parse_declaration, declaredEncoding(xml));
    if (!result) {
        return {};
    }
    const pugi::xml_node list = doc.child("BWFXML").child("TRACK_LIST");
    if (!list) {
        return {};
    }
    const auto count = static_cast<std::size_t>(std::min(channels, kMaxNamedChannels));
    std::vector<std::string> names(count);
    bool any = false;
    int ordinal = 0;
    for (pugi::xml_node track = list.child("TRACK"); track && ordinal < kMaxTracksRead; track = track.next_sibling("TRACK")) {
        ++ordinal;
        int index = ordinal;
        if (const pugi::xml_node interleave = track.child("INTERLEAVE_INDEX")) {
            if (const auto number = wholeNumber(interleave.text().as_string())) {
                index = *number;
            }
        }
        if (index < 1 || static_cast<std::size_t>(index) > count || !names[static_cast<std::size_t>(index) - 1].empty()) {
            continue;
        }
        std::string name = cleanName(track.child("NAME").text().as_string());
        if (name.empty()) {
            continue;
        }
        names[static_cast<std::size_t>(index) - 1] = std::move(name);
        any = true;
    }
    return any ? names : std::vector<std::string>{};
}

std::vector<std::string> parseBextTrackNames(std::string_view description, int channels) {
    if (channels <= 0 || description.empty()) {
        return {};
    }
    // The description is ASCII, NUL-terminated inside its 256 bytes.
    if (const auto nul = description.find('\0'); nul != std::string_view::npos) {
        description = description.substr(0, nul);
    }
    const auto count = static_cast<std::size_t>(std::min(channels, kMaxNamedChannels));
    std::vector<std::string> names(count);
    bool any = false;
    std::size_t at = 0;
    while (at < description.size()) {
        std::size_t end = description.find_first_of("\r\n", at);
        if (end == std::string_view::npos) {
            end = description.size();
        }
        const std::string_view line = description.substr(at, end - at);
        at = end + 1;
        if (line.rfind("sTRK", 0) != 0) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto number = wholeNumber(line.substr(4, equals - 4));
        if (!number || *number < 1 || static_cast<std::size_t>(*number) > count ||
            !names[static_cast<std::size_t>(*number) - 1].empty()) {
            continue;
        }
        std::string name = cleanName(line.substr(equals + 1));
        if (name.empty()) {
            continue;
        }
        names[static_cast<std::size_t>(*number) - 1] = std::move(name);
        any = true;
    }
    return any ? names : std::vector<std::string>{};
}

namespace {

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint64_t le64(const unsigned char* p) { return static_cast<std::uint64_t>(le32(p)) | static_cast<std::uint64_t>(le32(p + 4)) << 32; }

} // namespace

std::vector<std::string> readWavChannelNames(const std::filesystem::path& file, int channels) {
    if (channels <= 0) {
        return {};
    }
    std::error_code ec;
    const std::uint64_t fileSize = std::filesystem::file_size(file, ec);
    if (ec || fileSize < 12) {
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    std::array<unsigned char, 12> header{};
    if (!in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()))) {
        return {};
    }
    const bool riff = std::memcmp(header.data(), "RIFF", 4) == 0 || std::memcmp(header.data(), "RF64", 4) == 0 ||
                      std::memcmp(header.data(), "BW64", 4) == 0;
    if (!riff || std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
        return {};
    }
    std::uint64_t dataSize64 = 0; // RF64 / BW64: the real size of a data chunk whose 32-bit size is 0xFFFFFFFF
    std::vector<std::string> fromBext;
    std::uint64_t pos = 12;
    for (int chunk = 0; chunk < kMaxChunks && pos + 8 <= fileSize; ++chunk) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(pos));
        std::array<unsigned char, 8> head{};
        if (!in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) {
            break;
        }
        std::uint64_t size = le32(head.data() + 4);
        const std::uint64_t body = pos + 8;
        if (std::memcmp(head.data(), "ds64", 4) == 0 && size >= 16) {
            std::array<unsigned char, 16> ds{};
            if (in.read(reinterpret_cast<char*>(ds.data()), static_cast<std::streamsize>(ds.size()))) {
                dataSize64 = le64(ds.data() + 8);
            }
        } else if (std::memcmp(head.data(), "data", 4) == 0 && size == 0xFFFFFFFFULL) {
            size = dataSize64;
        } else if (std::memcmp(head.data(), "iXML", 4) == 0 && size > 0 && size <= kMaxIxmlBytes && body + size <= fileSize) {
            std::string xml(static_cast<std::size_t>(size), '\0');
            in.clear();
            in.seekg(static_cast<std::streamoff>(body));
            if (in.read(xml.data(), static_cast<std::streamsize>(xml.size()))) {
                if (auto names = parseIxmlTrackNames(xml, channels); !names.empty()) {
                    return names; // the recorder's own list wins over the old description convention
                }
            }
        } else if (std::memcmp(head.data(), "bext", 4) == 0 && size > 0 && fromBext.empty()) {
            std::string description(static_cast<std::size_t>(std::min<std::uint64_t>(size, kBextDescriptionBytes)), '\0');
            in.clear();
            in.seekg(static_cast<std::streamoff>(body));
            if (in.read(description.data(), static_cast<std::streamsize>(description.size()))) {
                fromBext = parseBextTrackNames(description, channels);
            }
        }
        if (size > fileSize) {
            break; // a size past the end of the file: the rest is not chunks
        }
        pos = body + size + (size & 1U);
    }
    return fromBext;
}

} // namespace wabcut::media
