#include <wabcut/ai/video/Picture.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::ai::video {

namespace {

std::uint16_t toU16(float v) {
    return static_cast<std::uint16_t>(std::clamp(v + 0.5f, 0.0f, 65535.0f));
}

/// Bilinear resample of `channels` interleaved 16-bit channels.
void resample(const std::uint16_t* src, int sw, int sh, std::uint16_t* dst, int dw, int dh, int channels) {
    const float fx = static_cast<float>(sw) / static_cast<float>(dw);
    const float fy = static_cast<float>(sh) / static_cast<float>(dh);
    std::vector<int> x0(static_cast<std::size_t>(dw));
    std::vector<int> x1(static_cast<std::size_t>(dw));
    std::vector<float> wx(static_cast<std::size_t>(dw));
    for (int x = 0; x < dw; ++x) {
        const float sx = std::clamp((static_cast<float>(x) + 0.5f) * fx - 0.5f, 0.0f, static_cast<float>(sw - 1));
        const int ix = static_cast<int>(sx);
        x0[static_cast<std::size_t>(x)] = ix;
        x1[static_cast<std::size_t>(x)] = std::min(ix + 1, sw - 1);
        wx[static_cast<std::size_t>(x)] = sx - static_cast<float>(ix);
    }
    for (int y = 0; y < dh; ++y) {
        const float sy = std::clamp((static_cast<float>(y) + 0.5f) * fy - 0.5f, 0.0f, static_cast<float>(sh - 1));
        const int iy = static_cast<int>(sy);
        const int iy1 = std::min(iy + 1, sh - 1);
        const float wy = sy - static_cast<float>(iy);
        const std::uint16_t* r0 = src + static_cast<std::size_t>(iy) * static_cast<std::size_t>(sw) * static_cast<std::size_t>(channels);
        const std::uint16_t* r1 = src + static_cast<std::size_t>(iy1) * static_cast<std::size_t>(sw) * static_cast<std::size_t>(channels);
        std::uint16_t* out = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) * static_cast<std::size_t>(channels);
        for (int x = 0; x < dw; ++x) {
            const std::size_t a = static_cast<std::size_t>(x0[static_cast<std::size_t>(x)]) * static_cast<std::size_t>(channels);
            const std::size_t b = static_cast<std::size_t>(x1[static_cast<std::size_t>(x)]) * static_cast<std::size_t>(channels);
            const float w = wx[static_cast<std::size_t>(x)];
            for (int c = 0; c < channels; ++c) {
                const float top = static_cast<float>(r0[a + static_cast<std::size_t>(c)]) * (1.0f - w) +
                                  static_cast<float>(r0[b + static_cast<std::size_t>(c)]) * w;
                const float bottom = static_cast<float>(r1[a + static_cast<std::size_t>(c)]) * (1.0f - w) +
                                     static_cast<float>(r1[b + static_cast<std::size_t>(c)]) * w;
                *out++ = toU16(top * (1.0f - wy) + bottom * wy);
            }
        }
    }
}

} // namespace

Picture resized(const Picture& in, int width, int height) {
    Picture out;
    if (!in.valid() || width <= 0 || height <= 0) {
        return out;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(out.pixels() * 3);
    resample(in.rgb.data(), in.width, in.height, out.rgb.data(), width, height, 3);
    if (in.hasAlpha()) {
        out.alpha.resize(out.pixels());
        resample(in.alpha.data(), in.width, in.height, out.alpha.data(), width, height, 1);
    }
    return out;
}

std::vector<std::uint16_t> resizedPlane(const std::vector<std::uint16_t>& plane, int width, int height, int newWidth, int newHeight) {
    std::vector<std::uint16_t> out;
    if (width <= 0 || height <= 0 || newWidth <= 0 || newHeight <= 0 ||
        plane.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return out;
    }
    out.resize(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight));
    resample(plane.data(), width, height, out.data(), newWidth, newHeight, 1);
    return out;
}

