// p02 audio: the processors of Premiere Pro's Amplitude and Compression, Special and Time and Pitch audio effects
// (descriptors: src/model/src/EffectsAudioAmplitude.cpp). Own DSP, written from the math (no third-party code):
//
//   * Dynamics: feed-forward gain computers with a hard or quadratic soft knee and the smooth decoupled peak detector of
//     Giannoulis, Massberg and Reiss ("Digital Dynamic Range Compressor Design - A Tutorial and Analysis", JAES 2012):
//     the gain reduction is captured instantly, released with the release time and smoothed with the attack time, so a
//     steady tone settles on the static curve.
//   * Lookahead limiting: the required gain of each frame is held over the lookahead window (a sliding minimum) and
//     smoothed by a box filter of the same length, so the gain is fully down when the delayed peak plays: a brick wall
//     with no overshoot (sample peaks), then a release of so many dB per millisecond.
//   * Multiband Compressor: Linkwitz-Riley 4th-order crossovers (two Butterworth sections each) with all-pass
//     compensation of the lower bands, so the four bands sum to an all-pass: flat magnitude when nothing compresses.
//   * Pitch Shifter: time-domain splicing. Overlapping grains of the input, each resampled by the pitch ratio, start
//     every hop on the output's timeline at the input position that keeps them on time (constant latency, unchanged
//     duration); each start is moved, within a search window, to where the input best matches the outgoing grain
//     (normalised cross-correlation), so the raised-cosine crossfades join waveforms in phase. Precision sets the search
//     window and the interpolation (linear, or windowed sinc stretched to low-pass the input when reading faster).
//
// Every processor allocates in its constructor only; process() and reset() are RT-safe and work sample by sample, so
// the output does not depend on how the stream is cut into blocks.

#include "PremiereEffects.h"

#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels
#include <wabcut/audio/dsp/Space.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace wabcut::audio::premiere {

namespace {

using dsp::BiquadCoefficients;
using dsp::BiquadState;
using dsp::kMaxDspChannels;

constexpr std::string_view kAmplify = "wabcut.audio.amplify";
constexpr std::string_view kChannelMixer = "wabcut.audio.channelMixer";
constexpr std::string_view kDynamics = "wabcut.audio.dynamics";
constexpr std::string_view kHardLimiter = "wabcut.audio.hardLimiter";
constexpr std::string_view kMultibandCompressor = "wabcut.audio.multibandCompressor";
constexpr std::string_view kSingleBandCompressor = "wabcut.audio.singleBandCompressor";
constexpr std::string_view kTubeModeledCompressor = "wabcut.audio.tubeModeledCompressor";
constexpr std::string_view kDistortion = "wabcut.audio.distortion";
constexpr std::string_view kGuitarSuite = "wabcut.audio.guitarSuite";
constexpr std::string_view kMastering = "wabcut.audio.mastering";
constexpr std::string_view kVocalEnhancer = "wabcut.audio.vocalEnhancer";
constexpr std::string_view kPitchShifter = "wabcut.audio.pitchShifter";

constexpr double kPi = std::numbers::pi;
constexpr double kFlush = 1e-30;
constexpr double kButterworthQ = 0.70710678118654752;

// ---- Small helpers ----------------------------------------------------------------------------------------------

double dbToGain(double db) noexcept { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }
double gainToDb(double gain) noexcept { return gain > 1e-12 ? 20.0 * std::log10(gain) : -240.0; }
bool isOn(double value) noexcept { return value >= 0.5; }
int choiceOf(double value, int count) noexcept { return std::clamp(static_cast<int>(std::lround(value)), 0, count - 1); }
/// One-pole coefficient of a time constant (1 - 1/e of a step in `ms`); 0 ms = instant.
double timeCoefficient(double ms, double rate) noexcept { return ms > 0.0 ? std::exp(-1000.0 / (ms * rate)) : 0.0; }
int framesOf(double ms, double rate) noexcept { return static_cast<int>(std::lround(std::max(ms, 0.0) * rate / 1000.0)); }
double flushed(double v) noexcept { return std::abs(v) < kFlush ? 0.0 : v; }
double safeFrequency(double hz, double rate) noexcept { return std::clamp(hz, 10.0, 0.45 * rate); }
/// The factor a limiter's gain may rise by per frame to recover 12 dB in `ms` (Audition's release definition).
double riseFactor(double ms, double rate) noexcept {
    const double frames = std::max(ms, 0.0) * rate / 1000.0;
    return frames >= 1.0 ? std::pow(10.0, 12.0 / 20.0 / frames) : 1e9;
}
int settleCap(int rate) noexcept { return 2 * rate; }
/// Settle time of a release (five time constants), capped at 2 s.
int releaseSettle(double ms, int rate) noexcept { return std::min(framesOf(5.0 * ms, rate), settleCap(rate)); }

/// The constant (build-time) values by key.
class Constants {
public:
    Constants(const EffectDescriptor& d, std::span<const double> values) noexcept : m_d(d), m_values(values) {}
    double operator()(std::string_view key) const noexcept {
        const auto i = static_cast<std::size_t>(paramIndex(m_d, key));
        if (i < m_values.size()) {
            return m_values[i];
        }
        return i < m_d.params.size() ? m_d.params[i].defaultValue : 0.0;
    }

private:
    const EffectDescriptor& m_d;
    std::span<const double> m_values;
};

/// A value ramped linearly over a cell (sample-accurate automation): `value` at this call's first frame, `step` per frame.
struct Ramp {
    double value = 0.0;
    double step = 0.0;
};
Ramp rampOf(const ParamCell& cell, int index) noexcept {
    const double a = cell.start[index];
    const double step = (cell.end[index] - a) / cell.length;
    return {a + step * cell.offset, step};
}
/// A dB parameter's linear gain ramped over the cell.
Ramp gainRampOf(const ParamCell& cell, int index) noexcept {
    const double a = dbToGain(cell.start[index]);
    const double step = (dbToGain(cell.end[index]) - a) / cell.length;
    return {a + step * cell.offset, step};
}

double framePeak(const float* frame, std::size_t channels) noexcept {
    double peak = 0.0;
    for (std::size_t c = 0; c < channels; ++c) {
        peak = std::max(peak, static_cast<double>(std::fabs(frame[c])));
    }
    return peak;
}

// Cookbook sections Biquad.h does not have.
BiquadCoefficients allPass(double rate, double frequency, double q) noexcept {
    const double w0 = 2.0 * kPi * frequency / rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    return {(1.0 - alpha) / a0, -2.0 * cosw / a0, 1.0, -2.0 * cosw / a0, (1.0 - alpha) / a0};
}
/// Band-pass with a 0 dB peak at `frequency`.
BiquadCoefficients bandPass(double rate, double frequency, double q) noexcept {
    const double w0 = 2.0 * kPi * frequency / rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    return {alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w0) / a0, (1.0 - alpha) / a0};
}

/// One-pole DC blocker (a high-pass near 10 Hz).
struct DcBlocker {
    double x1 = 0.0;
    double y1 = 0.0;
    double process(double x, double pole) noexcept {
        const double y = x - x1 + pole * y1;
        x1 = x;
        y1 = flushed(y);
        return y;
    }
    void reset() noexcept {
        x1 = 0.0;
        y1 = 0.0;
    }
};
double dcPole(double rate) noexcept { return std::exp(-2.0 * kPi * 10.0 / rate); }

// ---- Dynamics building blocks -------------------------------------------------------------------------------------

/// Gain reduction (dB, >= 0) of a downward compressor at a level (dB): 1/ratio above the threshold, a quadratic soft
/// knee `knee` dB wide centred on it (0: hard knee).
double compressionDb(double level, double threshold, double ratio, double knee) noexcept {
    const double slope = 1.0 - 1.0 / std::max(ratio, 1.0);
    const double over = level - threshold;
    if (knee > 0.0 && 2.0 * std::abs(over) <= knee) {
        const double d = over + 0.5 * knee;
        return slope * d * d / (2.0 * knee);
    }
    return over > 0.0 ? slope * over : 0.0;
}

/// The smooth decoupled peak detector on a gain reduction (dB, >= 0): rises instantly and falls with `release` (a peak
/// hold), then a one-pole with `attack`.
struct ReductionDetector {
    double held = 0.0;
    double smooth = 0.0;
    double step(double reduction, double attack, double release) noexcept {
        held = std::max(reduction, release * held + (1.0 - release) * reduction);
        smooth = attack * smooth + (1.0 - attack) * held;
        if (held < 1e-9 && smooth < 1e-9) {
            held = 0.0;
            smooth = 0.0;
        }
        return smooth;
    }
    void reset() noexcept {
        held = 0.0;
        smooth = 0.0;
    }
};

/// A compressor's per-cell settings: the curve and the detector's coefficients.
struct CompressorSettings {
    double threshold = 0.0;
    double ratio = 1.0;
    double knee = 0.0;
    double attack = 0.0;  ///< coefficient
    double release = 0.0; ///< coefficient
};

