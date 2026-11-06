#include <wabcut/audio/edit/MatchCurve.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wabcut::audio::edit {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

double matchBandHz(int band) noexcept { return 31.25 * std::pow(2.0, static_cast<double>(band) / 3.0); }

// ---- The curve ------------------------------------------------------------------------------------------------------

EqMatchCurve fitEqMatch(const SpectrumProfile& reference, const SpectrumProfile& target, const EqMatchOptions& options) {
    EqMatchCurve curve;
    curve.rawDb.fill(kNaN);
    if (!reference.usable() || !target.usable()) {
        return curve;
    }
    std::array<double, kMatchBands> diff{};
    std::array<bool, kMatchBands> known{};
    int count = 0;
    for (int i = 0; i < kMatchBands; ++i) {
        const auto b = static_cast<std::size_t>(kMatchFirstBand + i);
        const double r = reference.levelDb[b];
        const double t = target.levelDb[b];
        if (std::isfinite(r) && std::isfinite(t)) {
            diff[static_cast<std::size_t>(i)] = r - t;
            curve.rawDb[b] = r - t;
            known[static_cast<std::size_t>(i)] = true;
            ++count;
        }
    }
    if (count < kMatchBands / 2) {
        return curve;
    }
    // Holes (bands above a low sample rate's range) follow their nearest known neighbours.
    for (int i = 0; i < kMatchBands; ++i) {
        if (known[static_cast<std::size_t>(i)]) {
            continue;
        }
        int lo = i - 1;
        while (lo >= 0 && !known[static_cast<std::size_t>(lo)]) {
            --lo;
        }
        int hi = i + 1;
        while (hi < kMatchBands && !known[static_cast<std::size_t>(hi)]) {
            ++hi;
        }
        if (lo >= 0 && hi < kMatchBands) {
            const double t = static_cast<double>(i - lo) / (hi - lo);
            diff[static_cast<std::size_t>(i)] = diff[static_cast<std::size_t>(lo)] * (1.0 - t) + diff[static_cast<std::size_t>(hi)] * t;
        } else {
            diff[static_cast<std::size_t>(i)] = diff[static_cast<std::size_t>(lo >= 0 ? lo : hi)];
        }
    }
    // The overall level difference is Level Match's business.
    double mean = 0.0;
    for (const double d : diff) {
        mean += d;
    }
    mean /= kMatchBands;
    curve.levelOffsetDb = mean;
    for (double& d : diff) {
        d -= mean;
    }
    // Smoothing: [1 2 1] / 4 passes, the edges repeating themselves.
    const int passes = static_cast<int>(std::lround(std::clamp(options.smoothing, 0.0, 1.0) * 4.0));
    for (int p = 0; p < passes; ++p) {
        std::array<double, kMatchBands> next{};
        for (int i = 0; i < kMatchBands; ++i) {
            const double a = diff[static_cast<std::size_t>(std::max(i - 1, 0))];
            const double c = diff[static_cast<std::size_t>(std::min(i + 1, kMatchBands - 1))];
            next[static_cast<std::size_t>(i)] = 0.25 * a + 0.5 * diff[static_cast<std::size_t>(i)] + 0.25 * c;
        }
        diff = next;
    }
    const double limit = std::clamp(options.maxGainDb, 0.0, kMatchMaxGainDb);
    const double amount = std::clamp(options.amount, 0.0, 1.0);
    for (int i = 0; i < kMatchBands; ++i) {
        curve.gainDb[static_cast<std::size_t>(kMatchFirstBand + i)] =
            std::clamp(diff[static_cast<std::size_t>(i)] * amount, -limit, limit);
    }
    // The bands beside the matched ones fade out: below, where a boost would only lift rumble, at half per band; above, where
    // the last matched band is the plateau of a shelf and nothing measurable is left to disagree, at 0.9 per band (a steep
    // fade curls the matched top band's own upper edge back towards 0 dB: it has no filter above it to hold it).
    const double lowEdge = curve.gainDb[static_cast<std::size_t>(kMatchFirstBand)];
    for (int b = kMatchFirstBand - 1, k = 1; b >= 0; --b, ++k) {
        curve.gainDb[static_cast<std::size_t>(b)] = lowEdge * std::pow(0.5, k);
    }
    const double highEdge = curve.gainDb[static_cast<std::size_t>(kMatchFirstBand + kMatchBands - 1)];
    for (int b = kMatchFirstBand + kMatchBands, k = 1; b < kMatchBandCount; ++b, ++k) {
        curve.gainDb[static_cast<std::size_t>(b)] = highEdge * std::pow(0.9, k);
    }
    curve.valid = true;
    return curve;
}

