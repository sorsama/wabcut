// Frame interpolation with motion, the CPU reference of shaders/flowmix.frag (Interpolate.h).

#include <wabcut/motion/Interpolate.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::motion {

namespace {

/// Bilinear weights and corners at fractions (u, v) of a w x h grid (pixel centres at (i + 0.5) / w), clamped.
struct Tap {
    int x0, y0, x1, y1;
    float fx, fy;
};

Tap tapAt(float u, float v, int w, int h) {
    // NaN fails every comparison: it goes to the edge rather than through std::clamp.
    const float sx = u * static_cast<float>(w) - 0.5f;
    const float sy = v * static_cast<float>(h) - 0.5f;
    const float x = sx >= 0.0f ? std::min(sx, static_cast<float>(w - 1)) : 0.0f;
    const float y = sy >= 0.0f ? std::min(sy, static_cast<float>(h - 1)) : 0.0f;
    Tap t{};
    t.x0 = std::min(static_cast<int>(x), w - 1);
    t.y0 = std::min(static_cast<int>(y), h - 1);
    t.x1 = std::min(t.x0 + 1, w - 1);
    t.y1 = std::min(t.y0 + 1, h - 1);
    t.fx = x - static_cast<float>(t.x0);
    t.fy = y - static_cast<float>(t.y0);
    return t;
}

std::array<float, 4> bilinear4(const std::vector<float>& data, int w, int h, float u, float v) {
    const Tap t = tapAt(u, v, w, h);
    const auto at = [&](int x, int y, int c) {
        return data[4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) +
                    static_cast<std::size_t>(c)];
    };
    std::array<float, 4> out{};
    for (int c = 0; c < 4; ++c) {
        const float top = at(t.x0, t.y0, c) + (at(t.x1, t.y0, c) - at(t.x0, t.y0, c)) * t.fx;
        const float bottom = at(t.x0, t.y1, c) + (at(t.x1, t.y1, c) - at(t.x0, t.y1, c)) * t.fx;
        out[static_cast<std::size_t>(c)] = top + (bottom - top) * t.fy;
    }
    return out;
}

} // namespace

std::array<float, 4> RgbaImage::sample(float u, float v) const {
    if (empty()) {
        return {};
    }
    return bilinear4(px, width, height, u, v);
}

std::array<float, 4> sampleFlow(const FlowPair& flow, float u, float v) {
    if (flow.empty()) {
        return {};
    }
    return bilinear4(flow.data, flow.width, flow.height, u, v);
}

RgbaImage interpolate(const RgbaImage& a, const RgbaImage& b, const FlowPair& flow, double t, int threads) {
    if (a.empty() || a.width != b.width || a.height != b.height || flow.empty()) {
        return {};
    }
    const auto ft = static_cast<float>(std::clamp(t, 0.0, 1.0));
    const float fw = static_cast<float>(flow.width);
    const float fh = static_cast<float>(flow.height);
    RgbaImage out(a.width, a.height);
    parallelFor(a.height, threads, [&](int y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(a.height);
        for (int x = 0; x < a.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(a.width);
            const std::array<float, 4> f = sampleFlow(flow, u, v);
            const float s = 1.0f - ft;
            const float x0u = u - s * ft * f[0] + ft * ft * f[2];
            const float x0v = v - s * ft * f[1] + ft * ft * f[3];
            const float x1u = u + s * s * f[0] - ft * s * f[2];
            const float x1v = v + s * s * f[1] - ft * s * f[3];
            // Forward-backward disagreement where each side is read, in flow pixels.
            const std::array<float, 4> f0 = sampleFlow(flow, x0u, x0v);
            const std::array<float, 4> g0 = sampleFlow(flow, x0u + f0[0], x0v + f0[1]);
            const float e0u = (f0[0] + g0[2]) * fw;
            const float e0v = (f0[1] + g0[3]) * fh;
            const std::array<float, 4> f1 = sampleFlow(flow, x1u, x1v);
            const std::array<float, 4> g1 = sampleFlow(flow, x1u + f1[2], x1v + f1[3]);
            const float e1u = (f1[2] + g1[0]) * fw;
            const float e1v = (f1[3] + g1[1]) * fh;
            const float v0 = 1.0f / (1.0f + e0u * e0u + e0v * e0v);
            const float v1 = 1.0f / (1.0f + e1u * e1u + e1v * e1v);
            float w0 = s * v0;
            float w1 = ft * v1;
            const float sum = w0 + w1;
            if (sum > 1e-6f) {
                w0 /= sum;
                w1 /= sum;
            } else {
                w0 = s;
                w1 = ft;
            }
            const std::array<float, 4> pa = a.sample(x0u, x0v);
            const std::array<float, 4> pb = b.sample(x1u, x1v);
            float* o = out.px.data() + 4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x));
            for (std::size_t c = 0; c < 4; ++c) {
                o[c] = w0 * pa[c] + w1 * pb[c];
            }
        }
    });
    return out;
}

RgbaImage blend(const RgbaImage& a, const RgbaImage& b, double t) {
    if (a.empty() || a.width != b.width || a.height != b.height) {
        return {};
    }
    RgbaImage out(a.width, a.height);
    const auto ft = static_cast<float>(std::clamp(t, 0.0, 1.0));
    for (std::size_t i = 0; i < out.px.size(); ++i) {
        out.px[i] = a.px[i] + (b.px[i] - a.px[i]) * ft;
    }
    return out;
}

double psnr(const RgbaImage& reference, const RgbaImage& test, double margin) {
    if (reference.empty() || reference.width != test.width || reference.height != test.height) {
        return 0.0;
    }
    const int mx = static_cast<int>(std::floor(reference.width * std::clamp(margin, 0.0, 0.49)));
    const int my = static_cast<int>(std::floor(reference.height * std::clamp(margin, 0.0, 0.49)));
    double sum = 0.0;
    std::size_t n = 0;
    for (int y = my; y < reference.height - my; ++y) {
        for (int x = mx; x < reference.width - mx; ++x) {
            const std::size_t i = 4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(reference.width) + static_cast<std::size_t>(x));
            for (std::size_t c = 0; c < 3; ++c) {
                const double d = static_cast<double>(reference.px[i + c]) - test.px[i + c];
                sum += d * d;
                ++n;
            }
        }
    }
    if (n == 0) {
        return 0.0;
    }
    const double mse = sum / static_cast<double>(n);
    return mse <= 0.0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1.0 / mse);
}

} // namespace wabcut::motion