/// Linked feed-forward compressor: one gain for all channels from their peak.
struct LinkedCompressor {
    ReductionDetector detector;
    /// Gain (linear) for a frame whose channel peak is `peak`.
    double gain(double peak, const CompressorSettings& s) noexcept {
        const double reduction = compressionDb(gainToDb(peak), s.threshold, s.ratio, s.knee);
        const double db = detector.step(reduction, s.attack, s.release);
        return db > 0.0 ? dbToGain(-db) : 1.0;
    }
    void reset() noexcept { detector.reset(); }
};

/// Lookahead brick-wall limiter (sample peaks): see the file comment. One gain for all channels (linked) or one per
/// channel. Output delayed by latencyFrames().
class LookaheadLimiter {
public:
    LookaheadLimiter(int channels, int lookahead, bool linked)
        : m_channels(channels), m_lookahead(std::max(lookahead, 0)), m_groups(linked ? 1 : channels),
          m_capacity(m_lookahead + 2), m_boxLength(std::max(m_lookahead, 1)) {
        const auto groups = static_cast<std::size_t>(m_groups);
        m_dequeValue.resize(groups * static_cast<std::size_t>(m_capacity));
        m_dequeIndex.resize(groups * static_cast<std::size_t>(m_capacity));
        m_box.resize(groups * static_cast<std::size_t>(m_boxLength));
        m_delay.resize(static_cast<std::size_t>(m_lookahead + 1) * static_cast<std::size_t>(channels));
        reset();
    }

    int latencyFrames() const noexcept { return m_lookahead; }

    void reset() noexcept {
        std::fill(m_delay.begin(), m_delay.end(), 0.0f);
        std::fill(m_box.begin(), m_box.end(), 1.0);
        for (int g = 0; g < m_groups; ++g) {
            const auto i = static_cast<std::size_t>(g);
            m_head[i] = 0;
            m_size[i] = 0;
            m_sum[i] = static_cast<double>(m_boxLength);
            m_gain[i] = 1.0;
        }
        m_index = 0;
        m_boxPos = 0;
        m_delayPos = 0;
    }

    /// Limits one frame in place: the output (the frame latencyFrames() ago) never exceeds `ceiling` (linear > 0); the
    /// gain rises by at most `rise` (>= 1) per frame.
    void process(float* frame, double ceiling, double rise) noexcept {
        const auto ch = static_cast<std::size_t>(m_channels);
        const auto cap = static_cast<std::size_t>(m_capacity);
        for (int g = 0; g < m_groups; ++g) {
            const auto gi = static_cast<std::size_t>(g);
            const double peak = m_groups == 1 ? framePeak(frame, ch) : static_cast<double>(std::fabs(frame[gi]));
            const double required = peak > ceiling ? ceiling / peak : 1.0;
            // Sliding minimum over frames [index - lookahead, index] (a monotonic deque in a ring).
            double* values = m_dequeValue.data() + gi * cap;
            std::int64_t* indices = m_dequeIndex.data() + gi * cap;
            std::size_t& head = m_head[gi];
            std::size_t& size = m_size[gi];
            while (size > 0 && values[(head + size - 1) % cap] >= required) {
                --size;
            }
            values[(head + size) % cap] = required;
            indices[(head + size) % cap] = m_index;
            ++size;
            while (indices[head] < m_index - m_lookahead) {
                head = (head + 1) % cap;
                --size;
            }
            const double held = values[head];
            double smooth = held;
            if (m_lookahead > 0) {
                double* box = m_box.data() + gi * static_cast<std::size_t>(m_boxLength);
                m_sum[gi] += held - box[m_boxPos];
                box[m_boxPos] = held;
                smooth = m_sum[gi] / static_cast<double>(m_boxLength);
            }
            m_gain[gi] = std::min({smooth, m_gain[gi] * rise, 1.0});
        }
        if (m_lookahead > 0 && ++m_boxPos == static_cast<std::size_t>(m_boxLength)) {
            m_boxPos = 0;
            for (int g = 0; g < m_groups; ++g) { // resynchronise the running sums (no drift)
                const auto gi = static_cast<std::size_t>(g);
                const double* box = m_box.data() + gi * static_cast<std::size_t>(m_boxLength);
                double sum = 0.0;
                for (int i = 0; i < m_boxLength; ++i) {
                    sum += box[i];
                }
                m_sum[gi] = sum;
            }
        }
        const auto slots = static_cast<std::size_t>(m_lookahead + 1);
        float* slot = m_delay.data() + m_delayPos * ch;
        for (std::size_t c = 0; c < ch; ++c) {
            slot[c] = frame[c];
        }
        const float* delayed = m_delay.data() + ((m_delayPos + 1) % slots) * ch; // the frame `lookahead` ago
        const auto limit = static_cast<float>(ceiling);
        for (std::size_t c = 0; c < ch; ++c) {
            const double gain = m_gain[m_groups == 1 ? 0 : c];
            frame[c] = std::clamp(static_cast<float>(delayed[c] * gain), -limit, limit);
        }
        m_delayPos = (m_delayPos + 1) % slots;
        ++m_index;
    }

private:
    int m_channels;
    int m_lookahead;
    int m_groups;
    int m_capacity;
    int m_boxLength;
    std::vector<double> m_dequeValue;
    std::vector<std::int64_t> m_dequeIndex;
    std::vector<double> m_box;
    std::vector<float> m_delay;
    std::array<std::size_t, kMaxDspChannels> m_head{};
    std::array<std::size_t, kMaxDspChannels> m_size{};
    std::array<double, kMaxDspChannels> m_sum{};
    std::array<double, kMaxDspChannels> m_gain{};
    std::int64_t m_index = 0;
    std::size_t m_boxPos = 0;
    std::size_t m_delayPos = 0;
};

// ---- Amplify ------------------------------------------------------------------------------------------------------

class AmplifyProcessor final : public EffectProcessor {
public:
    AmplifyProcessor(const EffectDescriptor& d, int channels, std::span<const double> values)
        : m_channels(channels), m_left(paramIndex(d, "leftGain")), m_right(paramIndex(d, "rightGain")),
          m_link(isOn(Constants(d, values)("link"))) {}

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        Ramp left = gainRampOf(cell, m_left);
        Ramp right = m_link ? left : gainRampOf(cell, m_right);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const auto l = static_cast<float>(left.value);
            const auto r = static_cast<float>(right.value);
            frame[0] *= l;
            if (ch > 1) {
                frame[1] *= r;
            }
            for (std::size_t c = 2; m_link && c < ch; ++c) {
                frame[c] *= l;
            }
            left.value += left.step;
            right.value += right.step;
        }
    }
    void reset() noexcept override {}

private:
    int m_channels;
    int m_left;
    int m_right;
    bool m_link;
};

// ---- Channel Mixer ------------------------------------------------------------------------------------------------

class ChannelMixerProcessor final : public EffectProcessor {
public:
    ChannelMixerProcessor(const EffectDescriptor& d, int channels, std::span<const double> values)
        : m_channels(channels), m_ll(paramIndex(d, "leftFromLeft")), m_lr(paramIndex(d, "leftFromRight")),
          m_rl(paramIndex(d, "rightFromLeft")), m_rr(paramIndex(d, "rightFromRight")) {
        const Constants k(d, values);
        m_invertLeft = isOn(k("invertLeft")) ? -1.0 : 1.0;
        m_invertRight = isOn(k("invertRight")) ? -1.0 : 1.0;
    }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        if (m_channels < 2) {
            return; // mono: nothing to mix
        }
        Ramp ll = rampOf(cell, m_ll);
        Ramp lr = rampOf(cell, m_lr);
        Ramp rl = rampOf(cell, m_rl);
        Ramp rr = rampOf(cell, m_rr);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const double l = frame[0];
            const double r = frame[1];
            frame[0] = static_cast<float>(m_invertLeft * (ll.value * l + lr.value * r) / 100.0);
            frame[1] = static_cast<float>(m_invertRight * (rl.value * l + rr.value * r) / 100.0);
            for (Ramp* p : {&ll, &lr, &rl, &rr}) {
                p->value += p->step;
            }
        }
    }
    void reset() noexcept override {}

private:
    int m_channels;
    int m_ll, m_lr, m_rl, m_rr;
    double m_invertLeft = 1.0;
    double m_invertRight = 1.0;
};

// ---- Hard Limiter -------------------------------------------------------------------------------------------------

class HardLimiterProcessor final : public EffectProcessor {
public:
    HardLimiterProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate), m_ceiling(paramIndex(d, "maximumAmplitude")),
          m_boost(paramIndex(d, "inputBoost")), m_release(paramIndex(d, "releaseTime")),
          m_limiter(channels, std::max(1, framesOf(Constants(d, values)("lookAheadTime"), sampleRate)),
                    isOn(Constants(d, values)("linkChannels"))),
          m_settle(std::min(m_limiter.latencyFrames() + framesOf(4.0 * Constants(d, values)("releaseTime"), sampleRate),
                            settleCap(sampleRate))) {}

    int latencyFrames() const noexcept override { return m_limiter.latencyFrames(); }
    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        Ramp boost = gainRampOf(cell, m_boost);
        const double ceiling = std::max(dbToGain(cell.start[m_ceiling]), 1e-6);
        const double rise = riseFactor(cell.start[m_release], m_rate);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const auto b = static_cast<float>(boost.value);
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] *= b;
            }
            m_limiter.process(frame, ceiling, rise);
            boost.value += boost.step;
        }
    }
    void reset() noexcept override { m_limiter.reset(); }

