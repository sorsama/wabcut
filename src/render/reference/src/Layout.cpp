#include <wabcut/render/reference/Layout.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference {

namespace {

constexpr double kPi = 3.14159265358979323846;
/// render::kMaskSegmentsPerCurve (render/Mask.h).
constexpr int kSegmentsPerCurve = 16;
/// render::kMaskEmptyDistance: the distance of every point from a shape that covers nothing.
constexpr double kEmptyDistance = 1.0e4;

double clamp01(double v) { return std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0); }

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

using Buffer = std::vector<Rgba>;

Buffer toBuffer(const Image& image) {
    Buffer out(image.pixelCount());
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)] =
                image.atD(x, y);
        }
    }
    return out;
}

Image fromBuffer(int w, int h, const Buffer& buffer) {
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            out.setD(x, y, buffer[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]);
        }
    }
    return out;
}

/// One separable Gaussian pass (fx.frag blur() at lod 0) along x (dx = 1) or y (dy = 1).
Buffer blurPass(const Buffer& in, int w, int h, int dx, int dy, double sigma, bool repeatEdges) {
    Buffer out(in.size());
    const int taps = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> weights(static_cast<std::size_t>(taps) + 1, 1.0);
    for (int i = 1; i <= taps; ++i) {
        const double fi = static_cast<double>(i);
        weights[static_cast<std::size_t>(i)] = std::exp(-0.5 * fi * fi / (sigma * sigma));
    }
    const auto fetch = [&](int x, int y) -> Rgba {
        if (!repeatEdges && (x < 0 || y < 0 || x >= w || y >= h)) {
            return {0.0, 0.0, 0.0, 0.0};
        }
        const int cx = std::clamp(x, 0, w - 1);
        const int cy = std::clamp(y, 0, h - 1);
        return in[static_cast<std::size_t>(cy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(cx)];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Rgba acc = fetch(x, y);
            double total = 1.0;
            for (int i = 1; i <= taps; ++i) {
                const double wi = weights[static_cast<std::size_t>(i)];
                const Rgba a = fetch(x + i * dx, y + i * dy);
                const Rgba b = fetch(x - i * dx, y - i * dy);
                for (std::size_t c = 0; c < 4; ++c) {
                    acc[c] += wi * (a[c] + b[c]);
                }
                total += 2.0 * wi;
            }
            for (double& v : acc) {
                v /= total;
            }
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = acc;
        }
    }
    return out;
}

Rgb linearUserColor(const Rgb& color) {
    return {srgbToLinear(color[0]), srgbToLinear(color[1]), srgbToLinear(color[2])};
}

} // namespace

// ---- Gaussian blur ------------------------------------------------------------------------------------------------

Image gaussianBlurSigma(const Image& picture, double sigma, bool repeatEdges) {
    if (picture.empty() || !std::isfinite(sigma) || sigma < 0.05) {
        return picture;
    }
    const Buffer horizontal = blurPass(toBuffer(picture), picture.width, picture.height, 1, 0, sigma, repeatEdges);
    const Buffer vertical = blurPass(horizontal, picture.width, picture.height, 0, 1, sigma, repeatEdges);
    return fromBuffer(picture.width, picture.height, vertical);
}

Image gaussianBlur(const Image& picture, double radius, bool repeatEdges) {
    return gaussianBlurSigma(picture, std::max(0.0, radius) / 3.0, repeatEdges);
}

// ---- Drop shadow --------------------------------------------------------------------------------------------------

DropShadowParams dropShadowParams(const EffectInstance& instance, Ticks curveTime) {
    const DropShadowParams d;
    DropShadowParams p;
    p.opacity = effectParameterAt(instance, "opacity", curveTime, d.opacity);
    p.angle = effectParameterAt(instance, "angle", curveTime, d.angle);
    p.distance = effectParameterAt(instance, "distance", curveTime, d.distance);
    p.softness = effectParameterAt(instance, "softness", curveTime, d.softness);
    p.color = {effectParameterAt(instance, "red", curveTime, d.color[0]),
               effectParameterAt(instance, "green", curveTime, d.color[1]),
               effectParameterAt(instance, "blue", curveTime, d.color[2])};
    return p;
}

Image dropShadow(const Image& layer, const DropShadowParams& params) {
    if (layer.empty()) {
        return layer;
    }
    const double angle = params.angle * kPi / 180.0;
    const double distance = std::max(0.0, params.distance);
    const double dx = distance * std::sin(angle);
    const double dy = -distance * std::cos(angle);
    const double opacity = clamp01(params.opacity);
    const Rgb color = linearUserColor(params.color);
    const double w = static_cast<double>(layer.width);
    const double h = static_cast<double>(layer.height);
    Image shadow(layer.width, layer.height);
    for (int y = 0; y < layer.height; ++y) {
        for (int x = 0; x < layer.width; ++x) {
            const double sx = static_cast<double>(x) + 0.5 - dx;
            const double sy = static_cast<double>(y) + 0.5 - dy;
            double a = sampleBilinear(layer, sx, sy)[3];
            if (sx < 0.0 || sy < 0.0 || sx > w || sy > h) {
                a = 0.0;
            }
            const double k = a * opacity;
            shadow.setD(x, y, {color[0] * k, color[1] * k, color[2] * k, k});
        }
    }
    const double sigma = std::max(0.0, params.softness) / 3.0;
    if (sigma >= 0.05) {
        shadow = gaussianBlurSigma(shadow, sigma, false);
    }
    Image out(layer.width, layer.height);
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const double keep = 1.0 - static_cast<double>(layer.rgba[i + 3]);
        for (std::size_t c = 0; c < 4; ++c) {
            out.rgba[i + c] =
                static_cast<float>(static_cast<double>(layer.rgba[i + c]) + static_cast<double>(shadow.rgba[i + c]) * keep);
        }
    }
    return out;
}