Picture halved(const Picture& in) {
    Picture out;
    if (!in.valid() || in.width < 2 || in.height < 2) {
        return in;
    }
    out.width = in.width / 2;
    out.height = in.height / 2;
    out.rgb.resize(out.pixels() * 3);
    const auto box = [&](const std::vector<std::uint16_t>& src, std::vector<std::uint16_t>& dst, int channels) {
        for (int y = 0; y < out.height; ++y) {
            const int ya = y * 2;
            const int yb = std::min(ya + 1, in.height - 1);
            for (int x = 0; x < out.width; ++x) {
                const int xa = x * 2;
                const int xb = std::min(xa + 1, in.width - 1);
                for (int c = 0; c < channels; ++c) {
                    const auto at = [&](int px, int py) {
                        return static_cast<unsigned>(
                            src[(static_cast<std::size_t>(py) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(px)) *
                                    static_cast<std::size_t>(channels) +
                                static_cast<std::size_t>(c)]);
                    };
                    dst[(static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) + static_cast<std::size_t>(x)) *
                            static_cast<std::size_t>(channels) +
                        static_cast<std::size_t>(c)] = static_cast<std::uint16_t>((at(xa, ya) + at(xb, ya) + at(xa, yb) + at(xb, yb) + 2) / 4);
                }
            }
        }
    };
    box(in.rgb, out.rgb, 3);
    if (in.hasAlpha()) {
        out.alpha.resize(out.pixels());
        box(in.alpha, out.alpha, 1);
    }
    return out;
}

Picture mixed(const Picture& from, const Picture& to, double k) {
    if (!from.valid() || !to.valid() || from.width != to.width || from.height != to.height) {
        return to;
    }
    const float t = static_cast<float>(std::clamp(k, 0.0, 1.0));
    if (t >= 1.0f) {
        return to;
    }
    Picture out = to;
    for (std::size_t i = 0; i < out.rgb.size(); ++i) {
        const float a = static_cast<float>(from.rgb[i]);
        const float b = static_cast<float>(to.rgb[i]);
        out.rgb[i] = toU16(a + t * (b - a));
    }
    return out;
}

double meanAbsDifference(const Picture& a, const Picture& b) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        return 1.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        sum += std::abs(static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]));
    }
    return sum / (65535.0 * static_cast<double>(a.rgb.size()));
}

double psnr(const Picture& a, const Picture& b) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        const double d = (static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i])) / 65535.0;
        sum += d * d;
    }
    const double mse = sum / static_cast<double>(a.rgb.size());
    return mse <= 0.0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1.0 / mse);
}

} // namespace wabcut::ai::video

namespace wabcut::ai::video {

std::vector<float> normalisedDepth(const std::vector<float>& raw) {
    std::vector<float> out(raw.size(), 0.5f);
    if (raw.empty()) {
        return out;
    }
    std::vector<float> sorted;
    sorted.reserve(raw.size());
    for (const float v : raw) {
        sorted.push_back(std::isfinite(v) ? v : -std::numeric_limits<float>::max());
    }
    const auto percentile = [&](double p) {
        const auto k = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(sorted.size() - 1));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
        return sorted[k];
    };
    const float lo = percentile(0.02);
    const float hi = percentile(0.98);
    if (!(hi - lo > 1e-6f)) {
        return out; // flat
    }
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const float v = std::isfinite(raw[i]) ? raw[i] : lo;
        out[i] = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    }
    return out;
}

Picture greyPicture(const std::vector<float>& values, int width, int height) {
    Picture p;
    p.width = width;
    p.height = height;
    if (width <= 0 || height <= 0 || values.size() != p.pixels()) {
        return Picture{};
    }
    p.rgb.resize(values.size() * 3);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto v = static_cast<std::uint16_t>(std::lround(std::clamp(values[i], 0.0f, 1.0f) * 65535.0f));
        p.rgb[3 * i] = v;
        p.rgb[3 * i + 1] = v;
        p.rgb[3 * i + 2] = v;
    }
    return p;
}

std::vector<std::uint8_t> distanceMap8(const Picture& grey) {
    std::vector<std::uint8_t> out;
    if (!grey.valid()) {
        return out;
    }
    out.resize(grey.pixels());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double nearness = static_cast<double>(grey.rgb[3 * i]) / 65535.0; // 1 = nearest
        out[i] = static_cast<std::uint8_t>(std::lround((1.0 - nearness) * 255.0));
    }
    return out;
}

} // namespace wabcut::ai::video