private:
    int m_channels;
    double m_rate;
    int m_ceiling, m_boost, m_release;
    LookaheadLimiter m_limiter;
    int m_settle;
};

// ---- Single-band and Tube-modeled Compressor ------------------------------------------------------------------------

class SingleBandCompressorProcessor final : public EffectProcessor {
public:
    SingleBandCompressorProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values,
                                  bool tube)
        : m_channels(channels), m_rate(sampleRate), m_tube(tube), m_threshold(paramIndex(d, "threshold")),
          m_ratio(paramIndex(d, "ratio")), m_attack(paramIndex(d, "attack")), m_release(paramIndex(d, "release")),
          m_output(paramIndex(d, "outputGain")), m_pole(dcPole(sampleRate)),
          m_settle(releaseSettle(Constants(d, values)("release") + Constants(d, values)("attack"), sampleRate) +
                   (tube ? sampleRate / 10 : 0)) {}

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        CompressorSettings s;
        s.threshold = v[m_threshold];
        s.ratio = v[m_ratio];
        s.knee = m_tube ? 6.0 : 0.0;
        s.attack = timeCoefficient(v[m_attack], m_rate);
        s.release = timeCoefficient(v[m_release], m_rate);
        Ramp output = gainRampOf(cell, m_output);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const double g = m_comp.gain(framePeak(frame, ch), s);
            // Tube colour (variable-mu): second and third harmonic in proportion to the gain reduction, DC-blocked.
            const double depth = m_tube ? 1.0 - g : 0.0;
            for (std::size_t c = 0; c < ch; ++c) {
                double y = frame[c] * g;
                if (m_tube) {
                    const double colour = depth * (kSecond * y * y - kThird * y * y * y);
                    y += m_dc[c].process(colour, m_pole);
                }
                frame[c] = static_cast<float>(y * output.value);
            }
            output.value += output.step;
        }
    }
    void reset() noexcept override {
        m_comp.reset();
        for (DcBlocker& b : m_dc) {
            b.reset();
        }
    }

private:
    static constexpr double kSecond = 0.3;
    static constexpr double kThird = 0.3;

    int m_channels;
    double m_rate;
    bool m_tube;
    int m_threshold, m_ratio, m_attack, m_release, m_output;
    double m_pole;
    int m_settle;
    LinkedCompressor m_comp;
    std::array<DcBlocker, kMaxDspChannels> m_dc{};
};

// ---- Multiband Compressor -------------------------------------------------------------------------------------------

class MultibandCompressorProcessor final : public EffectProcessor {
public:
    static constexpr std::size_t kBands = 4;

    MultibandCompressorProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate), m_low(paramIndex(d, "lowCrossover")),
          m_mid(paramIndex(d, "midCrossover")), m_high(paramIndex(d, "highCrossover")),
          m_output(paramIndex(d, "outputGain")), m_limitThreshold(paramIndex(d, "limiterThreshold")),
          m_limitAttack(paramIndex(d, "limiterAttack")), m_limitRelease(paramIndex(d, "limiterRelease")),
          m_margin(paramIndex(d, "limiterMargin")), m_brickwall(isOn(Constants(d, values)("brickwall"))),
          m_limiter(channels, m_brickwall ? framesOf(5.0, sampleRate) : 0, true) {
        static constexpr std::array<std::array<std::string_view, 7>, kBands> kKeys{{
            {"band1Threshold", "band1Gain", "band1Ratio", "band1Attack", "band1Release", "band1Bypass", "band1Solo"},
            {"band2Threshold", "band2Gain", "band2Ratio", "band2Attack", "band2Release", "band2Bypass", "band2Solo"},
            {"band3Threshold", "band3Gain", "band3Ratio", "band3Attack", "band3Release", "band3Bypass", "band3Solo"},
            {"band4Threshold", "band4Gain", "band4Ratio", "band4Attack", "band4Release", "band4Bypass", "band4Solo"},
        }};
        const Constants k(d, values);
        double longest = k("limiterRelease");
        for (std::size_t b = 0; b < kBands; ++b) {
            Band& band = m_bands[b];
            band.threshold = paramIndex(d, kKeys[b][0]);
            band.gain = paramIndex(d, kKeys[b][1]);
            band.ratio = paramIndex(d, kKeys[b][2]);
            band.attack = paramIndex(d, kKeys[b][3]);
            band.release = paramIndex(d, kKeys[b][4]);
            band.bypass = isOn(k(kKeys[b][5]));
            band.solo = isOn(k(kKeys[b][6]));
            m_anySolo = m_anySolo || band.solo;
            longest = std::max(longest, k(kKeys[b][4]));
        }
        m_settle = std::max(sampleRate / 5, std::min(m_limiter.latencyFrames() + releaseSettle(longest, sampleRate),
                                                     settleCap(sampleRate)));
    }

    int latencyFrames() const noexcept override { return m_limiter.latencyFrames(); }
    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        setCrossovers(v[m_low], v[m_mid], v[m_high]);
        std::array<CompressorSettings, kBands> settings{};
        std::array<double, kBands> makeup{};
        std::array<bool, kBands> audible{};
        for (std::size_t b = 0; b < kBands; ++b) {
            const Band& band = m_bands[b];
            settings[b].threshold = v[band.threshold];
            settings[b].ratio = v[band.ratio];
            settings[b].attack = timeCoefficient(v[band.attack], m_rate);
            settings[b].release = timeCoefficient(v[band.release], m_rate);
            makeup[b] = band.bypass ? 1.0 : dbToGain(v[band.gain]);
            audible[b] = !m_anySolo || band.solo;
        }
        Ramp output = gainRampOf(cell, m_output);
        CompressorSettings limit;
        limit.threshold = v[m_limitThreshold];
        limit.ratio = 1e9; // a limiter: no rise above the threshold
        limit.attack = timeCoefficient(v[m_limitAttack], m_rate);
        limit.release = timeCoefficient(v[m_limitRelease], m_rate);
        const double margin = dbToGain(v[m_margin]);
        const double raise = dbToGain(v[m_margin] - v[m_limitThreshold]);
        const double ceiling = dbToGain(v[m_limitThreshold]);
        const double rise = riseFactor(v[m_limitRelease], m_rate);

        const auto ch = static_cast<std::size_t>(m_channels);
        std::array<std::array<double, kMaxDspChannels>, kBands> split{};
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                splitBands(frame[c], m_filters[c], split, c);
            }
            std::array<double, kBands> gains{};
            for (std::size_t b = 0; b < kBands; ++b) {
                double gain = makeup[b];
                if (!m_bands[b].bypass) {
                    double peak = 0.0;
                    for (std::size_t c = 0; c < ch; ++c) {
                        peak = std::max(peak, std::abs(split[b][c]));
                    }
                    gain *= m_comp[b].gain(peak, settings[b]);
                }
                gains[b] = audible[b] ? gain : 0.0;
            }
            for (std::size_t c = 0; c < ch; ++c) {
                double y = 0.0;
                for (std::size_t b = 0; b < kBands; ++b) {
                    y += gains[b] * split[b][c];
                }
                frame[c] = static_cast<float>(y * output.value);
            }
            output.value += output.step;
            // The limiter: hold the level to its threshold, then raise it to the margin.
            if (m_brickwall) {
                m_limiter.process(frame, ceiling, rise);
                const auto top = static_cast<float>(margin);
                for (std::size_t c = 0; c < ch; ++c) {
                    frame[c] = std::clamp(static_cast<float>(frame[c] * raise), -top, top);
                }
            } else {
                const double g = m_softLimit.gain(framePeak(frame, ch), limit) * raise;
                for (std::size_t c = 0; c < ch; ++c) {
                    frame[c] = static_cast<float>(frame[c] * g);
                }
            }
        }
    }

    void reset() noexcept override {
        for (Filters& f : m_filters) {
            f = Filters{};
        }
        for (LinkedCompressor& c : m_comp) {
            c.reset();
        }
        m_softLimit.reset();
        m_limiter.reset();
    }

