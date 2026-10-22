#include "PremiereEffects.h"

// p02 audio: Premiere Pro's Delay and Echo, Modulation, Reverb and Noise Reduction/Restoration effects (descriptors:
// src/model/src/EffectsAudioTime.cpp). Own implementations written from the textbook math (no third-party code):
//
//   * Delay, Analog Delay, Multitap Delay: circular delay lines sized for the parameters' maxima at construction, read
//     with 4-point cubic Hermite interpolation at a delay that glides (a 50 ms one-pole) to the time asked, so
//     automation never zips. Linear feedback is capped below runaway (0.98); Analog Delay's loop saturates instead (its
//     Feedback goes to 200 % like the hardware's: the repeats grow until the loop's soft clipper holds them).
//   * Chorus/Flanger, Flanger, Phaser: LFOs counted in samples from the processor's reset (the chain's origin, or the
//     start of the pre-roll after a seek); swept delay lines (chorus, flanger) and a cascade of first-order all-pass
//     sections (phaser).
//   * DeReverb: on dsp::Stft, the late reverberation of each bin is predicted from its smoothed power 50 ms earlier,
//     decayed by an assumed RT60 (after Lebart, Boucher and Denbigh 2001), and subtracted with a floor.
//   * DeNoise, Adaptive Noise Reduction: on dsp::Stft, each bin's noise floor is the minimum of its smoothed power over
//     ~1.5 s (minimum statistics, after Martin 2001), made robust against steady tones by a median over neighbouring
//     bins; decision-directed Wiener gains (Ephraim and Malah 1984) with a floor.
//   * Automatic Click Remover: the second difference of the signal against its local (robust) level finds impulses; a
//     click is replaced by a crossfade of forward and backward linear prediction (autocorrelation method,
//     Levinson-Durbin) from the clean sound on either side.
//   * Convolution Reverb: impulse responses synthesised at construction from seeded noise (three bands with their own
//     exponential decays, plus early reflections), convolved by uniformly partitioned overlap-save FFT convolution.
//   * Studio Reverb: a feedback delay network (Jot and Chaigne 1991, like dsp::Reverb) with its own early
//     reflections, input diffusers, damping, band limits, width and dry/wet levels. Surround Reverb is the same network
//     read with four orthogonal sign patterns: decorrelated front and surround tails for 5.1 / 7.1.
//
// Everything advances per sample (the STFT effects per frame, counted in samples), so the output never depends on how
// the stream is cut into blocks. Construction allocates; process() and reset() are RT-safe.

#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace wabcut::audio::premiere {

namespace {

constexpr std::string_view kDelay = "wabcut.audio.delay";
constexpr std::string_view kAnalogDelay = "wabcut.audio.analogDelay";
constexpr std::string_view kMultitapDelay = "wabcut.audio.multitapDelay";
constexpr std::string_view kChorusFlanger = "wabcut.audio.chorusFlanger";
constexpr std::string_view kFlanger = "wabcut.audio.flanger";
constexpr std::string_view kPhaser = "wabcut.audio.phaser";
constexpr std::string_view kConvolutionReverb = "wabcut.audio.convolutionReverb";
constexpr std::string_view kStudioReverb = "wabcut.audio.studioReverb";
constexpr std::string_view kSurroundReverb = "wabcut.audio.surroundReverb";
constexpr std::string_view kDeReverb = "wabcut.audio.deReverb";
constexpr std::string_view kSpectralDenoise = "wabcut.audio.spectralDenoise";
constexpr std::string_view kAdaptiveNoiseReduction = "wabcut.audio.adaptiveNoiseReduction";
constexpr std::string_view kClickRemover = "wabcut.audio.automaticClickRemover";

constexpr int kMaxChannels = 8;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kTiny = 1e-25;       ///< state below this is flushed to exactly 0 (no denormals in long silences)
constexpr double kMaxFeedback = 0.98; ///< linear feedback loops stay below runaway

double dbToGain(double db) noexcept { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }
double flushed(double v) noexcept { return std::fabs(v) < kTiny ? 0.0 : v; }
/// A percentage as 0..1.
double unit(double percent) noexcept { return std::clamp(percent * 0.01, 0.0, 1.0); }

/// A constant (non-animatable) parameter's value at build time: fixed for the processor's life.
double constantOf(const EffectDescriptor& d, std::span<const double> values, std::string_view key) noexcept {
    const auto i = static_cast<std::size_t>(paramIndex(d, key));
    return i < values.size() ? values[i] : 0.0;
}

int choiceOf(const EffectDescriptor& d, std::span<const double> values, std::string_view key, int lo, int hi) noexcept {
    return std::clamp(static_cast<int>(std::lround(constantOf(d, values, key))), lo, hi);
}

double maxOf(const EffectDescriptor& d, std::string_view key) noexcept {
    const auto i = static_cast<std::size_t>(paramIndex(d, key));
    return i < d.params.size() ? d.params[i].maxValue : 0.0;
}

/// An animatable parameter over one process() call: linear from the cell's start value to its end value, per frame
/// (the same samples however the cell is split into calls).
class Ramp {
public:
    Ramp() = default;
    Ramp(const ParamCell& cell, int index) noexcept {
        const double a = cell.start[index];
        const double b = cell.end[index];
        m_step = (b - a) / static_cast<double>(std::max(cell.length, 1));
        m_first = a + m_step * cell.offset;
    }
    double at(int frame) const noexcept { return m_first + m_step * frame; }

private:
    double m_first = 0.0;
    double m_step = 0.0;
};

double wrapPhase(double phase) noexcept { return phase - std::floor(phase); }
/// 0 at phase 0, 1 at phase 1/2.
double triangle(double phase) noexcept {
    const double p = wrapPhase(phase);
    return p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p;
}
/// 0 at phase 0, 1 at phase 1/2, sinusoidal in between.
double raisedCosine(double phase) noexcept { return 0.5 - 0.5 * std::cos(kTwoPi * phase); }

/// A power-of-two size spanning about as much time at `sampleRate` as `frames48k` frames do at 48 kHz.
int scaledPowerOfTwo(int frames48k, int sampleRate, int lo, int hi) noexcept {
    const double want = frames48k * (sampleRate / 48000.0);
    int n = lo;
    while (n < hi && n * std::numbers::sqrt2 < want) {
        n <<= 1;
    }
    return n;
}

/// Mid/side width of a stereo pair: 0 = mono, 1 = unchanged, 2 = the side doubled.
void widen(double& left, double& right, double width) noexcept {
    const double mid = 0.5 * (left + right);
    const double side = 0.5 * (left - right) * width;
    left = mid + side;
    right = mid - side;
}

/// Pre-roll of an echo: the delay times the number of repeats until they are 60 dB down, at most 2 s.
int echoSettle(double delayMs, double feedback, int sampleRate) noexcept {
    const double fb = std::clamp(feedback, 0.0, kMaxFeedback);
    const double repeats = fb > 1e-3 ? 1.0 + std::ceil(-3.0 / std::log10(fb)) : 1.0;
    return static_cast<int>(std::min(2.0 * sampleRate, std::max(delayMs, 0.0) * 1e-3 * sampleRate * repeats));
}

/// One channel's past input, read at fractional delays (4-point cubic Hermite; linear below 2 frames).
class DelayLine {
public:
    void allocate(int maxDelay) {
        int size = 8;
        while (size < maxDelay + 4) {
            size <<= 1;
        }
        m_buffer.assign(static_cast<std::size_t>(size), 0.0f);
        m_mask = size - 1;
        m_max = std::max(maxDelay, 1);
        m_position = 0;
    }
    void clear() noexcept {
        std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
        m_position = 0;
    }
    void write(double v) noexcept {
        m_buffer[static_cast<std::size_t>(m_position)] = static_cast<float>(flushed(v));
        m_position = (m_position + 1) & m_mask;
    }
    /// The sample written `delay` frames ago (1 = the last one written).
    double at(int delay) const noexcept { return m_buffer[static_cast<std::size_t>((m_position - delay) & m_mask)]; }
    /// At a fractional delay, clamped to [1, the allocated maximum].
    double read(double delay) const noexcept {
        delay = std::clamp(delay, 1.0, static_cast<double>(m_max));
        const int i = static_cast<int>(delay);
        const double t = delay - i;
        const double y1 = at(i);
        if (t == 0.0) {
            return y1;
        }
        const double y2 = at(i + 1);
        if (i < 2) {
            return y1 + t * (y2 - y1);
        }
        const double y0 = at(i - 1);
        const double y3 = at(i + 2);
        const double c1 = 0.5 * (y2 - y0);
        const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

private:
    std::vector<float> m_buffer;
    int m_mask = 0;
    int m_max = 1;
    int m_position = 0;
};

/// A delay time (frames) that glides to its target: a one-pole of `seconds`. Starts at the first target after reset.
class Glide {
public:
    void setTime(double seconds, double sampleRate) noexcept {
        m_coefficient = 1.0 - std::exp(-1.0 / std::max(1.0, seconds * sampleRate));
    }
    double next(double target) noexcept {
        if (!m_primed) {
            m_value = target;
            m_primed = true;
            return m_value;
        }
        m_value += (target - m_value) * m_coefficient;
        if (std::fabs(target - m_value) < 1e-6) {
            m_value = target;
        }
        return m_value;
    }
    void reset() noexcept { m_primed = false; }

private:
    double m_coefficient = 1.0;
    double m_value = 0.0;
    bool m_primed = false;
};

// ---- Delay and Echo ---------------------------------------------------------------------------------------------

/// Premiere's Delay: one echo per channel, fed back for repeats; Mix crossfades dry and echo.
class DelayProcessor final : public EffectProcessor {
public:
    DelayProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_msToFrames(sampleRate * 1e-3), m_delay(paramIndex(d, "delay")),
          m_feedback(paramIndex(d, "feedback")), m_mix(paramIndex(d, "mix")),
          m_settle(echoSettle(constantOf(d, values, "delay"), constantOf(d, values, "feedback") * 0.01, sampleRate)) {
        const int maxFrames = static_cast<int>(std::ceil(maxOf(d, "delay") * m_msToFrames)) + 1;
        m_lines.resize(static_cast<std::size_t>(channels));
        for (DelayLine& line : m_lines) {
            line.allocate(maxFrames);
        }
        m_glide.setTime(0.05, sampleRate);
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const Ramp delay(cell, m_delay);
        const Ramp feedback(cell, m_feedback);
        const Ramp mix(cell, m_mix);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            const double d = m_glide.next(delay.at(i) * m_msToFrames);
            const double fb = std::clamp(feedback.at(i) * 0.01, 0.0, kMaxFeedback);
            const double wet = unit(mix.at(i));
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                DelayLine& line = m_lines[c];
                const double x = frame[c];
                const double echo = line.read(d);
                line.write(x + fb * echo);
                frame[c] = static_cast<float>((1.0 - wet) * x + wet * echo);
            }
        }
    }

    void reset() noexcept override {
        for (DelayLine& line : m_lines) {
            line.clear();
        }
        m_glide.reset();
    }

private:
    int m_channels;
    double m_msToFrames;
    int m_delay, m_feedback, m_mix;
    int m_settle;
    std::vector<DelayLine> m_lines;
    Glide m_glide;
};

/// Premiere's Multitap Delay: four echoes, each its own delay line (so each tap's feedback repeats only itself).
class MultitapDelayProcessor final : public EffectProcessor {
public:
    static constexpr int kTaps = 4;

    MultitapDelayProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_msToFrames(sampleRate * 1e-3), m_mix(paramIndex(d, "mix")) {
        static constexpr std::array<std::string_view, kTaps> kDelayKeys{"delay1", "delay2", "delay3", "delay4"};
        static constexpr std::array<std::string_view, kTaps> kFeedbackKeys{"feedback1", "feedback2", "feedback3",
                                                                           "feedback4"};
        static constexpr std::array<std::string_view, kTaps> kLevelKeys{"level1", "level2", "level3", "level4"};
        m_lines.resize(static_cast<std::size_t>(kTaps * channels));
        for (int t = 0; t < kTaps; ++t) {
            const auto ti = static_cast<std::size_t>(t);
            m_delay[ti] = paramIndex(d, kDelayKeys[ti]);
            m_feedback[ti] = paramIndex(d, kFeedbackKeys[ti]);
            m_level[ti] = paramIndex(d, kLevelKeys[ti]);
            m_glides[ti].setTime(0.05, sampleRate);
            const int maxFrames = static_cast<int>(std::ceil(maxOf(d, kDelayKeys[ti]) * m_msToFrames)) + 1;
            for (int c = 0; c < channels; ++c) {
                m_lines[static_cast<std::size_t>(t * channels + c)].allocate(maxFrames);
            }
            m_settle = std::max(m_settle, echoSettle(constantOf(d, values, kDelayKeys[ti]),
                                                     constantOf(d, values, kFeedbackKeys[ti]) * 0.01, sampleRate));
        }
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        std::array<Ramp, kTaps> delay{};
        std::array<Ramp, kTaps> feedback{};
        std::array<Ramp, kTaps> level{};
        for (std::size_t t = 0; t < kTaps; ++t) {
            delay[t] = Ramp(cell, m_delay[t]);
            feedback[t] = Ramp(cell, m_feedback[t]);
            level[t] = Ramp(cell, m_level[t]);
        }
        const Ramp mix(cell, m_mix);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            std::array<double, kTaps> d{};
            std::array<double, kTaps> fb{};
            std::array<double, kTaps> gain{};
            for (std::size_t t = 0; t < kTaps; ++t) {
                d[t] = m_glides[t].next(delay[t].at(i) * m_msToFrames);
                fb[t] = std::clamp(feedback[t].at(i) * 0.01, 0.0, kMaxFeedback);
                gain[t] = dbToGain(level[t].at(i));
            }
            const double wet = unit(mix.at(i));
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                const double x = frame[c];
                double echoes = 0.0;
                for (std::size_t t = 0; t < kTaps; ++t) {
                    DelayLine& line = m_lines[t * ch + c];
                    const double echo = line.read(d[t]);
                    line.write(x + fb[t] * echo);
                    echoes += gain[t] * echo;
                }
                frame[c] = static_cast<float>((1.0 - wet) * x + wet * echoes);
            }
        }
    }

    void reset() noexcept override {
        for (DelayLine& line : m_lines) {
            line.clear();
        }
        for (Glide& g : m_glides) {
            g.reset();
        }
    }

private:
    int m_channels;
    double m_msToFrames;
    std::array<int, kTaps> m_delay{};
    std::array<int, kTaps> m_feedback{};
    std::array<int, kTaps> m_level{};
    int m_mix;
    int m_settle = 0;
    std::vector<DelayLine> m_lines; ///< tap-major: tap * channels + channel
    std::array<Glide, kTaps> m_glides{};
};

/// Audition's (and Premiere's) Analog Delay: the echo's feedback loop carries the hardware's character - a saturating
/// stage (symmetric for tape and bucket-brigade, asymmetric for the tube) and its band (tape bright, analog dark).
class AnalogDelayProcessor final : public EffectProcessor {
public:
    AnalogDelayProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_sampleRate(sampleRate), m_msToFrames(sampleRate * 1e-3),
          m_mode(choiceOf(d, values, "mode", 0, 2)), m_dry(paramIndex(d, "dryOutput")), m_wet(paramIndex(d, "wetOutput")),
          m_delay(paramIndex(d, "delay")), m_feedback(paramIndex(d, "feedback")), m_trash(paramIndex(d, "trash")),
          m_spread(paramIndex(d, "spread")),
          m_settle(echoSettle(constantOf(d, values, "delay"), constantOf(d, values, "feedback") * 0.01, sampleRate)) {
        // The loop's band per mode: tape (bright), tape and tube (a little darker), analog bucket-brigade (dark).
        static constexpr std::array<double, 3> kLowPassHz{8000.0, 6000.0, 3500.0};
        static constexpr std::array<double, 3> kHighPassHz{40.0, 60.0, 80.0};
        const auto m = static_cast<std::size_t>(m_mode);
        m_lowPass = dsp::lowPass(sampleRate, std::min(kLowPassHz[m], 0.45 * sampleRate));
        m_highPass = dsp::highPass(sampleRate, kHighPassHz[m]);
        const int maxFrames = static_cast<int>(std::ceil(maxOf(d, "delay") * m_msToFrames)) + 1;
        m_state.resize(static_cast<std::size_t>(channels));
        for (Channel& s : m_state) {
            s.line.allocate(maxFrames);
        }
        m_glide.setTime(0.05, sampleRate);
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double trashNow = unit(cell.start[m_trash]);
        if (trashNow != m_shelfTrash) {
            m_shelfTrash = trashNow;
            m_shelf = dsp::lowShelf(m_sampleRate, 200.0, 9.0 * trashNow); // Trash: low-end warmth on the echoes
        }
        const Ramp dry(cell, m_dry);
        const Ramp wet(cell, m_wet);
        const Ramp delay(cell, m_delay);
        const Ramp feedback(cell, m_feedback);
        const Ramp trash(cell, m_trash);
        const Ramp spread(cell, m_spread);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            const double d = m_glide.next(delay.at(i) * m_msToFrames);
            const double fb = std::clamp(feedback.at(i) * 0.01, 0.0, 2.0);
            const double tr = unit(trash.at(i));
            const double drive = 1.0 + 8.0 * tr;
            const double dryGain = unit(dry.at(i));
            const double wetGain = unit(wet.at(i));
            float* frame = io + static_cast<std::size_t>(i) * ch;
            std::array<double, kMaxChannels> echo{};
            for (std::size_t c = 0; c < ch; ++c) {
                Channel& s = m_state[c];
                const double x = frame[c];
                const double delayed = s.line.read(d);
                // The loop: saturation (bounded whatever the feedback), then the mode's band.
                const double loop = s.highPass.process(m_highPass, s.lowPass.process(m_lowPass, static_cast<float>(saturate(fb * delayed))));
                // Trash drives the input into the same kind of saturation (blended in, so it starts from clean).
                const double in = tr > 0.0 ? (1.0 - tr) * x + tr * std::tanh(drive * x) / drive : x;
                s.line.write(in + loop);
                echo[c] = s.shelf.process(m_shelf, static_cast<float>(delayed));
            }
            if (ch >= 2) {
                widen(echo[0], echo[1], std::clamp(spread.at(i) * 0.01, 0.0, 2.0));
            }
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(dryGain * frame[c] + wetGain * echo[c]);
            }
        }
    }

    void reset() noexcept override {
        for (Channel& s : m_state) {
            s.line.clear();
            s.lowPass.reset();
            s.highPass.reset();
            s.shelf.reset();
        }
        m_glide.reset();
    }

private:
    struct Channel {
        DelayLine line;
        dsp::BiquadState lowPass;
        dsp::BiquadState highPass;
        dsp::BiquadState shelf;
    };

    double saturate(double v) const noexcept {
        if (m_mode == 1) { // tube: asymmetric (even harmonics); the loop's high-pass removes the DC it makes
            return v >= 0.0 ? std::tanh(v) : std::tanh(1.3 * v) / 1.3;
        }
        return std::tanh(v);
    }

    int m_channels;
    int m_sampleRate;
    double m_msToFrames;
    int m_mode;
    int m_dry, m_wet, m_delay, m_feedback, m_trash, m_spread;
    int m_settle;
    dsp::BiquadCoefficients m_lowPass{};
    dsp::BiquadCoefficients m_highPass{};
    dsp::BiquadCoefficients m_shelf{}; ///< identity until Trash moves
    double m_shelfTrash = 0.0;
    std::vector<Channel> m_state;
    Glide m_glide;
};

// ---- Modulation -------------------------------------------------------------------------------------------------

/// Audition's Chorus/Flanger: Chorus = three voices delayed 12 ms + up to 12 ms (Width), their sweeps a third of a
/// cycle apart; Flanger = one voice 0.3 ms + up to 6 ms with feedback. Intensity crossfades towards an equal mix,
/// Transience boosts the wet signal on attacks (a fast over slow envelope).
class ChorusFlangerProcessor final : public EffectProcessor {
public:
    static constexpr int kVoices = 3;
    static constexpr double kChorusDelayMs = 12.0;
    static constexpr double kChorusSweepMs = 12.0;
    static constexpr double kFlangerDelayMs = 0.3;
    static constexpr double kFlangerSweepMs = 6.0;
    static constexpr double kFlangerFeedback = 0.5;

    ChorusFlangerProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_sampleRate(sampleRate), m_flanger(choiceOf(d, values, "mode", 0, 1) == 1),
          m_speed(paramIndex(d, "speed")), m_width(paramIndex(d, "width")), m_intensity(paramIndex(d, "intensity")),
          m_transience(paramIndex(d, "transience")) {
        const int maxFrames = static_cast<int>(std::ceil((kChorusDelayMs + kChorusSweepMs) * 1e-3 * sampleRate)) + 4;
        m_state.resize(static_cast<std::size_t>(channels));
        for (Channel& s : m_state) {
            s.line.allocate(maxFrames);
        }
        const auto coefficient = [sampleRate](double seconds) { return std::exp(-1.0 / (seconds * sampleRate)); };
        m_fastAttack = coefficient(0.0005);
        m_fastRelease = coefficient(0.02);
        m_slowAttack = coefficient(0.03);
        m_slowRelease = coefficient(0.15);
    }

    int settleFrames() const noexcept override { return m_sampleRate / 5; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const Ramp speed(cell, m_speed);
        const Ramp width(cell, m_width);
        const Ramp intensity(cell, m_intensity);
        const Ramp transience(cell, m_transience);
        const auto ch = static_cast<std::size_t>(m_channels);
        const double msToFrames = m_sampleRate * 1e-3;
        for (int i = 0; i < frames; ++i) {
            const double w = unit(width.at(i));
            const double mixAmount = 0.5 * unit(intensity.at(i));
            const double t = unit(transience.at(i));
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                Channel& s = m_state[c];
                const double x = frame[c];
                const double offset = 0.25 * static_cast<double>(c); // 90 degrees between neighbouring channels
                double wet = 0.0;
                if (m_flanger) {
                    const double delay = (kFlangerDelayMs + kFlangerSweepMs * w * raisedCosine(m_phase + offset)) * msToFrames;
                    wet = s.line.read(delay);
                    s.line.write(x + kFlangerFeedback * wet);
                } else {
                    for (int v = 0; v < kVoices; ++v) {
                        const double u = raisedCosine(m_phase + offset + static_cast<double>(v) / kVoices);
                        wet += s.line.read((kChorusDelayMs + kChorusSweepMs * w * u) * msToFrames);
                    }
                    wet /= kVoices;
                    s.line.write(x);
                }
                // Transience: the wet signal goes up while the fast envelope runs ahead of the slow one (attacks).
                const double a = std::fabs(x);
                s.fast = flushed(a + (s.fast - a) * (a > s.fast ? m_fastAttack : m_fastRelease));
                s.slow = flushed(a + (s.slow - a) * (a > s.slow ? m_slowAttack : m_slowRelease));
                double shaped = wet;
                if (t > 0.0 && s.slow > 1e-6) {
                    shaped *= 1.0 + 2.0 * t * std::clamp(s.fast / s.slow - 1.0, 0.0, 2.0);
                }
                frame[c] = static_cast<float>((1.0 - mixAmount) * x + mixAmount * shaped);
            }
            m_phase = wrapPhase(m_phase + std::max(speed.at(i), 0.0) / m_sampleRate);
        }
    }

    void reset() noexcept override {
        for (Channel& s : m_state) {
            s.line.clear();
            s.fast = 0.0;
            s.slow = 0.0;
        }
        m_phase = 0.0;
    }

