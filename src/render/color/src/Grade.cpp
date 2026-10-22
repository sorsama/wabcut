#include <wabcut/color/Grade.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

namespace wabcut::color {

namespace {

// ---- Parameter table: registry key <-> GradeParams field ----

struct ParamField {
    std::string_view key;
    double (*get)(const GradeParams&);
    void (*set)(GradeParams&, double);
};

#define WABCUT_FIELD(KEY, EXPR)                                                                                        \
    ParamField {                                                                                                       \
        KEY, [](const GradeParams& p) -> double { return static_cast<double>(p.EXPR); },                               \
            [](GradeParams& p, double v) { p.EXPR = v; }                                                               \
    }
#define WABCUT_SWITCH(KEY, EXPR)                                                                                       \
    ParamField {                                                                                                       \
        KEY, [](const GradeParams& p) -> double { return p.EXPR ? 1.0 : 0.0; },                                        \
            [](GradeParams& p, double v) { p.EXPR = v > 0.5; }                                                         \
    }

// Sorted by key (binary search).
const std::array<ParamField, 62> kFields = [] {
    std::array<ParamField, 62> f{{
        WABCUT_SWITCH("basic.enabled", basic),
        WABCUT_SWITCH("creative.enabled", creative),
        WABCUT_SWITCH("curves.enabled", curves),
        WABCUT_SWITCH("wheels.enabled", wheels),
        WABCUT_SWITCH("hsl.enabled", hsl),
        WABCUT_SWITCH("vignette.enabled", vignette),
        WABCUT_FIELD("basic.temperature", temperature),
        WABCUT_FIELD("basic.tint", tint),
        WABCUT_FIELD("basic.exposure", exposure),
        WABCUT_FIELD("basic.contrast", contrast),
        WABCUT_FIELD("basic.highlights", highlights),
        WABCUT_FIELD("basic.shadows", shadows),
        WABCUT_FIELD("basic.whites", whites),
        WABCUT_FIELD("basic.blacks", blacks),
        WABCUT_FIELD("basic.saturation", saturation),
        WABCUT_FIELD("basic.vibrance", vibrance),
        WABCUT_FIELD("creative.lookIntensity", lookIntensity),
        WABCUT_FIELD("creative.fadedFilm", fadedFilm),
        WABCUT_FIELD("creative.vibrance", creativeVibrance),
        WABCUT_FIELD("creative.saturation", creativeSaturation),
        WABCUT_FIELD("creative.shadowTint.hue", shadowTintHue),
        WABCUT_FIELD("creative.shadowTint.amount", shadowTintAmount),
        WABCUT_FIELD("creative.highlightTint.hue", highlightTintHue),
        WABCUT_FIELD("creative.highlightTint.amount", highlightTintAmount),
        WABCUT_FIELD("creative.tintBalance", tintBalance),
        WABCUT_FIELD("wheels.lift.red", lift[0]),
        WABCUT_FIELD("wheels.lift.green", lift[1]),
        WABCUT_FIELD("wheels.lift.blue", lift[2]),
        WABCUT_FIELD("wheels.gamma.red", gamma[0]),
        WABCUT_FIELD("wheels.gamma.green", gamma[1]),
        WABCUT_FIELD("wheels.gamma.blue", gamma[2]),
        WABCUT_FIELD("wheels.gain.red", gain[0]),
        WABCUT_FIELD("wheels.gain.green", gain[1]),
        WABCUT_FIELD("wheels.gain.blue", gain[2]),
        WABCUT_FIELD("wheels.offset.red", offset[0]),
        WABCUT_FIELD("wheels.offset.green", offset[1]),
        WABCUT_FIELD("wheels.offset.blue", offset[2]),
        WABCUT_FIELD("wheels.saturation", cdlSaturation),
        WABCUT_FIELD("hsl.hue.center", hueCenter),
        WABCUT_FIELD("hsl.hue.width", hueWidth),
        WABCUT_FIELD("hsl.hue.softness", hueSoftness),
        WABCUT_FIELD("hsl.sat.low", satLow),
        WABCUT_FIELD("hsl.sat.high", satHigh),
        WABCUT_FIELD("hsl.sat.softness", satSoftness),
        WABCUT_FIELD("hsl.luma.low", lumaLow),
        WABCUT_FIELD("hsl.luma.high", lumaHigh),
        WABCUT_FIELD("hsl.luma.softness", lumaSoftness),
        WABCUT_SWITCH("hsl.invert", invert),
        WABCUT_FIELD("hsl.denoise", denoise),
        WABCUT_FIELD("hsl.blur", blur),
        WABCUT_FIELD("hsl.temperature", hslTemperature),
        WABCUT_FIELD("hsl.tint", hslTint),
        WABCUT_FIELD("hsl.contrast", hslContrast),
        WABCUT_FIELD("hsl.sharpen", hslSharpen),
        WABCUT_FIELD("hsl.saturation", hslSaturation),
        WABCUT_FIELD("hsl.offset.red", hslOffset[0]),
        WABCUT_FIELD("hsl.offset.green", hslOffset[1]),
        WABCUT_FIELD("hsl.offset.blue", hslOffset[2]),
        WABCUT_FIELD("vignette.amount", vignetteAmount),
        WABCUT_FIELD("vignette.midpoint", vignetteMidpoint),
        WABCUT_FIELD("vignette.roundness", vignetteRoundness),
        WABCUT_FIELD("vignette.feather", vignetteFeather),
    }};
    std::sort(f.begin(), f.end(), [](const ParamField& a, const ParamField& b) { return a.key < b.key; });
    return f;
}();

#undef WABCUT_FIELD
#undef WABCUT_SWITCH

const ParamField* fieldOf(std::string_view key) {
    const auto it = std::lower_bound(kFields.begin(), kFields.end(), key,
                                     [](const ParamField& f, std::string_view k) { return f.key < k; });
    return it != kFields.end() && it->key == key ? &*it : nullptr;
}

// ---- Shape-preserving cubics (Fritsch-Carlson / PCHIP) ----

struct Spline {
    std::vector<double> x, y, m;
};

double sign(double v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; }

/// Interior tangent at k from the neighbouring secants (weighted harmonic mean; 0 at extrema).
double interiorTangent(double h0, double h1, double d0, double d1) {
    if (d0 * d1 <= 0.0) {
        return 0.0;
    }
    const double w1 = 2.0 * h1 + h0;
    const double w2 = h1 + 2.0 * h0;
    return (w1 + w2) / (w1 / d0 + w2 / d1);
}

/// One-sided end tangent (three-point, shape-preserving).
double endTangent(double h0, double h1, double d0, double d1) {
    double m = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (sign(m) != sign(d0)) {
        m = 0.0;
    } else if (sign(d0) != sign(d1) && std::abs(m) > std::abs(3.0 * d0)) {
        m = 3.0 * d0;
    }
    return m;
}

Spline pchip(const std::vector<EffectPoint>& points) {
    Spline s;
    for (const EffectPoint& p : points) {
        if (!s.x.empty() && !(p.x > s.x.back())) {
            s.y.back() = p.y; // duplicate or unsorted x: the last one wins
            continue;
        }
        s.x.push_back(p.x);
        s.y.push_back(p.y);
    }
    const std::size_t n = s.x.size();
    s.m.assign(n, 0.0);
    if (n < 2) {
        return s;
    }
    std::vector<double> h(n - 1), d(n - 1);
    for (std::size_t k = 0; k + 1 < n; ++k) {
        h[k] = s.x[k + 1] - s.x[k];
        d[k] = (s.y[k + 1] - s.y[k]) / h[k];
    }
    if (n == 2) {
        s.m[0] = s.m[1] = d[0];
        return s;
    }
    for (std::size_t k = 1; k + 1 < n; ++k) {
        s.m[k] = interiorTangent(h[k - 1], h[k], d[k - 1], d[k]);
    }
    s.m[0] = endTangent(h[0], h[1], d[0], d[1]);
    s.m[n - 1] = endTangent(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
    return s;
}

double hermite(const Spline& s, std::size_t k, double x) {
    const double h = s.x[k + 1] - s.x[k];
    const double t = (x - s.x[k]) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * s.y[k] + (t3 - 2 * t2 + t) * h * s.m[k] + (-2 * t3 + 3 * t2) * s.y[k + 1] +
           (t3 - t2) * h * s.m[k + 1];
}

/// The spline inside its points, flat outside.
double evaluateFlat(const Spline& s, double x) {
    const std::size_t n = s.x.size();
    if (x <= s.x.front()) {
        return s.y.front();
    }
    if (x >= s.x.back()) {
        return s.y.back();
    }
    const auto it = std::upper_bound(s.x.begin(), s.x.end(), x);
    return hermite(s, std::min(static_cast<std::size_t>(it - s.x.begin()) - 1, n - 2), x);
}

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* b = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ b[i]) * 1099511628211ull;
    }
    return h;
}

