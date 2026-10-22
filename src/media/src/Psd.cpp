#include <wabcut/media/Psd.h>

#include <wabcut/model/MediaLocator.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

// The format: Adobe's "Photoshop File Formats Specification" (header, colour mode data, image resources, layer and mask
// information, image data). Layer records are stored bottom layer first; groups are a folder record ("lsct" 1 or 2)
// above their layers and an end marker ("lsct" 3) below them. 16- and 32-bit documents usually keep their layers in the
// "Lr16" / "Lr32" block of the global additional layer information instead of the layer info. 32-bit samples are
// big-endian IEEE floats, linear light; CMYK samples are stored inverted (the largest value is no ink).

namespace wabcut::media {

namespace {

Error malformed(std::string_view what) {
    return Error{ErrorCode::InvalidArgument, "Not a valid Photoshop file: " + std::string(what)};
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}

    bool ok() const { return !m_failed; }
    std::size_t tell() const { return m_pos; }
    std::size_t size() const { return m_data.size(); }
    std::size_t remaining() const { return m_pos <= m_data.size() ? m_data.size() - m_pos : 0; }
    void seek(std::uint64_t pos) {
        if (pos > m_data.size()) {
            m_failed = true;
            m_pos = m_data.size();
            return;
        }
        m_pos = static_cast<std::size_t>(pos);
    }
    void skip(std::uint64_t n) { seek(n > remaining() ? std::uint64_t{m_data.size()} + 1 : m_pos + n); }
    std::span<const std::uint8_t> bytes(std::uint64_t n) {
        if (n > remaining()) {
            m_failed = true;
            m_pos = m_data.size();
            return {};
        }
        auto out = m_data.subspan(m_pos, static_cast<std::size_t>(n));
        m_pos += static_cast<std::size_t>(n);
        return out;
    }
    std::uint64_t be(int count) {
        const auto b = bytes(static_cast<std::uint64_t>(count));
        if (b.size() != static_cast<std::size_t>(count)) {
            return 0;
        }
        std::uint64_t v = 0;
        for (const std::uint8_t c : b) {
            v = (v << 8) | c;
        }
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(be(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(be(2)); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(be(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return be(8); }
    std::uint64_t length(bool wide) { return wide ? u64() : u32(); }
    std::string key() {
        const auto b = bytes(4);
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }
    bool atSignature() const {
        if (remaining() < 4) {
            return false;
        }
        const char* p = reinterpret_cast<const char*>(m_data.data() + m_pos);
        return std::string_view(p, 4) == "8BIM" || std::string_view(p, 4) == "8B64";
    }
    std::uint8_t peek() const { return m_pos < m_data.size() ? m_data[m_pos] : 0; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
    bool m_failed = false;
};

struct ChannelInfo {
    std::int16_t id = 0;
    std::uint64_t length = 0;  ///< compression word included
    std::uint64_t offset = 0;  ///< of its compression word in the file
};

struct LayerRecord {
    PsdLayer layer;
    std::vector<ChannelInfo> channels;
    bool ownVisible = true;
    double ownOpacity = 1.0;
    int section = 0; ///< "lsct": 0 a layer, 1 / 2 a group's folder (open / closed), 3 a group's end marker
    // The user mask (channel -2): its rectangle, the value outside it, and whether it is disabled.
    bool hasMask = false;
    int maskLeft = 0, maskTop = 0, maskRight = 0, maskBottom = 0;
    std::uint8_t maskDefault = 0;
    bool maskDisabled = false;
};

struct Parsed {
    PsdDocument doc;
    int channels = 0;
    std::vector<LayerRecord> records;
    std::uint64_t mergedOffset = 0; ///< the image data section
};

/// Photoshop's 8-byte-length keys in PSB files.
bool wideKey(std::string_view key) {
    static constexpr std::array<std::string_view, 13> kWide{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                                           "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    return std::ranges::find(kWide, key) != kWide.end();
}

std::string latin1ToUtf8(std::span<const std::uint8_t> text) {
    std::string out;
    for (const std::uint8_t c : text) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string utf16beToUtf8(Reader& r, std::uint32_t count) {
    std::string out;
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        std::uint32_t c = r.u16();
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {
            const std::uint32_t low = r.u16();
            ++i;
            c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        }
        if (c == 0) {
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

/// Layer info (the count, the records and the channel image data) of `length` bytes at the reader's position.
std::expected<void, Error> parseLayerInfo(Reader& r, std::uint64_t length, Parsed& p) {
    const std::uint64_t start = r.tell();
    const std::uint64_t end = start + length;
    if (length > r.remaining()) {
        return std::unexpected(malformed("the layer information runs past the end of the file"));
    }
    const int count = std::abs(static_cast<int>(r.i16())); // negative: the first alpha channel is the merged transparency
    if (static_cast<std::size_t>(count) > kMaxPsdLayers) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Too many layers in the Photoshop file"});
    }
    const bool psb = p.doc.psb;
    std::vector<LayerRecord> records(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        LayerRecord& rec = records[static_cast<std::size_t>(i)];
        rec.layer.index = i;
        rec.layer.top = r.i32();
        rec.layer.left = r.i32();
        rec.layer.bottom = r.i32();
        rec.layer.right = r.i32();
        const int channels = r.u16();
        if (channels > 56) {
            return std::unexpected(malformed("a layer with too many channels"));
        }
        for (int c = 0; c < channels; ++c) {
            ChannelInfo ch;
            ch.id = r.i16();
            ch.length = r.length(psb);
            rec.channels.push_back(ch);
        }
        if (r.key() != "8BIM") {
            return std::unexpected(malformed("a layer's blend mode signature"));
        }
        rec.layer.blendMode = r.key();
        rec.ownOpacity = r.u8() / 255.0;
        rec.layer.clipped = r.u8() != 0;
        const std::uint8_t flags = r.u8();
        rec.ownVisible = (flags & 0x02) == 0; // bit 1 set = hidden (what Photoshop writes)
        r.u8();                               // filler
        const std::uint64_t extraLength = r.u32();
        const std::uint64_t extraStart = r.tell();
        const std::uint64_t extraEnd = extraStart + extraLength;
        if (!r.ok() || extraLength > r.remaining()) {
            return std::unexpected(malformed("a layer record runs past the end of the file"));
        }
        // Layer mask / adjustment layer data.
        const std::uint32_t maskLength = r.u32();
        const std::uint64_t maskStart = r.tell();
        if (maskLength >= 18) {
            rec.maskTop = r.i32();
            rec.maskLeft = r.i32();
            rec.maskBottom = r.i32();
            rec.maskRight = r.i32();
            rec.maskDefault = r.u8();
            const std::uint8_t maskFlags = r.u8();
            rec.maskDisabled = (maskFlags & 0x02) != 0;
            rec.hasMask = true;
        }
        r.seek(maskStart + maskLength);
        r.skip(r.u32()); // blending ranges
        // The Pascal name, padded to a multiple of 4 bytes (length byte included).
        const std::uint8_t nameLength = r.u8();
        rec.layer.name = latin1ToUtf8(r.bytes(nameLength));
        const std::uint64_t used = 1u + nameLength;
        r.skip((4 - used % 4) % 4);
        // Additional layer information.
        while (r.ok() && r.tell() + 12 <= extraEnd) {
            if (!r.atSignature()) {
                break;
            }
            r.key();
            const std::string key = r.key();
            const std::uint64_t blockLength = r.length(psb && wideKey(key));
            const std::uint64_t blockStart = r.tell();
            if (blockLength > extraEnd - std::min(extraEnd, blockStart)) {
                break; // a block running out of the record: ignore the rest
            }
            if (key == "luni" && blockLength >= 4) {
                const std::uint32_t chars = r.u32();
                if (chars <= (blockLength - 4) / 2) {
                    rec.layer.name = utf16beToUtf8(r, chars);
                }
            } else if ((key == "lsct" || key == "lsdk") && blockLength >= 4) {
                rec.section = static_cast<int>(std::min<std::uint32_t>(r.u32(), 3));
            }
            r.seek(blockStart + blockLength);
            for (int pad = 0; pad < 3 && r.tell() < extraEnd && !r.atSignature() && r.peek() == 0; ++pad) {
                r.skip(1);
            }
        }
        r.seek(extraEnd);
        if (!r.ok()) {
            return std::unexpected(malformed("a layer record runs past the end of the file"));
        }
        const std::int64_t w = static_cast<std::int64_t>(rec.layer.right) - rec.layer.left;
        const std::int64_t h = static_cast<std::int64_t>(rec.layer.bottom) - rec.layer.top;
        if (w < 0 || h < 0 || w > kMaxPsdSide || h > kMaxPsdSide || w * h > kMaxPsdPixels) {
            return std::unexpected(malformed("a layer's bounds"));
        }
    }
    // Channel image data, layer by layer, channel by channel.
    for (LayerRecord& rec : records) {
        for (ChannelInfo& ch : rec.channels) {
            ch.offset = r.tell();
            if (ch.length > r.remaining() || (ch.length > 0 && ch.length < 2)) {
                return std::unexpected(malformed("a layer's pixels run past the end of the file"));
            }
            r.skip(ch.length);
        }
    }
    if (r.tell() > end) {
        return std::unexpected(malformed("the layers' pixels run past their section"));
    }
    r.seek(end);
    p.records = std::move(records);
    return {};
}

/// Visibility, opacity and groups: each layer as its groups (the folders above it) show it.
void resolveGroups(Parsed& p) {
    struct Frame {
        bool visible = true;
        double opacity = 1.0;
        std::string name;
    };
    std::vector<Frame> stack{Frame{}};
    for (auto it = p.records.rbegin(); it != p.records.rend(); ++it) {
        LayerRecord& rec = *it;
        const Frame& parent = stack.back();
        if (rec.section == 1 || rec.section == 2) { // a folder: its layers follow (below it)
            rec.layer.group = true;
            rec.layer.visible = parent.visible && rec.ownVisible;
            rec.layer.opacity = parent.opacity * rec.ownOpacity;
            for (std::size_t i = 1; i < stack.size(); ++i) {
                rec.layer.groups.push_back(stack[i].name);
            }
            if (stack.size() < 64) {
                stack.push_back({rec.layer.visible, rec.layer.opacity, rec.layer.name});
            }
            continue;
        }
        if (rec.section == 3) { // a group's end marker
            rec.layer.group = true;
            rec.layer.visible = false;
            if (stack.size() > 1) {
                stack.pop_back();
            }
            continue;
        }
        rec.layer.visible = parent.visible && rec.ownVisible;
        rec.layer.opacity = parent.opacity * rec.ownOpacity;
        for (std::size_t i = 1; i < stack.size(); ++i) {
            rec.layer.groups.push_back(stack[i].name);
        }
    }
    p.doc.layers.clear();
    for (const LayerRecord& rec : p.records) {
        p.doc.layers.push_back(rec.layer);
    }
}

std::expected<Parsed, Error> parse(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    if (bytes.size() < 26 || r.key() != "8BPS") {
        return std::unexpected(Error{ErrorCode::Unsupported, "Not a Photoshop file"});
    }
    Parsed p;
    const int version = r.u16();
    if (version != 1 && version != 2) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Unknown Photoshop file version"});
    }
    p.doc.psb = version == 2;
    r.skip(6);
    p.channels = r.u16();
    p.doc.height = static_cast<int>(std::min<std::uint32_t>(r.u32(), std::numeric_limits<int>::max()));
    p.doc.width = static_cast<int>(std::min<std::uint32_t>(r.u32(), std::numeric_limits<int>::max()));
    p.doc.depth = r.u16();
    const int mode = r.u16();
    const int maxSide = p.doc.psb ? kMaxPsdSide : 30'000;
    if (p.channels < 1 || p.channels > 56 || p.doc.width < 1 || p.doc.height < 1 || p.doc.width > maxSide ||
        p.doc.height > maxSide ||
        static_cast<std::int64_t>(p.doc.width) * p.doc.height > kMaxPsdPixels) {
        return std::unexpected(malformed("the header"));
    }
    if (p.doc.depth != 8 && p.doc.depth != 16 && p.doc.depth != 32) {
        return std::unexpected(Error{ErrorCode::Unsupported,
                                     std::to_string(p.doc.depth) + "-bit Photoshop files are not supported: save it as "
                                                                   "8, 16 or 32 bits per channel"});
    }
    if (mode != 1 && mode != 3 && mode != 4) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Only RGB, CMYK and grayscale Photoshop files can be "
                                                             "imported: convert it to RGB first"});
    }
    p.doc.grayscale = mode == 1;
    p.doc.cmyk = mode == 4;
    r.skip(r.u32()); // colour mode data
    r.skip(r.u32()); // image resources
    if (!r.ok()) {
        return std::unexpected(malformed("the sections before the layers"));
    }
    const std::uint64_t layerAndMask = r.length(p.doc.psb);
    const std::uint64_t sectionStart = r.tell();
    if (layerAndMask > r.remaining()) {
        return std::unexpected(malformed("the layer and mask section runs past the end of the file"));
    }
    const std::uint64_t sectionEnd = sectionStart + layerAndMask;
    if (layerAndMask > 0) {
        const std::uint64_t infoLength = r.length(p.doc.psb);
        const std::uint64_t infoStart = r.tell();
        if (infoLength > 0) {
            if (auto parsed = parseLayerInfo(r, infoLength, p); !parsed) {
                return std::unexpected(parsed.error());
            }
        }
        r.seek(infoStart + infoLength);
        // The global layer mask info, then the global additional layer information ("Lr16" / "Lr32": 16- and 32-bit
        // documents' layers).
        if (r.tell() + 4 <= sectionEnd) {
            r.skip(r.u32());
        }
        while (r.ok() && r.tell() + 12 <= sectionEnd) {
            for (int pad = 0; pad < 3 && !r.atSignature() && r.tell() < sectionEnd && r.peek() == 0; ++pad) {
                r.skip(1);
            }
            if (!r.atSignature()) {
                break;
            }
            r.key();
            const std::string key = r.key();
            const std::uint64_t blockLength = r.length(p.doc.psb && wideKey(key));
            const std::uint64_t blockStart = r.tell();
            if (blockLength > sectionEnd - std::min(sectionEnd, blockStart)) {
                break;
            }
            if ((key == "Lr16" || key == "Lr32") && p.records.empty() && blockLength > 0) {
                if (auto parsed = parseLayerInfo(r, blockLength, p); !parsed) {
                    return std::unexpected(parsed.error());
                }
            }
            r.seek(blockStart + blockLength);
        }
        r.seek(sectionEnd);
    }
    if (!r.ok()) {
        return std::unexpected(malformed("the layer and mask section"));
    }
    p.mergedOffset = r.tell();
    resolveGroups(p);
    return p;
}

std::expected<std::vector<std::uint8_t>, Error> readFile(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::NotFound, "File not found: " + pathToUtf8(file)});
    }
    if (size > (std::uint64_t{4} << 30)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The Photoshop file is too large to import"});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + pathToUtf8(file)});
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in && !in.eof()) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + pathToUtf8(file)});
    }
    return bytes;
}

