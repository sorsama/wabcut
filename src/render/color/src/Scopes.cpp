#include <wabcut/color/Scopes.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::color {

namespace {

constexpr double kCb = 1.8556;
constexpr double kCr = 1.5748;

int level(double v, int levels) {
    const double c = std::clamp(v, 0.0, 1.0);
    return std::min(levels - 1, static_cast<int>(std::floor(c * levels)));
}

/// Vectorscope bin of a chroma component (zoomed): centred on zero.
int vbin(double c, int size) {
    const double pos = std::floor(static_cast<double>(size) * (0.5 + c) + 0.5);
    return static_cast<int>(std::clamp(pos, 0.0, static_cast<double>(size - 1)));
}

void allocate(ScopeData& d) {
    const ScopeSettings& s = d.settings;
    const std::size_t wave = static_cast<std::size_t>(s.columns) * static_cast<std::size_t>(s.levels);
    d.waveLuma.assign(wave, 0);
    for (auto& w : d.waveRgb) {
        w.assign(wave, 0);
    }
    for (auto& w : d.waveCbCr) {
        w.assign(wave, 0);
    }
    d.vector.assign(static_cast<std::size_t>(s.vectorSize) * static_cast<std::size_t>(s.vectorSize), 0);
    for (auto& h : d.histogram) {
        h.assign(static_cast<std::size_t>(s.levels), 0);
    }
}

/// One pixel (R'G'B' as given) at source column x.
void add(ScopeData& d, int x, double r, double g, double b) {
    const ScopeSettings& s = d.settings;
    const int column = std::min(s.columns - 1, static_cast<int>(static_cast<std::int64_t>(x) * s.columns / d.sourceWidth));
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double cb = (b - y) / kCb;
    const double cr = (r - y) / kCr;
    const auto wave = [&](std::vector<std::uint32_t>& w, double v) {
        ++w[static_cast<std::size_t>(level(v, s.levels)) * static_cast<std::size_t>(s.columns) + static_cast<std::size_t>(column)];
    };
    wave(d.waveLuma, y);
    wave(d.waveRgb[0], r);
    wave(d.waveRgb[1], g);
    wave(d.waveRgb[2], b);
    wave(d.waveCbCr[0], cb + 0.5);
    wave(d.waveCbCr[1], cr + 0.5);
    const int vx = vbin(cb * s.vectorZoom, s.vectorSize);
    const int vy = vbin(-cr * s.vectorZoom, s.vectorSize);
    ++d.vector[static_cast<std::size_t>(vy) * static_cast<std::size_t>(s.vectorSize) + static_cast<std::size_t>(vx)];
    ++d.histogram[0][static_cast<std::size_t>(level(r, s.levels))];
    ++d.histogram[1][static_cast<std::size_t>(level(g, s.levels))];
    ++d.histogram[2][static_cast<std::size_t>(level(b, s.levels))];
    ++d.histogram[3][static_cast<std::size_t>(level(y, s.levels))];
    ++d.pixels;
}

double percentile(const std::vector<std::uint32_t>& hist, std::uint64_t total, double fraction) {
    if (total == 0) {
        return 0.0;
    }
    const double target = fraction * static_cast<double>(total);
    double cumulative = 0.0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const double next = cumulative + hist[i];
        if (next >= target && hist[i] > 0) {
            // Linear inside the bin.
            const double t = (target - cumulative) / hist[i];
            return (static_cast<double>(i) + std::clamp(t, 0.0, 1.0)) / static_cast<double>(hist.size());
        }
        cumulative = next;
    }
    return 1.0;
}

std::uint8_t intensity(std::uint32_t count, double scale) {
    if (count == 0) {
        return 0;
    }
    // The log of the count over the peak's, square-rooted and lifted: a few hits next to a flat area's thousands
    // still read clearly, like a phosphor trace.
    const double t = std::clamp(std::log1p(static_cast<double>(count)) * scale, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(255.0 * (0.4 + 0.6 * std::sqrt(t))));
}

double logScale(const std::vector<std::uint32_t>& counts) {
    const std::uint32_t peak = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
    return peak > 0 ? 1.0 / std::log1p(static_cast<double>(peak)) : 0.0;
}

} // namespace

