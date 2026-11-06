#pragma once

// A synthetic ColorChecker picture for the chart-match tests (tests/color and the Color panel's UI test): a gradient wall,
// optional flat squares of about a patch's size, the chart's dark frame and its 24 patches at the colours the reference
// gives them, seen through a cast (linear light in and out), placed by four corners (turned, tilted, mirrored).

#include <wabcut/color/ChartMatch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>

namespace wabcut::test::chart {

using namespace wabcut::color;

using Cast = std::function<Rgb(const Rgb&)>;

/// A synthetic picture: a gradient wall, optional flat squares of about a patch's size, the chart's dark frame and its 24
/// patches at the colours the reference gives them, seen through `cast` (linear light in and out).
struct Scene {
    int width = 1280;
    int height = 720;
    ChartCorners corners = defaultChartCorners(1280.0 / 720.0);
    Cast cast = [](const Rgb& c) { return c; };
    double noise = 0.0;
    bool distractors = false;
    bool chart = true;
    ChartEdition edition = ChartEdition::AfterNov2014;
    int speck = -1; ///< a bright dot in the middle of this patch
};

inline bool insideConvex(const std::array<ChartPoint, 4>& quad, double x, double y) {
    double sign = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const ChartPoint &a = quad[i], &b = quad[(i + 1) % 4];
        const double cross = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (cross == 0.0) {
            continue;
        }
        if (sign == 0.0) {
            sign = cross > 0 ? 1.0 : -1.0;
        } else if ((cross > 0 ? 1.0 : -1.0) != sign) {
            return false;
        }
    }
    return true;
}

inline void fillQuad(ColorImage& image, const std::array<ChartPoint, 4>& quad, const Rgb& colour) {
    double x0 = 1, x1 = 0, y0 = 1, y1 = 0;
    for (const ChartPoint& p : quad) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0 * image.width))), ix1 = std::min(image.width - 1, static_cast<int>(std::ceil(x1 * image.width)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0 * image.height))), iy1 = std::min(image.height - 1, static_cast<int>(std::ceil(y1 * image.height)));
    for (int y = iy0; y <= iy1; ++y) {
        for (int x = ix0; x <= ix1; ++x) {
            if (!insideConvex(quad, (x + 0.5) / image.width, (y + 0.5) / image.height)) {
                continue;
            }
            float* p = image.rgba.data() + (static_cast<std::size_t>(y) * image.width + x) * 4;
            p[0] = static_cast<float>(colour[0]);
            p[1] = static_cast<float>(colour[1]);
            p[2] = static_cast<float>(colour[2]);
        }
    }
}

/// The code values the camera records for a reference colour.
inline Rgb recorded(const Scene& scene, const Rgb& referenceCode) {
    const Rgb linear = scene.cast(decode709(referenceCode));
    return encode709(Rgb{std::max(linear[0], 0.0), std::max(linear[1], 0.0), std::max(linear[2], 0.0)});
}