// ---- Radar ----------------------------------------------------------------------------------------------------------

namespace {

double lufsEnergy(double lufs) { return std::pow(10.0, lufs / 10.0); }
double energyLufs(double energy) { return 10.0 * std::log10(std::max(energy, 1e-30)); }
bool measured(float v) { return std::isfinite(v) && v > -70.0F; }

} // namespace

RadarFrame foldProgramme(std::span<const float> shortTerm, double periodSeconds, int slots) {
    RadarFrame out;
    const std::size_t n = shortTerm.size();
    if (n == 0) {
        return out;
    }
    const auto count = static_cast<std::size_t>(std::clamp(slots, 1, kRadarMaxSlots));
    const std::size_t used = std::min(count, n);
    out.values.assign(used, std::numeric_limits<float>::quiet_NaN());
    out.secondsPerSlot = static_cast<double>(n) * periodSeconds / static_cast<double>(used);
    std::vector<double> energy(used, 0.0);
    std::vector<int> samples(used, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!measured(shortTerm[i])) {
            continue;
        }
        const std::size_t slot = std::min(i * used / n, used - 1);
        energy[slot] += lufsEnergy(shortTerm[i]);
        ++samples[slot];
    }
    for (std::size_t s = 0; s < used; ++s) {
        if (samples[s] > 0) {
            out.values[s] = static_cast<float>(energyLufs(energy[s] / samples[s]));
        }
    }
    return out;
}

RadarFrame foldLive(std::span<const float> shortTerm, int lapSeconds, int maxSlots) {
    RadarFrame out;
    const int lap = std::max(lapSeconds, 1);
    const int slots = std::clamp(std::min(lap, std::max(maxSlots, 1)), 1, kRadarMaxSlots);
    out.values.assign(static_cast<std::size_t>(slots), std::numeric_limits<float>::quiet_NaN());
    out.secondsPerSlot = static_cast<double>(lap) / slots;
    if (shortTerm.empty()) {
        return out;
    }
    std::vector<double> energy(static_cast<std::size_t>(slots), 0.0);
    std::vector<int> samples(static_cast<std::size_t>(slots), 0);
    std::vector<std::int64_t> lapOf(static_cast<std::size_t>(slots), -1);
    int last = 0;
    for (std::size_t k = 0; k < shortTerm.size(); ++k) {
        const auto lapIndex = static_cast<std::int64_t>(k) / lap;
        const int t = static_cast<int>(static_cast<std::int64_t>(k) % lap);
        const auto slot = static_cast<std::size_t>(std::min(static_cast<int>(t / out.secondsPerSlot), slots - 1));
        if (lapOf[slot] != lapIndex) { // a newer lap replaces the older one
            lapOf[slot] = lapIndex;
            energy[slot] = 0.0;
            samples[slot] = 0;
        }
        last = static_cast<int>(slot);
        if (measured(shortTerm[k])) {
            energy[slot] += lufsEnergy(shortTerm[k]);
            ++samples[slot];
        }
    }
    for (std::size_t s = 0; s < energy.size(); ++s) {
        if (samples[s] > 0) {
            out.values[s] = static_cast<float>(energyLufs(energy[s] / samples[s]));
        }
    }
    out.head = last;
    return out;
}

LoudnessBand loudnessBand(std::span<const float> shortTerm) {
    LoudnessBand band;
    std::vector<double> values;
    double energy = 0.0;
    for (const float v : shortTerm) {
        if (measured(v)) {
            values.push_back(static_cast<double>(v));
            energy += lufsEnergy(static_cast<double>(v));
        }
    }
    if (values.size() < 2) {
        return band;
    }
    const double relative = energyLufs(energy / static_cast<double>(values.size())) - 20.0;
    std::erase_if(values, [relative](double v) { return v < relative; });
    if (values.size() < 2) {
        return band;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double p) {
        const double pos = p * static_cast<double>(values.size() - 1);
        const auto i = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(i);
        return i + 1 < values.size() ? values[i] * (1.0 - frac) + values[i + 1] * frac : values[i];
    };
    band.valid = true;
    band.lowLufs = at(0.10);
    band.highLufs = at(0.95);
    return band;
}

} // namespace wabcut::audio::edit