// ---- Pixels ------------------------------------------------------------------------------------------------

/// PackBits: `in` to exactly `want` bytes of `out`. False when it does not decode to that.
bool unpackBits(std::span<const std::uint8_t> in, std::uint8_t* out, std::size_t want) {
    std::size_t i = 0;
    std::size_t o = 0;
    while (o < want && i < in.size()) {
        const int n = static_cast<std::int8_t>(in[i++]);
        if (n >= 0) {
            const std::size_t count = static_cast<std::size_t>(n) + 1;
            if (i + count > in.size() || o + count > want) {
                return false;
            }
            std::memcpy(out + o, in.data() + i, count);
            i += count;
            o += count;
        } else if (n != -128) {
            const std::size_t count = static_cast<std::size_t>(1 - n);
            if (i >= in.size() || o + count > want) {
                return false;
            }
            std::memset(out + o, in[i++], count);
            o += count;
        }
    }
    return o == want;
}

bool inflateAll(std::span<const std::uint8_t> in, std::uint8_t* out, std::size_t want) {
    z_stream s{};
    if (inflateInit(&s) != Z_OK) {
        return false;
    }
    s.next_in = const_cast<Bytef*>(in.data());
    s.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), std::numeric_limits<uInt>::max()));
    s.next_out = out;
    s.avail_out = static_cast<uInt>(std::min<std::size_t>(want, std::numeric_limits<uInt>::max()));
    const int r = inflate(&s, Z_FINISH);
    const std::size_t produced = want - s.avail_out;
    inflateEnd(&s);
    return (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) && produced == want;
}

