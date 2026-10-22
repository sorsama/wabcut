#pragma once

// Writes small layered Photoshop files for the PSD tests (0.2, media/Psd.h): RGB or CMYK documents of 8, 16 or 32 bits
// with layers (bounds, pixels, opacity, visibility, blend mode, clipping, groups, a user mask, a Unicode name), their
// channels raw, PackBits RLE or ZIP with prediction, 16- and 32-bit layers in the "Lr16" / "Lr32" block as Photoshop
// stores them, PSD or PSB. CMYK documents get the inks that make the layers' RGB (stored inverted, as Photoshop does);
// 32-bit ones get the values as linear floats. Only what the reader needs; not a general writer.

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::media::test {

struct TestPsdLayer {
    std::string name = "Layer";
    int left = 0;
    int top = 0;
    int width = 1;
    int height = 1;
    /// Straight RGBA at the layer's pixel (x, y), 16-bit values (8-bit documents keep the top byte).
    std::function<std::array<std::uint16_t, 4>(int, int)> rgba = [](int, int) {
        return std::array<std::uint16_t, 4>{65535, 65535, 65535, 65535};
    };
    /// 32-bit documents: linear RGBA (above 1 for over-bright light); when unset, `rgba` / 65535.
    std::function<std::array<float, 4>(int, int)> rgbaFloat;
    std::uint8_t opacity = 255;
    bool hidden = false;
    std::string blend = "norm";
    bool clipped = false;
    int section = 0; ///< "lsct": 1 a group folder, 3 a group end marker
    /// A user mask: its rectangle in document pixels and its value there (0..255); the rest of the layer gets 0.
    std::optional<std::array<int, 4>> mask; ///< left, top, right, bottom
    std::uint8_t maskValue = 255;
};

enum class TestPsdCompression : std::uint16_t { Raw = 0, Rle = 1, Zip = 2, ZipPrediction = 3 };

enum class TestPsdMode : std::uint16_t { Rgb = 3, Cmyk = 4 };

namespace psdwriter {

inline void be(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

inline void tag(std::vector<std::uint8_t>& out, const char* four) {
    out.insert(out.end(), four, four + 4);
}

/// PackBits with literal runs only (valid, not small).
inline std::vector<std::uint8_t> packBits(const std::uint8_t* row, std::size_t n) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < n;) {
        const std::size_t run = std::min<std::size_t>(128, n - i);
        out.push_back(static_cast<std::uint8_t>(run - 1));
        out.insert(out.end(), row + i, row + i + run);
        i += run;
    }
    return out;
}

/// The samples of `plane` (values in [0, 1]; 32-bit ones may be above) as the file stores them at `depth`.
inline std::vector<std::uint8_t> samples(const std::vector<double>& plane, int depth) {
    std::vector<std::uint8_t> raw;
    for (const double v : plane) {
        if (depth == 32) {
            be(raw, std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
        } else if (depth == 16) {
            be(raw, static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0)), 2);
        } else {
            raw.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
        }
    }
    return raw;
}

/// One channel: compression word then its data, for `plane` of w x h samples (values in [0, 1]).
inline std::vector<std::uint8_t> channelData(const std::vector<double>& plane, int w, int h, int depth,
                                             TestPsdCompression compression, bool psb) {
    const std::size_t bps = depth == 32 ? 4 : depth == 16 ? 2 : 1;
    std::vector<std::uint8_t> raw = samples(plane, depth);
    std::vector<std::uint8_t> out;
    be(out, static_cast<std::uint16_t>(compression), 2);
    if (w == 0 || h == 0) {
        return out;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(w) * bps;
    switch (compression) {
    case TestPsdCompression::Raw:
        out.insert(out.end(), raw.begin(), raw.end());
        break;
    case TestPsdCompression::Rle: {
        std::vector<std::vector<std::uint8_t>> rows;
        for (int y = 0; y < h; ++y) {
            rows.push_back(packBits(raw.data() + rowBytes * y, rowBytes));
            be(out, rows.back().size(), psb ? 4 : 2);
        }
        for (const auto& r : rows) {
            out.insert(out.end(), r.begin(), r.end());
        }
        break;
    }
    case TestPsdCompression::Zip:
    case TestPsdCompression::ZipPrediction: {
        if (compression == TestPsdCompression::ZipPrediction) {
            for (int y = 0; y < h; ++y) {
                std::uint8_t* row = raw.data() + rowBytes * y;
                if (bps == 1) {
                    for (int x = w - 1; x > 0; --x) {
                        row[x] = static_cast<std::uint8_t>(row[x] - row[x - 1]);
                    }
                } else if (bps == 4) { // the floats' bytes a plane at a time, then each byte less the one before it
                    std::vector<std::uint8_t> planar(rowBytes);
                    for (int x = 0; x < w; ++x) {
                        for (std::size_t k = 0; k < 4; ++k) {
                            planar[k * static_cast<std::size_t>(w) + x] = row[4 * static_cast<std::size_t>(x) + k];
                        }
                    }
                    for (std::size_t i = rowBytes - 1; i > 0; --i) {
                        planar[i] = static_cast<std::uint8_t>(planar[i] - planar[i - 1]);
                    }
                    std::copy(planar.begin(), planar.end(), row);
                } else {
                    for (int x = w - 1; x > 0; --x) {
                        const std::uint16_t cur = static_cast<std::uint16_t>((row[2 * x] << 8) | row[2 * x + 1]);
                        const std::uint16_t prev = static_cast<std::uint16_t>((row[2 * x - 2] << 8) | row[2 * x - 1]);
                        const std::uint16_t d = static_cast<std::uint16_t>(cur - prev);
                        row[2 * x] = static_cast<std::uint8_t>(d >> 8);
                        row[2 * x + 1] = static_cast<std::uint8_t>(d & 0xFF);
                    }
                }
            }
        }
        uLongf size = compressBound(static_cast<uLong>(raw.size()));
        std::vector<std::uint8_t> z(size);
        compress(z.data(), &size, raw.data(), static_cast<uLong>(raw.size()));
        out.insert(out.end(), z.begin(), z.begin() + static_cast<std::ptrdiff_t>(size));
        break;
    }
    }
    return out;
}

/// The colour channels the file stores for straight RGB (values in [0, 1]): RGB as is; CMYK the inks that make it,
/// inverted (1 = no ink): black's complement is the largest of the three, each ink's the colour over it.
inline std::vector<double> storedColor(std::array<double, 3> rgb, TestPsdMode mode) {
    if (mode == TestPsdMode::Rgb) {
        return {rgb[0], rgb[1], rgb[2]};
    }
    const double k = std::max({rgb[0], rgb[1], rgb[2]});
    const auto ink = [&](double v) { return k > 0.0 ? v / k : 1.0; };
    return {ink(rgb[0]), ink(rgb[1]), ink(rgb[2]), k};
}

} // namespace psdwriter