std::uint64_t hashPoints(std::uint64_t h, const std::vector<EffectPoint>& points) {
    const std::uint64_t n = points.size();
    h = fnv(h, &n, sizeof n);
    for (const EffectPoint& p : points) {
        h = fnv(h, &p.x, sizeof p.x);
        h = fnv(h, &p.y, sizeof p.y);
    }
    return h;
}

std::uint64_t hashCurves(const GradeCurves& c) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto* list : {&c.master, &c.red, &c.green, &c.blue, &c.hueSat, &c.hueHue, &c.hueLuma, &c.lumaSat, &c.satSat}) {
        h = hashPoints(h, *list);
    }
    return h;
}

double skinWeight(double hueTurns) { return smoothstep(0.03, 0.07, hueTurns) * (1.0 - smoothstep(0.12, 0.16, hueTurns)); }

/// Saturation (and vibrance) about the luma of `c`; the weights come from `display` (the encoded value).
Rgb saturate(const Rgb& c, const Rgb& display, double saturation, double vibrance) {
    double factor = saturation / 100.0;
    if (vibrance != 0.0) {
        const Hsv hsv = toHsv(display);
        double v = vibrance / 100.0 * (1.0 - hsv.s) * (1.0 - hsv.s);
        if (vibrance > 0.0) {
            v *= 1.0 - 0.7 * skinWeight(hsv.h);
        }
        factor *= 1.0 + v;
    }
    factor = std::max(factor, 0.0);
    const double y = luma(c);
    return {y + (c[0] - y) * factor, y + (c[1] - y) * factor, y + (c[2] - y) * factor};
}

