#pragma once

// Synthetic moving textures for the tracker tests: a band-limited texture defined everywhere (a sum of plane waves of
// random orientation, periods 7..40 px, plus soft blobs that break its symmetry), sampled through a known motion, so
// sub-pixel truth is exact.

#include <wabcut/masks/Tracker.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

namespace wabcut::masks::test {

class Texture {
public:
    explicit Texture(std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int i = 0; i < 14; ++i) {
            const double angle = u(rng) * std::numbers::pi;
            const double period = 7.0 + 33.0 * u(rng);
            m_waves.push_back({std::cos(angle) * 2.0 * std::numbers::pi / period, std::sin(angle) * 2.0 * std::numbers::pi / period,
                               u(rng) * 2.0 * std::numbers::pi, 0.25 + 0.75 * u(rng)});
        }
        for (int i = 0; i < 40; ++i) {
            m_blobs.push_back({u(rng) * 1200.0 - 300.0, u(rng) * 900.0 - 250.0, 6.0 + 18.0 * u(rng), u(rng) * 2.0 - 1.0});
        }
    }

    double at(double x, double y) const {
        double v = 0.0;
        double norm = 0.0;
        for (const Wave& w : m_waves) {
            v += w.amplitude * std::sin(w.kx * x + w.ky * y + w.phase);
            norm += w.amplitude;
        }
        double b = 0.0;
        for (const Blob& blob : m_blobs) {
            const double dx = x - blob.x, dy = y - blob.y;
            b += blob.weight * std::exp(-(dx * dx + dy * dy) / (2.0 * blob.radius * blob.radius));
        }
        return 0.5 + 0.3 * v / norm + 0.2 * std::tanh(b);
    }

private:
    struct Wave {
        double kx, ky, phase, amplitude;
    };
    struct Blob {
        double x, y, radius, weight;
    };
    std::vector<Wave> m_waves;
    std::vector<Blob> m_blobs;
};

/// The picture whose pixel p shows the texture at `toTexture(p)` (the inverse of the content's motion).
inline GrayImage render(const Texture& texture, int width, int height, const Motion& toTexture) {
    GrayImage image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Point2 t = toTexture.map({static_cast<double>(x), static_cast<double>(y)});
            image.px[static_cast<std::size_t>(y) * width + x] = static_cast<float>(texture.at(t.x, t.y));
        }
    }
    return image;
}

/// A similarity about (cx, cy): rotate `degrees`, scale `s`, then move (tx, ty).
inline Motion similarity(double degrees, double s, double tx, double ty, double cx, double cy) {
    const double r = degrees * std::numbers::pi / 180.0;
    const double a = s * std::cos(r), d = s * std::sin(r);
    return {a, -d, cx + tx - (a * cx - d * cy), d, a, cy + ty - (d * cx + a * cy)};
}

/// Largest distance between where two motions put the corners of a box.
inline double cornerError(const Motion& m, const Motion& truth, double x0, double y0, double x1, double y1) {
    double worst = 0.0;
    for (const Point2 p : {Point2{x0, y0}, Point2{x1, y0}, Point2{x1, y1}, Point2{x0, y1}}) {
        const Point2 a = m.map(p);
        const Point2 b = truth.map(p);
        worst = std::max(worst, std::hypot(a.x - b.x, a.y - b.y));
    }
    return worst;
}

inline std::vector<Point2> box(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }

} // namespace wabcut::masks::test
