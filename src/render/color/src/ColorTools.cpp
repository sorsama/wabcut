#include <wabcut/color/ColorTools.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace wabcut::color {

namespace {

double weightedPercentile(std::vector<std::pair<double, double>> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    double total = 0.0;
    for (const auto& v : values) {
        total += v.second;
    }
    const double target = fraction * total;
    double acc = 0.0;
    for (const auto& v : values) {
        acc += v.second;
        if (acc >= target) {
            return v.first;
        }
    }
    return values.back().first;
}

double lumaOfLinear(const Rgb& c) { return luma(c); }

Rgb normalizedLuminance(const Rgb& c) {
    const double y = lumaOfLinear(c);
    return y > 1e-9 ? c * (1.0 / y) : Rgb{1, 1, 1};
}

double oklabChroma(const Rgb& linear) {
    const Rgb lab = linearToOklab({std::max(linear[0], 0.0), std::max(linear[1], 0.0), std::max(linear[2], 0.0)});
    return std::hypot(lab[1], lab[2]);
}

} // namespace

std::vector<ColorSample> samplePixels(const ColorImage& image, int maxSamples, const std::vector<float>* weights) {
    std::vector<ColorSample> out;
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        return out;
    }
    const double total = static_cast<double>(image.width) * image.height;
    const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(total / std::max(1, maxSamples)))));
    for (int y = step / 2; y < image.height; y += step) {
        for (int x = step / 2; x < image.width; x += step) {
            const std::size_t i = static_cast<std::size_t>(y) * image.width + x;
            const float* p = image.rgba.data() + i * 4;
            double w = std::clamp(static_cast<double>(p[3]), 0.0, 1.0);
            if (weights && i < weights->size()) {
                w *= (*weights)[i];
            }
            if (w <= 0.0) {
                continue;
            }
            out.push_back({{p[0], p[1], p[2]}, w});
        }
    }
    return out;
}

OklabStats oklabStats(const std::vector<Rgb>& linear, const std::vector<double>& weights) {
    OklabStats s;
    double total = 0.0;
    std::vector<Rgb> labs;
    labs.reserve(linear.size());
    for (std::size_t i = 0; i < linear.size(); ++i) {
        const Rgb c{std::max(linear[i][0], 0.0), std::max(linear[i][1], 0.0), std::max(linear[i][2], 0.0)};
        labs.push_back(linearToOklab(c));
        const double w = i < weights.size() ? weights[i] : 1.0;
        s.mean = s.mean + labs.back() * w;
        total += w;
    }
    if (total <= 0.0) {
        return s;
    }
    s.mean = s.mean * (1.0 / total);
    for (std::size_t i = 0; i < labs.size(); ++i) {
        const double w = i < weights.size() ? weights[i] : 1.0;
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = labs[i][c] - s.mean[c];
            s.deviation[c] += w * d * d;
        }
    }
    for (double& d : s.deviation) {
        d = std::sqrt(d / total);
    }
    return s;
}

double skinLikelihood(const Rgb& display) {
    const Hsv hsv = toHsv(display);
    const double hue = smoothstep(0.02, 0.05, hsv.h) * (1.0 - smoothstep(0.11, 0.15, hsv.h));
    const double sat = smoothstep(0.12, 0.2, hsv.s) * (1.0 - smoothstep(0.6, 0.75, hsv.s));
    const double lit = smoothstep(0.15, 0.25, hsv.v);
    return hue * sat * lit;
}