/// One sample of `bps` bytes, big-endian, scaled to 16 bits: 8 bits times 257, 16 bits as is, a 32-bit float (linear
/// light) clamped to [0, 1].
std::uint16_t sampleAt(const std::uint8_t* p, std::size_t bps) {
    if (bps == 1) {
        return static_cast<std::uint16_t>(p[0] * 257);
    }
    if (bps == 2) {
        return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }
    const std::uint32_t bits = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
    const float f = std::bit_cast<float>(bits);
    if (!(f > 0.0f)) { // negative, zero or NaN
        return 0;
    }
    return f >= 1.0f ? std::uint16_t{65535} : static_cast<std::uint16_t>(std::lround(f * 65535.0f));
}

std::size_t bytesPerSample(int depth) {
    return depth == 32 ? 4 : depth == 16 ? 2 : 1;
}

/// One layer channel's samples (scaled to 16 bits): `length` bytes at `offset` (the compression word first), of
/// `width` x `height` at `depth`.
std::expected<std::vector<std::uint16_t>, Error> decodePlane(Reader& r, std::uint64_t offset, std::uint64_t length,
                                                             int width, int height, int depth, bool psb) {
    const std::size_t bps = bytesPerSample(depth);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * bps;
    const std::size_t total = rowBytes * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> raw(total);
    r.seek(offset);
    const int compression = r.u16();
    const std::uint64_t dataLength = length >= 2 ? length - 2 : 0;
    const std::span<const std::uint8_t> data = r.bytes(dataLength);
    if (!r.ok()) {
        return std::unexpected(malformed("a channel runs past the end of the file"));
    }
    if (total == 0) {
        return std::vector<std::uint16_t>{};
    }
    switch (compression) {
    case 0:
        if (data.size() < total) {
            return std::unexpected(malformed("a raw channel is too short"));
        }
        std::memcpy(raw.data(), data.data(), total);
        break;
    case 1: {
        const std::size_t countBytes = psb ? 4 : 2;
        const std::size_t table = countBytes * static_cast<std::size_t>(height);
        if (data.size() < table) {
            return std::unexpected(malformed("an RLE channel's row table"));
        }
        std::size_t at = table;
        for (int y = 0; y < height; ++y) {
            std::size_t n = 0;
            for (std::size_t k = 0; k < countBytes; ++k) {
                n = (n << 8) | data[static_cast<std::size_t>(y) * countBytes + k];
            }
            if (n > data.size() - at || !unpackBits(data.subspan(at, n), raw.data() + rowBytes * y, rowBytes)) {
                return std::unexpected(malformed("an RLE row"));
            }
            at += n;
        }
        break;
    }
    case 2:
    case 3:
        if (!inflateAll(data, raw.data(), total)) {
            return std::unexpected(malformed("a ZIP channel"));
        }
        if (compression == 3) { // prediction: each sample is the difference to the one on its left
            std::vector<std::uint8_t> planar(bps == 4 ? rowBytes : 0);
            for (int y = 0; y < height; ++y) {
                std::uint8_t* row = raw.data() + rowBytes * y;
                if (bps == 1) {
                    for (int x = 1; x < width; ++x) {
                        row[x] = static_cast<std::uint8_t>(row[x] + row[x - 1]);
                    }
                } else if (bps == 4) {
                    // 32 bits: each byte is the difference to the one before it in the row, and the row holds the
                    // floats' bytes a plane at a time (every first byte, then every second byte, ...).
                    for (std::size_t i = 1; i < rowBytes; ++i) {
                        row[i] = static_cast<std::uint8_t>(row[i] + row[i - 1]);
                    }
                    for (int x = 0; x < width; ++x) {
                        for (std::size_t k = 0; k < 4; ++k) {
                            planar[4 * static_cast<std::size_t>(x) + k] = row[k * static_cast<std::size_t>(width) + x];
                        }
                    }
                    std::memcpy(row, planar.data(), rowBytes);
                } else {
                    std::uint16_t prev = static_cast<std::uint16_t>((row[0] << 8) | row[1]);
                    for (int x = 1; x < width; ++x) {
                        const std::uint16_t d = static_cast<std::uint16_t>((row[2 * x] << 8) | row[2 * x + 1]);
                        prev = static_cast<std::uint16_t>(prev + d);
                        row[2 * x] = static_cast<std::uint8_t>(prev >> 8);
                        row[2 * x + 1] = static_cast<std::uint8_t>(prev & 0xFF);
                    }
                }
            }
        }
        break;
    default:
        return std::unexpected(malformed("an unknown channel compression"));
    }
    std::vector<std::uint16_t> out(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = sampleAt(raw.data() + i * bps, bps);
    }
    return out;
}

