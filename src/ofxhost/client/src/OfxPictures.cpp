#include "OfxPictures.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

namespace wabcut::ofxhost {

void forRowBands(int rows, const std::function<void(int, int)>& body) {
    if (rows <= 0) {
        return;
    }
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const int threads = static_cast<int>(std::min<unsigned>({hardware, 8u, static_cast<unsigned>(std::max(1, rows / 64))}));
    if (threads <= 1) {
        body(0, rows);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads - 1));
    const int band = (rows + threads - 1) / threads;
    for (int t = 1; t < threads; ++t) {
        const int begin = t * band;
        const int end = std::min(rows, begin + band);
        if (begin >= end) {
            break;
        }
        try {
            workers.emplace_back([&body, begin, end] { body(begin, end); });
        } catch (const std::exception&) {
            body(begin, end); // no thread to be had: do it here
        }
    }
    body(0, std::min(rows, band));
    for (std::thread& w : workers) {
        w.join();
    }
}

namespace {

using media::PixelFormat;

struct Layout {
    bool semiPlanar = false;
    bool rgba = false;
    bool wide = false;      ///< 16-bit words
    bool highBits = false;  ///< P010: the value is in the high ten bits
    int shiftX = 0;         ///< log2 chroma subsampling
    int shiftY = 0;
    bool alphaPlane = false;
};

std::optional<Layout> layoutOf(PixelFormat f) {
    Layout l;
    switch (f) {
    case PixelFormat::NV12:
        l.semiPlanar = true;
        l.shiftX = 1;
        l.shiftY = 1;
        break;
    case PixelFormat::P010:
        l.semiPlanar = true;
        l.wide = true;
        l.highBits = true;
        l.shiftX = 1;
        l.shiftY = 1;
        break;
    case PixelFormat::YUV420P:
        l.shiftX = 1;
        l.shiftY = 1;
        break;
    case PixelFormat::YUV420P10:
        l.wide = true;
        l.shiftX = 1;
        l.shiftY = 1;
        break;
    case PixelFormat::YUV422P:
        l.shiftX = 1;
        break;
    case PixelFormat::YUV422P10:
        l.wide = true;
        l.shiftX = 1;
        break;
    case PixelFormat::YUV444P:
        break;
    case PixelFormat::YUV444P10:
        l.wide = true;
        break;
    case PixelFormat::RGBA8:
        l.rgba = true;
        break;
    case PixelFormat::YUVA420P:
        l.shiftX = 1;
        l.shiftY = 1;
        l.alphaPlane = true;
        break;
    case PixelFormat::YUVA444P10:
        l.wide = true;
        l.alphaPlane = true;
        break;
    default:
        return std::nullopt;
    }
    return l;
}

/// One sample of a plane, normalised to 0..1 of its storage's full scale (`wide`: 16-bit words).
inline float sampleAt(const media::Plane& p, int x, int y, bool wide) {
    const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
    if (!wide) {
        return static_cast<float>(row[x]) * (1.0f / 255.0f);
    }
    std::uint16_t v = 0;
    std::memcpy(&v, row + 2 * static_cast<std::ptrdiff_t>(x), 2);
    return static_cast<float>(v) * (1.0f / 65535.0f);
}

/// A chroma sample of an interleaved UV plane (NV12, P010): component `c` (0 U, 1 V).
inline float sampleUv(const media::Plane& p, int x, int y, int c, bool wide) {
    const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
    if (!wide) {
        return static_cast<float>(row[2 * x + c]) * (1.0f / 255.0f);
    }
    std::uint16_t v = 0;
    std::memcpy(&v, row + 4 * static_cast<std::ptrdiff_t>(x) + 2 * c, 2);
    return static_cast<float>(v) * (1.0f / 65535.0f);
}

} // namespace