double rangeMask(double x, double lo, double hi, double soft) {
    if (soft <= 0.0) {
        return x >= lo && x <= hi ? 1.0 : 0.0;
    }
    return smoothstep(lo - soft, lo, x) * (1.0 - smoothstep(hi, hi + soft, x));
}

bool cdlIdentity(const GradeParams& p) {
    return p.lift == Rgb{0, 0, 0} && p.gamma == Rgb{1, 1, 1} && p.gain == Rgb{1, 1, 1} && p.offset == Rgb{0, 0, 0} &&
           p.cdlSaturation == 1.0;
}

bool secondaryNeutral(const GradeParams& p) {
    return p.hslTemperature == 0 && p.hslTint == 0 && p.hslContrast == 0 && p.hslSharpen <= 0 && p.hslSaturation == 100 &&
           p.hslOffset == Rgb{0, 0, 0};
}

} // namespace

// ---- Curves ----

bool GradeCurves::rgbIdentity() const { return master.empty() && red.empty() && green.empty() && blue.empty(); }
bool GradeCurves::hueNeutral() const { return hueSat.empty() && hueHue.empty() && hueLuma.empty(); }
bool GradeCurves::satNeutral() const { return lumaSat.empty() && satSat.empty(); }

double evaluateCurve(const std::vector<EffectPoint>& points, double x) {
    if (points.empty()) {
        return x;
    }
    const Spline s = pchip(points);
    if (x < 0.0) {
        return evaluateFlat(s, 0.0) + x;
    }
    if (x > 1.0) {
        return evaluateFlat(s, 1.0) + (x - 1.0);
    }
    return evaluateFlat(s, x);
}

double evaluateNeutralCurve(const std::vector<EffectPoint>& points, double x) {
    if (points.empty()) {
        return 0.5;
    }
    return evaluateFlat(pchip(points), std::clamp(x, 0.0, 1.0));
}

