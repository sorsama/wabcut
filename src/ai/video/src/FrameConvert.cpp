#include <wabcut/ai/video/FrameConvert.h>

#include <wabcut/render/ColorPipeline.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wabcut::ai::video {

namespace {

using media::PixelFormat;

struct Layout {
    bool rgb = false;
    bool semiPlanar = false;
    bool hasAlpha = false;
    int bits = 8;
    int shiftX = 0;
    int shiftY = 0;
    bool highBits = false; ///< P010: the 10-bit value sits in the high bits of the word
};

std::optional<Layout> layoutOf(PixelFormat format) {
    Layout l;
    switch (format) {
    case PixelFormat::NV12: l.semiPlanar = true; l.shiftX = l.shiftY = 1; break;
    case PixelFormat::P010: l.semiPlanar = true; l.shiftX = l.shiftY = 1; l.bits = 10; l.highBits = true; break;
    case PixelFormat::YUV420P: l.shiftX = l.shiftY = 1; break;
    case PixelFormat::YUV420P10: l.shiftX = l.shiftY = 1; l.bits = 10; break;
    case PixelFormat::YUV422P: l.shiftX = 1; break;
    case PixelFormat::YUV422P10: l.shiftX = 1; l.bits = 10; break;
    case PixelFormat::YUV444P: break;
    case PixelFormat::YUV444P10: l.bits = 10; break;
    case PixelFormat::RGBA8: l.rgb = true; l.hasAlpha = true; break;
    case PixelFormat::YUVA420P: l.shiftX = l.shiftY = 1; l.hasAlpha = true; break;
    case PixelFormat::YUVA444P10: l.bits = 10; l.hasAlpha = true; break;
    default: return std::nullopt;
    }
    return l;
}

/// The matrix the colour tags name, with the conventional default for untagged streams (BT.709 from 720 lines up).
ColorMatrix matrixOf(const ColorInfo& color, int height) {
    if (color.matrix == ColorMatrix::Unknown) {
        return height >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601;
    }
    return color.matrix;
}

std::uint16_t toU16(float v) { return static_cast<std::uint16_t>(std::clamp(v * 65535.0f + 0.5f, 0.0f, 65535.0f)); }

/// One sample of a plane as the stream stores it (the 10-bit value of a P010 word shifted down).
class PlaneReader {
public:
    PlaneReader(const media::Plane& plane, int bytes, bool high) : m_plane(plane), m_bytes(bytes), m_high(high) {}
    int at(int x, int y) const {
        const std::uint8_t* row = m_plane.data + static_cast<std::ptrdiff_t>(y) * m_plane.stride;
        if (m_bytes == 1) {
            return row[x];
        }
        const int v = row[2 * x] | (row[2 * x + 1] << 8);
        return m_high ? v >> 6 : v;
    }

private:
    const media::Plane& m_plane;
    int m_bytes;
    bool m_high;
};

/// The chroma plane `which` (0 Cb, 1 Cr) of a semi-planar or planar frame, bilinearly upsampled to the luma size, as code
/// values (floating point). Left siting horizontally (chroma at even luma columns), centred vertically.
std::vector<float> upsampledChroma(const media::CpuPlanes& cpu, const Layout& l, int which, int width, int height) {
    const media::Plane& plane = l.semiPlanar ? cpu.planes[1] : cpu.planes[static_cast<std::size_t>(1 + which)];
    const int cw = plane.width;
    const int ch = plane.height;
    const PlaneReader reader(plane, l.bits == 8 ? 1 : 2, l.highBits);
    const auto at = [&](int cx, int cy) {
        cx = std::clamp(cx, 0, cw - 1);
        cy = std::clamp(cy, 0, ch - 1);
        if (l.semiPlanar) {
            // Interleaved U, V pairs: one texel is two samples.
            const std::uint8_t* row = plane.data + static_cast<std::ptrdiff_t>(cy) * plane.stride;
            if (l.bits == 8) {
                return static_cast<float>(row[2 * cx + which]);
            }
            const int v = row[4 * cx + 2 * which] | (row[4 * cx + 2 * which + 1] << 8);
            return static_cast<float>(l.highBits ? v >> 6 : v);
        }
        return static_cast<float>(reader.at(cx, cy));
    };
    std::vector<float> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const float sy = l.shiftY ? (static_cast<float>(y) - 0.5f) * 0.5f : static_cast<float>(y);
        const int y0 = static_cast<int>(std::floor(sy));
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < width; ++x) {
            const float sx = l.shiftX ? static_cast<float>(x) * 0.5f : static_cast<float>(x);
            const int x0 = static_cast<int>(sx);
            const float fx = sx - static_cast<float>(x0);
            const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
            const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = top * (1.0f - fy) + bottom * fy;
        }
    }
    return out;
}

} // namespace