std::vector<double> minimize(const std::function<double(const std::vector<double>&)>& cost, std::vector<double> start,
                             const std::vector<double>& step, const std::vector<double>& lo, const std::vector<double>& hi,
                             int maxEvaluations) {
    const std::size_t n = start.size();
    const auto clampPoint = [&](std::vector<double>& p) {
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = std::clamp(p[i], lo[i], hi[i]);
        }
    };
    clampPoint(start);
    std::vector<std::vector<double>> simplex(n + 1, start);
    for (std::size_t i = 0; i < n; ++i) {
        simplex[i + 1][i] += step[i];
        if (simplex[i + 1][i] > hi[i]) {
            simplex[i + 1][i] = start[i] - step[i];
        }
        clampPoint(simplex[i + 1]);
    }
    std::vector<double> values(n + 1);
    int evaluations = 0;
    const auto f = [&](std::vector<double>& p) {
        clampPoint(p);
        ++evaluations;
        return cost(p);
    };
    for (std::size_t i = 0; i <= n; ++i) {
        values[i] = f(simplex[i]);
    }
    while (evaluations < maxEvaluations) {
        std::vector<std::size_t> order(n + 1);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return values[a] < values[b]; });
        const std::size_t best = order.front(), worst = order.back(), second = order[n - 1];
        if (std::abs(values[worst] - values[best]) < 1e-12 * (1.0 + std::abs(values[best]))) {
            break;
        }
        std::vector<double> centroid(n, 0.0);
        for (std::size_t i = 0; i <= n; ++i) {
            if (i == worst) {
                continue;
            }
            for (std::size_t k = 0; k < n; ++k) {
                centroid[k] += simplex[i][k] / static_cast<double>(n);
            }
        }
        const auto along = [&](double t) {
            std::vector<double> p(n);
            for (std::size_t k = 0; k < n; ++k) {
                p[k] = centroid[k] + t * (simplex[worst][k] - centroid[k]);
            }
            return p;
        };
        std::vector<double> reflected = along(-1.0);
        const double fr = f(reflected);
        if (fr < values[best]) {
            std::vector<double> expanded = along(-2.0);
            const double fe = f(expanded);
            if (fe < fr) {
                simplex[worst] = expanded, values[worst] = fe;
            } else {
                simplex[worst] = reflected, values[worst] = fr;
            }
        } else if (fr < values[second]) {
            simplex[worst] = reflected, values[worst] = fr;
        } else {
            std::vector<double> contracted = fr < values[worst] ? along(-0.5) : along(0.5);
            const double fc = f(contracted);
            if (fc < std::min(fr, values[worst])) {
                simplex[worst] = contracted, values[worst] = fc;
            } else {
                for (std::size_t i = 0; i <= n; ++i) {
                    if (i == best) {
                        continue;
                    }
                    for (std::size_t k = 0; k < n; ++k) {
                        simplex[i][k] = simplex[best][k] + 0.5 * (simplex[i][k] - simplex[best][k]);
                    }
                    values[i] = f(simplex[i]);
                }
            }
        }
    }
    const auto bestIt = std::min_element(values.begin(), values.end());
    return simplex[static_cast<std::size_t>(bestIt - values.begin())];
}

