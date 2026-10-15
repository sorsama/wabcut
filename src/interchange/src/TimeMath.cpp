#include "TimeMath.h"

#include "TextUtil.h"

#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace wabcut::interchange::detail {

std::optional<MulDivResult> mulDivNearest(std::int64_t a, std::int64_t b, std::int64_t c) {
    const auto nearest = mulDiv(a, b, c, Rounding::Nearest);
    if (!nearest) {
        return std::nullopt;
    }
    // Exact when rounding down and up agree (both fit whenever the nearest value does, up to one unit).
    const auto floor = mulDiv(a, b, c, Rounding::Floor);
    const auto ceil = mulDiv(a, b, c, Rounding::Ceil);
    return MulDivResult{*nearest, floor && ceil && *floor == *ceil};
}

std::optional<Ticks> framesToTicksChecked(std::int64_t frames, Rate rate) {
    if (!rate.isValid()) {
        return std::nullopt;
    }
    return framesToTicks(frames, rate, Rounding::Nearest);
}

std::optional<MulDivResult> secondsToTicks(std::int64_t num, std::int64_t den) {
    if (den <= 0) {
        return std::nullopt;
    }
    return mulDivNearest(num, kTicksPerSecond, den);
}

std::optional<Rate> rateFromDouble(double fps) {
    if (!std::isfinite(fps) || fps <= 0.0 || fps > 1'000'000.0) {
        return std::nullopt;
    }
    const double whole = std::round(fps);
    if (whole >= 1.0 && std::abs(fps - whole) < 1e-6) {
        return Rate{static_cast<std::int32_t>(whole), 1};
    }
    // NTSC family: n * 1000 / 1001 (23.976, 29.97, 47.952, 59.94, 119.88, ...). Files often store a
    // truncated decimal (29.97), so the tolerance is loose; real rates are never that close to them.
    const double ntsc = std::round(fps * 1.001);
    if (ntsc >= 1.0 && ntsc <= 2'000'000.0 &&
        std::abs(fps - ntsc / 1.001) < 1e-4 * std::max(1.0, fps / 30.0)) {
        return Rate{static_cast<std::int32_t>(ntsc) * 1000, 1001}.normalized();
    }
    for (const std::int32_t den : {10, 100, 1000}) {
        const double num = std::round(fps * den);
        if (num >= 1.0 && num < 2'000'000'000.0 && std::abs(fps - num / den) < 1e-9 * fps) {
            return Rate{static_cast<std::int32_t>(num), den}.normalized();
        }
    }
    return std::nullopt;
}

std::optional<MulDivResult> rationalTimeToTicks(double value, double rate) {
    const auto r = rateFromDouble(rate);
    if (!r || !std::isfinite(value) || std::abs(value) > 9.0e15) {
        return std::nullopt;
    }
    const auto perSecond = checkedMul(kTicksPerSecond, r->den);
    if (!perSecond) {
        return std::nullopt;
    }
    const double integral = std::trunc(value);
    const auto base = mulDivNearest(static_cast<std::int64_t>(integral), *perSecond, r->num);
    if (!base) {
        return std::nullopt;
    }
    const double fraction = value - integral;
    if (fraction == 0.0) {
        return base;
    }
    // A sub-frame remainder: |fraction| < 1 frame, so double precision is far below one tick here.
    const double extra = std::round(fraction * static_cast<double>(*perSecond) / static_cast<double>(r->num));
    const auto sum = checkedAdd(base->value, static_cast<std::int64_t>(extra));
    if (!sum) {
        return std::nullopt;
    }
    return MulDivResult{*sum, false};
}

std::optional<Rate> rateFromTimebase(std::int64_t timebase, bool ntsc) {
    if (timebase <= 0 || timebase > 10'000) {
        return std::nullopt;
    }
    const auto tb = static_cast<std::int32_t>(timebase);
    return ntsc ? Rate{tb * 1000, 1001}.normalized() : Rate{tb, 1};
}