private:
    struct Channel {
        DelayLine line;
        double fast = 0.0;
        double slow = 0.0;
    };

    int m_channels;
    int m_sampleRate;
    bool m_flanger;
    int m_speed, m_width, m_intensity, m_transience;
    double m_fastAttack = 0.0, m_fastRelease = 0.0, m_slowAttack = 0.0, m_slowRelease = 0.0;
    double m_phase = 0.0;
    std::vector<Channel> m_state;
};

/// Audition's Flanger: a delay swept between Initial and Final Delay Time (triangle, or raised cosine when
/// Sinusoidal) with feedback, mixed with the original. Inverted flips the delayed signal (and its feedback); Special
/// Effects feeds back with the opposite sign of the output (normal and inverted flanging mixed).
class FlangerProcessor final : public EffectProcessor {
public:
    static constexpr double kMaxFlangerFeedback = 0.95;

    FlangerProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_sampleRate(sampleRate), m_initial(paramIndex(d, "initialDelay")),
          m_final(paramIndex(d, "finalDelay")), m_phasing(paramIndex(d, "stereoPhasing")),
          m_feedback(paramIndex(d, "feedback")), m_rate(paramIndex(d, "modulationRate")), m_mix(paramIndex(d, "mix")),
          m_gain(paramIndex(d, "outputGain")), m_inverted(choiceOf(d, values, "inverted", 0, 1) == 1),
          m_special(choiceOf(d, values, "specialEffects", 0, 1) == 1),
          m_sinusoidal(choiceOf(d, values, "sinusoidal", 0, 1) == 1) {
        const double maxMs = std::max(maxOf(d, "initialDelay"), maxOf(d, "finalDelay"));
        const int maxFrames = static_cast<int>(std::ceil(maxMs * 1e-3 * sampleRate)) + 4;
        m_lines.resize(static_cast<std::size_t>(channels));
        for (DelayLine& line : m_lines) {
            line.allocate(maxFrames);
        }
    }

    int settleFrames() const noexcept override { return m_sampleRate / 5; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const Ramp startDelay(cell, m_initial);
        const Ramp endDelay(cell, m_final);
        const Ramp phasing(cell, m_phasing);
        const Ramp feedback(cell, m_feedback);
        const Ramp rate(cell, m_rate);
        const Ramp mix(cell, m_mix);
        const Ramp gain(cell, m_gain);
        const auto ch = static_cast<std::size_t>(m_channels);
        const double msToFrames = m_sampleRate * 1e-3;
        const double sign = m_inverted ? -1.0 : 1.0;
        const double loopSign = m_special ? -sign : sign;
        for (int i = 0; i < frames; ++i) {
            const double from = std::max(startDelay.at(i), 0.0);
            const double to = std::max(endDelay.at(i), 0.0);
            const double fb = std::clamp(feedback.at(i) * 0.01, 0.0, kMaxFlangerFeedback);
            const double wetShare = unit(mix.at(i));
            const double out = dbToGain(gain.at(i));
            const double rightOffset = phasing.at(i) / 360.0;
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                DelayLine& line = m_lines[c];
                const double phase = m_phase + ((c & 1) != 0 ? rightOffset : 0.0);
                const double u = m_sinusoidal ? raisedCosine(phase) : triangle(phase);
                const double x = frame[c];
                const double delayed = line.read((from + (to - from) * u) * msToFrames);
                line.write(x + loopSign * fb * delayed);
                frame[c] = static_cast<float>(((1.0 - wetShare) * x + wetShare * sign * delayed) * out);
            }
            m_phase = wrapPhase(m_phase + std::max(rate.at(i), 0.0) / m_sampleRate);
        }
    }

    void reset() noexcept override {
        for (DelayLine& line : m_lines) {
            line.clear();
        }
        m_phase = 0.0;
    }

private:
    int m_channels;
    int m_sampleRate;
    int m_initial, m_final, m_phasing, m_feedback, m_rate, m_mix, m_gain;
    bool m_inverted, m_special, m_sinusoidal;
    double m_phase = 0.0;
    std::vector<DelayLine> m_lines;
};

/// Audition's Phaser: Stages first-order all-pass sections whose common corner sweeps from Upper Frequency down by
/// Depth (100 % = to 0 Hz) at Modulation Rate; the phased signal (with Feedback around the cascade) is mixed with the
/// original by Intensity x Mix (an equal mix makes the deepest notches).
class PhaserProcessor final : public EffectProcessor {
public:
    static constexpr int kMaxStages = 12;
    static constexpr double kMaxPhaserFeedback = 0.9;

    PhaserProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_sampleRate(sampleRate), m_stages(choiceOf(d, values, "stages", 1, kMaxStages)),
          m_intensity(paramIndex(d, "intensity")), m_depth(paramIndex(d, "depth")), m_rate(paramIndex(d, "modulationRate")),
          m_phaseDifference(paramIndex(d, "phaseDifference")), m_upper(paramIndex(d, "upperFrequency")),
          m_feedback(paramIndex(d, "feedback")), m_mix(paramIndex(d, "mix")), m_gain(paramIndex(d, "outputGain")) {
        m_state.resize(static_cast<std::size_t>(channels));
    }

    int settleFrames() const noexcept override { return m_sampleRate / 10; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const Ramp intensity(cell, m_intensity);
        const Ramp depth(cell, m_depth);
        const Ramp rate(cell, m_rate);
        const Ramp phaseDifference(cell, m_phaseDifference);
        const Ramp upper(cell, m_upper);
        const Ramp feedback(cell, m_feedback);
        const Ramp mix(cell, m_mix);
        const Ramp gain(cell, m_gain);
        const auto ch = static_cast<std::size_t>(m_channels);
        const double nyquistLimit = 0.45 * m_sampleRate;
        for (int i = 0; i < frames; ++i) {
            const double top = std::clamp(upper.at(i), 10.0, nyquistLimit);
            const double sweep = unit(depth.at(i));
            const double fb = std::clamp(feedback.at(i) * 0.01, -kMaxPhaserFeedback, kMaxPhaserFeedback);
            const double share = unit(mix.at(i)) * unit(intensity.at(i));
            const double out = dbToGain(gain.at(i));
            const double rightOffset = -phaseDifference.at(i) / 360.0; // positive: the left channel leads
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                Channel& s = m_state[c];
                const double u = raisedCosine(m_phase + ((c & 1) != 0 ? rightOffset : 0.0));
                const double corner = std::max(top * (1.0 - sweep * u), 10.0);
                const double t = std::tan(std::numbers::pi * corner / m_sampleRate);
                const double a = (t - 1.0) / (t + 1.0);
                const double x = frame[c];
                double v = x + fb * s.last;
                for (int k = 0; k < m_stages; ++k) {
                    double& z = s.z[static_cast<std::size_t>(k)];
                    const double y = a * v + z; // (a + z^-1) / (1 + a z^-1), transposed direct form
                    z = flushed(v - a * y);
                    v = y;
                }
                s.last = flushed(v);
                frame[c] = static_cast<float>(((1.0 - share) * x + share * v) * out);
            }
            m_phase = wrapPhase(m_phase + std::max(rate.at(i), 0.0) / m_sampleRate);
        }
    }

    void reset() noexcept override {
        for (Channel& s : m_state) {
            s = Channel{};
        }
        m_phase = 0.0;
    }

private:
    struct Channel {
        std::array<double, kMaxStages> z{};
        double last = 0.0;
    };

    int m_channels;
    int m_sampleRate;
    int m_stages;
    int m_intensity, m_depth, m_rate, m_phaseDifference, m_upper, m_feedback, m_mix, m_gain;
    double m_phase = 0.0;
    std::vector<Channel> m_state;
};

// ---- Spectral helpers ---------------------------------------------------------------------------------------------

/// Processing Focus of DeNoise and DeReverb: how strongly a frequency is processed, 0.25 (outside the focus) .. 1.
/// 0 all frequencies, 1 low (below 400 Hz), 2 mid (400 Hz .. 4 kHz), 3 high (above 4 kHz), 4 low and high; the
/// weight falls off over two octaves outside the band.
double focusWeight(int focus, double hz) noexcept {
    const auto band = [hz](double lo, double hi) {
        const double f = std::max(hz, 1.0);
        double octaves = 0.0;
        if (f < lo) {
            octaves = std::log2(lo / f);
        } else if (f > hi) {
            octaves = std::log2(f / hi);
        }
        return std::clamp(1.0 - octaves / 2.0, 0.0, 1.0);
    };
    double w = 1.0;
    switch (focus) {
    case 1: w = band(1.0, 400.0); break;
    case 2: w = band(400.0, 4000.0); break;
    case 3: w = band(4000.0, 1e9); break;
    case 4: w = std::max(band(1.0, 400.0), band(4000.0, 1e9)); break;
    default: break;
    }
    return 0.25 + 0.75 * w;
}

/// Output gain after an STFT effect, ramped per sample.
void applyGain(float* io, int frames, int channels, const Ramp& gainDb) noexcept {
    const auto ch = static_cast<std::size_t>(channels);
    for (int i = 0; i < frames; ++i) {
        const double g = dbToGain(gainDb.at(i));
        if (g == 1.0) {
            continue;
        }
        float* frame = io + static_cast<std::size_t>(i) * ch;
        for (std::size_t c = 0; c < ch; ++c) {
            frame[c] = static_cast<float>(frame[c] * g);
        }
    }
}

// ---- DeReverb ---------------------------------------------------------------------------------------------------

/// DeReverb: per STFT bin, the late reverberation now is predicted as the smoothed power kLateMs ago decayed by an
/// RT60 of kAssumedRt60 (energy that arrived more than 50 ms ago and is still ringing), and subtracted with
/// over-subtraction 1.2 x Amount and a floor of -25 dB x Amount (both scaled by Processing Focus). Onsets (power
/// rising faster than the decay) pass; tails are pulled down. Auto Gain makes up the long-term level lost.
class DeReverbProcessor final : public EffectProcessor {
public:
    static constexpr double kAssumedRt60 = 1.0;  ///< seconds
    static constexpr double kLateMs = 50.0;      ///< early/late boundary
    static constexpr double kSmoothSeconds = 0.02;

    DeReverbProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_stft(channels, scaledPowerOfTwo(2048, sampleRate, 256, 16384)),
          m_amount(paramIndex(d, "amount")), m_gain(paramIndex(d, "gain")),
          m_autoGain(choiceOf(d, values, "autoGain", 0, 1) == 1) {
        const double hopSeconds = m_stft.hop() / static_cast<double>(sampleRate);
        m_lateFrames = std::max(1, static_cast<int>(std::lround(kLateMs * 1e-3 / hopSeconds)));
        m_decay = std::pow(10.0, -6.0 * hopSeconds * m_lateFrames / kAssumedRt60);
        m_smooth = std::exp(-hopSeconds / kSmoothSeconds);
        const int focus = choiceOf(d, values, "processingFocus", 0, 4);
        const auto bins = static_cast<std::size_t>(m_stft.bins());
        m_weight.resize(bins);
        for (std::size_t k = 0; k < bins; ++k) {
            m_weight[k] = focusWeight(focus, static_cast<double>(k) * sampleRate / m_stft.size());
        }
        m_state.resize(static_cast<std::size_t>(channels));
        for (Channel& s : m_state) {
            s.smooth.assign(bins, 0.0);
            s.history.assign(bins * static_cast<std::size_t>(m_lateFrames), 0.0);
        }
    }