std::expected<Picture, Error> pictureOf(const media::FrameHandle& frame) {
    const media::CpuPlanes* cpu = frame.cpu();
    if (cpu == nullptr) {
        return std::unexpected(Error(ErrorCode::Unsupported, "the frame is on the GPU; decode it to memory first"));
    }
    const auto layout = layoutOf(frame.format);
    if (!layout || frame.width <= 0 || frame.height <= 0 || cpu->planes[0].data == nullptr) {
        return std::unexpected(Error(ErrorCode::Unsupported, "this pixel format cannot go through the AI effects"));
    }
    const Layout& l = *layout;
    Picture p;
    p.width = frame.width;
    p.height = frame.height;
    p.rgb.resize(p.pixels() * 3);
    const auto pixel = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(p.width) + static_cast<std::size_t>(x); };
    if (l.rgb) {
        const media::Plane& plane = cpu->planes[0];
        for (int y = 0; y < p.height; ++y) {
            const std::uint8_t* row = plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride;
            for (int x = 0; x < p.width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    p.rgb[pixel(x, y) * 3 + static_cast<std::size_t>(c)] = static_cast<std::uint16_t>(row[4 * x + c] * 257);
                }
            }
        }
        if (frame.alpha != AlphaMode::None) {
            p.alpha.resize(p.pixels());
            for (int y = 0; y < p.height; ++y) {
                const std::uint8_t* row = plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride;
                for (int x = 0; x < p.width; ++x) {
                    p.alpha[pixel(x, y)] = static_cast<std::uint16_t>(row[4 * x + 3] * 257);
                }
            }
        }
        return p;
    }
    const auto coefficients = render::lumaCoefficients(matrixOf(frame.color, frame.height));
    const double kr = coefficients[0];
    const double kb = coefficients[1];
    const double kg = 1.0 - kr - kb;
    const bool full = frame.color.range == ColorRange::Full;
    const float scale = static_cast<float>(1 << (l.bits - 8));
    const float yOffset = full ? 0.0f : 16.0f * scale;
    const float yRange = full ? static_cast<float>((1 << l.bits) - 1) : 219.0f * scale;
    const float cMid = 128.0f * scale;
    const float cRange = full ? static_cast<float>((1 << l.bits) - 1) : 224.0f * scale;
    const std::vector<float> cb = upsampledChroma(*cpu, l, 0, p.width, p.height);
    const std::vector<float> cr = upsampledChroma(*cpu, l, 1, p.width, p.height);
    const PlaneReader luma(cpu->planes[0], l.bits == 8 ? 1 : 2, l.highBits);
    for (int y = 0; y < p.height; ++y) {
        for (int x = 0; x < p.width; ++x) {
            const std::size_t i = pixel(x, y);
            const float yy = (static_cast<float>(luma.at(x, y)) - yOffset) / yRange;
            const float pb = (cb[i] - cMid) / cRange;
            const float pr = (cr[i] - cMid) / cRange;
            const float r = yy + static_cast<float>(2.0 * (1.0 - kr)) * pr;
            const float b = yy + static_cast<float>(2.0 * (1.0 - kb)) * pb;
            const float g = (yy - static_cast<float>(kr) * r - static_cast<float>(kb) * b) / static_cast<float>(kg);
            p.rgb[i * 3] = toU16(r);
            p.rgb[i * 3 + 1] = toU16(g);
            p.rgb[i * 3 + 2] = toU16(b);
        }
    }
    if (l.hasAlpha && frame.alpha != AlphaMode::None) {
        const media::Plane& plane = cpu->planes[3];
        const PlaneReader reader(plane, l.bits == 8 ? 1 : 2, false);
        const float top = static_cast<float>((1 << l.bits) - 1);
        p.alpha.resize(p.pixels());
        for (int y = 0; y < p.height; ++y) {
            for (int x = 0; x < p.width; ++x) {
                p.alpha[pixel(x, y)] = toU16(static_cast<float>(reader.at(x, y)) / top);
            }
        }
    }
    return p;
}

std::expected<Picture, Error> pictureOf(const StoredFrame& stored) {
    if (!stored.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "not a valid stored picture"));
    }
    // A stored frame is a frame like any other: the same conversion, over its own planes.
    media::FrameHandle frame = frameOf(std::make_shared<const StoredFrame>(stored));
    return pictureOf(frame);
}