/// The colour channels a document stores: grayscale 1, RGB 3, CMYK 4.
int colorChannelsOf(const PsdDocument& doc) {
    return doc.grayscale ? 1 : doc.cmyk ? 4 : 3;
}

/// RGB from a document's colour planes at sample `i`. CMYK is stored inverted (65535 = no ink): each colour is its ink's
/// complement times black's, the conversion without a colour profile. A missing plane (empty) reads as black (no ink
/// for CMYK).
std::array<std::uint16_t, 3> rgbAt(const PsdDocument& doc, const std::array<std::vector<std::uint16_t>, 4>& planes,
                                   std::size_t i) {
    const auto at = [&](std::size_t c, std::uint16_t missing) {
        return planes[c].empty() ? missing : planes[c][i];
    };
    if (doc.grayscale) {
        const std::uint16_t v = at(0, 0);
        return {v, v, v};
    }
    if (doc.cmyk) {
        const std::uint32_t k = at(3, 65535);
        const auto ink = [&](std::size_t c) {
            return static_cast<std::uint16_t>((std::uint32_t{at(c, 65535)} * k + 32767) / 65535);
        };
        return {ink(0), ink(1), ink(2)};
    }
    return {at(0, 0), at(1, 0), at(2, 0)};
}

/// 32-bit documents are linear light: their colour is written out with the sRGB curve.
std::uint16_t encodeLinear(double v) {
    v = std::clamp(v, 0.0, 1.0);
    const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint16_t>(std::lround(std::clamp(e, 0.0, 1.0) * 65535.0));
}

