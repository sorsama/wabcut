#include <wabcut/media/RawDevelop.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace wabcut::media {

namespace {

/// The colour (0 R, 1 G, 2 B) of each site of the 2 x 2 pattern: [row parity][column parity].
std::array<std::array<int, 2>, 2> sitesOf(BayerPattern p) {
    switch (p) {
    case BayerPattern::BGGR:
        return {{{2, 1}, {1, 0}}};
    case BayerPattern::GRBG:
        return {{{1, 0}, {2, 1}}};
    case BayerPattern::GBRG:
        return {{{1, 2}, {0, 1}}};
    case BayerPattern::RGGB:
        break;
    }
    return {{{0, 1}, {1, 2}}};
}

double bt709Oetf(double l) {
    l = std::clamp(l, 0.0, 1.0);
    return l < 0.018 ? 4.5 * l : 1.099 * std::pow(l, 0.45) - 0.099;
}

constexpr std::array<double, 9> kXyzToRec709{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
                                            0.0415560, 0.0556434,  -0.2040259, 1.0572252};
constexpr std::array<double, 9> kRec709ToXyz{0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522,
                                            0.0721750, 0.0193339, 0.1191920, 0.9503041};
// Bradford's cone response and its inverse.
constexpr std::array<double, 9> kBradford{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
constexpr std::array<double, 9> kBradfordInverse{0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603,
                                                 0.0492912, -0.0085287, 0.0400428, 0.9684867};

std::array<double, 9> multiply(const std::array<double, 9>& a, const std::array<double, 9>& b) {
    std::array<double, 9> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double v = 0.0;
            for (int k = 0; k < 3; ++k) {
                v += a[static_cast<std::size_t>(r * 3 + k)] * b[static_cast<std::size_t>(k * 3 + c)];
            }
            out[static_cast<std::size_t>(r * 3 + c)] = v;
        }
    }
    return out;
}

/// The cone response (Bradford) of the light of chromaticity `xy` at Y = 1.
std::array<double, 3> coneResponse(const std::array<double, 2>& xy) {
    const double y = std::max(1e-6, xy[1]);
    const std::array<double, 3> xyz{xy[0] / y, 1.0, (1.0 - xy[0] - xy[1]) / y};
    std::array<double, 3> out{};
    for (std::size_t r = 0; r < 3; ++r) {
        out[r] = kBradford[r * 3] * xyz[0] + kBradford[r * 3 + 1] * xyz[1] + kBradford[r * 3 + 2] * xyz[2];
    }
    return out;
}

/// The Planckian locus at `kelvin` (Kim et al. 2002), xy.
std::array<double, 2> planckian(double kelvin) {
    const double t = std::clamp(kelvin, 1667.0, 25000.0);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x;
    const double x3 = x2 * x;
    const double y = t <= 2222.0   ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
                     : t <= 4000.0 ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
                                   : 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    return {x, y};
}

std::array<double, 2> toUv(const std::array<double, 2>& xy) {
    const double d = -2.0 * xy[0] + 12.0 * xy[1] + 3.0;
    return {4.0 * xy[0] / d, 6.0 * xy[1] / d};
}

} // namespace

std::array<double, 2> lightChromaticity(double kelvin, double tint) {
    const double t = std::clamp(kelvin, 1667.0, 25000.0);
    const std::array<double, 2> xy = planckian(t);
    if (tint == 0.0) {
        return xy;
    }
    // The normal to the locus in uv, on its green side (larger v).
    const std::array<double, 2> uv = toUv(xy);
    const double step = t < 24000.0 ? 50.0 : -50.0;
    const std::array<double, 2> next = toUv(planckian(t + step));
    double du = next[0] - uv[0];
    double dv = next[1] - uv[1];
    const double length = std::hypot(du, dv);
    if (length <= 0.0) {
        return xy;
    }
    du /= length;
    dv /= length;
    std::array<double, 2> normal{-dv, du};
    if (normal[1] < 0.0) {
        normal = {dv, -du};
    }
    const double duv = tint / 3000.0;
    const double u = uv[0] + normal[0] * duv;
    const double v = uv[1] + normal[1] * duv;
    const double d = 2.0 * u - 8.0 * v + 4.0;
    return {3.0 * u / d, 2.0 * v / d};
}

