// The Stylize family's CPU twins (shaders/fx_stylize.frag and Compositor.cpp's passes for it), for
// render/reference/LibraryEffects.h.

#include "LibraryInternal.h"

#include <wabcut/render/reference/Layout.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference::detail {

namespace {

using E = LibraryEffect;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double radians(double d) { return d * 3.14159265358979 / 180.0; }
double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

/// `img` at canvas position (x, y), transparent outside the canvas.
Rgba at(const Image& img, double x, double y) {
    if (x < 0.0 || y < 0.0 || x > img.width || y > img.height) {
        return {};
    }
    return sampleBilinear(img, x, y);
}

Rgb encoded(const Rgba& c) {
    const double a = clamp01(c[3]);
    if (a <= 0.0) {
        return {};
    }
    return {toEncoded(c[0] / a), toEncoded(c[1] / a), toEncoded(c[2] / a)};
}

Rgba premultiplied(const Rgb& e, double a) { return {toLinear(e[0]) * a, toLinear(e[1]) * a, toLinear(e[2]) * a, a}; }

Rgb mix3(const Rgb& a, const Rgb& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

Rgb srgb3(double r, double g, double b) { return {srgbToLinear(r), srgbToLinear(g), srgbToLinear(b)}; }

template <class Fn>
Image perPixel(const Image& in, Fn&& fn) {
    Image out(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            out.setD(x, y, fn(x, y, x + 0.5, y + 0.5, in.atD(x, y)));
        }
    }
    return out;
}

// Value noise (LibraryDistort's twin of fx_stylize.frag's copy).
double valueAt(int x, int y, int z, std::uint32_t seed) {
    return 2.0 * libraryHash01(static_cast<std::uint32_t>(x) + 2654435761u * static_cast<std::uint32_t>(y),
                               static_cast<std::uint32_t>(z) + seed * 1013u, 11u) -
           1.0;
}

double noise3(double vx, double vy, double vz, int period, std::uint32_t seed) {
    const double ix = std::floor(vx);
    const double iy = std::floor(vy);
    const double iz = std::floor(vz);
    const double fx = vx - ix;
    const double fy = vy - iy;
    const double fz = vz - iz;
    const double wx = fx * fx * (3.0 - 2.0 * fx);
    const double wy = fy * fy * (3.0 - 2.0 * fy);
    const double wz = fz * fz * (3.0 - 2.0 * fz);
    const int x = static_cast<int>(ix);
    const int y = static_cast<int>(iy);
    int z0 = static_cast<int>(iz);
    int z1 = z0 + 1;
    if (period > 0) {
        z0 = z0 - period * static_cast<int>(std::floor(static_cast<double>(z0) / period));
        z1 = z1 - period * static_cast<int>(std::floor(static_cast<double>(z1) / period));
    }
    const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    const double a = lerp(lerp(valueAt(x, y, z0, seed), valueAt(x + 1, y, z0, seed), wx),
                          lerp(valueAt(x, y + 1, z0, seed), valueAt(x + 1, y + 1, z0, seed), wx), wy);
    const double b = lerp(lerp(valueAt(x, y, z1, seed), valueAt(x + 1, y, z1, seed), wx),
                          lerp(valueAt(x, y + 1, z1, seed), valueAt(x + 1, y + 1, z1, seed), wx), wy);
    return lerp(a, b, wz);
}

double fbm(double vx, double vy, double z, double complexity, int period, std::uint32_t seed) {
    double total = 0.0;
    double norm = 0.0;
    double amp = 1.0;
    double freq = 1.0;
    const int octaves = static_cast<int>(std::floor(complexity));
    const double last = complexity - octaves;
    for (int o = 0; o < 11; ++o) {
        const double weight = o < octaves ? 1.0 : (o == octaves ? last : 0.0);
        if (weight <= 0.0) {
            break;
        }
        total += weight * amp * noise3(vx * freq, vy * freq, z, period, seed + static_cast<std::uint32_t>(o) * 101u);
        norm += weight * amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0.0 ? total / norm : 0.0;
}

Rgba medianOf(const Image& in, double cx, double cy, double step) {
    std::array<Rgba, 9> s{};
    std::array<double, 9> l{};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const Rgba v = at(in, cx + (i - 1) * step, cy + (j - 1) * step);
            s[static_cast<std::size_t>(j * 3 + i)] = v;
            l[static_cast<std::size_t>(j * 3 + i)] = luma(encoded(v));
        }
    }
    Rgba result = s[4];
    for (std::size_t k = 0; k < 9; ++k) {
        int below = 0;
        int equalBefore = 0;
        for (std::size_t m = 0; m < 9; ++m) {
            if (l[m] < l[k]) {
                ++below;
            } else if (l[m] == l[k] && m < k) {
                ++equalBefore;
            }
        }
        if (below + equalBefore == 4) {
            result = s[k];
        }
    }
    return result;
}

} // namespace