    int latencyFrames() const noexcept override { return m_stft.latencyFrames(); }
    int settleFrames() const noexcept override { return m_stft.latencyFrames() + (m_lateFrames + 8) * m_stft.hop(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        m_amountNow = unit(cell.start[m_amount]);
        m_stft.process(io, frames, [this](int c, std::complex<double>* bins) { frame(c, bins); });
        applyGain(io, frames, m_channels, Ramp(cell, m_gain));
    }

    void reset() noexcept override {
        m_stft.reset();
        for (Channel& s : m_state) {
            std::fill(s.smooth.begin(), s.smooth.end(), 0.0);
            std::fill(s.history.begin(), s.history.end(), 0.0);
            s.slot = 0;
            s.inPower = 0.0;
            s.outPower = 0.0;
        }
    }

private:
    struct Channel {
        std::vector<double> smooth;  ///< smoothed power per bin
        std::vector<double> history; ///< the last m_lateFrames smoothed spectra (a ring of slots)
        int slot = 0;
        double inPower = 0.0;        ///< Auto Gain: long-term power in and out
        double outPower = 0.0;
    };

    void frame(int channel, std::complex<double>* x) noexcept {
        Channel& s = m_state[static_cast<std::size_t>(channel)];
        const auto bins = static_cast<std::size_t>(m_stft.bins());
        double* old = s.history.data() + static_cast<std::size_t>(s.slot) * bins;
        const double amount = m_amountNow;
        double in = 0.0;
        double out = 0.0;
        for (std::size_t k = 0; k < bins; ++k) {
            const double p = std::norm(x[k]);
            s.smooth[k] = flushed(m_smooth * s.smooth[k] + (1.0 - m_smooth) * p);
            const double late = m_decay * old[k];
            old[k] = s.smooth[k];
            double g = 1.0;
            if (amount > 0.0 && s.smooth[k] > 1e-30) {
                const double w = m_weight[k];
                const double lowest = std::pow(10.0, -25.0 * amount * w / 20.0);
                g = std::clamp(1.0 - 1.2 * amount * w * late / s.smooth[k], lowest, 1.0);
            }
            x[k] *= g;
            in += p;
            out += g * g * p;
        }
        s.slot = (s.slot + 1) % m_lateFrames;
        if (m_autoGain) {
            s.inPower = flushed(0.97 * s.inPower + 0.03 * in);
            s.outPower = flushed(0.97 * s.outPower + 0.03 * out);
            if (s.outPower > 1e-30) {
                const double makeup = std::clamp(std::sqrt(s.inPower / s.outPower), 1.0, 4.0);
                for (std::size_t k = 0; k < bins; ++k) {
                    x[k] *= makeup;
                }
            }
        }
    }

    int m_channels;
    dsp::Stft m_stft;
    int m_amount, m_gain;
    bool m_autoGain;
    int m_lateFrames = 1;
    double m_decay = 0.0;
    double m_smooth = 0.0;
    double m_amountNow = 0.0;
    std::vector<double> m_weight;
    std::vector<Channel> m_state;
};

// ---- DeNoise, Adaptive Noise Reduction --------------------------------------------------------------------------

/// How far the noise estimate (the median over 13 bins of each bin's minimum smoothed power over a window) lies below
/// the mean noise power, measured on white noise through dsp::Stft (75 % overlap, square-root Hann): rows are the
/// smoothing coefficient alpha (kBiasAlpha), columns the window length 32, 64, .. 4096 frames. It does not depend on
/// the FFT size.
constexpr std::array<double, 7> kBiasAlpha{0.70, 0.78, 0.85, 0.90, 0.95, 0.98, 0.99};
constexpr std::array<std::array<double, 8>, 7> kBiasTable{{
    {2.923, 3.576, 4.403, 5.262, 6.093, 6.893, 7.945, 8.922},
    {2.343, 2.811, 3.341, 3.930, 4.435, 4.949, 5.611, 6.173},
    {1.892, 2.192, 2.552, 2.922, 3.242, 3.589, 3.980, 4.351},
    {1.591, 1.791, 2.035, 2.291, 2.512, 2.734, 2.988, 3.222},
    {1.311, 1.415, 1.546, 1.687, 1.814, 1.938, 2.068, 2.198},
    {1.181, 1.206, 1.250, 1.316, 1.382, 1.448, 1.517, 1.588},
    {1.175, 1.153, 1.158, 1.187, 1.224, 1.267, 1.311, 1.357},
}};

/// Bilinear in alpha and log2(window).
double noiseBias(double alpha, int windowFrames) noexcept {
    const double a = std::clamp(alpha, kBiasAlpha.front(), kBiasAlpha.back());
    std::size_t row = 0;
    while (row + 2 < kBiasAlpha.size() && a > kBiasAlpha[row + 1]) {
        ++row;
    }
    const double t = (a - kBiasAlpha[row]) / (kBiasAlpha[row + 1] - kBiasAlpha[row]);
    const double x = std::clamp(std::log2(std::max(windowFrames, 1) / 32.0), 0.0, 7.0);
    const auto column = std::min<std::size_t>(static_cast<std::size_t>(x), 6);
    const double u = x - static_cast<double>(column);
    const auto at = [column, u](const std::array<double, 8>& r) { return r[column] + u * (r[column + 1] - r[column]); };
    return at(kBiasTable[row]) + t * (at(kBiasTable[row + 1]) - at(kBiasTable[row]));
}

/// The spectral noise reducer of DeNoise and Adaptive Noise Reduction (see the file comment). Settings change per
/// cell; the FFT size and Processing Focus are fixed.
class NoiseReducer {
public:
    struct Settings {
        double floorGain = 1.0;       ///< the lowest gain of a bin (1 = no reduction)
        double overSubtraction = 1.0; ///< Wiener gain xi / (xi + overSubtraction)
        double noiseScale = 1.0;      ///< multiplies the estimated noise floor
        double signalSnr = 0.0;       ///< a-posteriori SNR above which a bin is wanted sound (gain 1); 0 = none
        double releasePerFrame = 0.0; ///< a bin's gain falls by at most this factor per frame (0 = at once)
        int preserveBins = 0;         ///< wanted sound keeps its neighbours within this many bins
        bool noiseOnly = false;       ///< output what is removed
    };

    static constexpr int kSubWindows = 8;
    static constexpr double kWindowSeconds = 1.5; ///< minimum statistics window
    static constexpr int kMedianHalf = 6;         ///< bins on each side of the median across frequency
    static constexpr double kDecisionDirected = 0.92;

    NoiseReducer(int channels, int sampleRate, int fftSize, int focus)
        : m_stft(channels, fftSize), m_bins(m_stft.bins()), m_sampleRate(sampleRate) {
        const double hopSeconds = m_stft.hop() / static_cast<double>(sampleRate);
        m_hopSeconds = hopSeconds;
        m_binHz = static_cast<double>(sampleRate) / m_stft.size();
        // Smoothing over 30 ms (at least 4 frames), a window of 1.5 s (32 .. 4096 frames): inside the bias table.
        m_alpha = std::clamp(std::exp(-hopSeconds / std::max(0.03, 4.0 * hopSeconds)), kBiasAlpha.front(), kBiasAlpha.back());
        const int window = std::clamp(static_cast<int>(std::lround(kWindowSeconds / hopSeconds)), 32, 4096);
        m_subFrames = std::max(1, (window + kSubWindows - 1) / kSubWindows);
        m_bias = noiseBias(m_alpha, m_subFrames * kSubWindows);
        m_warmup = m_stft.size() / m_stft.hop();
        const auto bins = static_cast<std::size_t>(m_bins);
        m_weight.resize(bins);
        for (std::size_t k = 0; k < bins; ++k) {
            m_weight[k] = focusWeight(focus, static_cast<double>(k) * m_binHz);
        }
        m_state.resize(static_cast<std::size_t>(m_stft.channels()));
        for (Channel& s : m_state) {
            s.smooth.assign(bins, 0.0);
            s.power.assign(bins, 0.0);
            s.subMin.assign(bins, std::numeric_limits<double>::infinity());
            s.ring.assign(bins * kSubWindows, std::numeric_limits<double>::infinity());
            s.minimum.assign(bins, 0.0);
            s.gain.assign(bins, 1.0);
            s.clean.assign(bins, 0.0);
        }
        m_noise.assign(bins, 0.0);
        m_gains.assign(bins, 1.0);
        m_median.assign(2 * kMedianHalf + 1, 0.0);
    }

    int latencyFrames() const noexcept { return m_stft.latencyFrames(); }
    /// The minimum tracker's window after the warm-up, at most 2 s.
    int settleFrames() const noexcept {
        return m_stft.latencyFrames() + std::min((m_warmup + m_subFrames * kSubWindows) * m_stft.hop(), 2 * m_sampleRate);
    }
    double binHz() const noexcept { return m_binHz; }
    double hopSeconds() const noexcept { return m_hopSeconds; }

    void setSettings(const Settings& settings) noexcept { m_settings = settings; }

    void process(float* io, int frames) noexcept {
        m_stft.process(io, frames, [this](int c, std::complex<double>* bins) { frame(c, bins); });
    }

    void reset() noexcept {
        m_stft.reset();
        for (Channel& s : m_state) {
            std::fill(s.smooth.begin(), s.smooth.end(), 0.0);
            std::fill(s.power.begin(), s.power.end(), 0.0);
            std::fill(s.subMin.begin(), s.subMin.end(), std::numeric_limits<double>::infinity());
            std::fill(s.ring.begin(), s.ring.end(), std::numeric_limits<double>::infinity());
            std::fill(s.minimum.begin(), s.minimum.end(), 0.0);
            std::fill(s.gain.begin(), s.gain.end(), 1.0);
            std::fill(s.clean.begin(), s.clean.end(), 0.0);
            s.frames = 0;
            s.subCount = 0;
            s.ringSlot = 0;
            s.ringFilled = 0;
        }
    }

private:
    struct Channel {
        std::vector<double> smooth;  ///< smoothed power
        std::vector<double> power;   ///< this frame's power
        std::vector<double> subMin;  ///< minimum in the current sub-window
        std::vector<double> ring;    ///< minima of the last kSubWindows sub-windows (sub-window-major)
        std::vector<double> minimum; ///< minimum over the whole window
        std::vector<double> gain;    ///< last frame's gains
        std::vector<double> clean;   ///< last frame's |gain x X|^2 (decision-directed estimate)
        std::int64_t frames = 0;
        int subCount = 0;
        int ringSlot = 0;
        int ringFilled = 0;
    };

    void frame(int channel, std::complex<double>* x) noexcept {
        Channel& s = m_state[static_cast<std::size_t>(channel)];
        const auto bins = static_cast<std::size_t>(m_bins);
        ++s.frames;
        const bool warm = s.frames > m_warmup;
        // Smoothed power; during the warm-up (the STFT's first frames are partly the silence before the stream) it
        // follows the power directly and the minimum tracker waits.
        for (std::size_t k = 0; k < bins; ++k) {
            const double p = std::norm(x[k]);
            s.power[k] = p;
            s.smooth[k] = warm ? flushed(m_alpha * s.smooth[k] + (1.0 - m_alpha) * p) : p;
        }
        if (warm) {
            for (std::size_t k = 0; k < bins; ++k) {
                s.subMin[k] = std::min(s.subMin[k], s.smooth[k]);
            }
            if (++s.subCount == m_subFrames) {
                std::copy(s.subMin.begin(), s.subMin.end(), s.ring.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(s.ringSlot) * bins));
                std::fill(s.subMin.begin(), s.subMin.end(), std::numeric_limits<double>::infinity());
                s.ringSlot = (s.ringSlot + 1) % kSubWindows;
                s.ringFilled = std::min(s.ringFilled + 1, kSubWindows);
                s.subCount = 0;
            }
        }
        for (std::size_t k = 0; k < bins; ++k) {
            double m = s.subMin[k];
            for (int u = 0; u < s.ringFilled; ++u) {
                m = std::min(m, s.ring[static_cast<std::size_t>(u) * bins + k]);
            }
            s.minimum[k] = std::isinf(m) ? s.smooth[k] : m;
        }
        // The noise floor: the median of the minima around each bin (a steady tone a few bins wide is not noise).
        const Settings& set = m_settings;
        const int last = m_bins - 1;
        for (int k = 0; k < m_bins; ++k) {
            const int lo = std::max(0, k - kMedianHalf);
            const int hi = std::min(last, k + kMedianHalf);
            const int n = hi - lo + 1;
            std::copy(s.minimum.begin() + lo, s.minimum.begin() + hi + 1, m_median.begin());
            const auto middle = m_median.begin() + n / 2;
            std::nth_element(m_median.begin(), middle, m_median.begin() + n);
            m_noise[static_cast<std::size_t>(k)] = m_bias * set.noiseScale * *middle;
        }
        // Gains.
        for (std::size_t k = 0; k < bins; ++k) {
            const double noise = m_noise[k];
            const double p = s.power[k];
            double g = 1.0;
            if (noise > 1e-30) {
                const double w = m_weight[k];
                const double snr = p / noise;
                const double prior = std::max(kDecisionDirected * s.clean[k] / noise + (1.0 - kDecisionDirected) * std::max(snr - 1.0, 0.0), 1e-4);
                g = prior / (prior + set.overSubtraction * w);
                if (set.signalSnr > 0.0 && snr > set.signalSnr) {
                    g = 1.0;
                }
                g = std::clamp(g, std::pow(set.floorGain, w), 1.0);
            }
            if (set.releasePerFrame > 0.0) {
                g = std::max(g, s.gain[k] * set.releasePerFrame);
            }
            g = std::min(g, 1.0);
            // The estimator's own state follows the Wiener gain (preservation below is applied to the output only,
            // or preserved bins would pass for wanted sound in the next frame and spread).
            s.gain[k] = g;
            s.clean[k] = flushed(g * g * p);
            m_gains[k] = g;
        }
        // Broadband preservation: a bin takes the largest gain of wanted sound (gain >= 1/2) within reach.
        if (set.preserveBins > 0) {
            for (int k = 0; k < m_bins; ++k) {
                double g = s.gain[static_cast<std::size_t>(k)];
                const int lo = std::max(0, k - set.preserveBins);
                const int hi = std::min(last, k + set.preserveBins);
                for (int j = lo; j <= hi; ++j) {
                    const double near = s.gain[static_cast<std::size_t>(j)];
                    if (near >= 0.5) {
                        g = std::max(g, near);
                    }
                }
                m_gains[static_cast<std::size_t>(k)] = g;
            }
        }
        for (std::size_t k = 0; k < bins; ++k) {
            const double g = m_gains[k];
            x[k] *= set.noiseOnly ? 1.0 - g : g;
        }
    }