std::string rateLabel(Rate rate) {
    if (!rate.isValid()) {
        return "?";
    }
    if (rate.den == 1) {
        return std::to_string(rate.num);
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", rate.toDouble());
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

bool smoothRemapKeys(Curve& remap, const std::vector<bool>& smooth) {
    const std::size_t n = remap.keys.size();
    if (n < 2 || smooth.size() != n || std::ranges::none_of(smooth, [](bool s) { return s; })) {
        return false;
    }
    std::vector<double> secant(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double seconds = static_cast<double>((remap.keys[i + 1].time - remap.keys[i].time).value) /
                               static_cast<double>(kTicksPerSecond);
        if (!(seconds > 0.0)) {
            return false;
        }
        secant[i] = (remap.keys[i + 1].value - remap.keys[i].value) / seconds;
    }
    Curve curved = remap;
    for (std::size_t i = 0; i < n; ++i) {
        if (!smooth[i] || curved.keys[i].interpolation == Interpolation::Hold) {
            continue;
        }
        double slope = 0.0;
        if (i == 0) {
            slope = secant.front();
        } else if (i + 1 == n) {
            slope = secant.back();
        } else if (const double a = secant[i - 1], b = secant[i]; a * b > 0.0) {
            slope = 2.0 * a * b / (a + b);
        }
        Keyframe& k = curved.keys[i];
        k.interpolation = Interpolation::Bezier;
        k.in = BezierHandle{slope, 1.0 / 3.0};
        k.out = BezierHandle{slope, 1.0 / 3.0};
    }
    if (remapProblem(curved)) {
        return false;
    }
    remap = std::move(curved);
    return true;
}

std::optional<MulDivResult> parseFcpxTime(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.back() == 's') {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::string_view numerator = text;
    std::string_view denominator;
    if (const auto slash = text.find('/'); slash != std::string_view::npos) {
        numerator = text.substr(0, slash);
        denominator = text.substr(slash + 1);
    }
    std::int64_t den = 1;
    if (!denominator.empty() || text.find('/') != std::string_view::npos) {
        const auto d = parseInt64(denominator);
        if (!d || *d <= 0) {
            return std::nullopt;
        }
        den = *d;
    }
    // The numerator may be a decimal ("3.5"): scale it to an integer.
    std::int64_t num = 0;
    if (const auto dot = numerator.find('.'); dot != std::string_view::npos) {
        const std::string_view fraction = numerator.substr(dot + 1);
        if (fraction.empty() || fraction.size() > 9 ||
            fraction.find_first_not_of("0123456789") != std::string_view::npos) {
            return std::nullopt;
        }
        std::string digits(numerator.substr(0, dot));
        if (digits.empty() || digits == "-" || digits == "+") {
            digits += '0';
        }
        digits += fraction;
        const auto n = parseInt64(digits);
        if (!n) {
            return std::nullopt;
        }
        std::int64_t scale = 1;
        for (std::size_t i = 0; i < fraction.size(); ++i) {
            scale *= 10;
        }
        const auto scaledDen = checkedMul(den, scale);
        if (!scaledDen) {
            return std::nullopt;
        }
        num = *n;
        den = *scaledDen;
    } else {
        const auto n = parseInt64(numerator);
        if (!n) {
            return std::nullopt;
        }
        num = *n;
    }
    return secondsToTicks(num, den);
}

std::optional<Speed> speedFromRatio(std::int64_t num, std::int64_t den) {
    if (num <= 0 || den <= 0) {
        return std::nullopt;
    }
    const std::int64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
    if (num > kMax || den > kMax) {
        return std::nullopt;
    }
    return Speed{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)};
}

std::optional<ApproximateSpeed> speedFromDouble(double ratio) {
    if (!std::isfinite(ratio) || ratio <= 0.0 || ratio > 1e6) {
        return std::nullopt;
    }
    // Continued fraction convergents h/k of `ratio`, the last one with k <= 100000.
    constexpr std::int64_t kMaxDen = 100000;
    std::int64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    double x = ratio;
    for (int i = 0; i < 64; ++i) {
        const double a = std::floor(x);
        if (a > 1e12) {
            break;
        }
        const auto ai = static_cast<std::int64_t>(a);
        const std::int64_t h2 = ai * h1 + h0;
        const std::int64_t k2 = ai * k1 + k0;
        if (k2 > kMaxDen) {
            break;
        }
        h0 = h1;
        h1 = h2;
        k0 = k1;
        k1 = k2;
        const double frac = x - a;
        if (frac < 1e-12 || std::abs(static_cast<double>(h1) / static_cast<double>(k1) - ratio) <= 1e-12 * ratio) {
            break;
        }
        x = 1.0 / frac;
    }
    if (h1 <= 0 || k1 <= 0) {
        return std::nullopt;
    }
    const auto speed = speedFromRatio(h1, k1);
    if (!speed) {
        return std::nullopt;
    }
    const double value = static_cast<double>(speed->num) / static_cast<double>(speed->den);
    return ApproximateSpeed{*speed, std::abs(value - ratio) <= 1e-9 * ratio};
}

std::string speedLabel(Speed speed) {
    if (speed.den <= 0) {
        return "?";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", 100.0 * static_cast<double>(speed.num) / static_cast<double>(speed.den));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text + "%";
}

} // namespace wabcut::interchange::detail