namespace {

/// The periodic spline of a hue curve: points wrapped into [0, 1) and repeated one period each way, so every real
/// point is interior. Null when the curve is constant (no or one point; `constant` holds its value).
std::optional<Spline> periodicSpline(const std::vector<EffectPoint>& points, double& constant) {
    constant = points.empty() ? 0.5 : points.front().y;
    if (points.size() < 2) {
        return std::nullopt;
    }
    std::vector<EffectPoint> wrapped;
    for (const EffectPoint& p : points) {
        wrapped.push_back({p.x - std::floor(p.x), p.y});
    }
    std::sort(wrapped.begin(), wrapped.end(), [](const EffectPoint& a, const EffectPoint& b) { return a.x < b.x; });
    std::vector<EffectPoint> extended;
    extended.push_back({wrapped[wrapped.size() - 2].x - 1.0, wrapped[wrapped.size() - 2].y});
    extended.push_back({wrapped.back().x - 1.0, wrapped.back().y});
    extended.insert(extended.end(), wrapped.begin(), wrapped.end());
    extended.push_back({wrapped[0].x + 1.0, wrapped[0].y});
    extended.push_back({wrapped[1].x + 1.0, wrapped[1].y});
    return pchip(extended);
}

double evaluatePeriodic(const std::optional<Spline>& s, double constant, double turns) {
    return s ? evaluateFlat(*s, turns - std::floor(turns)) : constant;
}

} // namespace

double evaluateHueCurve(const std::vector<EffectPoint>& points, double turns) {
    double constant = 0.5;
    const auto s = periodicSpline(points, constant);
    return evaluatePeriodic(s, constant, turns);
}

std::shared_ptr<const BakedCurves> bakeCurves(const GradeCurves& curves) {
    auto baked = std::make_shared<BakedCurves>();
    const int n = BakedCurves::kSize;
    baked->rgbm.resize(static_cast<std::size_t>(n) * 4);
    baked->hue.resize(static_cast<std::size_t>(n) * 4);
    baked->sat.resize(static_cast<std::size_t>(n) * 4);
    const Spline master = pchip(curves.master), red = pchip(curves.red), green = pchip(curves.green), blue = pchip(curves.blue);
    double hueConst[3] = {0.5, 0.5, 0.5};
    const std::optional<Spline> hueSplines[3] = {periodicSpline(curves.hueSat, hueConst[0]),
                                                 periodicSpline(curves.hueHue, hueConst[1]),
                                                 periodicSpline(curves.hueLuma, hueConst[2])};
    const Spline lumaSat = pchip(curves.lumaSat), satSat = pchip(curves.satSat);
    const auto neutral = [](const std::vector<EffectPoint>& pts, const Spline& s, double x) { return pts.empty() ? 0.5 : evaluateFlat(s, x); };
    const auto rgb = [](const std::vector<EffectPoint>& pts, const Spline& s, double x) { return pts.empty() ? x : evaluateFlat(s, x); };
    double rgbError = 0.0, hueError = 0.0, satError = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / (n - 1);
        const std::size_t k = static_cast<std::size_t>(i) * 4;
        const double values[4] = {rgb(curves.red, red, x), rgb(curves.green, green, x), rgb(curves.blue, blue, x),
                                  rgb(curves.master, master, x)};
        for (std::size_t c = 0; c < 4; ++c) {
            baked->rgbm[k + c] = static_cast<float>(values[c]);
            rgbError = std::max(rgbError, std::abs(values[c] - x));
        }
        const double h = static_cast<double>(i) / n;
        const double hueValues[3] = {evaluatePeriodic(hueSplines[0], hueConst[0], h),
                                     evaluatePeriodic(hueSplines[1], hueConst[1], h),
                                     evaluatePeriodic(hueSplines[2], hueConst[2], h)};
        for (std::size_t c = 0; c < 3; ++c) {
            baked->hue[k + c] = static_cast<float>(hueValues[c]);
            hueError = std::max(hueError, std::abs(hueValues[c] - 0.5));
        }
        baked->hue[k + 3] = 0.0f;
        const double satValues[2] = {neutral(curves.lumaSat, lumaSat, x), neutral(curves.satSat, satSat, x)};
        baked->sat[k] = static_cast<float>(satValues[0]);
        baked->sat[k + 1] = static_cast<float>(satValues[1]);
        baked->sat[k + 2] = baked->sat[k + 3] = 0.0f;
        satError = std::max({satError, std::abs(satValues[0] - 0.5), std::abs(satValues[1] - 0.5)});
    }
    baked->rgbIdentity = rgbError < 1e-7;
    baked->hueNeutral = hueError < 1e-7;
    baked->satNeutral = satError < 1e-7;
    std::uint64_t h = 1469598103934665603ull;
    for (const auto* row : {&baked->rgbm, &baked->hue, &baked->sat}) {
        h = fnv(h, row->data(), row->size() * sizeof(float));
    }
    baked->key = h;
    return baked;
}