constexpr std::int64_t kMaxCanvas = 64'000'000;

/// The merged picture of the image data section (a flattened file, or a file without layers).
std::expected<PsdImage, Error> mergedImage(Reader& r, const Parsed& p) {
    const int w = p.doc.width;
    const int h = p.doc.height;
    if (static_cast<std::int64_t>(w) * h > kMaxCanvas) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The Photoshop file is too large to import as a picture"});
    }
    const std::size_t bps = bytesPerSample(p.doc.depth);
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    const int colorChannels = colorChannelsOf(p.doc);
    const int used = std::min(p.channels, colorChannels + 1);
    r.seek(p.mergedOffset);
    const int compression = r.u16();
    std::vector<std::vector<std::uint16_t>> planes(static_cast<std::size_t>(used));
    if (compression == 1) {
        const std::size_t countBytes = p.doc.psb ? 4 : 2;
        std::vector<std::size_t> counts(static_cast<std::size_t>(p.channels) * h);
        for (std::size_t& c : counts) {
            c = static_cast<std::size_t>(r.be(static_cast<int>(countBytes)));
        }
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(w) * bps);
        for (int c = 0; c < p.channels && r.ok(); ++c) {
            if (c < used) {
                planes[static_cast<std::size_t>(c)].resize(plane);
            }
            for (int y = 0; y < h; ++y) {
                const std::size_t n = counts[static_cast<std::size_t>(c) * h + y];
                const auto row = r.bytes(n);
                if (!r.ok() || !unpackBits(row, raw.data(), raw.size())) {
                    return std::unexpected(malformed("the merged picture"));
                }
                if (c < used) {
                    auto& dst = planes[static_cast<std::size_t>(c)];
                    for (int x = 0; x < w; ++x) {
                        dst[static_cast<std::size_t>(y) * w + x] = sampleAt(raw.data() + static_cast<std::size_t>(x) * bps, bps);
                    }
                }
            }
        }
    } else if (compression == 0) {
        for (int c = 0; c < used; ++c) {
            const auto data = r.bytes(plane * bps);
            if (!r.ok()) {
                return std::unexpected(malformed("the merged picture"));
            }
            auto& dst = planes[static_cast<std::size_t>(c)];
            dst.resize(plane);
            for (std::size_t i = 0; i < plane; ++i) {
                dst[i] = sampleAt(data.data() + i * bps, bps);
            }
        }
    } else {
        return std::unexpected(Error{ErrorCode::Unsupported, "The merged picture's compression is not supported"});
    }
    std::array<std::vector<std::uint16_t>, 4> color;
    for (int c = 0; c < std::min(used, colorChannels); ++c) {
        color[static_cast<std::size_t>(c)] = std::move(planes[static_cast<std::size_t>(c)]);
    }
    PsdImage image;
    image.width = w;
    image.height = h;
    image.depth = p.doc.depth;
    image.rgba.resize(plane * 4);
    for (std::size_t i = 0; i < plane; ++i) {
        const auto rgb = rgbAt(p.doc, color, i);
        for (std::size_t c = 0; c < 3; ++c) {
            image.rgba[4 * i + c] = p.doc.depth == 32 ? encodeLinear(rgb[c] / 65535.0) : rgb[c];
        }
        image.rgba[4 * i + 3] = used > colorChannels ? planes[static_cast<std::size_t>(colorChannels)][i] : 65535;
    }
    return image;
}

// ---- Compositing -------------------------------------------------------------------------------------------

enum class Blend {
    Normal, // and the modes Wabcut does not reproduce (Dissolve)
    Multiply,
    Screen,
    Overlay,
    HardLight,
    SoftLight,
    Darken,
    Lighten,
    Difference,
    Exclusion,
    LinearDodge,
    LinearBurn,
    ColorDodge,
    ColorBurn,
    Subtract,
    Divide,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    // The non-separable modes: the colour as a whole, not channel by channel.
    DarkerColor,
    LighterColor,
    Hue,
    Saturation,
    Color,
    Luminosity,
};