private:
    struct Band {
        int threshold = 0, gain = 0, ratio = 0, attack = 0, release = 0;
        bool bypass = false;
        bool solo = false;
    };
    /// One channel's crossover tree (Linkwitz-Riley = two Butterworth sections; all-passes keep the lower bands in
    /// phase with the upper ones).
    struct Filters {
        std::array<BiquadState, 2> lp1{}, hp1{}, lp2{}, hp2{}, lp3{}, hp3{};
        BiquadState lowAp2{}, lowAp3{}, lowMidAp3{};
    };
    struct Coefficients {
        BiquadCoefficients lp1, hp1, lp2, hp2, ap2, lp3, hp3, ap3;
    };

    void setCrossovers(double low, double mid, double high) noexcept {
        const double f1 = safeFrequency(low, m_rate);
        const double f2 = std::max(safeFrequency(mid, m_rate), f1);
        const double f3 = std::max(safeFrequency(high, m_rate), f2);
        if (f1 == m_f1 && f2 == m_f2 && f3 == m_f3) {
            return;
        }
        m_f1 = f1;
        m_f2 = f2;
        m_f3 = f3;
        m_k.lp1 = dsp::lowPass(m_rate, f1, kButterworthQ);
        m_k.hp1 = dsp::highPass(m_rate, f1, kButterworthQ);
        m_k.lp2 = dsp::lowPass(m_rate, f2, kButterworthQ);
        m_k.hp2 = dsp::highPass(m_rate, f2, kButterworthQ);
        m_k.ap2 = allPass(m_rate, f2, kButterworthQ);
        m_k.lp3 = dsp::lowPass(m_rate, f3, kButterworthQ);
        m_k.hp3 = dsp::highPass(m_rate, f3, kButterworthQ);
        m_k.ap3 = allPass(m_rate, f3, kButterworthQ);
    }

    void splitBands(float x, Filters& f, std::array<std::array<double, kMaxDspChannels>, kBands>& out,
                    std::size_t c) const noexcept {
        float low = f.lp1[1].process(m_k.lp1, f.lp1[0].process(m_k.lp1, x));
        low = f.lowAp3.process(m_k.ap3, f.lowAp2.process(m_k.ap2, low));
        const float rest = f.hp1[1].process(m_k.hp1, f.hp1[0].process(m_k.hp1, x));
        float lowMid = f.lp2[1].process(m_k.lp2, f.lp2[0].process(m_k.lp2, rest));
        lowMid = f.lowMidAp3.process(m_k.ap3, lowMid);
        const float upper = f.hp2[1].process(m_k.hp2, f.hp2[0].process(m_k.hp2, rest));
        const float highMid = f.lp3[1].process(m_k.lp3, f.lp3[0].process(m_k.lp3, upper));
        const float high = f.hp3[1].process(m_k.hp3, f.hp3[0].process(m_k.hp3, upper));
        out[0][c] = low;
        out[1][c] = lowMid;
        out[2][c] = highMid;
        out[3][c] = high;
    }

    int m_channels;
    double m_rate;
    int m_low, m_mid, m_high, m_output, m_limitThreshold, m_limitAttack, m_limitRelease, m_margin;
    bool m_brickwall;
    bool m_anySolo = false;
    std::array<Band, kBands> m_bands{};
    double m_f1 = -1.0, m_f2 = -1.0, m_f3 = -1.0;
    Coefficients m_k{};
    std::array<Filters, kMaxDspChannels> m_filters{};
    std::array<LinkedCompressor, kBands> m_comp{};
    LinkedCompressor m_softLimit;
    LookaheadLimiter m_limiter;
    int m_settle = 0;
};

// ---- Dynamics ----------------------------------------------------------------------------------------------------

class DynamicsProcessor final : public EffectProcessor {
public:
    DynamicsProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate), m_gateThreshold(paramIndex(d, "gateThreshold")),
          m_gateAttack(paramIndex(d, "gateAttack")), m_gateRelease(paramIndex(d, "gateRelease")),
          m_gateHold(paramIndex(d, "gateHold")), m_compThreshold(paramIndex(d, "compressorThreshold")),
          m_compRatio(paramIndex(d, "compressorRatio")), m_compAttack(paramIndex(d, "compressorAttack")),
          m_compRelease(paramIndex(d, "compressorRelease")), m_makeUp(paramIndex(d, "makeUp")),
          m_expThreshold(paramIndex(d, "expanderThreshold")), m_expRatio(paramIndex(d, "expanderRatio")),
          m_limitThreshold(paramIndex(d, "limiterThreshold")) {
        const Constants k(d, values);
        m_gate = isOn(k("gateEnable"));
        m_compressor = isOn(k("compressorEnable"));
        m_auto = isOn(k("compressorAuto"));
        m_expander = isOn(k("expanderEnable"));
        m_limiter = isOn(k("limiterEnable"));
        m_softClip = isOn(k("softClip"));
        m_envelopeRelease = timeCoefficient(20.0, sampleRate);
        m_expanderClose = timeCoefficient(20.0, sampleRate);
        m_expanderOpen = timeCoefficient(2.0, sampleRate);
        m_limiterRise = riseFactor(50.0, sampleRate);
        m_autoFastRelease = timeCoefficient(40.0, sampleRate);
        m_autoSlowAttack = timeCoefficient(300.0, sampleRate);
        m_autoSlowRelease = timeCoefficient(1000.0, sampleRate);
        int settle = 0;
        if (m_gate) {
            settle = std::max(settle, framesOf(k("gateHold") + k("gateRelease") + 100.0, sampleRate));
        }
        if (m_compressor) {
            settle = std::max(settle, releaseSettle(m_auto ? 1000.0 : k("compressorRelease"), sampleRate));
        }
        if (m_expander || m_limiter) {
            settle = std::max(settle, sampleRate / 4);
        }
        m_settle = std::min(settle, settleCap(sampleRate));
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        if (!m_gate && !m_compressor && !m_expander && !m_limiter) {
            return;
        }
        const double* v = cell.start;
        const double gateThreshold = v[m_gateThreshold];
        const double gateOpenStep = 1.0 / std::max(1.0, v[m_gateAttack] * m_rate / 1000.0);
        const double gateCloseStep = 1.0 / std::max(1.0, v[m_gateRelease] * m_rate / 1000.0);
        const auto hold = static_cast<std::int64_t>(framesOf(v[m_gateHold], m_rate));
        CompressorSettings comp;
        comp.threshold = v[m_compThreshold];
        comp.ratio = v[m_compRatio];
        comp.attack = timeCoefficient(v[m_compAttack], m_rate);
        comp.release = m_auto ? m_autoFastRelease : timeCoefficient(v[m_compRelease], m_rate);
        CompressorSettings slow = comp;
        slow.attack = m_autoSlowAttack;
        slow.release = m_autoSlowRelease;
        const double makeUp = dbToGain(v[m_makeUp]);
        const double expThreshold = v[m_expThreshold];
        const double expRatio = std::max(v[m_expRatio], 1.0);
        const double limit = dbToGain(v[m_limitThreshold]);

        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            double gain = 1.0;
            if (m_gate) {
                m_gateEnvelope = flushed(std::max(framePeak(frame, ch), m_gateEnvelope * m_envelopeRelease));
                if (gainToDb(m_gateEnvelope) > gateThreshold) {
                    m_holdLeft = hold;
                    m_gateGain = std::min(1.0, m_gateGain + gateOpenStep);
                } else if (m_holdLeft > 0) {
                    --m_holdLeft;
                    m_gateGain = std::min(1.0, m_gateGain + gateOpenStep);
                } else {
                    m_gateGain = std::max(0.0, m_gateGain - gateCloseStep);
                }
                gain *= m_gateGain;
            }
            if (m_compressor) {
                const double peak = framePeak(frame, ch) * gain;
                const double reduction = compressionDb(gainToDb(peak), comp.threshold, comp.ratio, 0.0);
                double db = m_compDetector.step(reduction, comp.attack, comp.release);
                if (m_auto) {
                    db = std::max(db, m_autoSlow.step(reduction, slow.attack, slow.release));
                }
                gain *= (db > 0.0 ? dbToGain(-db) : 1.0) * makeUp;
            }
            if (m_expander) {
                m_expEnvelope = flushed(std::max(framePeak(frame, ch) * gain, m_expEnvelope * m_envelopeRelease));
                const double level = gainToDb(m_expEnvelope);
                const double target = level < expThreshold ? std::max((level - expThreshold) * (expRatio - 1.0), -150.0) : 0.0;
                const double a = target < m_expDb ? m_expanderClose : m_expanderOpen;
                m_expDb = target + a * (m_expDb - target);
                if (std::abs(m_expDb - target) < 1e-9) {
                    m_expDb = target;
                }
                gain *= dbToGain(m_expDb);
            }
            if (m_limiter && !m_softClip) {
                const double peak = framePeak(frame, ch) * gain;
                const double required = peak > limit ? limit / peak : 1.0;
                m_limitGain = std::min(required, std::min(1.0, m_limitGain * m_limiterRise));
                gain *= m_limitGain;
            }
            for (std::size_t c = 0; c < ch; ++c) {
                double y = frame[c] * gain;
                if (m_limiter) {
                    y = m_softClip ? softClip(y, limit) : std::clamp(y, -limit, limit);
                }
                frame[c] = static_cast<float>(y);
            }
        }
    }

    void reset() noexcept override {
        m_gateEnvelope = 0.0;
        m_gateGain = 0.0;
        m_holdLeft = 0;
        m_compDetector.reset();
        m_autoSlow.reset();
        m_expEnvelope = 0.0;
        m_expDb = 0.0;
        m_limitGain = 1.0;
    }

    /// Linear up to half the limit, then a tanh shoulder that approaches the limit without reaching it.
    static double softClip(double x, double limit) noexcept {
        const double knee = 0.5 * limit;
        const double a = std::abs(x);
        if (a <= knee) {
            return x;
        }
        const double y = knee + (limit - knee) * std::tanh((a - knee) / (limit - knee));
        return x < 0.0 ? -y : y;
    }

