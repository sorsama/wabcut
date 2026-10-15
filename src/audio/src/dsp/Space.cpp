#include <wabcut/audio/dsp/Space.h>

#include <algorithm>
#include <cmath>

namespace wabcut::audio::dsp {

namespace {

bool isPrime(int n) noexcept {
    if (n < 2) {
        return false;
    }
    for (int d = 2; d * d <= n; ++d) {
        if (n % d == 0) {
            return false;
        }
    }
    return true;
}

int nextPrime(int n) noexcept {
    while (!isPrime(n)) {
        ++n;
    }
    return n;
}

int primeFrames(double ms, double sampleRate) { return nextPrime(std::max(2, static_cast<int>(std::lround(ms * 1e-3 * sampleRate)))); }

// Delay line lengths at size 0.5 (ms): mutually prime once rounded, spread over 31..71 ms so the echoes do not line up.
constexpr std::array<double, Reverb::kLines> kLineMs{31.1, 37.3, 41.9, 45.7, 51.7, 58.1, 63.7, 71.3};
// The all-pass diffusers (ms) and their gains, after Schroeder's series all-passes.
constexpr std::array<double, Reverb::kDiffusers> kDiffuserMs{4.77, 3.59, 12.73, 9.31};
constexpr std::array<double, Reverb::kDiffusers> kDiffuserGain{0.70, 0.70, 0.625, 0.625};
// Output gain of the wet signal (the lines read with signs, over sqrt(N)).
constexpr double kWetGain = 0.7;

/// The line length scale for a room size 0..1: 0.35 (a small room) .. 1.65 (a hall).
double sizeScale(double size) noexcept { return 0.35 + 1.3 * std::clamp(size, 0.0, 1.0); }

constexpr double kQuietLevel = 1e-10; ///< about -200 dBFS

} // namespace

// ---- Reverb ------------------------------------------------------------------------------------------------------

Reverb::Reverb(int channels, double sampleRate, double size, double predelayMs)
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {
    const double scale = sizeScale(size);
    m_predelayFrames = static_cast<int>(std::lround(std::clamp(predelayMs, 0.0, kMaxPredelayMs) * 1e-3 * m_sampleRate));
    m_predelay.buffer.assign(static_cast<std::size_t>(std::max(m_predelayFrames, 1)), 0.0);
    for (int i = 0; i < kDiffusers; ++i) {
        auto& d = m_diffusers[static_cast<std::size_t>(i)];
        d.delay.buffer.assign(static_cast<std::size_t>(primeFrames(kDiffuserMs[static_cast<std::size_t>(i)] * (0.5 + 0.5 * scale), m_sampleRate)), 0.0);
        d.g = kDiffuserGain[static_cast<std::size_t>(i)];
    }
    int previous = 0;
    for (int i = 0; i < kLines; ++i) {
        int frames = primeFrames(kLineMs[static_cast<std::size_t>(i)] * scale, m_sampleRate);
        if (frames <= previous) {
            frames = nextPrime(previous + 1); // distinct, increasing lengths even at very low rates
        }
        previous = frames;
        m_lines[static_cast<std::size_t>(i)].delay.buffer.assign(static_cast<std::size_t>(frames), 0.0);
    }
    computeAbsorption();
}

void Reverb::setSettings(const ReverbSettings& s) noexcept {
    if (s == m_settings) {
        return;
    }
    const bool decayChanged = s.decayMs != m_settings.decayMs || s.damping != m_settings.damping;
    m_settings = s;
    if (decayChanged) {
        computeAbsorption();
    }
}

void Reverb::computeAbsorption() noexcept {
    const double t60 = std::max(m_settings.decayMs, 1.0) * 1e-3;
    const double t60High = t60 * (1.0 - 0.9 * std::clamp(m_settings.damping, 0.0, 1.0));
    for (Line& line : m_lines) {
        const auto m = static_cast<double>(line.delay.buffer.size());
        // A pass through the line (m frames) must lose 60 dB per t60: gain 10^(-3 m / (fs t60)).
        const double dc = std::pow(10.0, -3.0 * m / (m_sampleRate * t60));
        const double ny = std::pow(10.0, -3.0 * m / (m_sampleRate * t60High));
        line.a = (dc - ny) / (dc + ny);
        line.b = dc * (1.0 - line.a);
    }
}

void Reverb::process(float* io, int frames) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    const double mix = std::clamp(m_settings.mix, 0.0, 1.0);
    const double dry = 1.0 - mix;
    const double width = std::clamp(m_settings.width, 0.0, 1.0);
    const double tap = kWetGain / std::sqrt(static_cast<double>(kLines));
    const double inject = 1.0 / std::sqrt(static_cast<double>(kLines));
    const auto silenceAfter = static_cast<std::int64_t>(m_lines.back().delay.buffer.size()) * 8 + m_predelayFrames + 64;
    // Review A5-P2-10: a 5.1 buffer (L R C LFE Ls Rs): the LFE neither feeds the tank nor gets a tail, the centre gets
    // the tail's mid, the surrounds their side's.
    constexpr std::size_t kLfe = 3;
    const bool surround = ch == 6;
    const double inputs = surround ? 5.0 : static_cast<double>(ch);
    std::array<double, kLines> out{};
    for (int i = 0; i < frames; ++i) {
        float* frame = io + static_cast<std::size_t>(i) * ch;
        double in = 0.0;
        bool any = false;
        for (std::size_t c = 0; c < ch; ++c) {
            if (surround && c == kLfe) {
                continue;
            }
            in += frame[c];
            any = any || frame[c] != 0.0f;
        }
        in /= inputs;
        if (m_silent && !any) {
            continue; // nothing in and no tail: the output is the silent input
        }
        m_silent = false;
        // Predelay.
        double x = in;
        if (m_predelayFrames > 0) {
            double& slot = m_predelay.buffer[static_cast<std::size_t>(m_predelay.position)];
            x = slot;
            slot = in;
            m_predelay.position = m_predelay.position + 1 == m_predelayFrames ? 0 : m_predelay.position + 1;
        }
        // Diffusion: series all-passes.
        for (Diffuser& d : m_diffusers) {
            double& slot = d.delay.buffer[static_cast<std::size_t>(d.delay.position)];
            const double delayed = slot;
            const double w = x + d.g * delayed;
            x = delayed - d.g * w;
            slot = w;
            const auto n = static_cast<int>(d.delay.buffer.size());
            d.delay.position = d.delay.position + 1 == n ? 0 : d.delay.position + 1;
        }
        // The network: read the lines through their absorption filters, mix them with the Householder matrix, feed
        // them back with the diffused input.
        double sum = 0.0;
        for (int k = 0; k < kLines; ++k) {
            Line& line = m_lines[static_cast<std::size_t>(k)];
            const double s = line.delay.buffer[static_cast<std::size_t>(line.delay.position)];
            line.state = line.b * s + line.a * line.state;
            out[static_cast<std::size_t>(k)] = line.state;
            sum += line.state;
        }
        const double householder = 2.0 / kLines * sum;
        double left = 0.0;
        double right = 0.0;
        double level = std::fabs(in);
        for (int k = 0; k < kLines; ++k) {
            Line& line = m_lines[static_cast<std::size_t>(k)];
            const double v = out[static_cast<std::size_t>(k)];
            const double sign = (k & 1) == 0 ? 1.0 : -1.0;
            line.delay.buffer[static_cast<std::size_t>(line.delay.position)] = v - householder + sign * inject * x;
            const auto n = static_cast<int>(line.delay.buffer.size());
            line.delay.position = line.delay.position + 1 == n ? 0 : line.delay.position + 1;
            left += sign * v;
            right += ((k >> 1) & 1) == 0 ? v : -v;
            level = std::max(level, std::fabs(v));
        }
        left *= tap;
        right *= tap;
        const double mid = 0.5 * (left + right);
        const double side = 0.5 * (left - right) * width;
        left = mid + side;
        right = mid - side;
        if (ch == 1) {
            frame[0] = static_cast<float>(dry * frame[0] + mix * mid);
        } else if (surround) {
            constexpr std::array<int, 6> kSide{-1, 1, 0, 2, -1, 1}; // -1 left, 1 right, 0 mid, 2 the LFE (dry)
            for (std::size_t c = 0; c < ch; ++c) {
                const int route = kSide[c];
                if (route != 2) {
                    frame[c] = static_cast<float>(dry * frame[c] + mix * (route < 0 ? left : route > 0 ? right : mid));
                }
            }
        } else {
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(dry * frame[c] + mix * ((c & 1) == 0 ? left : right));
            }
        }
        // A tail decayed below -200 dBFS with no input for a while: clear the state (no denormals, no work in silence).
        // Decided sample by sample, so it happens at the same frame however the audio is sliced.
        if (level < kQuietLevel) {
            if (++m_quiet >= silenceAfter) {
                reset();
            }
        } else {
            m_quiet = 0;
        }
    }
}