std::array<double, 9> whiteBalanceAdaptation(double shotKelvin, double kelvin, double tint) {
    const std::array<double, 3> shot = coneResponse(lightChromaticity(shotKelvin, 0.0));
    const std::array<double, 3> chosen = coneResponse(lightChromaticity(kelvin, tint));
    std::array<double, 9> scale{};
    for (std::size_t i = 0; i < 3; ++i) {
        scale[i * 3 + i] = chosen[i] > 0.0 ? shot[i] / chosen[i] : 1.0;
    }
    return multiply(kBradfordInverse, multiply(scale, kBradford));
}

void developRaw(const std::uint16_t* cfa, std::size_t strideSamples, int width, int height, int maxValue,
                const RawDevelopParams& params, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr,
                std::size_t outStride) {
    if (width < 2 || height < 2 || maxValue <= 0) {
        return;
    }
    const auto sites = sitesOf(params.pattern);
    const double range = std::max(1e-9, params.white - params.black);
    const double scale = 1.0 / (static_cast<double>(maxValue) * range);
    const double offset = params.black / range;
    const std::array<double, 3> wb{params.wbRed, 1.0, params.wbBlue};
    // Camera RGB -> linear Rec.709 in one matrix (identity without a camera matrix or a white balance setting), with
    // gain and exposure.
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (params.hasMatrix || params.whiteAdaptation) {
        const std::array<double, 9> toXyz = params.hasMatrix ? params.cameraToXyz : kRec709ToXyz;
        const std::array<double, 9> adapt = params.whiteAdaptation.value_or(std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1});
        m = multiply(kXyzToRec709, multiply(adapt, toXyz));
    }
    const double boost = params.gain * std::exp2(params.exposureStops);
    for (double& v : m) {
        v *= boost;
    }
    // Normalised, white-balanced value of the site at (x, y), edges clamped.
    const auto value = [&](int x, int yy) {
        x = std::clamp(x, 0, width - 1);
        yy = std::clamp(yy, 0, height - 1);
        const int colour = sites[static_cast<std::size_t>(yy & 1)][static_cast<std::size_t>(x & 1)];
        const double s = cfa[static_cast<std::size_t>(yy) * strideSamples + static_cast<std::size_t>(x)] * scale - offset;
        return std::max(0.0, s) * wb[static_cast<std::size_t>(colour)];
    };
    const auto rows = [&](int from, int to) {
        for (int yy = from; yy < to; ++yy) {
            for (int x = 0; x < width; ++x) {
                // Bilinear demosaic: each missing colour is the mean of that colour's sites in the 3 x 3 neighbourhood
                // (four orthogonal greens, two reds / blues beside a green, four diagonal ones beside the other colour).
                std::array<double, 3> sum{};
                std::array<int, 3> count{};
                const int own = sites[static_cast<std::size_t>(yy & 1)][static_cast<std::size_t>(x & 1)];
                std::array<double, 3> rgb{};
                rgb[static_cast<std::size_t>(own)] = value(x, yy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int c = sites[static_cast<std::size_t>((yy + dy) & 1)][static_cast<std::size_t>((x + dx) & 1)];
                        if (c != own) {
                            sum[static_cast<std::size_t>(c)] += value(x + dx, yy + dy);
                            ++count[static_cast<std::size_t>(c)];
                        }
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    if (c != own) {
                        rgb[static_cast<std::size_t>(c)] = count[static_cast<std::size_t>(c)] > 0
                                                               ? sum[static_cast<std::size_t>(c)] / count[static_cast<std::size_t>(c)]
                                                               : 0.0;
                    }
                }
                const double r = bt709Oetf(m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2]);
                const double g = bt709Oetf(m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2]);
                const double b = bt709Oetf(m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2]);
                const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                const std::size_t o = static_cast<std::size_t>(yy) * outStride + static_cast<std::size_t>(x);
                y[o] = static_cast<std::uint16_t>(std::lround(64.0 + 876.0 * luma));
                cb[o] = static_cast<std::uint16_t>(std::lround(std::clamp(512.0 + 896.0 * (b - luma) / 1.8556, 64.0, 960.0)));
                cr[o] = static_cast<std::uint16_t>(std::lround(std::clamp(512.0 + 896.0 * (r - luma) / 1.5748, 64.0, 960.0)));
            }
        }
    };
    const int threads = static_cast<long long>(width) * height >= 1'000'000
                            ? std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 8)
                            : 1;
    if (threads == 1) {
        rows(0, height);
        return;
    }
    std::vector<std::jthread> pool;
    const int band = (height + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int from = t * band;
        const int to = std::min(height, from + band);
        if (from < to) {
            pool.emplace_back([&, from, to] { rows(from, to); });
        }
    }
}

} // namespace wabcut::media