double sampleRow(const std::vector<float>& row, int stride, int c, double x, bool periodic) {
    const int n = static_cast<int>(row.size()) / stride;
    if (n < 2) {
        return n == 1 ? row[static_cast<std::size_t>(c)] : 0.0;
    }
    double pos = 0.0;
    int i0 = 0, i1 = 0;
    if (periodic) {
        pos = (x - std::floor(x)) * n;
        i0 = std::min(static_cast<int>(std::floor(pos)), n - 1);
        i1 = (i0 + 1) % n;
    } else {
        pos = std::clamp(x, 0.0, 1.0) * (n - 1);
        i0 = std::min(static_cast<int>(std::floor(pos)), n - 2);
        i1 = i0 + 1;
    }
    const double t = pos - i0;
    const double a = row[static_cast<std::size_t>(i0 * stride + c)];
    const double b = row[static_cast<std::size_t>(i1 * stride + c)];
    return a + (b - a) * t;
}

double sampleRgbCurve(const BakedCurves& baked, int c, double x) {
    if (x < 0.0) {
        return sampleRow(baked.rgbm, 4, c, 0.0, false) + x;
    }
    if (x > 1.0) {
        return sampleRow(baked.rgbm, 4, c, 1.0, false) + (x - 1.0);
    }
    return sampleRow(baked.rgbm, 4, c, x, false);
}

// ---- GradeSpec ----

bool GradeSpec::inputLutActive() const { return params.basic && inputLut && !inputLut->empty(); }

bool GradeSpec::basicActive() const {
    const GradeParams& p = params;
    return p.basic && (p.temperature != 0 || p.tint != 0 || p.exposure != 0 || p.contrast != 0 || p.highlights != 0 ||
                       p.shadows != 0 || p.whites != 0 || p.blacks != 0 || p.saturation != 100 || p.vibrance != 0);
}

bool GradeSpec::creativeActive() const {
    const GradeParams& p = params;
    return p.creative && ((look && !look->empty()) || p.fadedFilm > 0 || p.shadowTintAmount > 0 ||
                          p.highlightTintAmount > 0 || p.creativeSaturation != 100 || p.creativeVibrance != 0);
}

bool GradeSpec::curvesActive() const {
    return params.curves && curves && !(curves->rgbIdentity && curves->hueNeutral && curves->satNeutral);
}

bool GradeSpec::wheelsActive() const { return params.wheels && !cdlIdentity(params); }

bool GradeSpec::hslActive() const { return params.hsl && !secondaryNeutral(params); }

bool GradeSpec::vignetteActive() const { return params.vignette && params.vignetteAmount != 0; }

bool GradeSpec::active() const {
    return inputLutActive() || basicActive() || creativeActive() || curvesActive() || wheelsActive() || hslActive() ||
           vignetteActive();
}

bool GradeSpec::hslNeedsRefine() const {
    return hslActive() && (params.denoise > 0 || params.blur > 0 || params.hslSharpen > 0);
}

// ---- Reading the effect ----

GradeParams gradeParamsAt(const EffectInstance& instance, Ticks curveTime) {
    GradeParams p;
    for (const EffectParam& param : instance.params) {
        if (const ParamField* field = fieldOf(param.key)) {
            field->set(p, param.value.valueAt(curveTime));
        }
    }
    return p;
}