private:
    int m_channels;
    double m_rate;
    int m_gateThreshold, m_gateAttack, m_gateRelease, m_gateHold;
    int m_compThreshold, m_compRatio, m_compAttack, m_compRelease, m_makeUp;
    int m_expThreshold, m_expRatio, m_limitThreshold;
    bool m_gate = false, m_compressor = false, m_auto = false, m_expander = false, m_limiter = false, m_softClip = false;
    double m_envelopeRelease = 0.0, m_expanderClose = 0.0, m_expanderOpen = 0.0, m_limiterRise = 1.0;
    double m_autoFastRelease = 0.0, m_autoSlowAttack = 0.0, m_autoSlowRelease = 0.0;
    int m_settle = 0;
    double m_gateEnvelope = 0.0;
    double m_gateGain = 0.0;
    std::int64_t m_holdLeft = 0;
    ReductionDetector m_compDetector;
    ReductionDetector m_autoSlow;
    double m_expEnvelope = 0.0;
    double m_expDb = 0.0;
    double m_limitGain = 1.0;
};

// ---- Distortion ----------------------------------------------------------------------------------------------------

/// A transfer graph from (0, 0) to (1, 1) through one control point (p, q), Premiere's graph with its middle point
/// dragged up and left by the curve's amount: 0 % = (0.5, 0.5), the straight line; 100 % = (0.05, 0.95).
struct Bend {
    double p = 0.5;
    double q = 0.5;
};
Bend bendOf(double percent) noexcept {
    const double a = 0.45 * std::clamp(percent / 100.0, 0.0, 1.0);
    return {0.5 - a, 0.5 + a};
}

/// The graph at `t` >= 0: straight segments through the point, or (smooth) the quadratic Bezier curve it controls;
/// past 1, the last segment's slope.
double bendCurve(double t, const Bend& b, bool smooth) noexcept {
    if (t >= 1.0) {
        return 1.0 + (t - 1.0) * (1.0 - b.q) / (1.0 - b.p);
    }
    if (!smooth) {
        return t < b.p ? t * b.q / b.p : b.q + (t - b.p) * (1.0 - b.q) / (1.0 - b.p);
    }
    // x(s) = 2 s (1 - s) p + s^2, y(s) = 2 s (1 - s) q + s^2: solve x(s) = t for s in [0, 1].
    const double a = 1.0 - 2.0 * b.p;
    const double s = a < 1e-12 ? t : (-b.p + std::sqrt(b.p * b.p + a * t)) / a;
    return 2.0 * s * (1.0 - s) * b.q + s * s;
}

/// The transfer of a magnitude `u` (>= 0): the graph spans the top `range` dB of the level (levels below it pass
/// untouched), or 0 .. full scale on a linear scale.
double distortionCurve(double u, const Bend& b, double range, bool smooth, bool linear) noexcept {
    if (b.p == b.q || u <= 0.0) {
        return u;
    }
    if (linear) {
        return bendCurve(u, b, smooth);
    }
    const double t = (gainToDb(u) + range) / range;
    if (t <= 0.0) {
        return u;
    }
    return dbToGain((bendCurve(t, b, smooth) - 1.0) * range);
}

class DistortionProcessor final : public EffectProcessor {
public:
    DistortionProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate), m_positive(paramIndex(d, "positive")),
          m_negative(paramIndex(d, "negative")), m_time(paramIndex(d, "timeSmoothing")), m_range(paramIndex(d, "dbRange")),
          m_pole(dcPole(sampleRate)) {
        const Constants k(d, values);
        m_symmetric = isOn(k("symmetric"));
        m_smooth = isOn(k("curveSmoothing"));
        m_linear = isOn(k("linearScale"));
        m_dcFilter = isOn(k("postFilterDcOffset"));
    }

    int settleFrames() const noexcept override { return m_rate / 5; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        const Bend positive = bendOf(v[m_positive]);
        const Bend negative = m_symmetric ? positive : bendOf(v[m_negative]);
        const double range = std::clamp(v[m_range], 6.0, 96.0);
        // Time Smoothing: the level is read from |x| through a one-pole of up to 50 ms (its low-frequency content).
        const double smoothing = timeCoefficient(0.5 * std::clamp(v[m_time], 0.0, 100.0), m_rate);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                const double x = frame[c];
                const Bend& bend = x >= 0.0 ? positive : negative;
                double y = 0.0;
                if (smoothing > 0.0) {
                    m_level[c] = flushed(smoothing * m_level[c] + (1.0 - smoothing) * std::abs(x));
                    y = x * curveGain(m_level[c], bend, range);
                } else {
                    const double u = distortionCurve(std::abs(x), bend, range, m_smooth, m_linear);
                    y = x < 0.0 ? -u : u;
                }
                if (m_dcFilter) {
                    y = m_dc[c].process(y, m_pole);
                }
                frame[c] = static_cast<float>(y);
            }
        }
    }
    void reset() noexcept override {
        m_level.fill(0.0);
        for (DcBlocker& b : m_dc) {
            b.reset();
        }
    }

private:
    /// The curve's gain at a smoothed level (its small-signal slope near 0).
    double curveGain(double level, const Bend& bend, double range) const noexcept {
        if (level < 1e-9) {
            return m_linear ? bend.q / bend.p : 1.0;
        }
        return distortionCurve(level, bend, range, m_smooth, m_linear) / level;
    }

    int m_channels;
    int m_rate;
    int m_positive, m_negative, m_time, m_range;
    double m_pole;
    bool m_symmetric = true, m_smooth = false, m_linear = false, m_dcFilter = true;
    std::array<double, kMaxDspChannels> m_level{};
    std::array<DcBlocker, kMaxDspChannels> m_dc{};
};

// ---- Vocal Enhancer -------------------------------------------------------------------------------------------------

class VocalEnhancerProcessor final : public EffectProcessor {
public:
    VocalEnhancerProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate) {
        const int mode = choiceOf(Constants(d, values)("mode"), 3);
        const double r = sampleRate;
        const auto f = [r](double hz) { return safeFrequency(hz, r); };
        if (mode == 2) { // Music: carve the voice band so a bed does not compete with dialogue
            m_sections = {dsp::highPass(r, f(40.0)), dsp::peaking(r, f(500.0), -1.5, 1.0), dsp::peaking(r, f(2500.0), -4.0, 0.7),
                          dsp::highShelf(r, f(12000.0), 0.0)};
            m_comp = {-20.0, 1.5, 6.0, timeCoefficient(20.0, r), timeCoefficient(250.0, r)};
        } else { // Male, Female: rumble and mud out, presence and air in, gentle levelling
            const bool female = mode == 1;
            m_sections = {dsp::highPass(r, f(female ? 120.0 : 80.0)), dsp::peaking(r, f(female ? 400.0 : 300.0), -3.0, 1.0),
                          dsp::peaking(r, f(female ? 4500.0 : 3000.0), 3.0, 0.9),
                          dsp::highShelf(r, f(female ? 12000.0 : 10000.0), 2.0)};
            m_comp = {-24.0, 2.0, 6.0, timeCoefficient(5.0, r), timeCoefficient(120.0, r)};
        }
    }

    int settleFrames() const noexcept override { return m_rate / 2; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        (void)cell;
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                float x = frame[c];
                for (std::size_t s = 0; s < m_sections.size(); ++s) {
                    x = m_state[c][s].process(m_sections[s], x);
                }
                frame[c] = x;
            }
            const auto g = static_cast<float>(m_compressor.gain(framePeak(frame, ch), m_comp));
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] *= g;
            }
        }
    }
    void reset() noexcept override {
        for (auto& states : m_state) {
            for (BiquadState& s : states) {
                s.reset();
            }
        }
        m_compressor.reset();
    }

private:
    int m_channels;
    int m_rate;
    std::array<BiquadCoefficients, 4> m_sections{};
    std::array<std::array<BiquadState, 4>, kMaxDspChannels> m_state{};
    CompressorSettings m_comp;
    LinkedCompressor m_compressor;
};

// ---- Mastering -----------------------------------------------------------------------------------------------------