int scopeStep(const ScopeSettings& settings, int width, int height) {
    if (settings.step > 0) {
        return settings.step;
    }
    int step = 1;
    while (static_cast<std::int64_t>((width + step - 1) / step) * ((height + step - 1) / step) > 600000) {
        ++step;
    }
    return step;
}

ScopeData computeScopes(const float* pixels, int width, int height, int channels, const ScopeSettings& settings) {
    ScopeData d;
    d.settings = settings;
    d.sourceWidth = width;
    d.sourceHeight = height;
    d.step = scopeStep(settings, width, height);
    allocate(d);
    if (!pixels || width <= 0 || height <= 0) {
        return d;
    }
    for (int y = 0; y < height; y += d.step) {
        const float* row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
        for (int x = 0; x < width; x += d.step) {
            const float* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels);
            if (channels == 4 && p[3] <= 0.0f) {
                continue;
            }
            add(d, x, p[0], p[1], p[2]);
        }
    }
    return d;
}

ScopeData computeScopes8(const std::uint8_t* rgba, int width, int height, int stride, const ScopeSettings& settings) {
    ScopeData d;
    d.settings = settings;
    d.sourceWidth = width;
    d.sourceHeight = height;
    d.step = scopeStep(settings, width, height);
    allocate(d);
    if (!rgba || width <= 0 || height <= 0) {
        return d;
    }
    for (int y = 0; y < height; y += d.step) {
        const std::uint8_t* row = rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; x += d.step) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            if (p[3] == 0) {
                continue;
            }
            add(d, x, p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
        }
    }
    return d;
}

ScopeStats scopeStats(const ScopeData& d) {
    ScopeStats s;
    s.pixels = d.pixels;
    if (d.pixels == 0) {
        s.exposure = "empty";
        s.cast = "neutral";
        return s;
    }
    const auto& luma = d.histogram[3];
    const double n = static_cast<double>(d.pixels);
    const double levels = static_cast<double>(d.settings.levels);
    double sum = 0.0;
    int lo = -1, hi = -1;
    for (std::size_t i = 0; i < luma.size(); ++i) {
        if (luma[i] > 0) {
            lo = lo < 0 ? static_cast<int>(i) : lo;
            hi = static_cast<int>(i);
        }
        sum += (static_cast<double>(i) + 0.5) / levels * luma[i];
    }
    s.lumaMin = 100.0 * lo / levels;
    s.lumaMax = 100.0 * (hi + 1) / levels;
    s.lumaMean = 100.0 * sum / n;
    s.p01 = 100.0 * percentile(luma, d.pixels, 0.01);
    s.p05 = 100.0 * percentile(luma, d.pixels, 0.05);
    s.p50 = 100.0 * percentile(luma, d.pixels, 0.50);
    s.p95 = 100.0 * percentile(luma, d.pixels, 0.95);
    s.p99 = 100.0 * percentile(luma, d.pixels, 0.99);
    // Clipping: the top bin of any channel (>= 99.6 % at 256 levels), the bottom bin of luma.
    std::uint64_t top = 0;
    for (int c = 0; c < 3; ++c) {
        top = std::max<std::uint64_t>(top, d.histogram[static_cast<std::size_t>(c)].back());
    }
    s.clippedHigh = static_cast<double>(top) / n;
    s.clippedLow = static_cast<double>(luma.front()) / n;
    for (int c = 0; c < 3; ++c) {
        double m = 0.0;
        const auto& h = d.histogram[static_cast<std::size_t>(c)];
        for (std::size_t i = 0; i < h.size(); ++i) {
            m += (static_cast<double>(i) + 0.5) / levels * h[i];
        }
        s.meanRgb[static_cast<std::size_t>(c)] = m / n;
    }
    // Chroma from the vectorscope bins (bin centres, zoom undone).
    const int size = d.settings.vectorSize;
    double sumCb = 0.0, sumCr = 0.0, sumChroma = 0.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::uint32_t count = d.vector[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)];
            if (count == 0) {
                continue;
            }
            const double cb = (static_cast<double>(x) / size - 0.5) / d.settings.vectorZoom;
            const double cr = (0.5 - static_cast<double>(y) / size) / d.settings.vectorZoom;
            sumCb += cb * count;
            sumCr += cr * count;
            sumChroma += std::hypot(cb, cr) * count;
        }
    }
    s.saturationMean = 100.0 * sumChroma / n;
    const double mcb = sumCb / n, mcr = sumCr / n;
    s.castStrength = 100.0 * std::hypot(mcb, mcr);
    s.castHue = std::fmod(std::atan2(mcr, mcb) * 180.0 / std::numbers::pi + 360.0, 360.0);

    if (s.p50 < 25.0 && s.p99 < 70.0) {
        s.exposure = "underexposed";
    } else if (s.p50 > 75.0 && s.p01 > 30.0) {
        s.exposure = "overexposed";
    } else if (s.clippedHigh > 0.05) {
        s.exposure = "clipped highlights";
    } else if (s.clippedLow > 0.10) {
        s.exposure = "crushed blacks";
    } else if (s.p99 - s.p01 < 40.0) {
        s.exposure = "low contrast";
    } else {
        s.exposure = "ok";
    }
    if (s.castStrength < 1.5) {
        s.cast = "neutral";
    } else {
        // Hue names around the vectorscope (Cb right, Cr up): magenta ~ 50, red ~ 103, skin/orange ~ 123, yellow ~ 175,
        // green ~ 230, cyan ~ 283, blue ~ 355 degrees.
        const double h = s.castHue;
        s.cast = h < 25 || h >= 330 ? "blue"
                 : h < 80           ? "magenta"
                 : h < 113          ? "red"
                 : h < 150          ? "warm/orange"
                 : h < 200          ? "yellow"
                 : h < 260          ? "green"
                                    : "cyan";
    }
    return s;
}