GradeCurves gradeCurvesOf(const EffectInstance& instance) {
    GradeCurves c;
    const auto read = [&](std::string_view key, std::vector<EffectPoint>& out) {
        if (const EffectData* d = instance.findData(key); d && d->points()) {
            out = *d->points();
        }
    };
    read(gradekey::master, c.master);
    read(gradekey::red, c.red);
    read(gradekey::green, c.green);
    read(gradekey::blue, c.blue);
    read(gradekey::hueSat, c.hueSat);
    read(gradekey::hueHue, c.hueHue);
    read(gradekey::hueLuma, c.hueLuma);
    read(gradekey::lumaSat, c.lumaSat);
    read(gradekey::satSat, c.satSat);
    return c;
}

std::vector<EffectParam> gradeParamsToEffect(const GradeParams& params, bool onlyChanged) {
    const GradeParams defaults;
    std::vector<EffectParam> out;
    for (const ParamField& f : kFields) { // sorted by key, as EffectInstance::params wants
        const double v = f.get(params);
        if (!onlyChanged || v != f.get(defaults)) {
            out.push_back(EffectParam{std::string(f.key), ParamValue(v)});
        }
    }
    return out;
}

GradeSpec evaluateGrade(const EffectInstance& instance, Ticks curveTime, const LutResolver& luts) {
    GradeSpec spec;
    spec.params = gradeParamsAt(instance, curveTime);
    const GradeCurves curves = gradeCurvesOf(instance);
    if (!(curves == GradeCurves{})) {
        // Baked once per distinct set of curves (they are not keyframable), shared by every frame and clip.
        static std::mutex mutex;
        static std::map<std::uint64_t, std::shared_ptr<const BakedCurves>> cache;
        const std::uint64_t key = hashCurves(curves);
        std::lock_guard lock(mutex);
        if (const auto it = cache.find(key); it != cache.end()) {
            spec.curves = it->second;
        } else {
            if (cache.size() >= 256) {
                cache.clear();
            }
            spec.curves = bakeCurves(curves);
            cache.emplace(key, spec.curves);
        }
    }
    const auto resolve = [&](std::string_view key) -> std::shared_ptr<const Lut> {
        const EffectData* d = instance.findData(key);
        if (!d || !d->text() || d->text()->empty()) {
            return nullptr;
        }
        std::shared_ptr<const Lut> lut = luts ? luts(*d->text()) : nullptr;
        if (!lut) {
            ++spec.missingLuts;
        }
        return lut;
    };
    spec.inputLut = resolve(gradekey::inputLut);
    spec.look = resolve(gradekey::look);
    return spec;
}

// ---- CPU reference ----

namespace {

/// The tone curve's spline for (blacks, shadows, highlights, whites), cached per thread (the reference evaluates it
/// per pixel; the tools call it millions of times with the same values).
const Spline& toneSpline(const GradeParams& p) {
    static constexpr double kAmount = 0.12;
    thread_local std::array<double, 4> key{std::nan(""), 0, 0, 0};
    thread_local Spline spline;
    const std::array<double, 4> now{p.blacks, p.shadows, p.highlights, p.whites};
    if (now != key) {
        key = now;
        spline = pchip({{0.0, kAmount * p.blacks / 100.0},
                        {0.25, 0.25 + kAmount * p.shadows / 100.0},
                        {0.5, 0.5},
                        {0.75, 0.75 + kAmount * p.highlights / 100.0},
                        {1.0, 1.0 + kAmount * p.whites / 100.0}});
    }
    return spline;
}

const Mat3& cachedWhiteBalance(double temperature, double tint) {
    thread_local double t = std::nan(""), g = std::nan("");
    thread_local Mat3 m{};
    if (temperature != t || tint != g) {
        t = temperature;
        g = tint;
        m = whiteBalanceMatrix(temperature, tint);
    }
    return m;
}

} // namespace

std::array<double, 10> toneSplineCoefficients(const GradeParams& p) {
    const Spline& s = toneSpline(p);
    std::array<double, 10> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[i] = s.y[i];
        out[i + 5] = s.m[i];
    }
    return out;
}

double toneCurve(const GradeParams& p, double x) {
    // Monotone cubic through five points; straight lines with the end tangents outside [0, 1].
    const Spline& s = toneSpline(p);
    if (x < 0.0) {
        return s.y.front() + s.m.front() * x;
    }
    if (x > 1.0) {
        return s.y.back() + s.m.back() * (x - 1.0);
    }
    return evaluateFlat(s, x);
}