class MasteringProcessor final : public EffectProcessor {
public:
    MasteringProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate), m_lowF(paramIndex(d, "lowShelfFrequency")),
          m_lowG(paramIndex(d, "lowShelfGain")), m_peakF(paramIndex(d, "peakFrequency")), m_peakG(paramIndex(d, "peakGain")),
          m_peakQ(paramIndex(d, "peakQ")), m_highF(paramIndex(d, "highShelfFrequency")),
          m_highG(paramIndex(d, "highShelfGain")), m_reverb(paramIndex(d, "reverbAmount")),
          m_exciter(paramIndex(d, "exciterAmount")), m_widener(paramIndex(d, "widener")),
          m_maximizer(paramIndex(d, "loudnessMaximizer")), m_output(paramIndex(d, "outputGain")),
          m_room(channels, sampleRate, 0.4, 10.0), m_width(channels, sampleRate),
          m_limiter(channels, framesOf(3.0, sampleRate), true) {
        const Constants k(d, values);
        m_lowOn = isOn(k("lowShelfEnable"));
        m_peakOn = isOn(k("peakEnable"));
        m_highOn = isOn(k("highShelfEnable"));
        m_mode = choiceOf(k("exciterMode"), 3);
        static constexpr std::array<double, 3> kExciterHz{2000.0, 4000.0, 1500.0}; // Retro, Tape, Tube
        m_exciterBand = dsp::highPass(sampleRate, safeFrequency(kExciterHz[static_cast<std::size_t>(m_mode)], sampleRate));
        m_rise = riseFactor(100.0, sampleRate);
        m_settle = std::max({m_room.settleFrames(), sampleRate / 10, m_limiter.latencyFrames() + releaseSettle(100.0, sampleRate)});
    }

    int latencyFrames() const noexcept override { return m_limiter.latencyFrames(); }
    int settleFrames() const noexcept override { return std::min(m_settle, settleCap(m_rate) + m_limiter.latencyFrames()); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        const double r = m_rate;
        const auto ch = static_cast<std::size_t>(m_channels);
        // Equalizer.
        if (m_lowOn || m_peakOn || m_highOn) {
            const BiquadCoefficients low = dsp::lowShelf(r, safeFrequency(v[m_lowF], r), v[m_lowG]);
            const BiquadCoefficients peak = dsp::peaking(r, safeFrequency(v[m_peakF], r), v[m_peakG], std::max(v[m_peakQ], 0.05));
            const BiquadCoefficients high = dsp::highShelf(r, safeFrequency(v[m_highF], r), v[m_highG]);
            for (int i = 0; i < frames; ++i) {
                float* frame = io + static_cast<std::size_t>(i) * ch;
                for (std::size_t c = 0; c < ch; ++c) {
                    float x = frame[c];
                    if (m_lowOn) {
                        x = m_eq[c][0].process(low, x);
                    }
                    if (m_peakOn) {
                        x = m_eq[c][1].process(peak, x);
                    }
                    if (m_highOn) {
                        x = m_eq[c][2].process(high, x);
                    }
                    frame[c] = x;
                }
            }
        }
        // Reverb: a small room added to the dry signal (the room's crossfade mix, its dry loss made up: at 100 % the
        // tail is mixed in at 0.43 with the dry at full level).
        dsp::ReverbSettings room;
        room.decayMs = 1200.0;
        room.damping = 0.6;
        room.mix = 0.3 * std::clamp(v[m_reverb], 0.0, 100.0) / 100.0;
        room.width = 1.0;
        m_room.setSettings(room);
        m_room.process(io, frames);
        const double dryMakeUp = 1.0 / (1.0 - room.mix);
        // Exciter: harmonics of the top end (high-passed, shaped, high-passed again), added.
        const double excite = 0.5 * std::clamp(v[m_exciter], 0.0, 100.0) / 100.0;
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                const auto x = static_cast<float>(frame[c] * dryMakeUp);
                const float band = m_exciterIn[c].process(m_exciterBand, x);
                const float harmonics = m_exciterOut[c].process(m_exciterBand, static_cast<float>(shape(band)));
                frame[c] = static_cast<float>(x + excite * harmonics);
            }
        }
        // Widener: mid/side, -100 % mono .. +100 % twice the side.
        m_width.setSettings(1.0 + std::clamp(v[m_widener], -100.0, 100.0) / 100.0, 0.0);
        m_width.process(io, frames);
        // Loudness Maximizer (up to +12 dB into the limiter; ceiling 0 dBFS at 0 %, -0.3 dBFS at 100 %), Output Gain.
        const double amount = std::clamp(v[m_maximizer], 0.0, 100.0) / 100.0;
        const double drive = dbToGain(12.0 * amount);
        const double ceiling = dbToGain(-0.3 * amount);
        Ramp output = gainRampOf(cell, m_output);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(frame[c] * drive);
            }
            m_limiter.process(frame, ceiling, m_rise);
            const auto g = static_cast<float>(output.value);
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] *= g;
            }
            output.value += output.step;
        }
    }

    void reset() noexcept override {
        for (auto& states : m_eq) {
            for (BiquadState& s : states) {
                s.reset();
            }
        }
        for (std::size_t c = 0; c < m_exciterIn.size(); ++c) {
            m_exciterIn[c].reset();
            m_exciterOut[c].reset();
        }
        m_room.reset();
        m_width.reset();
        m_limiter.reset();
    }

private:
    /// The exciter's distortion products (the shaped band minus nothing linear): Retro both orders, Tape odd, Tube even.
    double shape(double band) const noexcept {
        const double h = std::clamp(4.0 * band, -1.0, 1.0);
        switch (m_mode) {
        case 1:
            return h * h * h;
        case 2:
            return h * h;
        default:
            return 0.6 * h * std::abs(h) + 0.3 * h * h;
        }
    }

    int m_channels;
    int m_rate;
    int m_lowF, m_lowG, m_peakF, m_peakG, m_peakQ, m_highF, m_highG, m_reverb, m_exciter, m_widener, m_maximizer, m_output;
    bool m_lowOn = false, m_peakOn = false, m_highOn = false;
    int m_mode = 0;
    BiquadCoefficients m_exciterBand{};
    double m_rise = 1.0;
    int m_settle = 0;
    std::array<std::array<BiquadState, 3>, kMaxDspChannels> m_eq{};
    std::array<BiquadState, kMaxDspChannels> m_exciterIn{};
    std::array<BiquadState, kMaxDspChannels> m_exciterOut{};
    dsp::Reverb m_room;
    dsp::StereoWidth m_width;
    LookaheadLimiter m_limiter;
};

// ---- Guitar Suite --------------------------------------------------------------------------------------------------

class GuitarSuiteProcessor final : public EffectProcessor {
public:
    GuitarSuiteProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_rate(sampleRate), m_compressor(paramIndex(d, "compressor")),
          m_filterFrequency(paramIndex(d, "filterFrequency")), m_filterResonance(paramIndex(d, "filterResonance")),
          m_distortion(paramIndex(d, "distortion")), m_mix(paramIndex(d, "mix")), m_output(paramIndex(d, "outputGain")),
          m_pole(dcPole(sampleRate)) {
        const Constants k(d, values);
        m_filterType = choiceOf(k("filterType"), 4);
        m_distortionType = choiceOf(k("distortionType"), 3);
        const double r = sampleRate;
        const auto f = [r](double hz) { return safeFrequency(hz, r); };
        switch (choiceOf(k("amplifier"), 4)) {
        case 1: // Combo (open-back 1x12)
            m_cabinet = {dsp::highPass(r, f(90.0)), dsp::peaking(r, f(200.0), 2.0, 1.0), dsp::peaking(r, f(500.0), -3.0, 1.0),
                         dsp::peaking(r, f(2200.0), 3.0, 1.2), dsp::lowPass(r, f(4500.0)), dsp::lowPass(r, f(4500.0))};
            m_cabinetSections = 6;
            break;
        case 2: // Stack (closed 4x12)
            m_cabinet = {dsp::highPass(r, f(70.0)), dsp::peaking(r, f(110.0), 4.0, 1.2), dsp::peaking(r, f(400.0), -4.0, 0.9),
                         dsp::peaking(r, f(1800.0), 3.0, 1.0), dsp::lowPass(r, f(5500.0)), dsp::lowPass(r, f(5500.0))};
            m_cabinetSections = 6;
            break;
        case 3: // Small practice amp
            m_cabinet = {dsp::highPass(r, f(150.0)), dsp::peaking(r, f(1000.0), 3.0, 0.8), dsp::lowPass(r, f(3200.0)),
                         dsp::lowPass(r, f(3200.0)), BiquadCoefficients{}, BiquadCoefficients{}};
            m_cabinetSections = 4;
            break;
        default:
            m_cabinetSections = 0;
            break;
        }
    }

