// Camera log auto-detection (CameraLog.h).

#include <wabcut/media/CameraLog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

namespace wabcut::media {

namespace {

/// Lower-case letters and digits only: "S-Log3.Cine" -> "slog3cine", "LOG-C" -> "logc".
std::string squeeze(std::string_view text) {
    std::string out;
    out.reserve(std::min<std::size_t>(text.size(), 128));
    for (const char ch : text.substr(0, 256)) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

/// The value of `<Item name="<name>" value="..."/>` in Sony's XML, or nullopt.
std::optional<std::string_view> itemValue(std::string_view xml, std::string_view name) {
    const std::string needle = "\"" + std::string(name) + "\"";
    const std::size_t at = xml.find(needle);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t end = xml.find('>', at);
    const std::string_view tag = xml.substr(at, end == std::string_view::npos ? 256 : end - at);
    const std::size_t value = tag.find("value=\"");
    if (value == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t from = value + 7;
    const std::size_t to = tag.find('"', from);
    if (to == std::string_view::npos) {
        return std::nullopt;
    }
    return tag.substr(from, to - from);
}

/// What a metadata box's text is searched for: the camera log, or (0.7) the camera's model.
using Extract = std::string (*)(std::string_view);

/// The camera model Sony's NonRealTimeMeta XML names: <Device manufacturer="Sony" modelName="ILCE-7M4" .../>.
std::string modelFromSonyXml(std::string_view xml) {
    const std::size_t device = xml.find("manufacturer=\"Sony\"");
    if (device == std::string_view::npos) {
        return {};
    }
    const std::size_t at = xml.find("modelName=\"", device);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t from = at + 11;
    const std::size_t to = xml.find('"', from);
    if (to == std::string_view::npos || to - from > 64) {
        return {};
    }
    // Printable ASCII only: the model is stored in a project (JSON, valid UTF-8), and a camera name is plain ASCII.
    std::string model;
    for (const char c : xml.substr(from, to - from)) {
        if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) {
            model.push_back(c);
        }
    }
    return model;
}

/// Reads at most `limit` bytes of `file` at `offset`.
std::string readAt(std::ifstream& in, std::uint64_t offset, std::size_t limit) {
    std::string out(limit, '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(out.data(), static_cast<std::streamsize>(limit));
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return out;
}

std::uint64_t be(const unsigned char* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

/// Walks the ISO-BMFF (MP4 / QuickTime) boxes in [begin, end): reads the metadata boxes ("meta", "uuid", "XMP_", "xml ")
/// and descends into "moov" and "udta"; never reads media data or track tables. Sony keeps its NonRealTimeMeta XML in
/// such a box. `budget`: the bytes still allowed.
std::string walkBoxes(std::ifstream& in, std::uint64_t begin, std::uint64_t end, int depth, std::size_t& budget, Extract extract) {
    std::uint64_t offset = begin;
    for (int boxes = 0; boxes < 256 && offset + 8 <= end && budget > 0; ++boxes) {
        const std::string header = readAt(in, offset, 16);
        if (header.size() < 8) {
            break;
        }
        const auto* h = reinterpret_cast<const unsigned char*>(header.data());
        std::uint64_t length = be(h, 4);
        std::uint64_t headerSize = 8;
        if (length == 1 && header.size() >= 16) {
            length = be(h + 8, 8);
            headerSize = 16;
        } else if (length == 0) {
            length = end - offset; // to the end of the enclosing box
        }
        if (length < headerSize || offset + length > end) {
            break; // not ISO-BMFF, or truncated
        }
        const std::string_view type(header.data() + 4, 4);
        if (depth == 0 && boxes == 0 && type != "ftyp" && type != "moov" && type != "wide" && type != "free" &&
            type != "mdat" && type != "skip") {
            break; // not an MP4 / QuickTime file
        }
        if ((type == "moov" || type == "udta") && depth < 2) {
            if (std::string log = walkBoxes(in, offset + headerSize, offset + length, depth + 1, budget, extract); !log.empty()) {
                return log;
            }
        } else if (type == "meta" || type == "uuid" || type == "XMP_" || type == "xml ") {
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(length - headerSize, budget));
            const std::string body = readAt(in, offset + headerSize, take);
            budget -= std::min(budget, body.size());
            if (std::string log = extract(body); !log.empty()) {
                return log;
            }
        }
        offset += length;
    }
    return {};
}

/// The camera log in an MP4 / QuickTime file's metadata boxes, reading at most kMaxMetadataBytes of them.
std::string fromIsoBoxes(const std::filesystem::path& file, Extract extract = cameraLogFromXml) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(file, ec);
    if (ec || size < 8) {
        return {};
    }
    std::size_t budget = kMaxMetadataBytes;
    return walkBoxes(in, 0, size, 0, budget, extract);
}

/// Sony's sidecar: "C0001.MP4" -> "C0001M01.XML" in the same folder (any case).
std::string fromSonySidecar(const std::filesystem::path& file, Extract extract = cameraLogFromXml) {
    for (const char* suffix : {"M01.XML", "M01.xml"}) {
        std::filesystem::path sidecar = file.parent_path() / file.stem();
        sidecar += suffix;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sidecar, ec)) {
            continue;
        }
        std::ifstream in(sidecar, std::ios::binary);
        std::string xml(kMaxMetadataBytes, '\0');
        in.read(xml.data(), static_cast<std::streamsize>(xml.size()));
        xml.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
        if (std::string log = extract(xml); !log.empty()) {
            return log;
        }
    }
    return {};
}

} // namespace