    dsp::Stft m_stft;
    int m_bins;
    int m_sampleRate;
    double m_hopSeconds = 0.0;
    double m_binHz = 0.0;
    double m_alpha = 0.0;
    double m_bias = 1.0;
    int m_subFrames = 1;
    int m_warmup = 4;
    Settings m_settings;
    std::vector<double> m_weight;
    std::vector<Channel> m_state;
    std::vector<double> m_noise;
    std::vector<double> m_gains;
    std::vector<double> m_median;
};

/// Premiere's DeNoise: Amount 0..100 % sets the floor (down to -30 dB) and the over-subtraction (1..2).
class DeNoiseProcessor final : public EffectProcessor {
public:
    DeNoiseProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_amount(paramIndex(d, "amount")), m_gain(paramIndex(d, "gain")),
          m_noiseOnly(choiceOf(d, values, "outputNoiseOnly", 0, 1) == 1),
          m_reducer(channels, sampleRate, scaledPowerOfTwo(1024, sampleRate, 256, 8192),
                    choiceOf(d, values, "processingFocus", 0, 4)) {}

    int latencyFrames() const noexcept override { return m_reducer.latencyFrames(); }
    int settleFrames() const noexcept override { return m_reducer.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double amount = unit(cell.start[m_amount]);
        NoiseReducer::Settings s;
        s.floorGain = dbToGain(-30.0 * amount);
        s.overSubtraction = 1.0 + amount;
        s.noiseOnly = m_noiseOnly;
        m_reducer.setSettings(s);
        m_reducer.process(io, frames);
        applyGain(io, frames, m_channels, Ramp(cell, m_gain));
    }

    void reset() noexcept override { m_reducer.reset(); }

private:
    int m_channels;
    int m_amount, m_gain;
    bool m_noiseOnly;
    NoiseReducer m_reducer;
};

/// Audition's (and Premiere's) Adaptive Noise Reduction.
class AdaptiveNoiseReductionProcessor final : public EffectProcessor {
public:
    AdaptiveNoiseReductionProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_reduce(paramIndex(d, "reduceNoiseBy")), m_noisiness(paramIndex(d, "noisiness")),
          m_floor(paramIndex(d, "noiseFloor")), m_threshold(paramIndex(d, "signalThreshold")),
          m_decay(paramIndex(d, "spectralDecayRate")), m_preserve(paramIndex(d, "broadbandPreservation")),
          m_reducer(channels, sampleRate,
                    std::clamp(static_cast<int>(std::lround(constantOf(d, values, "fftSize"))), 128, 16384), 0) {}

    int latencyFrames() const noexcept override { return m_reducer.latencyFrames(); }
    int settleFrames() const noexcept override { return m_reducer.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        NoiseReducer::Settings s;
        s.floorGain = dbToGain(-std::max(v[m_reduce], 0.0));
        s.overSubtraction = 0.5 + 2.5 * unit(v[m_noisiness]);
        s.noiseScale = std::pow(10.0, v[m_floor] / 10.0);
        s.signalSnr = std::pow(10.0, (20.0 + v[m_threshold]) / 10.0); // wanted sound: 20 dB over the floor by default
        const double decayMs = v[m_decay];
        s.releasePerFrame = decayMs > 0.0 ? std::pow(10.0, -3.0 * m_reducer.hopSeconds() * 1e3 / decayMs) : 0.0;
        s.preserveBins = static_cast<int>(std::lround(std::max(v[m_preserve], 0.0) / m_reducer.binHz()));
        s.preserveBins = std::min(s.preserveBins, 64);
        m_reducer.setSettings(s);
        m_reducer.process(io, frames);
    }

    void reset() noexcept override { m_reducer.reset(); }

private:
    int m_reduce, m_noisiness, m_floor, m_threshold, m_decay, m_preserve;
    NoiseReducer m_reducer;
};

// ---- Automatic Click Remover --------------------------------------------------------------------------------------

/// Clicks are samples whose second difference stands out of the local level of the second difference (and of the
/// signal) by Threshold; the level is robust: the lower median of four 64-frame blocks before and after, the larger of
/// the two (so neither the click nor an onset from silence raises or lowers it). A click region is repaired once the
/// sound after it has arrived: a crossfade of forward and backward linear prediction (order and the longest region
/// from Complexity) from 256 frames on either side. The output is the input delayed by a constant lookahead.
class ClickRemoverProcessor final : public EffectProcessor {
public:
    static constexpr int kBlocks = 16;        ///< ring of block levels
    static constexpr int kMaxOrder = 32;
    static constexpr double kSignalRatio = 0.25; ///< a click's residual is at least this share of the signal's RMS
    static constexpr double kFloor = 1e-4;       ///< residuals below -80 dBFS are never clicks

    ClickRemoverProcessor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_channels(channels), m_threshold(paramIndex(d, "threshold")), m_complexity(paramIndex(d, "complexity")) {
        const double scale = sampleRate / 48000.0;
        m_block = std::max(16, static_cast<int>(std::lround(64.0 * scale)));
        m_detectDelay = 7 * m_block;
        m_maxGapLimit = std::max(32, static_cast<int>(std::lround(248.0 * scale)));
        m_merge = std::max(4, static_cast<int>(std::lround(8.0 * scale)));
        m_context = std::max(2 * kMaxOrder + 16, static_cast<int>(std::lround(256.0 * scale)));
        m_latency = m_detectDelay + m_maxGapLimit + m_merge + 16;
        int size = 64;
        while (size < 2 * (m_latency + m_context + m_maxGapLimit)) {
            size <<= 1;
        }
        m_mask = size - 1;
        m_state.resize(static_cast<std::size_t>(channels));
        for (Channel& s : m_state) {
            s.samples.assign(static_cast<std::size_t>(size), 0.0f);
            s.residual.assign(static_cast<std::size_t>(size), 0.0f);
        }
        const auto scratch = static_cast<std::size_t>(m_context + m_maxGapLimit + kMaxOrder + 8);
        m_left.assign(scratch, 0.0);
        m_right.assign(scratch, 0.0);
        m_window.resize(static_cast<std::size_t>(m_context));
        for (int i = 0; i < m_context; ++i) {
            m_window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / m_context);
        }
    }

    int latencyFrames() const noexcept override { return m_latency; }
    int settleFrames() const noexcept override { return m_latency + m_context; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double threshold = std::clamp(cell.start[m_threshold], 1.0, 100.0);
        const double complexity = std::clamp(cell.start[m_complexity], 1.0, 100.0);
        const double ratio = 2.0 + 0.3 * threshold; // 30 (default) = a residual 11 times the local level
        m_ratio2 = ratio * ratio;
        m_order = std::clamp(4 + static_cast<int>(std::lround(complexity * 0.28)), 2, kMaxOrder);
        m_maxGap = std::min(m_maxGapLimit, static_cast<int>(std::lround((16.0 + 2.3 * complexity) * m_block / 64.0)));
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const std::int64_t t = m_written;
            for (std::size_t c = 0; c < ch; ++c) {
                Channel& s = m_state[c];
                const double x = frame[c];
                const double r = x - 2.0 * s.x1 + s.x2;
                s.x2 = s.x1;
                s.x1 = x;
                s.samples[index(t)] = frame[c];
                s.residual[index(t)] = static_cast<float>(r);
                s.blockResidual += r * r;
                s.blockSignal += x * x;
                if ((t + 1) % m_block == 0) {
                    const auto b = static_cast<std::size_t>(((t + 1) / m_block - 1) % kBlocks);
                    s.residualLevel[b] = s.blockResidual / m_block;
                    s.signalLevel[b] = s.blockSignal / m_block;
                    s.blockResidual = 0.0;
                    s.blockSignal = 0.0;
                }
                if (t >= m_detectDelay) {
                    detect(s, t - m_detectDelay, t);
                }
                frame[c] = s.samples[index(t - m_latency)];
            }
            ++m_written;
        }
    }

    void reset() noexcept override {
        for (Channel& s : m_state) {
            std::fill(s.samples.begin(), s.samples.end(), 0.0f);
            std::fill(s.residual.begin(), s.residual.end(), 0.0f);
            s.residualLevel.fill(0.0);
            s.signalLevel.fill(0.0);
            s.blockResidual = 0.0;
            s.blockSignal = 0.0;
            s.x1 = 0.0;
            s.x2 = 0.0;
            s.open = false;
            s.blocked = false;
        }
        m_written = 0;
    }