    int settleFrames() const noexcept override { return m_rate / 2; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        const double r = m_rate;
        // Compressor: the amount sets threshold (0 .. -36 dB) and ratio (1 .. 8), with half the reduction made up.
        const double squeeze = std::clamp(v[m_compressor], 0.0, 100.0) / 100.0;
        CompressorSettings comp{-36.0 * squeeze, 1.0 + 7.0 * squeeze, 6.0, timeCoefficient(3.0, r), timeCoefficient(120.0, r)};
        const double makeUp = dbToGain(0.5 * 36.0 * squeeze * (1.0 - 1.0 / comp.ratio));
        // Filter.
        const double q = kButterworthQ + 0.093 * std::clamp(v[m_filterResonance], 0.0, 100.0);
        const double fc = safeFrequency(v[m_filterFrequency], r);
        BiquadCoefficients filter{};
        if (m_filterType == 1) {
            filter = dsp::lowPass(r, fc, q);
        } else if (m_filterType == 2) {
            filter = dsp::highPass(r, fc, q);
        } else if (m_filterType == 3) {
            filter = bandPass(r, fc, q);
        }
        // Distortion: up to 40 dB of drive into the shaper, the level compensated, crossfaded in over the first 10 %.
        const double grit = std::clamp(v[m_distortion], 0.0, 100.0) / 100.0;
        const double drive = dbToGain(40.0 * grit);
        const double compensate = 1.0 / std::sqrt(drive);
        const double blend = std::min(1.0, 10.0 * grit);
        const double mix = std::clamp(v[m_mix], 0.0, 100.0) / 100.0;
        Ramp output = gainRampOf(cell, m_output);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const double g = squeeze > 0.0 ? m_comp.gain(framePeak(frame, ch), comp) * makeUp : 1.0;
            for (std::size_t c = 0; c < ch; ++c) {
                const double dry = frame[c];
                double y = dry * g;
                if (m_filterType != 0) {
                    y = m_filterState[c].process(filter, static_cast<float>(y));
                }
                if (blend > 0.0) {
                    y = (1.0 - blend) * y + blend * compensate * distort(y * drive, c);
                }
                for (int s = 0; s < m_cabinetSections; ++s) {
                    const auto si = static_cast<std::size_t>(s);
                    y = m_cabinetState[c][si].process(m_cabinet[si], static_cast<float>(y));
                }
                frame[c] = static_cast<float>(((1.0 - mix) * dry + mix * y) * output.value);
            }
            output.value += output.step;
        }
    }

    void reset() noexcept override {
        m_comp.reset();
        for (std::size_t c = 0; c < m_filterState.size(); ++c) {
            m_filterState[c].reset();
            m_dc[c].reset();
            for (BiquadState& s : m_cabinetState[c]) {
                s.reset();
            }
        }
    }

private:
    double distort(double x, std::size_t c) noexcept {
        switch (m_distortionType) {
        case 1: { // Crunch: a cubic soft clipper with a hard ceiling
            const double v = std::clamp(x, -1.0, 1.0);
            return 1.5 * v - 0.5 * v * v * v;
        }
        case 2: // Fuzz: asymmetric (even harmonics), its offset removed
            return m_dc[c].process(std::tanh(x + 0.4) - std::tanh(0.4), m_pole);
        default: // Overdrive
            return std::tanh(x);
        }
    }

    int m_channels;
    int m_rate;
    int m_compressor, m_filterFrequency, m_filterResonance, m_distortion, m_mix, m_output;
    double m_pole;
    int m_filterType = 0;
    int m_distortionType = 0;
    int m_cabinetSections = 0;
    std::array<BiquadCoefficients, 6> m_cabinet{};
    LinkedCompressor m_comp;
    std::array<BiquadState, kMaxDspChannels> m_filterState{};
    std::array<DcBlocker, kMaxDspChannels> m_dc{};
    std::array<std::array<BiquadState, 6>, kMaxDspChannels> m_cabinetState{};
};

// ---- Pitch Shifter --------------------------------------------------------------------------------------------------

class PitchShifterProcessor final : public EffectProcessor {
public:
    PitchShifterProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_semitones(paramIndex(d, "semitones")), m_cents(paramIndex(d, "cents")),
          m_ratio(paramIndex(d, "ratio")) {
        const Constants k(d, values);
        const double rate = sampleRate;
        m_precision = choiceOf(k("precision"), 3);
        const auto p = static_cast<std::size_t>(m_precision);
        double splicing = k("splicingFrequency");
        double overlap = k("overlapping");
        if (isOn(k("useDefaultSettings"))) { // Low, Medium, High
            static constexpr std::array<double, 3> kSplicing{40.0, 30.0, 25.0};
            static constexpr std::array<double, 3> kOverlap{15.0, 25.0, 35.0};
            splicing = kSplicing[p];
            overlap = kOverlap[p];
        }
        splicing = std::clamp(splicing, 5.0, 200.0);
        overlap = std::clamp(overlap, 0.0, 50.0) / 100.0;
        // A splice (grain) of 1 / Splicing Frequency, of which Overlapping is shared with each neighbour (crossfade),
        // at least 1 ms of crossfade so splices never click.
        m_grain = std::max(8, static_cast<int>(std::lround(rate / splicing)));
        const int minFade = std::min(std::max(4, framesOf(1.0, rate)), m_grain / 2);
        m_fadeFrames = std::clamp(static_cast<int>(std::lround(overlap * m_grain)), minFade, m_grain / 2);
        m_hop = m_grain - m_fadeFrames;
        // Precision: the search for the best splice point (+- ms: half the period of the lowest pitch it aligns, about
        // 170, 85 and 50 Hz) and the interpolation (linear, 8 or 16 sinc taps).
        static constexpr std::array<double, 3> kSearchMs{3.0, 6.0, 10.0};
        static constexpr std::array<int, 3> kHalfTaps{0, 4, 8};
        m_search = framesOf(kSearchMs[p], rate);
        m_taps = kHalfTaps[p];
        m_reach = m_taps == 0 ? 2 : 2 * m_taps + 2; // samples past a read position the interpolation touches (ratio <= 2)
        m_window = std::min(m_fadeFrames, std::max(8, framesOf(10.0, rate)));
        // The latency that keeps every read (and the splice search) in the past for ratios 0.5 .. 2 (see startGrain()).
        m_latency = static_cast<int>(std::ceil(0.5 * m_hop + 1.5 * m_fadeFrames)) + m_search + m_reach + 2;
        std::size_t size = 1;
        while (size < static_cast<std::size_t>(m_latency + 2 * m_grain + 2 * m_search + 2 * m_reach + 64)) {
            size <<= 1;
        }
        m_mask = static_cast<std::int64_t>(size) - 1;
        m_ring.assign(size * static_cast<std::size_t>(channels), 0.0f);
        m_mono.assign(size, 0.0f);
        m_scores.assign(static_cast<std::size_t>(2 * m_search + 3), 0.0);
        m_fade.resize(static_cast<std::size_t>(m_fadeFrames));
        for (int j = 0; j < m_fadeFrames; ++j) {
            m_fade[static_cast<std::size_t>(j)] = 0.5 - 0.5 * std::cos(kPi * (j + 0.5) / m_fadeFrames);
        }
        if (m_taps > 0) { // windowed sinc (Blackman), sampled every 1 / kResolution; exact zeros at the integers
            m_kernel.resize(static_cast<std::size_t>(m_taps * kResolution + 2), 0.0);
            for (int j = 0; j <= m_taps * kResolution; ++j) {
                const double t = static_cast<double>(j) / kResolution;
                double w = 0.0;
                if (j == 0) {
                    w = 1.0;
                } else if (j % kResolution != 0) {
                    const double u = t / m_taps;
                    w = std::sin(kPi * t) / (kPi * t) * (0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2.0 * kPi * u));
                }
                m_kernel[static_cast<std::size_t>(j)] = w;
            }
        }
        reset();
    }

    int latencyFrames() const noexcept override { return m_latency; }
    int settleFrames() const noexcept override { return m_latency + m_grain; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double ratio = ratioOf(cell);
        const auto ch = static_cast<std::size_t>(m_channels);
        const std::size_t size = m_mono.size();
        for (int f = 0; f < frames; ++f) {
            float* frame = io + static_cast<std::size_t>(f) * ch;
            const auto slot = static_cast<std::size_t>(m_time & m_mask);
            float mono = 0.0f;
            for (std::size_t c = 0; c < ch; ++c) {
                m_ring[c * size + slot] = frame[c];
                mono += frame[c];
            }
            m_mono[slot] = mono;
            if (m_time == m_nextGrain) {
                startGrain(ratio);
                m_nextGrain += m_hop;
            }
            const double wIn = m_current.age < m_fadeFrames ? m_fade[static_cast<std::size_t>(m_current.age)] : 1.0;
            const double wOut = m_previous.active ? 1.0 - m_fade[static_cast<std::size_t>(m_previous.age - m_hop)] : 0.0;
            for (std::size_t c = 0; c < ch; ++c) {
                const float* ring = m_ring.data() + c * size;
                double out = wIn * read(ring, m_current);
                if (m_previous.active) {
                    out += wOut * read(ring, m_previous);
                }
                frame[c] = static_cast<float>(out);
            }
            advance(m_current);
            advance(m_previous);
            ++m_time;
        }
    }

    void reset() noexcept override {
        std::fill(m_ring.begin(), m_ring.end(), 0.0f);
        std::fill(m_mono.begin(), m_mono.end(), 0.0f);
        m_time = 0;
        m_nextGrain = 0;
        m_current = Grain{};
        m_previous = Grain{};
    }