bool isStylize(LibraryEffect effect) {
    const int v = static_cast<int>(effect);
    return v >= 70 && v < 100;
}

Image stylize(const Image& in, LibraryEffect op, const LibraryParams& q) {
    switch (op) {
    case E::UnsharpMask: {
        const Image b = gaussianBlurSigma(in, std::max(q[1], 0.0), true);
        return perPixel(in, [&](int x, int y, double, double, const Rgba& c) {
            const Rgba bl = b.atD(x, y);
            const Rgb d{c[0] - bl[0], c[1] - bl[1], c[2] - bl[2]};
            const double m = std::max({std::abs(d[0]), std::abs(d[1]), std::abs(d[2])});
            const double t = q[2] / 255.0;
            const double w = q[2] <= 0.0 ? 1.0 : smoothstep(t - 0.5 / 255.0, t + 0.5 / 255.0, m);
            return Rgba{std::max(c[0] + q[0] / 100.0 * d[0] * w, 0.0), std::max(c[1] + q[0] / 100.0 * d[1] * w, 0.0),
                        std::max(c[2] + q[0] / 100.0 * d[2] * w, 0.0), c[3]};
        });
    }
    case E::Emboss:
    case E::ColorEmboss:
        return perPixel(in, [&](int, int, double cx, double cy, const Rgba& c) {
            const double a = radians(q[0]);
            const double dx = std::sin(a) * std::max(q[1], 0.0);
            const double dy = -std::cos(a) * std::max(q[1], 0.0);
            const double relief = luma(encoded(at(in, cx + dx, cy + dy))) - luma(encoded(at(in, cx - dx, cy - dy)));
            const double v = 0.5 + q[2] / 100.0 * 0.5 * relief;
            const Rgb e = encoded(c);
            Rgb r{};
            if (op == E::Emboss) {
                r = {clamp01(v), clamp01(v), clamp01(v)};
            } else {
                r = {clamp01(e[0] + v - 0.5), clamp01(e[1] + v - 0.5), clamp01(e[2] + v - 0.5)};
            }
            return premultiplied(mix3(r, e, q[3]), clamp01(c[3]));
        });
    case E::FindEdges:
        return perPixel(in, [&](int, int, double cx, double cy, const Rgba& c) {
            const auto s = [&](int i, int j) { return encoded(at(in, cx + i, cy + j)); };
            Rgb r{};
            for (std::size_t ch = 0; ch < 3; ++ch) {
                const double gx = (s(1, -1)[ch] + 2.0 * s(1, 0)[ch] + s(1, 1)[ch]) - (s(-1, -1)[ch] + 2.0 * s(-1, 0)[ch] + s(-1, 1)[ch]);
                const double gy = (s(-1, 1)[ch] + 2.0 * s(0, 1)[ch] + s(1, 1)[ch]) - (s(-1, -1)[ch] + 2.0 * s(0, -1)[ch] + s(1, -1)[ch]);
                const double mag = clamp01(std::sqrt(gx * gx + gy * gy) * 0.25);
                r[ch] = q[0] > 0.5 ? mag : 1.0 - mag;
            }
            return premultiplied(mix3(r, encoded(c), q[1]), clamp01(c[3]));
        });
    case E::ConvolutionKernel:
        return perPixel(in, [&](int, int, double cx, double cy, const Rgba& c) {
            // Premiere's 3 x 3 kernel (M11 top left) on the encoded colour: sum M x neighbour / Scale + Offset / 255.
            const double scale = std::max(q[10], 1.0);
            Rgb sum{};
            double alpha = 0.0;
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 3; ++i) {
                    const Rgba s = at(in, cx + (i - 1), cy + (j - 1));
                    const double m = q[static_cast<std::size_t>(j * 3 + i)];
                    const Rgb e = encoded(s);
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        sum[ch] += m * e[ch];
                    }
                    alpha += m * s[3];
                }
            }
            Rgb r{};
            for (std::size_t ch = 0; ch < 3; ++ch) {
                r[ch] = clamp01(sum[ch] / scale + q[9] / 255.0);
            }
            const double a = q[11] > 0.5 ? clamp01(alpha / scale + q[9] / 255.0) : clamp01(c[3]);
            return premultiplied(r, a);
        });
    case E::Median:
    case E::DustScratches:
        return perPixel(in, [&](int, int, double cx, double cy, const Rgba& c) {
            const double radius = q[0];
            const bool alpha = op == E::Median ? q[1] > 0.5 : q[2] > 0.5;
            if (radius < 0.5) {
                return c;
            }
            Rgba med = medianOf(in, cx, cy, std::max(radius, 1.0));
            if (op == E::DustScratches) {
                const Rgb e = encoded(c);
                const Rgb em = encoded(med);
                const double diff = std::max({std::abs(e[0] - em[0]), std::abs(e[1] - em[1]), std::abs(e[2] - em[2])});
                if (diff * 255.0 <= q[1]) {
                    med = c;
                }
            }
            return alpha ? med : premultiplied(encoded(med), clamp01(c[3]));
        });
    case E::AlphaGlow: {
        const Image b = gaussianBlurSigma(in, std::max(q[0], 0.0) / 2.0, false);
        const Rgb start = srgb3(q[2], q[3], q[4]);
        const Rgb end = srgb3(q[5], q[6], q[7]);
        return perPixel(in, [&](int x, int y, double, double, const Rgba& c) {
            const double g = clamp01(b.atD(x, y)[3]);
            const double k = q[9] > 0.5 ? g : (g > 0.004 ? 1.0 : 0.0);
            const Rgb color = q[8] > 0.5 ? mix3(end, start, g) : start;
            const double a = k * clamp01(q[1] / 255.0);
            return Rgba{c[0] + color[0] * a * (1.0 - c[3]), c[1] + color[1] * a * (1.0 - c[3]),
                        c[2] + color[2] * a * (1.0 - c[3]), c[3] + a * (1.0 - c[3])};
        });
    }
    case E::BevelAlpha: {
        const Image b = gaussianBlurSigma(in, std::max(q[0], 0.0), false);
        const Rgb light = srgb3(q[2], q[3], q[4]);
        return perPixel(in, [&](int, int, double cx, double cy, const Rgba& c) {
            const double gx = (at(b, cx + 1.0, cy)[3] - at(b, cx - 1.0, cy)[3]) * 0.5;
            const double gy = (at(b, cx, cy + 1.0)[3] - at(b, cx, cy - 1.0)[3]) * 0.5;
            const double a = radians(q[1]);
            const double shade =
                std::clamp(-(gx * std::sin(a) - gy * std::cos(a)) * std::max(q[0], 1.0) * 2.0, -1.0, 1.0) * q[5];
            Rgba out = c;
            for (std::size_t i = 0; i < 3; ++i) {
                out[i] = shade > 0.0 ? c[i] + (light[i] * c[3] - c[i]) * shade : c[i] * (1.0 + shade);
            }
            return out;
        });
    }
    case E::RadialShadow: {
        const double lx = q[4] * in.width;
        const double ly = q[5] * in.height;
        const double k = 1.0 + std::max(q[6], 0.0) / 100.0;
        const Rgb shadowColor = srgb3(q[0], q[1], q[2]);
        Image shadow = perPixel(in, [&](int, int, double cx, double cy, const Rgba&) {
            const Rgba src = at(in, lx + (cx - lx) / k, ly + (cy - ly) / k);
            Rgb sc = shadowColor;
            if (q[8] > 0.5 && src[3] > 0.0) {
                sc = mix3(sc, {src[0] / src[3], src[1] / src[3], src[2] / src[3]}, clamp01(q[9]));
            }
            const double sa = src[3] * q[3];
            return Rgba{sc[0] * sa, sc[1] * sa, sc[2] * sa, sa};
        });
        if (q[7] / 3.0 >= 0.05) {
            shadow = gaussianBlurSigma(shadow, q[7] / 3.0, false);
        }
        return perPixel(in, [&](int x, int y, double, double, const Rgba& c) {
            const Rgba s = shadow.atD(x, y);
            if (q[10] > 0.5) {
                return s;
            }
            return Rgba{c[0] + s[0] * (1.0 - c[3]), c[1] + s[1] * (1.0 - c[3]), c[2] + s[2] * (1.0 - c[3]),
                        c[3] + s[3] * (1.0 - c[3])};
        });
    }
    case E::RoughenEdges: {
        const Image b = gaussianBlurSigma(in, std::max(q[4], 0.0) / 2.0, false);
        return perPixel(in, [&](int x, int y, double cx, double cy, const Rgba& c) {
            const double ab = clamp01(b.atD(x, y)[3]);
            const int type = static_cast<int>(std::floor(q[0] + 0.5));
            const double scale = std::max(q[7], 1.0);
            const double stretch = std::exp2(q[8] * 0.25);
            const double vx = (cx - (q[9] - 0.5) * in.width) / scale / stretch;
            const double vy = (cy - (q[10] - 0.5) * in.height) / scale * stretch;
            const int period = q[13] > 0.5 ? std::max(1, static_cast<int>(std::floor(q[14] + 0.5))) : 0;
            double z = q[12] / 360.0;
            if (period > 0) {
                z = z - period * std::floor(z / period);
            }
            const double n = fbm(vx, vy, z, q[11], period, static_cast<std::uint32_t>(std::max(0.0, q[15])));
            double sharp = std::max(q[5], 0.0);
            if (type == 2 || type == 3) {
                sharp += 6.0;
            }
            const double w = 0.5 / (1.0 + 2.0 * sharp);
            const double level = ab + n * q[6] * 0.5;
            const double m = smoothstep(0.5 - w, 0.5 + w, level);
            Rgba rough{c[0] * m, c[1] * m, c[2] * m, c[3] * m};
            const bool colored = type == 1 || type == 5 || type == 7;
            if (colored && c[3] > 0.0) {
                const double edge = 1.0 - clamp01((level - 0.5) * 4.0);
                const Rgb ec = srgb3(q[1], q[2], q[3]);
                for (std::size_t i = 0; i < 3; ++i) {
                    rough[i] = rough[i] + (ec[i] * rough[3] - rough[i]) * edge;
                }
            }
            return rough;
        });
    }
    case E::ColorKey: {
        Image matte = perPixel(in, [&](int, int, double, double, const Rgba& c) {
            const Rgb e = encoded(c);
            const double d = std::max({std::abs(e[0] - q[0]), std::abs(e[1] - q[1]), std::abs(e[2] - q[2])}) * 255.0;
            const double m = smoothstep(q[3] - 0.5, q[3] + 0.5, d);
            return Rgba{m, m, m, m};
        });
        if (q[4] != 0.0) {
            const double r = std::abs(q[4]);
            const int reach = static_cast<int>(std::floor(r));
            const bool erode = q[4] > 0.0;
            const Image source = matte;
            matte = perPixel(source, [&](int, int, double cx, double cy, const Rgba& c) {
                double v = c[3];
                for (int j = -reach; j <= reach; ++j) {
                    for (int i = -reach; i <= reach; ++i) {
                        if (static_cast<double>(i * i + j * j) > r * r) {
                            continue;
                        }
                        const double s = at(source, cx + i, cy + j)[3];
                        v = erode ? std::min(v, s) : std::max(v, s);
                    }
                }
                return Rgba{v, v, v, v};
            });
        }
        if (q[5] >= 0.05) {
            matte = gaussianBlurSigma(matte, q[5], true);
        }
        return perPixel(in, [&](int x, int y, double, double, const Rgba& c) {
            const double m = clamp01(matte.atD(x, y)[3]);
            return Rgba{c[0] * m, c[1] * m, c[2] * m, c[3] * m};
        });
    }
    default:
        return in;
    }
}

} // namespace wabcut::render::reference::detail