Rgb gradeMain(const GradeSpec& spec, const Rgb& linear) {
    const auto encodeW = [&](const Rgb& l) { return encodeWorking(l, spec.encoding); };
    const auto decodeW = [&](const Rgb& v) { return decodeWorking(v, spec.encoding); };
    const GradeParams& p = spec.params;
    Rgb c = linear;
    if (spec.inputLutActive()) {
        c = decodeW(applyLut(*spec.inputLut, encodeW(c), spec.interpolation));
    }
    if (spec.basicActive()) {
        if (p.temperature != 0 || p.tint != 0) {
            c = mul(cachedWhiteBalance(p.temperature, p.tint), c);
        }
        c = c * std::exp2(p.exposure);
        if (p.contrast != 0) {
            const double k = 1.0 + p.contrast / 100.0;
            for (double& v : c) {
                v = v <= 0.0 ? v : 0.18 * std::pow(v / 0.18, k);
            }
        }
        if (p.highlights != 0 || p.shadows != 0 || p.whites != 0 || p.blacks != 0) {
            Rgb e = encodeW(c);
            const double y = luma(e);
            const double d = toneCurve(p, y) - y;
            c = decodeW({e[0] + d, e[1] + d, e[2] + d});
        }
        if (p.saturation != 100 || p.vibrance != 0) {
            c = saturate(c, encodeW(c), p.saturation, p.vibrance);
        }
    }
    Rgb e = encodeW(c);
    if (spec.creativeActive()) {
        if (spec.look && !spec.look->empty()) {
            const Rgb l = applyLut(*spec.look, e, spec.interpolation);
            e = e + (l - e) * (p.lookIntensity / 100.0);
        }
        if (p.fadedFilm > 0) {
            const double f = p.fadedFilm / 100.0;
            for (double& v : e) {
                const double x = std::clamp(v, 0.0, 1.0);
                v += 0.2 * f * (1.0 - x) * (1.0 - x);
            }
        }
        if (p.shadowTintAmount > 0 || p.highlightTintAmount > 0) {
            const double u = std::clamp(luma(e) + 0.5 * p.tintBalance / 100.0, 0.0, 1.0);
            const double ws = (1.0 - u) * (1.0 - u) * p.shadowTintAmount / 100.0 * 0.25;
            const double wh = u * u * p.highlightTintAmount / 100.0 * 0.25;
            e = e + hueChroma(p.shadowTintHue / 360.0) * ws + hueChroma(p.highlightTintHue / 360.0) * wh;
        }
        if (p.creativeSaturation != 100 || p.creativeVibrance != 0) {
            e = saturate(e, e, p.creativeSaturation, p.creativeVibrance);
        }
    }
    if (spec.curvesActive()) {
        const BakedCurves& b = *spec.curves;
        if (!b.rgbIdentity) {
            for (int ch = 0; ch < 3; ++ch) {
                const double m = sampleRgbCurve(b, 3, e[static_cast<std::size_t>(ch)]);
                e[static_cast<std::size_t>(ch)] = sampleRgbCurve(b, ch, m);
            }
        }
        if (!b.hueNeutral || !b.satNeutral) {
            const Hsv hsv = toHsv(e);
            const double y = luma(e);
            const double hueSat = sampleRow(b.hue, 4, 0, hsv.h, true);
            const double hueHue = sampleRow(b.hue, 4, 1, hsv.h, true);
            const double hueLuma = sampleRow(b.hue, 4, 2, hsv.h, true);
            const double lumaSat = sampleRow(b.sat, 4, 0, std::clamp(y, 0.0, 1.0), false);
            const double satSat = sampleRow(b.sat, 4, 1, hsv.s, false);
            const double factor = (2.0 * hueSat) * (2.0 * lumaSat) * (2.0 * satSat);
            const Rgb r = rotateHue(e, hueHue - 0.5);
            e = {y + (r[0] - y) * factor, y + (r[1] - y) * factor, y + (r[2] - y) * factor};
            const double dl = (hueLuma - 0.5) * hsv.s * 0.5;
            e = {e[0] + dl, e[1] + dl, e[2] + dl};
        }
    }
    if (spec.wheelsActive()) {
        for (std::size_t ch = 0; ch < 3; ++ch) {
            const double slope = p.gain[ch] - p.lift[ch];
            const double off = p.lift[ch] + p.offset[ch];
            e[ch] = std::pow(std::clamp(e[ch] * slope + off, 0.0, 1.0), p.gamma[ch]);
        }
        if (p.cdlSaturation != 1.0) {
            const double y = luma(e);
            for (double& v : e) {
                v = std::clamp(y + (v - y) * p.cdlSaturation, 0.0, 1.0);
            }
        }
    }
    return e;
}