// ---- Masks --------------------------------------------------------------------------------------------------------

std::vector<std::array<double, 2>> flattenMaskOutline(const std::vector<MaskVertex>& outline) {
    std::vector<std::array<double, 2>> out;
    const std::size_t n = outline.size();
    if (n < 2) {
        return out;
    }
    out.reserve(n * kSegmentsPerCurve);
    for (std::size_t i = 0; i < n; ++i) {
        const MaskVertex& a = outline[i];
        const MaskVertex& b = outline[(i + 1) % n];
        out.push_back({a.x, a.y});
        const bool straight = a.outX == 0.0 && a.outY == 0.0 && b.inX == 0.0 && b.inY == 0.0;
        if (straight) {
            continue;
        }
        const double c1x = a.x + a.outX;
        const double c1y = a.y + a.outY;
        const double c2x = b.x + b.inX;
        const double c2y = b.y + b.inY;
        for (int k = 1; k < kSegmentsPerCurve; ++k) {
            const double t = static_cast<double>(k) / kSegmentsPerCurve;
            const double s = 1.0 - t;
            const double w0 = s * s * s;
            const double w1 = 3.0 * s * s * t;
            const double w2 = 3.0 * s * t * t;
            const double w3 = t * t * t;
            out.push_back({w0 * a.x + w1 * c1x + w2 * c2x + w3 * b.x, w0 * a.y + w1 * c1y + w2 * c2y + w3 * b.y});
        }
    }
    return out;
}

double polygonSignedDistance(const std::vector<std::array<double, 2>>& polygon, double px, double py, double aspect) {
    const std::size_t n = polygon.size();
    if (n < 3) {
        return -1e9;
    }
    const double qx = px * aspect;
    const double qy = py;
    double best = 1e300;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double x0 = polygon[j][0] * aspect;
        const double y0 = polygon[j][1];
        const double x1 = polygon[i][0] * aspect;
        const double y1 = polygon[i][1];
        const double ex = x1 - x0;
        const double ey = y1 - y0;
        const double len2 = ex * ex + ey * ey;
        const double t = len2 > 0.0 ? std::clamp(((qx - x0) * ex + (qy - y0) * ey) / len2, 0.0, 1.0) : 0.0;
        const double rx = qx - (x0 + t * ex);
        const double ry = qy - (y0 + t * ey);
        best = std::min(best, rx * rx + ry * ry);
        if ((y0 > qy) != (y1 > qy) && qx < x0 + (qy - y0) * ex / ey) {
            inside = !inside;
        }
    }
    const double d = std::sqrt(best);
    return inside ? d : -d;
}

Image maskCoverage(const Mask& mask, Ticks curveTime, int width, int height, int supersample) {
    Image out(width, height);
    if (out.empty()) {
        return out;
    }
    if (!mask.enabled) {
        return Image::filled(width, height, {1.0f, 1.0f, 1.0f, 1.0f});
    }
    const double w = static_cast<double>(width);
    const double h = static_cast<double>(height);
    const double aspect = w / h;
    const std::vector<std::array<double, 2>> polygon = flattenMaskOutline(maskOutlineAt(mask, curveTime, aspect));
    const double feather = std::max(0.0, maskValue(mask, "feather", curveTime));
    const double expansion = maskValue(mask, "expansion", curveTime);
    const double opacity = clamp01(maskValue(mask, "opacity", curveTime));
    const double aa = 1.0 / h; // one pixel, in frame heights
    const auto edge = [&](double px, double py) {
        const double d = polygon.size() < 3 ? -kEmptyDistance : polygonSignedDistance(polygon, px, py, aspect);
        return d + expansion;
    };
    const int n = std::max(supersample, 1);
    const double halfDiagonal = 0.75 * aa; // > sqrt(2) / 2 pixel: beyond it a pixel is wholly in or out
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double cx = (static_cast<double>(x) + 0.5) / w;
            const double cy = (static_cast<double>(y) + 0.5) / h;
            const double e = edge(cx, cy);
            double c = 0.0;
            if (feather > aa) {
                c = smoothstep(-0.5 * feather, 0.5 * feather, e);
            } else if (supersample <= 1) {
                c = std::clamp(0.5 + e / aa, 0.0, 1.0);
            } else if (std::abs(e) > halfDiagonal) {
                c = e > 0.0 ? 1.0 : 0.0;
            } else {
                int insideCount = 0;
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        const double sx = (static_cast<double>(x) + (static_cast<double>(i) + 0.5) / n) / w;
                        const double sy = (static_cast<double>(y) + (static_cast<double>(j) + 0.5) / n) / h;
                        if (edge(sx, sy) > 0.0) {
                            ++insideCount;
                        }
                    }
                }
                c = static_cast<double>(insideCount) / static_cast<double>(n * n);
            }
            c = mask.inverted ? 1.0 - c : c;
            c *= opacity;
            if (mask.mode == MaskMode::Subtract) {
                c = 1.0 - c;
            }
            const float v = static_cast<float>(c);
            out.set(x, y, {v, v, v, v});
        }
    }
    return out;
}

