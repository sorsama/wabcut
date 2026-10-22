#pragma once

// A texture defined everywhere for the motion tests (P.2): a sum of plane waves with a 1/f-like spectrum (periods from
// about 7 to 250 px), so pictures of it moved by any sub-pixel amount are exact and the coarse pyramid levels still
// see structure (as in real footage). Shared by the motion, render and playback motion tests.

#include <wabcut/motion/Image.h>
#include <wabcut/motion/Interpolate.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

namespace wabcut::motion::test {

class MovingTexture {
public:
    explicit MovingTexture(std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        double total = 0.0;
        for (int i = 0; i < 48; ++i) {
            const double f = std::exp(std::log(0.004) + unit(rng) * (std::log(0.15) - std::log(0.004)));
            const double angle = unit(rng) * 2.0 * std::numbers::pi;
            const double amplitude = std::pow(f, -0.8);
            total += amplitude;
            m_waves.push_back({f * std::cos(angle), f * std::sin(angle), unit(rng) * 2.0 * std::numbers::pi, amplitude});
        }
        for (Wave& w : m_waves) {
            w.amplitude *= 0.9 / total;
        }
    }

    /// The texture at (x, y), 0..1.
    double at(double x, double y) const {
        double v = 0.5;
        for (const Wave& w : m_waves) {
            v += w.amplitude * std::sin(2.0 * std::numbers::pi * (w.fx * x + w.fy * y) + w.phase);
        }
        return std::clamp(v, 0.0, 1.0);
    }

    /// A w x h picture showing the texture moved by (dx, dy) and zoomed by `zoom` about (cx, cy): pixel p shows
    /// the texture at c + (p - c - d) / zoom.
    Plane picture(int w, int h, double dx, double dy, double zoom = 1.0, double cx = 0.0, double cy = 0.0) const {
        Plane p(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                p(x, y) = static_cast<float>(at(cx + (x - cx - dx) / zoom, cy + (y - cy - dy) / zoom));
            }
        }
        return p;
    }

private:
    struct Wave {
        double fx, fy, phase, amplitude;
    };
    std::vector<Wave> m_waves;
};

/// A grey picture as opaque RGBA.
inline RgbaImage rgbaOf(const Plane& p) {
    RgbaImage out(p.width, p.height);
    for (std::size_t i = 0; i < p.px.size(); ++i) {
        out.px[4 * i + 0] = out.px[4 * i + 1] = out.px[4 * i + 2] = p.px[i];
        out.px[4 * i + 3] = 1.0f;
    }
    return out;
}

} // namespace wabcut::motion::test