Blend blendOf(std::string_view key) {
    static constexpr std::array<std::pair<std::string_view, Blend>, 25> kModes{{
        {"mul ", Blend::Multiply},     {"scrn", Blend::Screen},       {"over", Blend::Overlay},
        {"hLit", Blend::HardLight},    {"sLit", Blend::SoftLight},    {"dark", Blend::Darken},
        {"lite", Blend::Lighten},      {"diff", Blend::Difference},   {"smud", Blend::Exclusion},
        {"lddg", Blend::LinearDodge},  {"lbrn", Blend::LinearBurn},   {"div ", Blend::ColorDodge},
        {"idiv", Blend::ColorBurn},    {"fsub", Blend::Subtract},     {"fdiv", Blend::Divide},
        {"vLit", Blend::VividLight},   {"lLit", Blend::LinearLight},  {"pLit", Blend::PinLight},
        {"hMix", Blend::HardMix},      {"dkCl", Blend::DarkerColor},  {"lgCl", Blend::LighterColor},
        {"hue ", Blend::Hue},          {"sat ", Blend::Saturation},   {"colr", Blend::Color},
        {"lum ", Blend::Luminosity},
    }};
    for (const auto& [k, mode] : kModes) {
        if (k == key) {
            return mode;
        }
    }
    return Blend::Normal;
}

double colorDodge(double b, double s) {
    return b == 0.0 ? 0.0 : s >= 1.0 ? 1.0 : std::min(1.0, b / (1.0 - s));
}

double colorBurn(double b, double s) {
    return b >= 1.0 ? 1.0 : s <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - b) / s);
}

double blendChannel(Blend mode, double b, double s) {
    switch (mode) {
    case Blend::Multiply:
        return b * s;
    case Blend::Screen:
        return b + s - b * s;
    case Blend::Overlay: // hard light with the layers swapped
        return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    case Blend::HardLight:
        return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    case Blend::SoftLight: { // the W3C formula
        if (s <= 0.5) {
            return b - (1.0 - 2.0 * s) * b * (1.0 - b);
        }
        const double d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : std::sqrt(b);
        return b + (2.0 * s - 1.0) * (d - b);
    }
    case Blend::Darken:
        return std::min(b, s);
    case Blend::Lighten:
        return std::max(b, s);
    case Blend::Difference:
        return std::abs(b - s);
    case Blend::Exclusion:
        return b + s - 2.0 * b * s;
    case Blend::LinearDodge: // add
        return std::min(1.0, b + s);
    case Blend::LinearBurn:
        return std::max(0.0, b + s - 1.0);
    case Blend::ColorDodge:
        return colorDodge(b, s);
    case Blend::ColorBurn:
        return colorBurn(b, s);
    case Blend::Subtract:
        return std::max(0.0, b - s);
    case Blend::Divide:
        return s <= 0.0 ? 1.0 : std::min(1.0, b / s);
    case Blend::VividLight: // colour burn below the middle, colour dodge above it
        return s <= 0.5 ? colorBurn(b, 2.0 * s) : colorDodge(b, 2.0 * (s - 0.5));
    case Blend::LinearLight:
        return std::clamp(b + 2.0 * s - 1.0, 0.0, 1.0);
    case Blend::PinLight:
        return s <= 0.5 ? std::min(b, 2.0 * s) : std::max(b, 2.0 * s - 1.0);
    case Blend::HardMix:
        return b + s >= 1.0 ? 1.0 : 0.0;
    default:
        return s;
    }
}

using Rgb = std::array<double, 3>;

// The non-separable modes as the W3C's Compositing and Blending spells out Photoshop's: luminosity is
// 0.3 R + 0.59 G + 0.11 B, saturation the largest channel minus the smallest.
double lum(const Rgb& c) {
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}

Rgb clipColor(Rgb c) {
    const double l = lum(c);
    const double n = std::min({c[0], c[1], c[2]});
    const double x = std::max({c[0], c[1], c[2]});
    for (double& v : c) {
        if (n < 0.0 && l - n > 0.0) {
            v = l + (v - l) * l / (l - n);
        }
        if (x > 1.0 && x - l > 0.0) {
            v = l + (v - l) * (1.0 - l) / (x - l);
        }
    }
    return c;
}

Rgb setLum(Rgb c, double l) {
    const double d = l - lum(c);
    for (double& v : c) {
        v += d;
    }
    return clipColor(c);
}

double sat(const Rgb& c) {
    return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
}

Rgb setSat(Rgb c, double s) {
    std::array<std::size_t, 3> order{0, 1, 2}; // smallest, middle, largest
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return c[a] < c[b]; });
    double& cmin = c[order[0]];
    double& cmid = c[order[1]];
    double& cmax = c[order[2]];
    if (cmax > cmin) {
        cmid = (cmid - cmin) * s / (cmax - cmin);
        cmax = s;
    } else {
        cmid = 0.0;
        cmax = 0.0;
    }
    cmin = 0.0;
    return c;
}

/// The layer's colour `s` over the backdrop `b`, both straight colour in [0, 1].
Rgb blendColor(Blend mode, const Rgb& b, const Rgb& s) {
    switch (mode) {
    case Blend::Hue:
        return setLum(setSat(s, sat(b)), lum(b));
    case Blend::Saturation:
        return setLum(setSat(b, sat(s)), lum(b));
    case Blend::Color:
        return setLum(s, lum(b));
    case Blend::Luminosity:
        return setLum(b, lum(s));
    case Blend::DarkerColor: // the colour with the smaller total of its channels
        return s[0] + s[1] + s[2] < b[0] + b[1] + b[2] ? s : b;
    case Blend::LighterColor:
        return s[0] + s[1] + s[2] > b[0] + b[1] + b[2] ? s : b;
    default:
        return {blendChannel(mode, b[0], s[0]), blendChannel(mode, b[1], s[1]), blendChannel(mode, b[2], s[2])};
    }
}

struct Canvas {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<double> rgba; ///< straight colour, [0, 1]
};

/// A layer's colour planes and coverage (alpha x mask x opacity, in [0, 1]) over its own bounds.
struct LayerPixels {
    std::array<std::vector<std::uint16_t>, 3> color;
    std::vector<double> coverage;
};