std::array<double, 2> vectorscopeTarget(const Rgb& color, double amount) {
    const Rgb c = color * amount;
    const double y = luma(c);
    return {(c[2] - y) / kCb, (c[0] - y) / kCr};
}

std::vector<std::uint8_t> renderWaveform(const ScopeData& d, int mode, int& width, int& height) {
    const int columns = d.settings.columns;
    const int levels = d.settings.levels;
    const int panels = mode == 0 ? 1 : 3;
    width = columns * panels;
    height = levels;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    for (int panel = 0; panel < panels; ++panel) {
        const std::vector<std::uint32_t>& counts = mode == 0   ? d.waveLuma
                                                   : mode == 1 ? d.waveRgb[static_cast<std::size_t>(panel)]
                                                   : panel == 0 ? d.waveLuma
                                                                : d.waveCbCr[static_cast<std::size_t>(panel - 1)];
        const double scale = logScale(counts);
        std::array<std::uint8_t, 3> tint{235, 235, 235};
        if (mode == 1) {
            tint = panel == 0 ? std::array<std::uint8_t, 3>{255, 90, 80}
                   : panel == 1 ? std::array<std::uint8_t, 3>{90, 235, 110}
                                : std::array<std::uint8_t, 3>{100, 140, 255};
        } else if (mode == 2 && panel > 0) {
            tint = panel == 1 ? std::array<std::uint8_t, 3>{120, 170, 255} : std::array<std::uint8_t, 3>{255, 120, 150};
        }
        for (int l = 0; l < levels; ++l) {
            for (int c = 0; c < columns; ++c) {
                const std::uint8_t v = intensity(counts[static_cast<std::size_t>(l) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(c)], scale);
                if (v == 0) {
                    continue;
                }
                const int row = levels - 1 - l; // high levels at the top
                std::uint8_t* p = out.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                                                 static_cast<std::size_t>(panel * columns + c)) * 4;
                p[0] = static_cast<std::uint8_t>(tint[0] * v / 255);
                p[1] = static_cast<std::uint8_t>(tint[1] * v / 255);
                p[2] = static_cast<std::uint8_t>(tint[2] * v / 255);
                p[3] = 255;
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> renderVectorscope(const ScopeData& d, int& width, int& height) {
    const int size = d.settings.vectorSize;
    width = height = size;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    const double scale = logScale(d.vector);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::uint8_t v = intensity(d.vector[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)], scale);
            if (v == 0) {
                continue;
            }
            // Colour each bin by its own hue at a fixed luma, so the trace reads like the colours it shows.
            const double cb = (static_cast<double>(x) / size - 0.5) / d.settings.vectorZoom;
            const double cr = (0.5 - static_cast<double>(y) / size) / d.settings.vectorZoom;
            const double yl = 0.6;
            const double r = std::clamp(yl + 1.5748 * cr, 0.0, 1.0);
            const double b = std::clamp(yl + 1.8556 * cb, 0.0, 1.0);
            const double g = std::clamp((yl - 0.2126 * r - 0.0722 * b) / 0.7152, 0.0, 1.0);
            std::uint8_t* p = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4;
            p[0] = static_cast<std::uint8_t>((80 + 175 * r) * v / 255);
            p[1] = static_cast<std::uint8_t>((80 + 175 * g) * v / 255);
            p[2] = static_cast<std::uint8_t>((80 + 175 * b) * v / 255);
            p[3] = 255;
        }
    }
    return out;
}