private:
    static constexpr int kResolution = 512;

    struct Grain {
        double position = 0.0; ///< input frame read next (fractional)
        double step = 1.0;     ///< the pitch ratio: input frames per output frame
        double cutoff = 1.0;   ///< the interpolator's bandwidth (< 1 when reading faster than real time)
        int age = 0;           ///< output frames since it started
        bool active = false;
    };

    double ratioOf(const ParamCell& cell) const noexcept {
        const double semitones = cell.start[m_semitones];
        const double cents = cell.start[m_cents];
        const double ratio = semitones == 0.0 && cents == 0.0 ? cell.start[m_ratio]
                                                              : std::exp2((semitones + cents / 100.0) / 12.0);
        return std::clamp(ratio, 0.5, 2.0);
    }

    void advance(Grain& g) const noexcept {
        if (!g.active) {
            return;
        }
        g.position += g.step;
        if (++g.age >= m_grain) {
            g.active = false;
        }
    }

    /// Starts a grain at the current frame. It reads the input at `ratio` from a position whose middle lines up with
    /// the output m_latency frames ago (nominal), moved by up to m_search frames to where the input matches what the
    /// outgoing grain is about to play. With D = m_latency, P = m_hop, X = m_fadeFrames and G = m_grain, the outgoing
    /// grain's reads end at most (1 + r) G / 2 - P - D + search after now, which D >= P / 2 + 3 X / 2 + search keeps in
    /// the past for r <= 2 (and its own reads, (r - 1) G / 2 + search, too).
    void startGrain(double ratio) noexcept {
        const double nominal = static_cast<double>(m_time) - m_latency + (1.0 - ratio) * m_grain * 0.5;
        double start = nominal;
        if (m_current.active && !(ratio == 1.0 && m_current.step == 1.0)) {
            start = alignedStart(m_current.position, nominal, std::min(1.0, m_current.step));
        }
        m_previous = m_current; // fades out over the next m_fadeFrames
        m_current = Grain{start, ratio, std::min(1.0, 1.0 / ratio), 0, true};
    }

    /// The start nearest `nominal` (within m_search) whose input best continues the outgoing grain read at `tail`: the
    /// normalised cross-correlation of the mono input over the crossfade at whole-frame offsets from `tail`, its peak
    /// refined to a fraction of a frame by a parabola through its neighbours (a whole-frame splice of a 440 Hz tone
    /// would leave up to 0.03 rad of phase error at every splice).
    double alignedStart(double tail, double nominal, double tailRatio) noexcept {
        const double offset = nominal - tail;
        const auto lo = static_cast<std::int64_t>(std::ceil(offset - m_search));
        const auto hi = static_cast<std::int64_t>(std::floor(offset + m_search));
        const int window = std::min(m_window, static_cast<int>(tailRatio * m_fadeFrames));
        const auto onTime = std::clamp(static_cast<std::int64_t>(std::llround(offset)), std::min(lo, hi), std::max(lo, hi));
        if (window < 8 || hi < lo) {
            return tail + static_cast<double>(onTime);
        }
        const auto base = static_cast<std::int64_t>(std::floor(tail));
        const auto at = [this](std::int64_t i) { return static_cast<double>(m_mono[static_cast<std::size_t>(i & m_mask)]); };
        double reference = 0.0;
        for (int m = 0; m < window; ++m) {
            reference += at(base + m) * at(base + m);
        }
        if (reference < 1e-12 * window) {
            return tail + static_cast<double>(onTime); // silence: stay on time
        }
        double energy = 0.0;
        for (int m = 0; m < window; ++m) {
            energy += at(base + lo + m) * at(base + lo + m);
        }
        double bestScore = 0.0;
        std::int64_t best = onTime;
        for (std::int64_t delta = lo; delta <= hi; ++delta) {
            if (delta > lo) {
                const double gone = at(base + delta - 1);
                const double added = at(base + delta + window - 1);
                energy += added * added - gone * gone;
            }
            double cross = 0.0;
            for (int m = 0; m < window; ++m) {
                cross += at(base + m) * at(base + delta + m);
            }
            const double score = energy > 1e-12 * window ? cross / std::sqrt(energy) : 0.0;
            m_scores[static_cast<std::size_t>(delta - lo)] = score;
            // Ties (a periodic input matches at every period) go to the start nearest the nominal one.
            const double tolerance = 1e-9 * std::abs(bestScore);
            const bool closer = std::abs(static_cast<double>(delta) - offset) < std::abs(static_cast<double>(best) - offset);
            if (delta == lo || score > bestScore + tolerance || (score >= bestScore - tolerance && closer)) {
                bestScore = score;
                best = delta;
            }
        }
        double fraction = 0.0;
        if (best > lo && best < hi) {
            const auto i = static_cast<std::size_t>(best - lo);
            const double before = m_scores[i - 1];
            const double after = m_scores[i + 1];
            const double curvature = before - 2.0 * bestScore + after;
            if (curvature < 0.0) {
                fraction = std::clamp(0.5 * (before - after) / curvature, -0.5, 0.5);
            }
        }
        return tail + static_cast<double>(best) + fraction;
    }

    /// The channel's input at the grain's read position.
    float read(const float* ring, const Grain& g) const noexcept {
        const double position = g.position;
        const auto base = static_cast<std::int64_t>(std::floor(position));
        const double frac = position - static_cast<double>(base);
        if (m_taps == 0) {
            const double a = ring[static_cast<std::size_t>(base & m_mask)];
            const double b = ring[static_cast<std::size_t>((base + 1) & m_mask)];
            return static_cast<float>(a + frac * (b - a));
        }
        // Windowed sinc, stretched by 1 / cutoff when reading faster than real time (low-passes what would alias).
        const double span = m_taps / g.cutoff;
        const auto reach = static_cast<std::int64_t>(std::ceil(span));
        double sum = 0.0;
        double weights = 0.0;
        for (std::int64_t i = base - reach + 1; i <= base + reach; ++i) {
            const double t = std::abs(position - static_cast<double>(i)) * g.cutoff;
            if (t >= m_taps) {
                continue;
            }
            const double x = t * kResolution;
            const auto j = static_cast<std::size_t>(x);
            const double w = m_kernel[j] + (x - static_cast<double>(j)) * (m_kernel[j + 1] - m_kernel[j]);
            sum += w * ring[static_cast<std::size_t>(i & m_mask)];
            weights += w;
        }
        return weights > 1e-9 ? static_cast<float>(sum / weights) : 0.0f;
    }

    int m_channels;
    int m_semitones, m_cents, m_ratio;
    int m_precision = 2;
    int m_grain = 0;
    int m_fadeFrames = 0;
    int m_hop = 0;
    int m_search = 0;
    int m_taps = 0;
    int m_reach = 0;
    int m_window = 0;
    int m_latency = 0;
    std::int64_t m_mask = 0;
    std::vector<float> m_ring; ///< per channel, m_mono.size() frames each
    std::vector<float> m_mono; ///< the channels' sum (the splice search)
    std::vector<double> m_fade;
    std::vector<double> m_kernel;
    std::vector<double> m_scores; ///< the splice search's correlation per offset
    std::int64_t m_time = 0;
    std::int64_t m_nextGrain = 0;
    Grain m_current;
    Grain m_previous;
};

} // namespace

bool isAmplitudeEffect(std::string_view id) noexcept {
    return id == kAmplify || id == kChannelMixer || id == kDynamics || id == kHardLimiter || id == kMultibandCompressor ||
           id == kSingleBandCompressor || id == kTubeModeledCompressor || id == kDistortion || id == kGuitarSuite ||
           id == kMastering || id == kVocalEnhancer || id == kPitchShifter;
}

std::unique_ptr<EffectProcessor> createAmplitudeProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                          std::span<const double> values) {
    channels = std::clamp(channels, 1, kMaxDspChannels);
    sampleRate = std::max(sampleRate, 8000);
    const EffectDescriptor& d = descriptor;
    if (values.size() < d.params.size()) {
        return nullptr;
    }
    if (d.id == kAmplify) {
        return std::make_unique<AmplifyProcessor>(d, channels, values);
    }
    if (d.id == kChannelMixer) {
        return std::make_unique<ChannelMixerProcessor>(d, channels, values);
    }
    if (d.id == kDynamics) {
        return std::make_unique<DynamicsProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kHardLimiter) {
        return std::make_unique<HardLimiterProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kMultibandCompressor) {
        return std::make_unique<MultibandCompressorProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kSingleBandCompressor) {
        return std::make_unique<SingleBandCompressorProcessor>(d, channels, sampleRate, values, false);
    }
    if (d.id == kTubeModeledCompressor) {
        return std::make_unique<SingleBandCompressorProcessor>(d, channels, sampleRate, values, true);
    }
    if (d.id == kDistortion) {
        return std::make_unique<DistortionProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kGuitarSuite) {
        return std::make_unique<GuitarSuiteProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kMastering) {
        return std::make_unique<MasteringProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kVocalEnhancer) {
        return std::make_unique<VocalEnhancerProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kPitchShifter) {
        return std::make_unique<PitchShifterProcessor>(d, channels, sampleRate, values);
    }
    return nullptr;
}

} // namespace wabcut::audio::premiere