std::expected<LayerPixels, Error> layerPixels(Reader& r, const Parsed& p, const LayerRecord& rec) {
    const int w = rec.layer.width();
    const int h = rec.layer.height();
    LayerPixels out;
    out.coverage.assign(static_cast<std::size_t>(w) * h, rec.layer.opacity);
    const int colorChannels = colorChannelsOf(p.doc);
    std::array<std::vector<std::uint16_t>, 4> color; // as stored: gray, RGB or CMYK
    for (const ChannelInfo& ch : rec.channels) {
        const bool mask = ch.id == -2;
        if (!mask && (ch.id < -1 || ch.id >= colorChannels)) {
            continue; // a real user mask (-3) or extra channels: not reproduced
        }
        const int cw = mask ? rec.maskRight - rec.maskLeft : w;
        const int chh = mask ? rec.maskBottom - rec.maskTop : h;
        if (cw < 0 || chh < 0 || static_cast<std::int64_t>(cw) * chh > kMaxPsdPixels) {
            return std::unexpected(malformed("a layer mask's bounds"));
        }
        auto plane = decodePlane(r, ch.offset, ch.length, cw, chh, p.doc.depth, p.doc.psb);
        if (!plane) {
            return std::unexpected(plane.error());
        }
        if (ch.id >= 0) {
            color[static_cast<std::size_t>(ch.id)] = std::move(*plane);
        } else if (ch.id == -1) {
            for (std::size_t i = 0; i < out.coverage.size(); ++i) {
                out.coverage[i] *= (*plane)[i] / 65535.0;
            }
        } else if (rec.hasMask && !rec.maskDisabled) {
            const double outside = rec.maskDefault / 255.0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int dx = rec.layer.left + x - rec.maskLeft;
                    const int dy = rec.layer.top + y - rec.maskTop;
                    const double m = dx >= 0 && dy >= 0 && dx < cw && dy < chh
                                         ? (*plane)[static_cast<std::size_t>(dy) * cw + dx] / 65535.0
                                         : outside;
                    out.coverage[static_cast<std::size_t>(y) * w + x] *= m;
                }
            }
        }
    }
    const std::size_t n = static_cast<std::size_t>(w) * h;
    for (std::vector<std::uint16_t>& plane : color) {
        if (!plane.empty() && plane.size() != n) {
            return std::unexpected(malformed("a layer channel's size"));
        }
    }
    if (!p.doc.grayscale && !p.doc.cmyk) {
        for (std::size_t c = 0; c < 3; ++c) {
            out.color[c] = color[c].empty() ? std::vector<std::uint16_t>(n, 0) : std::move(color[c]);
        }
        return out;
    }
    // Grayscale (one colour channel) and CMYK to RGB.
    for (std::vector<std::uint16_t>& plane : out.color) {
        plane.resize(n);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto rgb = rgbAt(p.doc, color, i);
        for (std::size_t c = 0; c < 3; ++c) {
            out.color[c][i] = rgb[c];
        }
    }
    return out;
}