private:
    struct Channel {
        std::vector<float> samples;  ///< the input (repaired in place before it is output)
        std::vector<float> residual; ///< its second difference
        std::array<double, kBlocks> residualLevel{};
        std::array<double, kBlocks> signalLevel{};
        double blockResidual = 0.0;
        double blockSignal = 0.0;
        double x1 = 0.0;
        double x2 = 0.0;
        bool open = false;     ///< a click region is open
        bool blocked = false;  ///< a run too long for a click: wait until it ends
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::int64_t lastHit = 0;
    };

    std::size_t index(std::int64_t t) const noexcept { return static_cast<std::size_t>(t & m_mask); }

    /// The robust level around block `b`: the larger of the lower medians of blocks b-5..b-2 and b+2..b+5.
    static double localLevel(const std::array<double, kBlocks>& level, std::int64_t b) noexcept {
        const auto lowerMedian = [&level](std::int64_t first) {
            std::array<double, 4> v{};
            for (std::size_t j = 0; j < 4; ++j) {
                const std::int64_t k = first + static_cast<std::int64_t>(j);
                v[j] = k < 0 ? 0.0 : level[static_cast<std::size_t>(k % kBlocks)];
            }
            std::sort(v.begin(), v.end());
            return v[1];
        };
        return std::max(lowerMedian(b - 5), lowerMedian(b + 2));
    }

    void detect(Channel& s, std::int64_t at, std::int64_t newest) noexcept {
        const double r = s.residual[index(at)];
        const std::int64_t b = at / m_block;
        bool hit = std::fabs(r) > kFloor;
        if (hit) {
            const double residualLevel = localLevel(s.residualLevel, b);
            const double signalLevel = localLevel(s.signalLevel, b);
            hit = r * r > m_ratio2 * residualLevel && r * r > kSignalRatio * kSignalRatio * signalLevel;
        }
        if (s.blocked) {
            if (hit) {
                s.lastHit = at;
            } else if (at > s.lastHit + m_merge) {
                s.blocked = false;
            }
            return;
        }
        if (hit) {
            if (!s.open) {
                s.open = true;
                s.start = at - 3;
            }
            s.end = at + 3;
            s.lastHit = at;
            if (s.end - s.start + 1 > m_maxGapLimit) { // too long for a click: leave it alone
                s.open = false;
                s.blocked = true;
            }
            return;
        }
        if (s.open && at > s.end + m_merge) {
            s.open = false;
            if (s.end - s.start + 1 <= m_maxGap && s.end + m_context <= newest) {
                repair(s, s.start, s.end);
            }
        }
    }

    /// Levinson-Durbin on the windowed autocorrelation of `data` (m_context values): the prediction-error filter
    /// a[0..order] (a[0] = 1) in m_a. False when the data is silent.
    bool predictor(const double* data) noexcept {
        std::array<double, kMaxOrder + 1> r{};
        for (int lag = 0; lag <= m_order; ++lag) {
            double sum = 0.0;
            for (int n = lag; n < m_context; ++n) {
                sum += data[n] * m_window[static_cast<std::size_t>(n)] * data[n - lag] * m_window[static_cast<std::size_t>(n - lag)];
            }
            r[static_cast<std::size_t>(lag)] = sum;
        }
        if (r[0] < 1e-20) {
            return false;
        }
        r[0] *= 1.0 + 1e-9; // white-noise correction: keeps the recursion well conditioned
        m_a.fill(0.0);
        m_a[0] = 1.0;
        double error = r[0];
        for (int i = 1; i <= m_order; ++i) {
            double acc = r[static_cast<std::size_t>(i)];
            for (int j = 1; j < i; ++j) {
                acc += m_a[static_cast<std::size_t>(j)] * r[static_cast<std::size_t>(i - j)];
            }
            const double k = -acc / error;
            std::array<double, kMaxOrder + 1> next = m_a;
            for (int j = 1; j < i; ++j) {
                next[static_cast<std::size_t>(j)] = m_a[static_cast<std::size_t>(j)] + k * m_a[static_cast<std::size_t>(i - j)];
            }
            next[static_cast<std::size_t>(i)] = k;
            m_a = next;
            error *= 1.0 - k * k;
            if (error <= r[0] * 1e-12) {
                break;
            }
        }
        return true;
    }

    /// Extends `data` (m_context known values followed by room for `count` more) by linear prediction.
    void extend(double* data, int count, double limit) noexcept {
        for (int n = m_context; n < m_context + count; ++n) {
            double v = 0.0;
            for (int j = 1; j <= m_order; ++j) {
                v -= m_a[static_cast<std::size_t>(j)] * data[n - j];
            }
            data[n] = std::clamp(v, -limit, limit);
        }
    }

    void repair(Channel& s, std::int64_t first, std::int64_t last) noexcept {
        const int count = static_cast<int>(last - first + 1);
        double peak = 0.0;
        for (int n = 0; n < m_context; ++n) {
            m_left[static_cast<std::size_t>(n)] = s.samples[index(first - m_context + n)];
            m_right[static_cast<std::size_t>(n)] = s.samples[index(last + m_context - n)]; // reversed
            peak = std::max({peak, std::fabs(m_left[static_cast<std::size_t>(n)]), std::fabs(m_right[static_cast<std::size_t>(n)])});
        }
        const double limit = 1.5 * peak;
        const bool forward = predictor(m_left.data());
        if (forward) {
            extend(m_left.data(), count, limit);
        }
        const bool backward = predictor(m_right.data());
        if (backward) {
            extend(m_right.data(), count, limit);
        }
        for (int n = 0; n < count; ++n) {
            const double f = forward ? m_left[static_cast<std::size_t>(m_context + n)] : 0.0;
            const double b = backward ? m_right[static_cast<std::size_t>(m_context + count - 1 - n)] : 0.0;
            const double w = (n + 1.0) / (count + 1.0);
            s.samples[index(first + n)] = static_cast<float>((1.0 - w) * f + w * b);
        }
    }

    int m_channels;
    int m_threshold, m_complexity;
    int m_block = 64;
    int m_detectDelay = 0;
    int m_maxGapLimit = 0;
    int m_merge = 8;
    int m_context = 256;
    int m_latency = 0;
    std::int64_t m_mask = 0;
    double m_ratio2 = 100.0;
    int m_order = 8;
    int m_maxGap = 32;
    std::int64_t m_written = 0;
    std::vector<Channel> m_state;
    std::vector<double> m_left;  ///< left context, then the forward prediction
    std::vector<double> m_right; ///< right context reversed, then the backward prediction
    std::vector<double> m_window;
    std::array<double, kMaxOrder + 1> m_a{};
};

// ---- Convolution Reverb -------------------------------------------------------------------------------------------

/// A built-in impulse response: per-band decay times (RT60 below 250 Hz, between, above 4 kHz), early reflections
/// (count, span, level) and the diffuse tail's arrival and build-up.
struct ImpulsePreset {
    double rt60Low, rt60Mid, rt60High; ///< seconds
    int reflections;
    double reflectionsMs;
    double reflectionLevel;
    double tailGapMs;
    double buildMs;
};

constexpr std::array<ImpulsePreset, 5> kImpulsePresets{{
    {0.45, 0.40, 0.25, 10, 18.0, 0.55, 2.0, 6.0},     // Small Room
    {0.90, 0.80, 0.50, 12, 32.0, 0.50, 4.0, 12.0},    // Medium Room
    {2.40, 2.00, 1.20, 16, 70.0, 0.40, 12.0, 30.0},   // Large Hall
    {1.70, 1.60, 1.40, 0, 0.0, 0.0, 0.5, 2.0},        // Plate
    {4.50, 3.80, 2.00, 18, 110.0, 0.35, 20.0, 60.0},  // Cathedral
}};

constexpr double kMaxImpulseSeconds = 4.0;

std::uint32_t xorshift(std::uint32_t& s) noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
double uniform(std::uint32_t& s) noexcept { return static_cast<double>(xorshift(s)) / 4294967295.0 * 2.0 - 1.0; }

/// One channel of a preset's impulse response at a room size (0.05..1 of the full room) and damping (0..1): seeded
/// noise split into three bands (Linkwitz-Riley crossovers at 250 Hz and 4 kHz), each with its own exponential decay,
/// after early reflections. Deterministic for a seed.
std::vector<double> synthesiseImpulse(const ImpulsePreset& p, double size, double dampLow, double dampHigh,
                                      int preDelayFrames, int sampleRate, std::uint32_t seed) {
    const double fs = sampleRate;
    const double rtLow = std::max(0.05, p.rt60Low * size * (1.0 - 0.75 * dampLow));
    const double rtMid = std::max(0.05, p.rt60Mid * size);
    const double rtHigh = std::max(0.05, p.rt60High * size * (1.0 - 0.75 * dampHigh));
    const double levelLow = 1.0 - 0.8 * dampLow;
    const double levelHigh = 1.0 - 0.8 * dampHigh;
    const int gap = static_cast<int>(std::lround(p.tailGapMs * size * 1e-3 * fs));
    const double longest = std::max({rtLow, rtMid, rtHigh});
    const int tailFrames = static_cast<int>(std::ceil(longest * fs));
    const int total = std::min(preDelayFrames + gap + tailFrames, static_cast<int>(kMaxImpulseSeconds * fs));
    std::vector<double> h(static_cast<std::size_t>(std::max(total, 1)), 0.0);

    // The diffuse tail.
    const dsp::BiquadCoefficients lowCross = dsp::lowPass(fs, 250.0);
    const dsp::BiquadCoefficients highCross = dsp::highPass(fs, std::min(4000.0, 0.45 * fs));
    dsp::BiquadState low1, low2, high1, high2;
    std::uint32_t state = seed;
    const double buildFrames = std::max(1.0, p.buildMs * size * 1e-3 * fs);
    const double kDecay = 3.0 * std::numbers::ln10; // amplitude: e^(-kDecay t / rt) is -60 dB at t = rt
    for (int i = preDelayFrames + gap; i < total; ++i) {
        const double t = static_cast<double>(i - preDelayFrames - gap) / fs;
        const auto n = static_cast<float>(uniform(state));
        const double lowBand = low2.process(lowCross, low1.process(lowCross, n));
        const double rest = n - lowBand;
        const double highBand = high2.process(highCross, high1.process(highCross, static_cast<float>(rest)));
        const double midBand = rest - highBand;
        const double build = 1.0 - std::exp(-(i - preDelayFrames - gap) / buildFrames);
        h[static_cast<std::size_t>(i)] =
            build * (levelLow * lowBand * std::exp(-kDecay * t / rtLow) + midBand * std::exp(-kDecay * t / rtMid) +
                     levelHigh * highBand * std::exp(-kDecay * t / rtHigh));
    }
    // Early reflections: sparse impulses, louder first, alternating signs.
    for (int j = 0; j < p.reflections; ++j) {
        const double r = 0.5 + 0.5 * uniform(state);
        const double ms = (1.0 + (p.reflectionsMs - 1.0) * std::pow((j + r) / p.reflections, 1.3)) * size;
        const int at = preDelayFrames + static_cast<int>(std::lround(ms * 1e-3 * fs));
        if (at < total) {
            const double gainJ = p.reflectionLevel * (1.0 - 0.6 * ms / std::max(p.reflectionsMs, 1.0)) *
                                 (0.6 + 0.4 * (0.5 + 0.5 * uniform(state)));
            h[static_cast<std::size_t>(at)] += (j % 2 == 0 ? 1.0 : -1.0) * gainJ * 0.5;
        }
    }
    // Fade out the last 5 % (a truncated tail must not click).
    const int fade = std::max(1, (total - preDelayFrames) / 20);
    for (int i = 0; i < fade && total - 1 - i >= 0; ++i) {
        h[static_cast<std::size_t>(total - 1 - i)] *= 0.5 - 0.5 * std::cos(std::numbers::pi * i / fade);
    }
    return h;
}