AutoResult autoCorrect(const std::vector<ColorSample>& samples, const GradeSpec& base) {
    AutoResult result;
    result.params = base.params;
    // The grade's input, linear, without the Basic stage this fit replaces.
    struct Px {
        Rgb lin;
        double w;
    };
    std::vector<Px> valid;
    std::vector<Px> all;
    for (const ColorSample& s : samples) {
        const Rgb lin = decode709(s.display);
        all.push_back({lin, s.weight});
        const double mx = std::max({s.display[0], s.display[1], s.display[2]});
        if (mx < 0.98 && luma(s.display) > 0.04) {
            valid.push_back({lin, s.weight});
        }
    }
    if (valid.size() < 16) {
        valid = all;
    }
    if (valid.empty()) {
        result.summary = "no pixels to analyse";
        return result;
    }
    // Illuminant: grey world on all, then iteratively on the most neutral third after correcting by the estimate,
    // blended with the brightest (white patch) pixels.
    Rgb illum{0, 0, 0};
    double total = 0.0;
    for (const Px& p : valid) {
        illum = illum + p.lin * p.w;
        total += p.w;
    }
    illum = normalizedLuminance(illum * (1.0 / total));
    for (int iteration = 0; iteration < 4; ++iteration) {
        std::vector<std::pair<double, double>> chroma;
        std::vector<std::pair<double, std::size_t>> bright;
        for (std::size_t i = 0; i < valid.size(); ++i) {
            const Rgb corrected{valid[i].lin[0] / illum[0], valid[i].lin[1] / illum[1], valid[i].lin[2] / illum[2]};
            chroma.push_back({oklabChroma(corrected), valid[i].w});
            bright.push_back({lumaOfLinear(corrected), i});
        }
        // The most neutral fifth, weighted towards the least saturated.
        const double cut = weightedPercentile(chroma, 0.2);
        Rgb grey{0, 0, 0};
        double greyWeight = 0.0;
        for (std::size_t i = 0; i < valid.size(); ++i) {
            if (chroma[i].first <= cut) {
                const double w = valid[i].w / (chroma[i].first + 0.005);
                grey = grey + normalizedLuminance(valid[i].lin) * w;
                greyWeight += w;
            }
        }
        std::sort(bright.begin(), bright.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const std::size_t topCount = std::max<std::size_t>(1, bright.size() * 3 / 100);
        Rgb white{0, 0, 0};
        for (std::size_t k = 0; k < topCount; ++k) {
            white = white + valid[bright[k].second].lin;
        }
        const Rgb g1 = normalizedLuminance(grey * (1.0 / std::max(greyWeight, 1e-12)));
        const Rgb g2 = normalizedLuminance(white);
        Rgb blend{};
        for (std::size_t c = 0; c < 3; ++c) {
            blend[c] = std::pow(std::max(g1[c], 1e-6), 0.7) * std::pow(std::max(g2[c], 1e-6), 0.3);
        }
        illum = normalizedLuminance(blend);
    }
    result.illuminant = illum;
    result.castBefore = oklabChroma(illum);
    // Temperature and tint that make the illuminant neutral.
    const auto castCost = [&](const std::vector<double>& v) {
        const Rgb balanced = mul(whiteBalanceMatrix(v[0], v[1]), illum);
        const Rgb lab = linearToOklab({std::max(balanced[0], 0.0), std::max(balanced[1], 0.0), std::max(balanced[2], 0.0)});
        return lab[1] * lab[1] + lab[2] * lab[2] + 1e-9 * (v[0] * v[0] + v[1] * v[1]);
    };
    const auto wb = minimize(castCost, {0.0, 0.0}, {25.0, 25.0}, {-100.0, -100.0}, {100.0, 100.0}, 400);
    result.params.temperature = std::round(wb[0] * 10.0) / 10.0;
    result.params.tint = std::round(wb[1] * 10.0) / 10.0;
    result.castAfter = std::sqrt(std::max(0.0, castCost({result.params.temperature, result.params.tint})));

    // Levels: exposure, whites and blacks against the histogram percentiles of the graded result.
    GradeSpec spec = base;
    spec.params = result.params;
    spec.params.basic = true;
    const auto levelsCost = [&](const std::vector<double>& v) {
        spec.params.exposure = v[0];
        spec.params.whites = v[1];
        spec.params.blacks = v[2];
        std::vector<std::pair<double, double>> lumas;
        lumas.reserve(all.size());
        for (const Px& p : all) {
            const Rgb out = encode709(gradePixel(spec, p.lin));
            lumas.push_back({luma(out), p.w});
        }
        const double lo = weightedPercentile(lumas, 0.005);
        const double hi = weightedPercentile(lumas, 0.995);
        const double mid = weightedPercentile(lumas, 0.5);
        // Highlights to 92 %, blacks towards 4 % (only pulled down, a lifted black stays a choice of the picture
        // when it would take the whole range of the control), the median drawn towards mid grey; small moves win.
        const double loExcess = std::max(0.0, lo - 0.04);
        return 4.0 * (hi - 0.92) * (hi - 0.92) + 1.0 * loExcess * loExcess + 0.3 * (mid - 0.42) * (mid - 0.42) +
               0.01 * ((v[1] / 100.0) * (v[1] / 100.0) + (v[2] / 100.0) * (v[2] / 100.0));
    };
    const auto levels = minimize(levelsCost, {base.params.exposure, base.params.whites, base.params.blacks}, {0.5, 25.0, 25.0},
                                 {-3.0, -100.0, -100.0}, {3.0, 100.0, 100.0}, 240);
    result.params.exposure = std::round(levels[0] * 100.0) / 100.0;
    result.params.whites = std::round(levels[1]);
    result.params.blacks = std::round(levels[2]);
    result.summary = std::format("white balance: temperature {:+.1f}, tint {:+.1f} (cast {:.3f} -> {:.3f}); exposure {:+.2f}, "
                                 "whites {:+.0f}, blacks {:+.0f}",
                                 result.params.temperature, result.params.tint, result.castBefore, result.castAfter,
                                 result.params.exposure, result.params.whites, result.params.blacks);
    return result;
}

MatchResult colorMatch(const std::vector<ColorSample>& current, const std::vector<ColorSample>& reference, const GradeSpec& base,
                       const MatchOptions& options) {
    MatchResult result;
    result.params = base.params;
    if (current.empty() || reference.empty()) {
        result.summary = "no pixels to match";
        return result;
    }
    const auto weightsOf = [&](const std::vector<ColorSample>& samples, const std::vector<double>& extra) {
        std::vector<double> w;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            double x = samples[i].weight * (i < extra.size() ? extra[i] : 1.0);
            if (options.skinPriority) {
                x *= 1.0 + 3.0 * skinLikelihood(samples[i].display);
            }
            w.push_back(x);
        }
        return w;
    };
    const std::vector<double> wCur = weightsOf(current, options.currentWeights);
    const std::vector<double> wRef = weightsOf(reference, options.referenceWeights);
    std::vector<Rgb> refLinear;
    for (const ColorSample& s : reference) {
        refLinear.push_back(decode709(s.display));
    }
    result.reference = oklabStats(refLinear, wRef);
    std::vector<Rgb> curInput;
    for (const ColorSample& s : current) {
        curInput.push_back(decode709(s.display));
    }
    GradeSpec spec = base;
    spec.params.basic = true;
    const auto gradedStats = [&]() {
        std::vector<Rgb> out;
        out.reserve(curInput.size());
        for (const Rgb& c : curInput) {
            out.push_back(gradePixel(spec, c));
        }
        return oklabStats(out, wCur);
    };
    result.current = gradedStats();
    const double s = std::clamp(options.strength, 0.0, 1.0);
    OklabStats& t = result.target;
    const Rgb maxShift{0.35, 0.1, 0.1};
    for (std::size_t c = 0; c < 3; ++c) {
        const double shift = std::clamp(result.reference.mean[c] - result.current.mean[c], -maxShift[c], maxShift[c]);
        t.mean[c] = result.current.mean[c] + s * shift;
        const double ratio = result.current.deviation[c] > 1e-6
                                 ? std::clamp(result.reference.deviation[c] / result.current.deviation[c], 0.5, 2.0)
                                 : 1.0;
        t.deviation[c] = result.current.deviation[c] * (1.0 + s * (ratio - 1.0));
    }
    const GradeParams start = base.params;
    const auto cost = [&](const std::vector<double>& v) {
        spec.params.exposure = v[0];
        spec.params.contrast = v[1];
        spec.params.temperature = v[2];
        spec.params.tint = v[3];
        spec.params.saturation = v[4];
        const OklabStats g = gradedStats();
        double e = 4.0 * std::pow(g.mean[0] - t.mean[0], 2) + 2.0 * std::pow(g.deviation[0] - t.deviation[0], 2);
        for (std::size_t c = 1; c < 3; ++c) {
            e += 30.0 * std::pow(g.mean[c] - t.mean[c], 2) + 10.0 * std::pow(g.deviation[c] - t.deviation[c], 2);
        }
        // Prefer small moves from where the grade started.
        e += 1e-6 * (std::pow(v[0] - start.exposure, 2) + std::pow((v[1] - start.contrast) / 50.0, 2) +
                     std::pow((v[2] - start.temperature) / 50.0, 2) + std::pow((v[3] - start.tint) / 50.0, 2) +
                     std::pow((v[4] - start.saturation) / 50.0, 2));
        return e;
    };
    const auto best = minimize(cost, {start.exposure, start.contrast, start.temperature, start.tint, start.saturation},
                               {0.4, 15.0, 20.0, 20.0, 20.0}, {-3.0, -60.0, -100.0, -100.0, 20.0}, {3.0, 60.0, 100.0, 100.0, 200.0},
                               500);
    result.params.exposure = std::round(best[0] * 100.0) / 100.0;
    result.params.contrast = std::round(best[1] * 10.0) / 10.0;
    result.params.temperature = std::round(best[2] * 10.0) / 10.0;
    result.params.tint = std::round(best[3] * 10.0) / 10.0;
    result.params.saturation = std::round(best[4] * 10.0) / 10.0;
    result.params.basic = true;
    spec.params = result.params;
    result.achieved = gradedStats();
    result.summary = std::format("exposure {:+.2f}, contrast {:+.1f}, temperature {:+.1f}, tint {:+.1f}, saturation {:.1f}; "
                                 "Oklab L {:.3f} -> {:.3f} (reference {:.3f})",
                                 result.params.exposure, result.params.contrast, result.params.temperature, result.params.tint,
                                 result.params.saturation, result.current.mean[0], result.achieved.mean[0], result.reference.mean[0]);
    return result;
}

} // namespace wabcut::color