std::expected<PsdImage, Error> composite(std::span<const std::uint8_t> bytes, std::span<const int> wanted,
                                         bool documentSize) {
    auto parsed = parse(bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const Parsed& p = *parsed;
    Reader r(bytes);
    if (p.records.empty()) {
        if (!wanted.empty()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The Photoshop file has no layers"});
        }
        return mergedImage(r, p); // a flattened file
    }
    std::vector<int> chosen;
    if (wanted.empty()) {
        for (const LayerRecord& rec : p.records) {
            if (rec.layer.visible && rec.layer.hasPixels()) {
                chosen.push_back(rec.layer.index);
            }
        }
    } else {
        for (const int i : wanted) {
            if (i < 0 || i >= static_cast<int>(p.records.size())) {
                return std::unexpected(Error{ErrorCode::InvalidArgument, "The Photoshop file has no layer " + std::to_string(i)});
            }
            const PsdLayer& l = p.records[static_cast<std::size_t>(i)].layer;
            if (l.visible && l.hasPixels()) {
                chosen.push_back(i);
            }
        }
        std::ranges::sort(chosen);
        chosen.erase(std::ranges::unique(chosen).begin(), chosen.end());
    }
    Canvas canvas;
    if (documentSize) {
        canvas.width = p.doc.width;
        canvas.height = p.doc.height;
    } else {
        int l = std::numeric_limits<int>::max(), t = l, rr = std::numeric_limits<int>::min(), b = rr;
        for (const int i : chosen) {
            const PsdLayer& layer = p.records[static_cast<std::size_t>(i)].layer;
            l = std::min(l, layer.left);
            t = std::min(t, layer.top);
            rr = std::max(rr, layer.right);
            b = std::max(b, layer.bottom);
        }
        if (chosen.empty()) {
            l = t = 0;
            rr = b = 1;
        }
        canvas.left = l;
        canvas.top = t;
        canvas.width = rr - l;
        canvas.height = b - t;
    }
    if (canvas.width <= 0 || canvas.height <= 0 || static_cast<std::int64_t>(canvas.width) * canvas.height > kMaxCanvas) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The layers are too large to import as a picture"});
    }
    const std::size_t pixels = static_cast<std::size_t>(canvas.width) * canvas.height;
    canvas.rgba.assign(pixels * 4, 0.0);
    // Clipping masks: a clipped layer shows only where its base (the nearest unclipped layer below it) is.
    std::optional<int> baseIndex;
    std::vector<double> baseCoverage; // the base's coverage on the canvas
    const auto baseOf = [&](int index) -> std::optional<int> {
        for (int k = index - 1; k >= 0; --k) {
            const LayerRecord& rec = p.records[static_cast<std::size_t>(k)];
            if (!rec.layer.group && !rec.layer.clipped) {
                return k;
            }
        }
        return std::nullopt;
    };
    for (const int index : chosen) {
        const LayerRecord& rec = p.records[static_cast<std::size_t>(index)];
        auto px = layerPixels(r, p, rec);
        if (!px) {
            return std::unexpected(px.error());
        }
        const int w = rec.layer.width();
        const int h = rec.layer.height();
        const Blend mode = blendOf(rec.layer.blendMode);
        const std::optional<int> base = rec.layer.clipped ? baseOf(index) : std::nullopt;
        if (rec.layer.clipped && base && base != baseIndex) {
            baseIndex = base;
            baseCoverage.assign(pixels, 0.0);
            const LayerRecord& b = p.records[static_cast<std::size_t>(*base)];
            if (b.layer.visible && b.layer.hasPixels()) {
                auto bp = layerPixels(r, p, b);
                if (!bp) {
                    return std::unexpected(bp.error());
                }
                for (int y = 0; y < b.layer.height(); ++y) {
                    const int cy = b.layer.top + y - canvas.top;
                    for (int x = 0; x < b.layer.width(); ++x) {
                        const int cx = b.layer.left + x - canvas.left;
                        if (cx >= 0 && cy >= 0 && cx < canvas.width && cy < canvas.height) {
                            // The clipped layers take on the base's transparency and opacity (Photoshop).
                            baseCoverage[static_cast<std::size_t>(cy) * canvas.width + cx] =
                                bp->coverage[static_cast<std::size_t>(y) * b.layer.width() + x];
                        }
                    }
                }
            }
        }
        for (int y = 0; y < h; ++y) {
            const int cy = rec.layer.top + y - canvas.top;
            if (cy < 0 || cy >= canvas.height) {
                continue;
            }
            for (int x = 0; x < w; ++x) {
                const int cx = rec.layer.left + x - canvas.left;
                if (cx < 0 || cx >= canvas.width) {
                    continue;
                }
                const std::size_t si = static_cast<std::size_t>(y) * w + x;
                const std::size_t di = static_cast<std::size_t>(cy) * canvas.width + cx;
                double as = std::clamp(px->coverage[si], 0.0, 1.0);
                if (rec.layer.clipped) {
                    as *= base ? std::clamp(baseCoverage[di], 0.0, 1.0) : 0.0;
                }
                if (as <= 0.0) {
                    continue;
                }
                double* d = &canvas.rgba[di * 4];
                const double ab = d[3];
                const double ao = as + ab * (1.0 - as);
                const Rgb cs{px->color[0][si] / 65535.0, px->color[1][si] / 65535.0, px->color[2][si] / 65535.0};
                const Rgb cb{d[0], d[1], d[2]};
                const Rgb blended = mode == Blend::Normal ? cs : blendColor(mode, cb, cs);
                for (std::size_t c = 0; c < 3; ++c) {
                    const double mixed = (1.0 - ab) * cs[c] + ab * blended[c];
                    const double premult = as * mixed + (1.0 - as) * ab * cb[c];
                    d[c] = ao > 0.0 ? premult / ao : 0.0;
                }
                d[3] = ao;
            }
        }
    }
    PsdImage image;
    image.width = canvas.width;
    image.height = canvas.height;
    image.left = canvas.left;
    image.top = canvas.top;
    image.depth = p.doc.depth;
    image.rgba.resize(pixels * 4);
    const bool linear = p.doc.depth == 32;
    for (std::size_t i = 0; i < pixels * 4; ++i) {
        image.rgba[i] = linear && i % 4 != 3
                            ? encodeLinear(canvas.rgba[i])
                            : static_cast<std::uint16_t>(std::lround(std::clamp(canvas.rgba[i], 0.0, 1.0) * 65535.0));
    }
    return image;
}

} // namespace

std::expected<PsdDocument, Error> readPsdDocument(std::span<const std::uint8_t> bytes) {
    auto parsed = parse(bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::move(parsed->doc);
}

std::expected<PsdDocument, Error> readPsdDocument(const std::filesystem::path& file) {
    auto bytes = readFile(file);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return readPsdDocument(std::span<const std::uint8_t>(*bytes));
}

std::expected<PsdImage, Error> compositePsd(std::span<const std::uint8_t> bytes, std::span<const int> layers,
                                            bool documentSize) {
    return composite(bytes, layers, documentSize);
}

std::expected<PsdImage, Error> compositePsd(const std::filesystem::path& file, std::span<const int> layers,
                                            bool documentSize) {
    auto bytes = readFile(file);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return composite(*bytes, layers, documentSize);
}

std::string toPam(const PsdImage& image) {
    const bool wide = image.depth >= 16;
    std::string out = "P7\nWIDTH " + std::to_string(image.width) + "\nHEIGHT " + std::to_string(image.height) +
                      "\nDEPTH 4\nMAXVAL " + (wide ? std::string("65535") : std::string("255")) +
                      "\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    const std::size_t header = out.size();
    out.resize(header + image.rgba.size() * (wide ? 2 : 1));
    char* p = out.data() + header;
    for (const std::uint16_t v : image.rgba) {
        if (wide) {
            *p++ = static_cast<char>(v >> 8);
            *p++ = static_cast<char>(v & 0xFF);
        } else {
            *p++ = static_cast<char>((v + 128) / 257);
        }
    }
    return out;
}

} // namespace wabcut::media