/// Uniformly partitioned overlap-save convolution with a stereo pair of synthesised impulse responses (input channel
/// c is convolved with impulse c % 2; a 5.1 or 7.1 LFE stays dry). Latency: one partition (512 frames at 48 kHz).
class ConvolutionReverbProcessor final : public EffectProcessor {
public:
    ConvolutionReverbProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_channels(channels), m_block(scaledPowerOfTwo(512, sampleRate, 128, 4096)), m_fft(2 * m_block),
          m_bins(m_block + 1), m_mix(paramIndex(d, "mix")), m_width(paramIndex(d, "width")), m_gain(paramIndex(d, "gain")) {
        const ImpulsePreset& preset = kImpulsePresets[static_cast<std::size_t>(choiceOf(d, values, "impulse", 0, 4))];
        const double size = std::max(0.05, unit(constantOf(d, values, "roomSize")));
        const double dampLow = unit(constantOf(d, values, "dampingLF"));
        const double dampHigh = unit(constantOf(d, values, "dampingHF"));
        const int preDelay = static_cast<int>(std::lround(std::clamp(constantOf(d, values, "preDelay"), 0.0, 200.0) * 1e-3 * sampleRate));
        std::array<std::vector<double>, 2> impulse{
            synthesiseImpulse(preset, size, dampLow, dampHigh, preDelay, sampleRate, 0x9E3779B9u),
            synthesiseImpulse(preset, size, dampLow, dampHigh, preDelay, sampleRate, 0x7F4A7C15u)};
        // Unit energy (averaged over the pair): white noise comes out about as loud as it went in.
        double energy = 0.0;
        for (const auto& h : impulse) {
            for (const double v : h) {
                energy += v * v;
            }
        }
        const double norm = energy > 0.0 ? 1.0 / std::sqrt(energy / 2.0) : 0.0;
        const std::size_t length = std::max(impulse[0].size(), impulse[1].size());
        m_partitions = std::max(1, static_cast<int>((length + static_cast<std::size_t>(m_block) - 1) / static_cast<std::size_t>(m_block)));
        m_impulseFrames = static_cast<int>(length);
        const auto bins = static_cast<std::size_t>(m_bins);
        const auto parts = static_cast<std::size_t>(m_partitions);
        const auto block = static_cast<std::size_t>(m_block);
        std::vector<double> time(2 * block, 0.0);
        std::vector<std::complex<double>> spectrum(bins);
        for (std::size_t ir = 0; ir < 2; ++ir) {
            m_irRe[ir].assign(parts * bins, 0.0);
            m_irIm[ir].assign(parts * bins, 0.0);
            for (std::size_t p = 0; p < parts; ++p) {
                std::fill(time.begin(), time.end(), 0.0);
                for (std::size_t n = 0; n < block; ++n) {
                    const std::size_t at = p * block + n;
                    time[n] = at < impulse[ir].size() ? impulse[ir][at] * norm : 0.0;
                }
                m_fft.forwardReal(time.data(), spectrum.data());
                for (std::size_t k = 0; k < bins; ++k) {
                    m_irRe[ir][p * bins + k] = spectrum[k].real();
                    m_irIm[ir][p * bins + k] = spectrum[k].imag();
                }
            }
        }
        m_state.resize(static_cast<std::size_t>(channels));
        for (std::size_t c = 0; c < m_state.size(); ++c) {
            Channel& s = m_state[c];
            s.convolved = !(channels >= 6 && c == 3); // 5.1 / 7.1: the LFE stays dry
            s.impulse = c % 2;
            s.previous.assign(block, 0.0);
            s.current.assign(block, 0.0);
            s.output.assign(block, 0.0);
            if (s.convolved) {
                s.fdlRe.assign(parts * bins, 0.0);
                s.fdlIm.assign(parts * bins, 0.0);
            }
        }
        m_time.assign(2 * block, 0.0);
        m_spectrum.assign(bins, {});
        m_accRe.assign(bins, 0.0);
        m_accIm.assign(bins, 0.0);
        m_settle = m_block + std::min(m_impulseFrames, 2 * sampleRate);
    }

    int latencyFrames() const noexcept override { return m_block; }
    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const Ramp mix(cell, m_mix);
        const Ramp width(cell, m_width);
        const Ramp gain(cell, m_gain);
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const auto at = static_cast<std::size_t>(m_fill);
            std::array<double, kMaxChannels> wet{};
            std::array<double, kMaxChannels> dry{};
            for (std::size_t c = 0; c < ch; ++c) {
                Channel& s = m_state[c];
                s.current[at] = frame[c];
                s.nonZero = s.nonZero || frame[c] != 0.0f;
                dry[c] = s.previous[at]; // the input one block ago: aligned with the convolution's output
                wet[c] = s.output[at];
            }
            if (ch >= 2) {
                widen(wet[0], wet[1], std::clamp(width.at(i) * 0.01, 0.0, 2.0));
            }
            const double share = unit(mix.at(i));
            const double out = dbToGain(gain.at(i));
            for (std::size_t c = 0; c < ch; ++c) {
                const double w = m_state[c].convolved ? wet[c] : 0.0;
                frame[c] = static_cast<float>(((1.0 - share) * dry[c] + share * w) * out);
            }
            if (++m_fill == m_block) {
                m_fill = 0;
                runBlock();
            }
        }
    }

    void reset() noexcept override {
        for (Channel& s : m_state) {
            std::fill(s.previous.begin(), s.previous.end(), 0.0);
            std::fill(s.current.begin(), s.current.end(), 0.0);
            std::fill(s.output.begin(), s.output.end(), 0.0);
            std::fill(s.fdlRe.begin(), s.fdlRe.end(), 0.0);
            std::fill(s.fdlIm.begin(), s.fdlIm.end(), 0.0);
            s.zeroSpectra = m_partitions;
            s.nonZero = false;
            s.previousNonZero = false;
        }
        m_fill = 0;
        m_slot = 0;
    }

private:
    struct Channel {
        bool convolved = true;
        std::size_t impulse = 0;
        std::vector<double> previous; ///< the last complete input block
        std::vector<double> current;  ///< the block being filled
        std::vector<double> output;   ///< the wet output due during this block
        std::vector<double> fdlRe;    ///< spectra of the last m_partitions input windows (frequency-domain delay line)
        std::vector<double> fdlIm;
        int zeroSpectra = 1 << 30;    ///< consecutive all-zero spectra written to the delay line
        bool nonZero = false;         ///< the block being filled has a non-zero sample
        bool previousNonZero = false;
    };

    void runBlock() noexcept {
        const auto bins = static_cast<std::size_t>(m_bins);
        const auto block = static_cast<std::size_t>(m_block);
        const auto parts = static_cast<std::size_t>(m_partitions);
        const auto slot = static_cast<std::size_t>(m_slot);
        for (Channel& s : m_state) {
            if (!s.convolved) {
                std::swap(s.previous, s.current);
                continue;
            }
            double* re = s.fdlRe.data() + slot * bins;
            double* im = s.fdlIm.data() + slot * bins;
            if (s.nonZero || s.previousNonZero) {
                std::copy(s.previous.begin(), s.previous.end(), m_time.begin());
                std::copy(s.current.begin(), s.current.end(), m_time.begin() + static_cast<std::ptrdiff_t>(block));
                m_fft.forwardReal(m_time.data(), m_spectrum.data());
                for (std::size_t k = 0; k < bins; ++k) {
                    re[k] = m_spectrum[k].real();
                    im[k] = m_spectrum[k].imag();
                }
                s.zeroSpectra = 0;
            } else {
                std::fill(re, re + bins, 0.0);
                std::fill(im, im + bins, 0.0);
                s.zeroSpectra = std::min(s.zeroSpectra + 1, 1 << 30);
            }
            s.previousNonZero = s.nonZero;
            s.nonZero = false;
            std::swap(s.previous, s.current);
            if (s.zeroSpectra >= m_partitions) {
                std::fill(s.output.begin(), s.output.end(), 0.0); // the whole delay line is silent
                continue;
            }
            std::fill(m_accRe.begin(), m_accRe.end(), 0.0);
            std::fill(m_accIm.begin(), m_accIm.end(), 0.0);
            const double* hRe = m_irRe[s.impulse].data();
            const double* hIm = m_irIm[s.impulse].data();
            for (std::size_t p = 0; p < parts; ++p) {
                const std::size_t from = ((slot + parts - p) % parts) * bins;
                const double* xr = s.fdlRe.data() + from;
                const double* xi = s.fdlIm.data() + from;
                const double* hr = hRe + p * bins;
                const double* hi = hIm + p * bins;
                for (std::size_t k = 0; k < bins; ++k) {
                    m_accRe[k] += xr[k] * hr[k] - xi[k] * hi[k];
                    m_accIm[k] += xr[k] * hi[k] + xi[k] * hr[k];
                }
            }
            for (std::size_t k = 0; k < bins; ++k) {
                m_spectrum[k] = {m_accRe[k], m_accIm[k]};
            }
            m_fft.inverseReal(m_spectrum.data(), m_time.data());
            // Overlap-save: the second half is the linear convolution of this block.
            for (std::size_t n = 0; n < block; ++n) {
                s.output[n] = flushed(m_time[block + n]);
            }
        }
        m_slot = (m_slot + 1) % m_partitions;
    }

    int m_channels;
    int m_block;
    dsp::Fft m_fft;
    int m_bins;
    int m_mix, m_width, m_gain;
    int m_partitions = 1;
    int m_impulseFrames = 0;
    int m_settle = 0;
    int m_fill = 0;
    int m_slot = 0;
    std::array<std::vector<double>, 2> m_irRe;
    std::array<std::vector<double>, 2> m_irIm;
    std::vector<Channel> m_state;
    std::vector<double> m_time;
    std::vector<std::complex<double>> m_spectrum;
    std::vector<double> m_accRe;
    std::vector<double> m_accIm;
};

// ---- Studio Reverb ------------------------------------------------------------------------------------------------

/// Audition's Studio Reverb as a feedback delay network: early reflections (a tapped delay, per side), Diffusion
/// (series all-passes before the network: none at 0 %), eight delay lines (prime lengths scaled by Room Size) fed back
/// through a Householder matrix, each with a one-pole absorption that gives the Decay RT60 at low frequencies and a
/// shorter one at high frequencies (Damping), the wet band limited by Low and High Frequency Cut, Width as mid/side.
/// A 5.1 buffer: the LFE neither feeds the network nor gets a tail; the centre gets the front tail's mid.
///
/// Surround Reverb is the same network read four ways (orthogonal sign patterns of the lines: four decorrelated tails)
/// for the front and rear pairs of a 5.1 / 7.1 buffer; Center Input and LFE Input say how much of those channels feed
/// it. With fewer than six channels it is Studio Reverb.
class StudioReverbProcessor final : public EffectProcessor {
public:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;
    static constexpr int kTaps = 12;

    StudioReverbProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values,
                          bool surround = false)
        : m_channels(channels), m_sampleRate(sampleRate), m_decay(paramIndex(d, "decay")),
          m_early(paramIndex(d, "earlyReflections")), m_width(paramIndex(d, "width")),
          m_highCut(paramIndex(d, "highFrequencyCut")), m_lowCut(paramIndex(d, "lowFrequencyCut")),
          m_damping(paramIndex(d, "damping")), m_diffusion(paramIndex(d, "diffusion")), m_dry(paramIndex(d, "dry")),
          m_wet(paramIndex(d, "wet")), m_surround(surround && channels >= 6),
          m_centerInput(surround ? paramIndex(d, "centerInput") : -1), m_lfeInput(surround ? paramIndex(d, "lfeInput") : -1) {
        const double size = unit(constantOf(d, values, "roomSize"));
        const double scale = 0.35 + 1.3 * size; // a small room .. a hall
        static constexpr std::array<double, kLines> kLineMs{31.1, 37.3, 41.9, 45.7, 51.7, 58.1, 63.7, 71.3};
        static constexpr std::array<double, kDiffusers> kDiffuserMs{4.77, 3.59, 12.73, 9.31};
        static constexpr std::array<double, kTaps> kLeftTapMs{3.1, 5.9, 8.7, 12.3, 15.2, 19.8, 23.6, 28.4, 33.9, 39.1, 46.2, 54.7};
        static constexpr std::array<double, kTaps> kRightTapMs{4.2, 6.8, 10.1, 13.5, 17.4, 21.1, 25.9, 31.2, 36.0, 42.3, 49.6, 58.8};
        int previous = 0;
        for (std::size_t k = 0; k < kLines; ++k) {
            int frames = primeFrames(kLineMs[k] * scale);
            if (frames <= previous) {
                frames = nextPrime(previous + 1);
            }
            previous = frames;
            m_lines[k].buffer.assign(static_cast<std::size_t>(frames), 0.0);
        }
        for (std::size_t k = 0; k < kDiffusers; ++k) {
            m_diffusers[k].buffer.assign(static_cast<std::size_t>(primeFrames(kDiffuserMs[k] * (0.5 + 0.5 * scale))), 0.0);
        }
        double tapEnergy = 0.0;
        for (std::size_t j = 0; j < kTaps; ++j) {
            m_leftTaps[j] = std::max(1, static_cast<int>(std::lround(kLeftTapMs[j] * scale * 1e-3 * sampleRate)));
            m_rightTaps[j] = std::max(1, static_cast<int>(std::lround(kRightTapMs[j] * scale * 1e-3 * sampleRate)));
            const double g = std::pow(0.85, static_cast<double>(j)) * (j % 2 == 0 ? 1.0 : -1.0);
            m_tapGains[j] = g;
            tapEnergy += g * g;
        }
        for (double& g : m_tapGains) {
            g /= std::sqrt(tapEnergy);
        }
        m_earlyLine.allocate(std::max(m_leftTaps.back(), m_rightTaps.back()) + 1);
        // The state for the build-time values (process() follows their changes per cell).
        m_decayMs = std::max(constantOf(d, values, "decay"), 1.0);
        m_dampingNow = unit(constantOf(d, values, "damping"));
        computeAbsorption();
        setBand(constantOf(d, values, "lowFrequencyCut"), constantOf(d, values, "highFrequencyCut"));
        m_settle = static_cast<int>(std::min(2.0, m_decayMs * 1e-3) * sampleRate) + std::max(m_leftTaps.back(), m_rightTaps.back());
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        const double decay = std::max(v[m_decay], 1.0);
        const double damping = unit(v[m_damping]);
        if (decay != m_decayMs || damping != m_dampingNow) {
            m_decayMs = decay;
            m_dampingNow = damping;
            computeAbsorption();
        }
        setBand(v[m_lowCut], v[m_highCut]);
        const double diffusion = 0.7 * unit(v[m_diffusion]);
        const Ramp early(cell, m_early);
        const Ramp width(cell, m_width);
        const Ramp dry(cell, m_dry);
        const Ramp wet(cell, m_wet);
        const auto ch = static_cast<std::size_t>(m_channels);
        constexpr std::size_t kCenter = 2;
        constexpr std::size_t kLfe = 3;
        const bool layout51 = ch >= 6; // L R C LFE Ls Rs (Lb Rb)
        // How much of each channel feeds the network (5.1: the centre and the LFE as Center and LFE Input say; Studio
        // Reverb takes the centre whole and leaves the LFE out).
        std::array<double, kMaxChannels> feed{};
        double feeds = 0.0;
        for (std::size_t c = 0; c < ch; ++c) {
            feed[c] = !layout51 ? 1.0
                      : c == kLfe ? (m_lfeInput >= 0 ? unit(v[m_lfeInput]) : 0.0)
                      : c == kCenter ? (m_centerInput >= 0 ? unit(v[m_centerInput]) : 1.0)
                                     : 1.0;
            feeds += feed[c];
        }
        const double inputScale = 1.0 / std::max(feeds, 1.0);
        const double tap = 1.0 / std::sqrt(static_cast<double>(kLines));
        const double inject = 1.0 / std::sqrt(static_cast<double>(kLines));
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            double in = 0.0;
            for (std::size_t c = 0; c < ch; ++c) {
                in += feed[c] * frame[c];
            }
            in *= inputScale;
            // Early reflections.
            m_earlyLine.write(in);
            double earlyLeft = 0.0;
            double earlyRight = 0.0;
            for (std::size_t j = 0; j < kTaps; ++j) {
                earlyLeft += m_tapGains[j] * m_earlyLine.at(m_leftTaps[j]);
                earlyRight += m_tapGains[j] * m_earlyLine.at(m_rightTaps[j]);
            }
            // Diffusion.
            double x = in;
            for (Delay& dl : m_diffusers) {
                double& slot = dl.buffer[static_cast<std::size_t>(dl.position)];
                const double delayed = slot;
                const double w = x + diffusion * delayed;
                x = delayed - diffusion * w;
                slot = flushed(w);
                dl.position = dl.position + 1 == static_cast<int>(dl.buffer.size()) ? 0 : dl.position + 1;
            }
            // The network.
            std::array<double, kLines> out{};
            double sum = 0.0;
            for (std::size_t k = 0; k < kLines; ++k) {
                Line& line = m_lines[k];
                const double s = line.buffer[static_cast<std::size_t>(line.position)];
                line.state = flushed(line.b * s + line.a * line.state);
                out[k] = line.state;
                sum += line.state;
            }
            const double householder = 2.0 / kLines * sum;
            // Four orthogonal sign patterns over the lines (rows of a Hadamard matrix): decorrelated tails.
            double left = 0.0;
            double right = 0.0;
            double rearLeft = 0.0;
            double rearRight = 0.0;
            for (std::size_t k = 0; k < kLines; ++k) {
                Line& line = m_lines[k];
                const double sign = (k & 1) == 0 ? 1.0 : -1.0;
                line.buffer[static_cast<std::size_t>(line.position)] = flushed(out[k] - householder + sign * inject * x);
                line.position = line.position + 1 == static_cast<int>(line.buffer.size()) ? 0 : line.position + 1;
                left += sign * out[k];
                right += ((k >> 1) & 1) == 0 ? out[k] : -out[k];
                rearLeft += ((k >> 2) & 1) == 0 ? out[k] : -out[k];
                rearRight += ((k ^ (k >> 2)) & 1) == 0 ? out[k] : -out[k];
            }
            const double er = unit(early.at(i));
            const double spread = unit(width.at(i));
            left = left * tap + er * earlyLeft;
            right = right * tap + er * earlyRight;
            left = m_front[0].high.process(m_highCutCoefficients, m_front[0].low.process(m_lowCutCoefficients, static_cast<float>(left)));
            right = m_front[1].high.process(m_highCutCoefficients, m_front[1].low.process(m_lowCutCoefficients, static_cast<float>(right)));
            widen(left, right, spread);
            if (m_surround) {
                // The rears hear the early reflections of the opposite side, later and softer.
                rearLeft = rearLeft * tap + 0.7 * er * earlyRight;
                rearRight = rearRight * tap + 0.7 * er * earlyLeft;
                rearLeft = m_rear[0].high.process(m_highCutCoefficients, m_rear[0].low.process(m_lowCutCoefficients, static_cast<float>(rearLeft)));
                rearRight = m_rear[1].high.process(m_highCutCoefficients, m_rear[1].low.process(m_lowCutCoefficients, static_cast<float>(rearRight)));
                widen(rearLeft, rearRight, spread);
            } else {
                rearLeft = left;
                rearRight = right;
            }
            const double mid = 0.5 * (left + right);
            const double dryGain = unit(dry.at(i));
            const double wetGain = unit(wet.at(i));
            for (std::size_t c = 0; c < ch; ++c) {
                double tail = 0.0;
                if (ch == 1) {
                    tail = mid;
                } else if (layout51) {
                    tail = c == kLfe ? 0.0 : c == kCenter ? mid : c < 2 ? (c == 0 ? left : right) : (c & 1) == 0 ? rearLeft : rearRight;
                } else {
                    tail = (c & 1) == 0 ? left : right;
                }
                frame[c] = static_cast<float>(dryGain * frame[c] + wetGain * tail);
            }
        }
    }

    void reset() noexcept override {
        for (Line& line : m_lines) {
            std::fill(line.buffer.begin(), line.buffer.end(), 0.0);
            line.position = 0;
            line.state = 0.0;
        }
        for (Delay& dl : m_diffusers) {
            std::fill(dl.buffer.begin(), dl.buffer.end(), 0.0);
            dl.position = 0;
        }
        m_earlyLine.clear();
        for (Band& b : m_front) {
            b.low.reset();
            b.high.reset();
        }
        for (Band& b : m_rear) {
            b.low.reset();
            b.high.reset();
        }
    }

private:
    struct Delay {
        std::vector<double> buffer;
        int position = 0;
    };
    struct Line {
        std::vector<double> buffer;
        int position = 0;
        double a = 0.0; ///< absorption one-pole: y = b x + a y[n-1]
        double b = 1.0;
        double state = 0.0;
    };

    static bool isPrime(int n) noexcept {
        if (n < 2) {
            return false;
        }
        for (int k = 2; k * k <= n; ++k) {
            if (n % k == 0) {
                return false;
            }
        }
        return true;
    }
    static int nextPrime(int n) noexcept {
        while (!isPrime(n)) {
            ++n;
        }
        return n;
    }
    int primeFrames(double ms) const noexcept {
        return nextPrime(std::max(2, static_cast<int>(std::lround(ms * 1e-3 * m_sampleRate))));
    }

    void setBand(double lowCutHz, double highCutHz) noexcept {
        const double low = std::clamp(lowCutHz, 10.0, 0.45 * m_sampleRate);
        const double high = std::clamp(highCutHz, 20.0, 0.45 * m_sampleRate);
        if (low != m_lowHz || high != m_highHz) {
            m_lowHz = low;
            m_highHz = high;
            m_lowCutCoefficients = dsp::highPass(m_sampleRate, low);
            m_highCutCoefficients = dsp::lowPass(m_sampleRate, high);
        }
    }

    void computeAbsorption() noexcept {
        const double t60 = m_decayMs * 1e-3;
        const double t60High = t60 * (1.0 - 0.9 * m_dampingNow);
        for (Line& line : m_lines) {
            const auto m = static_cast<double>(line.buffer.size());
            // One pass through the line (m frames) loses 60 dB per t60: gain 10^(-3 m / (fs t60)).
            const double dc = std::pow(10.0, -3.0 * m / (m_sampleRate * t60));
            const double ny = std::pow(10.0, -3.0 * m / (m_sampleRate * t60High));
            line.a = (dc - ny) / (dc + ny);
            line.b = dc * (1.0 - line.a);
        }
    }

    int m_channels;
    int m_sampleRate;
    int m_decay, m_early, m_width, m_highCut, m_lowCut, m_damping, m_diffusion, m_dry, m_wet;
    bool m_surround;
    int m_centerInput; ///< -1: none (Studio Reverb)
    int m_lfeInput;
    double m_decayMs = -1.0;
    double m_dampingNow = -1.0;
    double m_lowHz = -1.0;
    double m_highHz = -1.0;
    std::array<Line, kLines> m_lines{};
    std::array<Delay, kDiffusers> m_diffusers{};
    std::array<int, kTaps> m_leftTaps{};
    std::array<int, kTaps> m_rightTaps{};
    std::array<double, kTaps> m_tapGains{};
    DelayLine m_earlyLine;
    dsp::BiquadCoefficients m_lowCutCoefficients{};
    dsp::BiquadCoefficients m_highCutCoefficients{};
    struct Band {
        dsp::BiquadState low;  ///< Low Frequency Cut (a high-pass)
        dsp::BiquadState high; ///< High Frequency Cut (a low-pass)
    };
    std::array<Band, 2> m_front{};
    std::array<Band, 2> m_rear{};
    int m_settle = 0;
};

} // namespace

bool isTimeEffect(std::string_view id) noexcept {
    return id == kDelay || id == kAnalogDelay || id == kMultitapDelay || id == kChorusFlanger || id == kFlanger ||
           id == kPhaser || id == kConvolutionReverb || id == kStudioReverb || id == kSurroundReverb || id == kDeReverb ||
           id == kSpectralDenoise || id == kAdaptiveNoiseReduction || id == kClickRemover;
}

std::unique_ptr<EffectProcessor> createTimeProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                     std::span<const double> values) {
    const EffectDescriptor& d = descriptor;
    channels = std::clamp(channels, 1, kMaxChannels);
    sampleRate = sampleRate > 0 ? sampleRate : 48000;
    if (values.size() < d.params.size()) {
        return nullptr;
    }
    if (d.id == kDelay) {
        return std::make_unique<DelayProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kAnalogDelay) {
        return std::make_unique<AnalogDelayProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kMultitapDelay) {
        return std::make_unique<MultitapDelayProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kChorusFlanger) {
        return std::make_unique<ChorusFlangerProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kFlanger) {
        return std::make_unique<FlangerProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kPhaser) {
        return std::make_unique<PhaserProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kConvolutionReverb) {
        return std::make_unique<ConvolutionReverbProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kStudioReverb) {
        return std::make_unique<StudioReverbProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kSurroundReverb) {
        return std::make_unique<StudioReverbProcessor>(d, channels, sampleRate, values, true);
    }
    if (d.id == kDeReverb) {
        return std::make_unique<DeReverbProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kSpectralDenoise) {
        return std::make_unique<DeNoiseProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kAdaptiveNoiseReduction) {
        return std::make_unique<AdaptiveNoiseReductionProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kClickRemover) {
        return std::make_unique<ClickRemoverProcessor>(d, channels, sampleRate);
    }
    return nullptr;
}

} // namespace wabcut::audio::premiere