std::expected<StoredFrame, Error> storedFrameOf(const Picture& picture, const media::FrameHandle& like) {
    if (!picture.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no picture to store"));
    }
    const auto layout = layoutOf(like.format);
    if (!layout) {
        return std::unexpected(Error(ErrorCode::Unsupported, "this pixel format cannot go through the AI effects"));
    }
    StoredFrame s;
    s.width = picture.width;
    s.height = picture.height;
    s.streamPts = like.streamPts;
    s.timeBaseNum = static_cast<std::int32_t>(std::clamp<std::int64_t>(like.timeBase.num, 1, INT32_MAX));
    s.timeBaseDen = static_cast<std::int32_t>(std::clamp<std::int64_t>(like.timeBase.den, 1, INT32_MAX));
    s.sampleNum = like.sampleAspect.isValid() ? like.sampleAspect.num : 1;
    s.sampleDen = like.sampleAspect.isValid() ? like.sampleAspect.den : 1;
    s.matrix = static_cast<std::uint8_t>(like.color.matrix);
    s.range = static_cast<std::uint8_t>(like.color.range);
    s.primaries = static_cast<std::uint8_t>(like.color.primaries);
    s.transfer = static_cast<std::uint8_t>(like.color.transfer);
    s.chroma = static_cast<std::uint8_t>(like.color.chromaLocation);
    const bool alpha = picture.hasAlpha() && like.alpha != AlphaMode::None;
    s.alpha = static_cast<std::uint8_t>(alpha ? like.alpha : AlphaMode::None);
    const std::size_t n = picture.pixels();
    if (layout->rgb) {
        s.format = static_cast<std::uint8_t>(PixelFormat::RGBA8);
        StoredPlane plane;
        plane.width = picture.width;
        plane.height = picture.height;
        plane.bytesPerTexel = 4;
        plane.data.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                plane.data[4 * i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((picture.rgb[3 * i + static_cast<std::size_t>(c)] + 128) / 257);
            }
            plane.data[4 * i + 3] = alpha ? static_cast<std::uint8_t>((picture.alpha[i] + 128) / 257) : 255;
        }
        s.planes.push_back(std::move(plane));
        return s;
    }
    s.format = static_cast<std::uint8_t>(alpha ? PixelFormat::YUVA444P10 : PixelFormat::YUV444P10);
    const auto k = render::lumaCoefficients(matrixOf(like.color, like.height));
    const double kr = k[0];
    const double kb = k[1];
    const double kg = 1.0 - kr - kb;
    const bool full = like.color.range == ColorRange::Full;
    const float yOffset = full ? 0.0f : 64.0f;
    const float yRange = full ? 1023.0f : 876.0f;
    const float cRange = full ? 1023.0f : 896.0f;
    const int planeCount = alpha ? 4 : 3;
    for (int i = 0; i < planeCount; ++i) {
        StoredPlane plane;
        plane.width = picture.width;
        plane.height = picture.height;
        plane.bytesPerTexel = 2;
        plane.data.resize(n * 2);
        s.planes.push_back(std::move(plane));
    }
    const auto store = [&](int plane, std::size_t i, float code) {
        const int v = static_cast<int>(std::clamp(code + 0.5f, 0.0f, 1023.0f));
        s.planes[static_cast<std::size_t>(plane)].data[2 * i] = static_cast<std::uint8_t>(v & 0xFF);
        s.planes[static_cast<std::size_t>(plane)].data[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const float r = static_cast<float>(picture.rgb[3 * i]) * (1.0f / 65535.0f);
        const float g = static_cast<float>(picture.rgb[3 * i + 1]) * (1.0f / 65535.0f);
        const float b = static_cast<float>(picture.rgb[3 * i + 2]) * (1.0f / 65535.0f);
        const float yy = static_cast<float>(kr) * r + static_cast<float>(kg) * g + static_cast<float>(kb) * b;
        const float pb = (b - yy) / static_cast<float>(2.0 * (1.0 - kb));
        const float pr = (r - yy) / static_cast<float>(2.0 * (1.0 - kr));
        store(0, i, yOffset + yy * yRange);
        store(1, i, 512.0f + pb * cRange);
        store(2, i, 512.0f + pr * cRange);
        if (alpha) {
            store(3, i, static_cast<float>(picture.alpha[i]) * (1023.0f / 65535.0f));
        }
    }
    return s;
}

media::FrameHandle frameOf(std::shared_ptr<const StoredFrame> stored, const media::FrameHandle* like) {
    media::FrameHandle f;
    if (!stored || !stored->valid()) {
        return f;
    }
    f.pts = Ticks{stored->pts};
    f.duration = Ticks{stored->duration};
    f.streamPts = stored->streamPts;
    f.timeBase = TimeBase{stored->timeBaseNum, stored->timeBaseDen};
    f.width = stored->width;
    f.height = stored->height;
    f.format = static_cast<PixelFormat>(stored->format);
    f.color.matrix = static_cast<ColorMatrix>(stored->matrix);
    f.color.range = static_cast<ColorRange>(stored->range);
    f.color.primaries = static_cast<ColorPrimaries>(stored->primaries);
    f.color.transfer = static_cast<TransferFunction>(stored->transfer);
    f.color.chromaLocation = static_cast<ChromaLocation>(stored->chroma);
    f.alpha = static_cast<AlphaMode>(stored->alpha);
    f.sampleAspect = Rate{stored->sampleNum, stored->sampleDen};
    if (like != nullptr) {
        f.orientation = like->orientation;
        f.source = like->source;
    }
    media::CpuPlanes cpu;
    for (std::size_t i = 0; i < stored->planes.size() && i < cpu.planes.size(); ++i) {
        const StoredPlane& p = stored->planes[i];
        cpu.planes[i] = media::Plane{p.data.data(), p.width * p.bytesPerTexel, p.width, p.height};
    }
    cpu.owner = stored;
    f.payload = std::move(cpu);
    return f;
}

} // namespace wabcut::ai::video