double qualifierMatte(const GradeParams& p, const Rgb& display) {
    const Hsv hsv = toHsv(display);
    double mh = 1.0;
    if (p.hueWidth < 360.0) {
        double d = std::abs(hsv.h * 360.0 - p.hueCenter);
        d = std::fmod(d, 360.0);
        d = std::min(d, 360.0 - d);
        const double inner = p.hueWidth / 2.0;
        mh = p.hueSoftness <= 0.0 ? (d <= inner ? 1.0 : 0.0) : 1.0 - smoothstep(inner, inner + p.hueSoftness, d);
    }
    const double s = hsv.s * 100.0;
    const double l = std::clamp(luma(display), 0.0, 1.0) * 100.0;
    double m = mh * rangeMask(s, p.satLow, p.satHigh, p.satSoftness) * rangeMask(l, p.lumaLow, p.lumaHigh, p.lumaSoftness);
    if (p.invert) {
        m = 1.0 - m;
    }
    return m;
}

double denoiseThreshold(double matte) { return smoothstep(0.25, 0.75, matte); }

double denoiseSigma(const GradeParams& p) { return std::max(0.0, p.denoise) / 10.0; }
double refineBlurSigma(const GradeParams& p) { return std::max(0.0, p.blur) / 3.0; }

Rgb applySecondary(const GradeParams& p, const Rgb& display, double matte, const Rgb& blurred, GradeEncoding encoding) {
    Rgb c = display;
    if (p.hslTemperature != 0 || p.hslTint != 0) {
        c = encodeWorking(mul(cachedWhiteBalance(p.hslTemperature, p.hslTint), decodeWorking(c, encoding)), encoding);
    }
    if (p.hslContrast != 0) {
        const double k = 1.0 + p.hslContrast / 100.0;
        for (double& v : c) {
            v = (v - 0.5) * k + 0.5;
        }
    }
    if (p.hslSharpen > 0) {
        c = c + (display - blurred) * (2.0 * p.hslSharpen / 100.0);
    }
    if (p.hslSaturation != 100) {
        const double y = luma(c);
        const double k = p.hslSaturation / 100.0;
        c = {y + (c[0] - y) * k, y + (c[1] - y) * k, y + (c[2] - y) * k};
    }
    c = c + p.hslOffset;
    return mix(display, c, std::clamp(matte, 0.0, 1.0));
}

Rgb applyVignette(const GradeParams& p, const Rgb& linear, double qx, double qy, double aspect) {
    const double roundness = p.vignetteRoundness / 100.0;
    qx *= 1.0 + (aspect - 1.0) * std::max(roundness, 0.0);
    const double n = 2.0 + 6.0 * std::max(-roundness, 0.0);
    const double d = std::pow(std::pow(std::abs(qx), n) + std::pow(std::abs(qy), n), 1.0 / n);
    const double mid = 0.3 + 0.9 * p.vignetteMidpoint / 100.0;
    const double width = std::max(0.01, p.vignetteFeather / 100.0);
    const double v = smoothstep(mid - 0.5 * width, mid + 0.5 * width, d);
    return linear * std::exp2(p.vignetteAmount * v);
}

Rgb gradePixel(const GradeSpec& spec, const Rgb& linear, double qx, double qy, double aspect) {
    Rgb e = gradeMain(spec, linear);
    if (spec.hslActive()) {
        e = applySecondary(spec.params, e, qualifierMatte(spec.params, e), e, spec.encoding);
    }
    Rgb c = decodeWorking(e, spec.encoding);
    if (spec.vignetteActive()) {
        c = applyVignette(spec.params, c, qx, qy, aspect);
    }
    return c;
}

} // namespace wabcut::color