std::expected<FloatPicture, Error> pictureFromFrame(const media::FrameHandle& frame) {
    const media::CpuPlanes* cpu = frame.cpu();
    if (cpu == nullptr || frame.width <= 0 || frame.height <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "OpenFX plug-ins need a decoded picture in memory"));
    }
    const auto layout = layoutOf(frame.format);
    if (!layout) {
        return std::unexpected(Error(ErrorCode::Unsupported, "OpenFX plug-ins do not take this pixel format yet"));
    }
    const int w = frame.width;
    const int h = frame.height;
    const int planes = media::pixelFormatPlaneCount(frame.format);
    for (int i = 0; i < planes; ++i) {
        const media::Plane& p = cpu->planes[static_cast<std::size_t>(i)];
        if (p.data == nullptr || p.stride == 0) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the frame has a missing plane"));
        }
    }
    const render::ColorPipeline pipeline;
    const render::ResolvedColor color = render::resolveSourceColor(frame.color, pipeline, layout->rgba);
    if (color.hdr) {
        return std::unexpected(Error(ErrorCode::Unsupported, "OpenFX plug-ins do not take HDR pictures yet"));
    }
    // The 10-bit encodings keep their value in a 16-bit word: Bits10Low as it is, P010 shifted up by six.
    const render::SampleEncoding encoding = !layout->wide ? render::SampleEncoding::Bits8
                                            : layout->highBits ? render::SampleEncoding::Bits10High
                                                               : render::SampleEncoding::Bits10Low;
    const render::SampleToRgb toRgb = render::sampleToRgb(color.matrix, color.range, encoding, layout->rgba);
    const auto m = toRgb.matrix.m;
    const auto o = toRgb.offset;
    // Straight alpha is premultiplied for the plug-in; premultiplied alpha is left; no alpha is opaque.
    const bool hasAlpha = layout->rgba || layout->alphaPlane ? frame.alpha != AlphaMode::None || layout->alphaPlane : false;
    const bool straight = frame.alpha == AlphaMode::Straight || (layout->alphaPlane && frame.alpha != AlphaMode::Premultiplied);

    FloatPicture out;
    out.width = w;
    out.height = h;
    out.hasAlpha = hasAlpha;
    out.color = color;
    out.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    float* dst = out.rgba.data();
    const media::Plane& p0 = cpu->planes[0];
    const media::Plane& p1 = cpu->planes[1];
    const media::Plane& p2 = cpu->planes[2];
    const media::Plane& p3 = cpu->planes[3];
    const int cw = layout->rgba ? w : std::max(1, std::min(p1.width, (w + (1 << layout->shiftX) - 1) >> layout->shiftX));
    const int ch = layout->rgba ? h : std::max(1, std::min(p1.height, (h + (1 << layout->shiftY) - 1) >> layout->shiftY));

    forRowBands(h, [&](int y0, int y1) {
        std::vector<float> cb(static_cast<std::size_t>(w));
        std::vector<float> cr(static_cast<std::size_t>(w));
        for (int y = y0; y < y1; ++y) {
            float* row = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
            if (layout->rgba) {
                const std::uint8_t* src = p0.data + static_cast<std::ptrdiff_t>(y) * p0.stride;
                for (int x = 0; x < w; ++x) {
                    const std::uint8_t* px = src + 4 * x;
                    const float a = hasAlpha ? static_cast<float>(px[3]) * (1.0f / 255.0f) : 1.0f;
                    for (int c = 0; c < 3; ++c) {
                        float v = static_cast<float>(px[c]) * (1.0f / 255.0f);
                        // RGB frames only apply the range (full by default).
                        v = static_cast<float>(m[static_cast<std::size_t>(c) * 4] * v + o[static_cast<std::size_t>(c)]);
                        v = std::clamp(v, 0.0f, 1.0f);
                        row[4 * x + c] = straight ? v * a : v;
                    }
                    row[4 * x + 3] = a;
                }
                continue;
            }
            // The chroma of this row, at the luma's resolution (4:2:0: vertically centred, horizontally left-sited).
            int ya = y >> layout->shiftY;
            int yb = ya;
            float wb = 0.0f;
            if (layout->shiftY == 1) {
                const int k = y >> 1;
                if (y & 1) {
                    ya = k;
                    yb = std::min(k + 1, ch - 1);
                    wb = 0.25f;
                } else {
                    ya = std::max(k - 1, 0);
                    yb = k;
                    wb = 0.75f;
                }
            }
            ya = std::min(ya, ch - 1);
            yb = std::min(yb, ch - 1);
            const auto chromaAt = [&](int component, int cx, int cyRow) {
                if (layout->semiPlanar) {
                    return sampleUv(p1, cx, cyRow, component, layout->wide);
                }
                return sampleAt(component == 0 ? p1 : p2, cx, cyRow, layout->wide);
            };
            for (int x = 0; x < w; ++x) {
                const int cx0 = std::min(x >> layout->shiftX, cw - 1);
                const int cx1 = layout->shiftX == 1 && (x & 1) ? std::min(cx0 + 1, cw - 1) : cx0;
                float u = 0.0f;
                float v = 0.0f;
                for (int component = 0; component < 2; ++component) {
                    float a0 = chromaAt(component, cx0, ya);
                    float a1 = cx1 != cx0 ? chromaAt(component, cx1, ya) : a0;
                    float s = cx1 != cx0 ? 0.5f * (a0 + a1) : a0;
                    if (yb != ya) {
                        float b0 = chromaAt(component, cx0, yb);
                        float b1 = cx1 != cx0 ? chromaAt(component, cx1, yb) : b0;
                        const float t = cx1 != cx0 ? 0.5f * (b0 + b1) : b0;
                        s = s + (t - s) * wb;
                    }
                    (component == 0 ? u : v) = s;
                }
                cb[static_cast<std::size_t>(x)] = u;
                cr[static_cast<std::size_t>(x)] = v;
            }
            for (int x = 0; x < w; ++x) {
                const float yy = sampleAt(p0, x, y, layout->wide);
                const float uu = cb[static_cast<std::size_t>(x)];
                const float vv = cr[static_cast<std::size_t>(x)];
                const float a = layout->alphaPlane ? sampleAt(p3, x, y, layout->wide) * (layout->wide ? 65535.0f / 1023.0f : 1.0f) : 1.0f;
                const float alpha = std::clamp(a, 0.0f, 1.0f);
                for (int c = 0; c < 3; ++c) {
                    const std::size_t k = static_cast<std::size_t>(c) * 3;
                    float r = static_cast<float>(m[k] * yy + m[k + 1] * uu + m[k + 2] * vv + o[static_cast<std::size_t>(c)]);
                    r = std::clamp(r, 0.0f, 1.0f);
                    row[4 * x + c] = straight ? r * alpha : r;
                }
                row[4 * x + 3] = alpha;
            }
        }
    });
    return out;
}

std::shared_ptr<const render::TitleRaster> rasterFromFloat(const float* rgba, int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    forRowBands(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* src = rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            std::uint8_t* dst = pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            for (int i = 0; i < width * 4; ++i) {
                dst[i] = static_cast<std::uint8_t>(std::lround(std::clamp(src[i], 0.0f, 1.0f) * 255.0f));
            }
        }
    });
    return std::make_shared<const render::TitleRaster>(width, height, std::move(pixels), render::TitleLayoutInfo{});
}

render::FrameView viewOfRaster(const std::shared_ptr<const render::TitleRaster>& raster, const render::ResolvedColor& color) {
    render::FrameView v = raster->view(raster);
    v.color.primaries = color.primaries;
    v.color.transfer = color.transfer;
    return v;
}

} // namespace wabcut::ofxhost