/// A PSD (or PSB) of `width` x `height` at `depth` bits (RGB, or CMYK of the same colours) with `layers` (bottom
/// first); the merged picture is `merged` everywhere.
inline std::vector<std::uint8_t> writePsd(int width, int height, int depth, const std::vector<TestPsdLayer>& layers,
                                          TestPsdCompression compression = TestPsdCompression::Rle, bool psb = false,
                                          std::array<std::uint16_t, 4> merged = {32768, 32768, 32768, 65535},
                                          TestPsdMode mode = TestPsdMode::Rgb) {
    using namespace psdwriter;
    const std::size_t colorChannels = mode == TestPsdMode::Cmyk ? 4 : 3;
    std::vector<std::uint8_t> out;
    tag(out, "8BPS");
    be(out, psb ? 2 : 1, 2);
    be(out, 0, 6);
    be(out, colorChannels + 1, 2); // the colour channels and alpha
    be(out, static_cast<std::uint64_t>(height), 4);
    be(out, static_cast<std::uint64_t>(width), 4);
    be(out, static_cast<std::uint64_t>(depth), 2);
    be(out, static_cast<std::uint16_t>(mode), 2);
    be(out, 0, 4); // colour mode data
    be(out, 0, 4); // image resources

    // Layer info: count, records, channel data.
    std::vector<std::uint8_t> info;
    be(info, layers.size(), 2);
    std::vector<std::vector<std::vector<std::uint8_t>>> data; // per layer, per channel
    for (const TestPsdLayer& l : layers) {
        const int w = l.section != 0 ? 0 : l.width;
        const int h = l.section != 0 ? 0 : l.height;
        std::array<std::vector<double>, 4> planes; // the stored colour channels
        std::vector<double> alpha;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::array<double, 4> px{};
                if (depth == 32 && l.rgbaFloat) {
                    const auto f = l.rgbaFloat(x, y);
                    px = {f[0], f[1], f[2], f[3]};
                } else {
                    const auto v = l.rgba(x, y);
                    px = {v[0] / 65535.0, v[1] / 65535.0, v[2] / 65535.0, v[3] / 65535.0};
                }
                const std::vector<double> stored = storedColor({px[0], px[1], px[2]}, mode);
                for (std::size_t c = 0; c < colorChannels; ++c) {
                    planes[c].push_back(stored[c]);
                }
                alpha.push_back(px[3]);
            }
        }
        std::vector<std::vector<std::uint8_t>> channels;
        std::vector<int> ids{-1};
        channels.push_back(channelData(alpha, w, h, depth, compression, psb));
        for (std::size_t c = 0; c < colorChannels; ++c) {
            channels.push_back(channelData(planes[c], w, h, depth, compression, psb));
            ids.push_back(static_cast<int>(c));
        }
        if (l.mask) {
            const int mw = (*l.mask)[2] - (*l.mask)[0];
            const int mh = (*l.mask)[3] - (*l.mask)[1];
            const std::vector<double> maskPlane(static_cast<std::size_t>(mw) * mh, l.maskValue / 255.0);
            channels.push_back(channelData(maskPlane, mw, mh, depth, compression, psb));
            ids.push_back(-2);
        }
        // The record.
        be(info, static_cast<std::uint32_t>(l.top), 4);
        be(info, static_cast<std::uint32_t>(l.left), 4);
        be(info, static_cast<std::uint32_t>(l.top + h), 4);
        be(info, static_cast<std::uint32_t>(l.left + w), 4);
        be(info, ids.size(), 2);
        for (std::size_t c = 0; c < ids.size(); ++c) {
            be(info, static_cast<std::uint16_t>(static_cast<std::int16_t>(ids[c])), 2);
            be(info, channels[c].size(), psb ? 8 : 4);
        }
        tag(info, "8BIM");
        std::string blend = l.blend;
        blend.resize(4, ' ');
        tag(info, blend.c_str());
        info.push_back(l.opacity);
        info.push_back(l.clipped ? 1 : 0);
        info.push_back(static_cast<std::uint8_t>(l.hidden ? 0x02 : 0x00));
        info.push_back(0);
        std::vector<std::uint8_t> extra;
        if (l.mask) {
            be(extra, 20, 4);
            be(extra, static_cast<std::uint32_t>((*l.mask)[1]), 4);
            be(extra, static_cast<std::uint32_t>((*l.mask)[0]), 4);
            be(extra, static_cast<std::uint32_t>((*l.mask)[3]), 4);
            be(extra, static_cast<std::uint32_t>((*l.mask)[2]), 4);
            extra.push_back(0); // default colour outside: hidden
            extra.push_back(0); // flags
            be(extra, 0, 2);
        } else {
            be(extra, 0, 4);
        }
        be(extra, 0, 4); // blending ranges
        // A Pascal name (Latin-1 placeholder), padded to 4, and the Unicode name.
        extra.push_back(static_cast<std::uint8_t>(3));
        extra.insert(extra.end(), {'l', 'y', 'r'});
        tag(extra, "8BIM");
        tag(extra, "luni");
        std::vector<std::uint8_t> uni;
        std::u16string name16;
        for (std::size_t i = 0; i < l.name.size();) { // UTF-8 -> UTF-16 (BMP)
            unsigned char c = static_cast<unsigned char>(l.name[i]);
            char16_t u = 0;
            if (c < 0x80) {
                u = c;
                i += 1;
            } else if ((c & 0xE0) == 0xC0) {
                u = static_cast<char16_t>(((c & 0x1F) << 6) | (l.name[i + 1] & 0x3F));
                i += 2;
            } else {
                u = static_cast<char16_t>(((c & 0x0F) << 12) | ((l.name[i + 1] & 0x3F) << 6) | (l.name[i + 2] & 0x3F));
                i += 3;
            }
            name16 += u;
        }
        be(uni, name16.size(), 4);
        for (const char16_t u : name16) {
            be(uni, u, 2);
        }
        while (uni.size() % 4 != 0) {
            uni.push_back(0);
        }
        be(extra, uni.size(), 4);
        extra.insert(extra.end(), uni.begin(), uni.end());
        if (l.section != 0) {
            tag(extra, "8BIM");
            tag(extra, "lsct");
            be(extra, 4, 4);
            be(extra, static_cast<std::uint32_t>(l.section), 4);
        }
        be(info, extra.size(), 4);
        info.insert(info.end(), extra.begin(), extra.end());
        data.push_back(std::move(channels));
    }
    for (const auto& channels : data) {
        for (const auto& c : channels) {
            info.insert(info.end(), c.begin(), c.end());
        }
    }
    while (info.size() % 2 != 0) {
        info.push_back(0);
    }
    // The layer and mask section: 8-bit layers in the layer info; 16- and 32-bit ones in "Lr16" / "Lr32" (the layer
    // info stays empty).
    std::vector<std::uint8_t> section;
    if (depth > 8) {
        be(section, 0, psb ? 8 : 4); // empty layer info
        be(section, 0, 4);           // global layer mask info
        tag(section, "8BIM");
        tag(section, depth == 32 ? "Lr32" : "Lr16");
        be(section, info.size(), psb ? 8 : 4);
        section.insert(section.end(), info.begin(), info.end());
    } else {
        be(section, info.size(), psb ? 8 : 4);
        section.insert(section.end(), info.begin(), info.end());
        be(section, 0, 4); // global layer mask info
    }
    be(out, section.size(), psb ? 8 : 4);
    out.insert(out.end(), section.begin(), section.end());
    // The merged picture, raw.
    be(out, 0, 2);
    std::vector<double> stored = storedColor({merged[0] / 65535.0, merged[1] / 65535.0, merged[2] / 65535.0}, mode);
    stored.push_back(merged[3] / 65535.0);
    for (const double v : stored) {
        const auto plane = samples(std::vector<double>(static_cast<std::size_t>(width) * height, v), depth);
        out.insert(out.end(), plane.begin(), plane.end());
    }
    return out;
}

inline void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace wabcut::media::test