std::string cameraLogFromName(std::string_view curve, std::string_view gamut) {
    const std::string c = squeeze(curve);
    const std::string g = squeeze(gamut);
    if (c.empty()) {
        return {};
    }
    // Order matters: the names are squeezed, so "Canon Log Cinema" holds "logc", "REDlogFilm" holds "dlog" and "Canon
    // Log" holds "nlog" - each is matched before the shorter name it contains.
    if (contains(c, "slog3")) {
        return contains(c, "cine") || contains(g, "cine") ? "s-log3-cine" : "s-log3";
    }
    if (contains(c, "slog2")) {
        return contains(c, "cine") || contains(g, "cine") ? "s-log2-cine" : "s-log2";
    }
    if (contains(c, "vlog")) {
        return "v-log";
    }
    if (contains(c, "canonlog3") || contains(c, "clog3")) {
        return "canon-log3";
    }
    if (contains(c, "canonlog2") || contains(c, "clog2")) {
        return "canon-log2";
    }
    if (contains(c, "canonlog") || c.starts_with("clog")) { // the original Canon Log: "Canon Log", "C-Log", "CLog"
        return "canon-log";
    }
    if (contains(c, "logc4")) {
        return "logc4";
    }
    if (contains(c, "logc")) { // "LOG-C", "LogC3", "LogC (EI 800)"
        return "logc3";
    }
    if (contains(c, "log3g10")) {
        return "red-log3g10";
    }
    if (contains(c, "redlogfilm")) {
        return "red-log-film";
    }
    if (contains(c, "applelog")) {
        return "apple-log";
    }
    if (c == "acescct") {
        return "acescct";
    }
    if (contains(c, "flog2c")) {
        return {}; // F-Log2 C: F-Gamut C, not a space here
    }
    if (contains(c, "flog2")) {
        return "f-log2";
    }
    if (contains(c, "flog")) {
        return "f-log";
    }
    if (c.starts_with("nlog") || contains(c, "nikonnlog")) {
        return "n-log";
    }
    if (c.starts_with("llog") || contains(c, "leicallog")) {
        return "l-log";
    }
    if (c.starts_with("dlogm") || contains(c, "djidlogm")) {
        return {}; // D-Log M: DJI publishes no equation for it
    }
    if (c.starts_with("dlog") || contains(c, "djidlog")) {
        return "d-log";
    }
    return {};
}

std::string cameraLogFromXml(std::string_view xml) {
    const auto gamma = itemValue(xml, "CaptureGammaEquation");
    if (!gamma) {
        return {};
    }
    const auto primaries = itemValue(xml, "CaptureColorPrimaries");
    return cameraLogFromName(*gamma, primaries.value_or(std::string_view{}));
}

std::string cameraLogFromTag(std::string_view key, std::string_view value) {
    const std::string k = squeeze(key);
    if (!(contains(k, "gamma") || contains(k, "transfer") || contains(k, "log") || contains(k, "colorspace") ||
          contains(k, "curve"))) {
        return {};
    }
    if (std::string log = cameraLogFromXml(value); !log.empty()) {
        return log; // a tag holding Sony's XML
    }
    return cameraLogFromName(value);
}

std::string cameraLogFromFile(const std::filesystem::path& file) {
    if (std::string log = fromIsoBoxes(file); !log.empty()) {
        return log;
    }
    return fromSonySidecar(file);
}

std::string cameraModelFromFile(const std::filesystem::path& file) {
    if (std::string model = fromIsoBoxes(file, modelFromSonyXml); !model.empty()) {
        return model;
    }
    return fromSonySidecar(file, modelFromSonyXml);
}

} // namespace wabcut::media