void Reverb::reset() noexcept {
    std::fill(m_predelay.buffer.begin(), m_predelay.buffer.end(), 0.0);
    m_predelay.position = 0;
    for (Diffuser& d : m_diffusers) {
        std::fill(d.delay.buffer.begin(), d.delay.buffer.end(), 0.0);
        d.delay.position = 0;
    }
    for (Line& line : m_lines) {
        std::fill(line.delay.buffer.begin(), line.delay.buffer.end(), 0.0);
        line.delay.position = 0;
        line.state = 0.0;
    }
    m_quiet = 0;
    m_silent = true;
}

int Reverb::settleFrames() const noexcept {
    return static_cast<int>(std::ceil(std::min(m_settings.decayMs * 1e-3, 3.0) * m_sampleRate)) + m_predelayFrames;
}

std::array<int, Reverb::kLines> Reverb::lineLengths() const noexcept {
    std::array<int, kLines> out{};
    for (int k = 0; k < kLines; ++k) {
        out[static_cast<std::size_t>(k)] = static_cast<int>(m_lines[static_cast<std::size_t>(k)].delay.buffer.size());
    }
    return out;
}

// ---- StereoWidth -------------------------------------------------------------------------------------------------

StereoWidth::StereoWidth(int channels, double sampleRate) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {}

void StereoWidth::setSettings(double width, double bassMonoHz) noexcept {
    m_width = std::clamp(width, 0.0, 2.0);
    const double bass = std::max(bassMonoHz, 0.0);
    if (bass != m_bassMono) {
        if (m_bassMono <= 0.0) {
            m_sideState.reset();
        }
        m_bassMono = bass;
        if (bass > 0.0) {
            m_sideHighPass = highPass(m_sampleRate, bass);
        }
    }
}

void StereoWidth::process(float* io, int frames) noexcept {
    if (m_channels < 2 || (m_width == 1.0 && m_bassMono <= 0.0)) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    for (int i = 0; i < frames; ++i) {
        float* frame = io + static_cast<std::size_t>(i) * ch;
        const double l = frame[0];
        const double r = frame[1];
        const double mid = 0.5 * (l + r);
        double side = 0.5 * (l - r);
        if (m_bassMono > 0.0) {
            side = m_sideState.process(m_sideHighPass, static_cast<float>(side));
        }
        side *= m_width;
        frame[0] = static_cast<float>(mid + side);
        frame[1] = static_cast<float>(mid - side);
    }
}

void StereoWidth::reset() noexcept { m_sideState.reset(); }

} // namespace wabcut::audio::dsp