Image applyMask(const Image& layer, const Image& coverage) {
    Image out(layer.width, layer.height);
    for (int y = 0; y < layer.height; ++y) {
        for (int x = 0; x < layer.width; ++x) {
            const double m = x < coverage.width && y < coverage.height ? static_cast<double>(coverage.alpha(x, y)) : 0.0;
            const Rgba p = layer.atD(x, y);
            out.setD(x, y, {p[0] * m, p[1] * m, p[2] * m, p[3] * m});
        }
    }
    return out;
}

// ---- Border -------------------------------------------------------------------------------------------------------

BorderParams borderParams(const EffectInstance& instance, Ticks curveTime) {
    const BorderParams d;
    BorderParams p;
    p.width = effectParameterAt(instance, "width", curveTime, d.width);
    p.color = {effectParameterAt(instance, "color.r", curveTime, d.color[0]),
               effectParameterAt(instance, "color.g", curveTime, d.color[1]),
               effectParameterAt(instance, "color.b", curveTime, d.color[2])};
    p.opacity = effectParameterAt(instance, "opacity", curveTime, d.opacity);
    return p;
}

Image border(const Image& layer, double width, const Rgb& color, double opacity) {
    const double amount = clamp01(opacity);
    if (layer.empty() || !(width > 0.0) || !(amount > 0.0)) {
        return layer;
    }
    // Disc offsets |(dx, dy)| <= r, r bounded by the picture (a larger disc reaches nothing more).
    const double r = std::min(width, static_cast<double>(std::max(layer.width, layer.height)) * 2.0);
    const int reach = static_cast<int>(std::floor(r));
    std::vector<std::array<int, 2>> disc;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            if (static_cast<double>(dx * dx + dy * dy) <= r * r) {
                disc.push_back({dx, dy});
            }
        }
    }
    const int w = layer.width;
    const int h = layer.height;
    std::vector<double> a(layer.pixelCount());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            a[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                clamp01(static_cast<double>(layer.alpha(x, y)));
        }
    }
    const Rgb lin = linearUserColor(color);
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double here = a[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
            double dilated = here;
            for (const auto& [dx, dy] : disc) {
                const int qx = x + dx;
                const int qy = y + dy;
                if (qx >= 0 && qy >= 0 && qx < w && qy < h) {
                    dilated = std::max(dilated, a[static_cast<std::size_t>(qy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(qx)]);
                }
            }
            // The dilated shape under the layer (Layout.h: not clamp(dilated - here), which leaves a seam at soft edges).
            const double b = std::clamp(dilated, 0.0, 1.0) * amount;
            const double under = b * (1.0 - here);
            const Rgba p = layer.atD(x, y);
            out.setD(x, y, {p[0] + lin[0] * under, p[1] + lin[1] * under, p[2] + lin[2] * under, p[3] + under});
        }
    }
    return out;
}

Image border(const Image& layer, const BorderParams& params) {
    return border(layer, params.width, params.color, params.opacity);
}

// ---- Blur fill ----------------------------------------------------------------------------------------------------

Image coverScale(const Image& picture, int canvasWidth, int canvasHeight) {
    Image out(canvasWidth, canvasHeight);
    if (out.empty() || picture.empty()) {
        return out;
    }
    const double cw = static_cast<double>(canvasWidth);
    const double ch = static_cast<double>(canvasHeight);
    const double pw = static_cast<double>(picture.width);
    const double ph = static_cast<double>(picture.height);
    const double s = std::max(cw / pw, ch / ph);
    for (int y = 0; y < canvasHeight; ++y) {
        for (int x = 0; x < canvasWidth; ++x) {
            const double sx = (static_cast<double>(x) + 0.5 - cw / 2.0) / s + pw / 2.0;
            const double sy = (static_cast<double>(y) + 0.5 - ch / 2.0) / s + ph / 2.0;
            out.setD(x, y, sampleBilinear(picture, sx, sy));
        }
    }
    return out;
}

Image blurFill(const Image& picture, int canvasWidth, int canvasHeight, double blurRadius) {
    return gaussianBlur(coverScale(picture, canvasWidth, canvasHeight), blurRadius, true);
}

} // namespace wabcut::render::reference