inline ColorImage renderScene(const Scene& scene) {
    ColorImage image;
    image.width = scene.width;
    image.height = scene.height;
    image.rgba.resize(static_cast<std::size_t>(scene.width) * scene.height * 4);
    for (int y = 0; y < scene.height; ++y) {
        for (int x = 0; x < scene.width; ++x) {
            float* p = image.rgba.data() + (static_cast<std::size_t>(y) * scene.width + x) * 4;
            const float wall = 0.30f + 0.12f * static_cast<float>(x) / static_cast<float>(scene.width) +
                               0.05f * static_cast<float>(y) / static_cast<float>(scene.height);
            p[0] = wall;
            p[1] = wall * 0.97f;
            p[2] = wall * 0.92f;
            p[3] = 1.0f;
        }
    }
    if (scene.distractors) {
        const double side = 0.07;
        const Rgb colours[] = {{0.8, 0.2, 0.2}, {0.2, 0.7, 0.3}, {0.2, 0.3, 0.8}, {0.9, 0.8, 0.2}, {0.6, 0.6, 0.6}, {0.1, 0.1, 0.1}};
        int k = 0;
        for (const double x : {0.03, 0.9}) {
            for (const double y : {0.05, 0.3, 0.6}) {
                const double h = side * scene.width / scene.height;
                fillQuad(image, {{ChartPoint{x, y}, ChartPoint{x + side, y}, ChartPoint{x + side, y + h}, ChartPoint{x, y + h}}}, colours[k++ % 6]);
            }
        }
    }
    if (scene.chart) {
        // The dark frame around the patches.
        const double m = 0.45;
        fillQuad(image,
                 {{chartToPicture(scene.corners, -m, -m), chartToPicture(scene.corners, kChartWidth + m, -m),
                   chartToPicture(scene.corners, kChartWidth + m, kChartHeight + m), chartToPicture(scene.corners, -m, kChartHeight + m)}},
                 {0.07, 0.07, 0.07});
        for (int i = 0; i < kChartPatches; ++i) {
            fillQuad(image, chartPatchQuad(scene.corners, i, 1.0), recorded(scene, chartReferenceCode(i, scene.edition)));
        }
        if (scene.speck >= 0) {
            const auto quad = chartPatchQuad(scene.corners, scene.speck, 0.12);
            fillQuad(image, quad, {1.0, 1.0, 1.0});
        }
    }
    if (scene.noise > 0.0) {
        std::uint32_t state = 12345u;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            for (std::size_t c = 0; c < 3; ++c) {
                state = state * 1664525u + 1013904223u;
                const double n = (static_cast<double>(state >> 8) / 16777216.0 - 0.5) * 2.0 * scene.noise;
                image.rgba[i + c] = static_cast<float>(std::clamp(image.rgba[i + c] + n, 0.0, 1.0));
            }
        }
    }
    return image;
}

/// A chart `fraction` of the picture's width wide, turned by `degrees`, with the top edge pulled in by `keystone`, optionally
/// mirrored; normalised corners in the order the chart reads upright.
inline ChartCorners placed(const Scene& scene, double fraction, double degrees, double keystone = 0.0, bool mirror = false) {
    const double w = fraction * scene.width, h = w * kChartHeight / kChartWidth;
    const double cx = scene.width / 2.0, cy = scene.height / 2.0;
    std::array<Rgb, 4> pts{{{-w / 2, -h / 2, 0}, {w / 2, -h / 2, 0}, {w / 2, h / 2, 0}, {-w / 2, h / 2, 0}}};
    pts[0][0] *= 1.0 - keystone;
    pts[1][0] *= 1.0 - keystone;
    ChartCorners out;
    const double a = degrees * std::numbers::pi / 180.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = pts[i][0] * std::cos(a) - pts[i][1] * std::sin(a), y = pts[i][0] * std::sin(a) + pts[i][1] * std::cos(a);
        out.p[i] = {(cx + x) / scene.width, (cy + y) / scene.height};
    }
    return mirror ? mirroredCorners(out) : out;
}

inline double cornerError(const ChartCorners& a, const ChartCorners& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        worst = std::max(worst, std::hypot(a.p[i].x - b.p[i].x, a.p[i].y - b.p[i].y));
    }
    return worst;
}

// Casts, in linear light.
inline Rgb diagonalCast(const Rgb& c) { return {c[0] * 1.28 * 0.8, c[1] * 0.8, c[2] * 0.72 * 0.8}; }
inline Rgb mixedCast(const Rgb& c) {
    const Rgb m{0.95 * c[0] + 0.08 * c[1] - 0.02 * c[2], 0.05 * c[0] + 0.92 * c[1] + 0.06 * c[2], -0.03 * c[0] + 0.10 * c[1] + 0.80 * c[2]};
    return {std::pow(std::max(m[0], 0.0), 1.06) * 0.85, std::pow(std::max(m[1], 0.0), 1.06) * 0.85, std::pow(std::max(m[2], 0.0), 1.06) * 0.85};
}


} // namespace wabcut::test::chart