std::vector<std::uint8_t> renderHistogram(const ScopeData& d, int& width, int& height) {
    const int levels = d.settings.levels;
    width = levels;
    height = 128;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    std::uint32_t peak = 1;
    for (const auto& h : d.histogram) {
        for (const std::uint32_t c : h) {
            peak = std::max(peak, c);
        }
    }
    const double scale = 1.0 / std::log1p(static_cast<double>(peak));
    const std::array<std::array<int, 3>, 4> colours{{{255, 70, 70}, {70, 230, 90}, {80, 120, 255}, {220, 220, 220}}};
    for (int x = 0; x < levels; ++x) {
        for (std::size_t c = 0; c < 4; ++c) {
            const double v = std::log1p(static_cast<double>(d.histogram[c][static_cast<std::size_t>(x)])) * scale;
            const int bar = static_cast<int>(std::round(v * (height - 1)));
            for (int y = 0; y < bar; ++y) {
                std::uint8_t* p = out.data() + (static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
                // Additive, so overlapping channels mix towards white.
                p[0] = static_cast<std::uint8_t>(std::min(255, p[0] + colours[c][0] / (c == 3 ? 3 : 2)));
                p[1] = static_cast<std::uint8_t>(std::min(255, p[1] + colours[c][1] / (c == 3 ? 3 : 2)));
                p[2] = static_cast<std::uint8_t>(std::min(255, p[2] + colours[c][2] / (c == 3 ? 3 : 2)));
                p[3] = 255;
            }
        }
    }
    return out;
}

ScopeData pooledScopes(const ScopeData& d, int columnFactor, int levelFactor, int vectorFactor) {
    const auto valid = [](int factor, int size) { return factor > 1 && size > 0 && size % factor == 0 ? factor : 1; };
    const int cf = valid(columnFactor, d.settings.columns);
    const int lf = valid(levelFactor, d.settings.levels);
    const int vf = valid(vectorFactor, d.settings.vectorSize);
    // Sums a width x height grid (row-major) into (width / fx) x (height / fy).
    const auto pool = [](const std::vector<std::uint32_t>& in, int width, int height, int fx, int fy) {
        const std::size_t w = static_cast<std::size_t>(width / fx);
        std::vector<std::uint32_t> out(w * static_cast<std::size_t>(height / fy), 0);
        if (in.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
            return out;
        }
        for (int y = 0; y < height; ++y) {
            const std::uint32_t* row = in.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            std::uint32_t* target = out.data() + static_cast<std::size_t>(y / fy) * w;
            for (int x = 0; x < width; ++x) {
                target[x / fx] += row[x];
            }
        }
        return out;
    };
    ScopeData out = d;
    out.settings.columns = d.settings.columns / cf;
    out.settings.levels = d.settings.levels / lf;
    out.settings.vectorSize = d.settings.vectorSize / vf;
    const int columns = d.settings.columns, levels = d.settings.levels;
    out.waveLuma = pool(d.waveLuma, columns, levels, cf, lf);
    for (std::size_t c = 0; c < 3; ++c) {
        out.waveRgb[c] = pool(d.waveRgb[c], columns, levels, cf, lf);
    }
    for (std::size_t c = 0; c < 2; ++c) {
        out.waveCbCr[c] = pool(d.waveCbCr[c], columns, levels, cf, lf);
    }
    out.vector = pool(d.vector, d.settings.vectorSize, d.settings.vectorSize, vf, vf);
    for (std::size_t c = 0; c < 4; ++c) {
        out.histogram[c] = pool(d.histogram[c], levels, 1, lf, 1);
    }
    return out;
}

} // namespace wabcut::color
