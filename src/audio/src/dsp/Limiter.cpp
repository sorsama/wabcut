#include <wabcut/audio/dsp/Limiter.h>

#include <algorithm>
#include <cmath>

namespace wabcut::audio::dsp {

namespace {

using Filter = std::array<std::array<float, TruePeakDetector::kTaps>, TruePeakDetector::kPhases>;

// ITU-R BS.1770-4, Annex 2: the 4x oversampling interpolation filter (phase p, tap k applies to x[n - k]).
constexpr Filter kTruePeakFilter{{
    {0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f, -0.0594482421875f, 0.1373291015625f,
     0.9721679687500f, -0.1022949218750f, 0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f},
    {-0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f, -0.1665039062500f, 0.4650878906250f,
     0.7797851562500f, -0.2003173828125f, 0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f},
    {-0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625000000f, -0.2003173828125f, 0.7797851562500f,
     0.4650878906250f, -0.1665039062500f, 0.0891113281250f, -0.0517578125000f, 0.0292968750000f, -0.0291748046875f},
    {-0.0083007812500f, 0.0148925781250f, -0.0266113281250f, 0.0476074218750f, -0.1022949218750f, 0.9721679687500f,
     0.1373291015625f, -0.0594482421875f, 0.0332031250000f, -0.0196533203125f, 0.0109863281250f, 0.0017089843750f},
}};

constexpr int kExtraHold = 4;    // hold window = L + 4: covers the detector's +-1 sample uncertainty
// The 4x interpolation reads true peaks up to a few tenths of a dB low (worst near fs/4): the gain aims this much
// under the ceiling so the output measures at or below it on an independent meter.
constexpr double kTruePeakMarginDb = 0.15;
constexpr int kExtraLatency = 2; // latency = L + kDelay + 2 (see the class comment)

double releaseCoefficient(double ms, double sampleRate) noexcept {
    return std::exp(-1.0 / (std::max(ms, 1e-3) * 1e-3 * sampleRate));
}

} // namespace

const Filter& truePeakFilter() noexcept { return kTruePeakFilter; }

// ---- TruePeakDetector -------------------------------------------------------------------------------------------

float TruePeakDetector::push(float x) noexcept {
    // Mirrored ring: the last kTaps samples are always contiguous at m_history[m_pos .. m_pos + kTaps), newest last.
    m_history[static_cast<std::size_t>(m_pos)] = x;
    m_history[static_cast<std::size_t>(m_pos + kTaps)] = x;
    m_pos = (m_pos + 1) % kTaps;
    const float* window = m_history.data() + m_pos; // window[kTaps - 1] = x[n], window[kTaps - 1 - k] = x[n - k]
    float peak = std::fabs(window[kTaps - 1 - kDelay]);
    for (const auto& phase : kTruePeakFilter) {
        float y = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            y += phase[static_cast<std::size_t>(k)] * window[kTaps - 1 - k];
        }
        peak = std::max(peak, std::fabs(y));
    }
    return peak;
}

void TruePeakDetector::reset() noexcept {
    m_history.fill(0.0f);
    m_pos = 0;
}

// ---- Limiter ----------------------------------------------------------------------------------------------------

Limiter::Limiter(int channels, double sampleRate, double lookaheadMs, double maxLookaheadMs)
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {
    const double ms = std::clamp(lookaheadMs, 0.0, std::max(maxLookaheadMs, 0.0));
    m_lookahead = static_cast<int>(std::lround(ms * 1e-3 * m_sampleRate));
    m_latency = m_lookahead + TruePeakDetector::kDelay + kExtraLatency;
    m_delayFrames = m_latency;
    m_delay = std::make_unique<float[]>(static_cast<std::size_t>(m_delayFrames) * static_cast<std::size_t>(m_channels));
    m_holdLength = m_lookahead + kExtraHold;
    m_dequeCapacity = m_holdLength + 1;
    m_dequeValue = std::make_unique<double[]>(static_cast<std::size_t>(m_dequeCapacity));
    m_dequeIndex = std::make_unique<std::int64_t[]>(static_cast<std::size_t>(m_dequeCapacity));
    m_boxLength = m_lookahead + 1;
    m_box = std::make_unique<double[]>(static_cast<std::size_t>(m_boxLength));
    setSettings(-1.0, 50.0);
    reset();
}

void Limiter::setSettings(double ceilingDb, double releaseMs) noexcept {
    m_ceiling = std::pow(10.0, std::min(ceilingDb, 0.0) / 20.0);
    m_target = std::pow(10.0, (std::min(ceilingDb, 0.0) - kTruePeakMarginDb) / 20.0);
    if (releaseMs != m_releaseMs || m_release == 0.0) {
        m_releaseMs = releaseMs;
        m_release = releaseCoefficient(releaseMs, m_sampleRate);
    }
}

int Limiter::settleFrames() const noexcept {
    const double ms = std::min(5.0 * m_releaseMs, 2000.0);
    return m_latency + static_cast<int>(std::ceil(ms * 1e-3 * m_sampleRate));
}

void Limiter::reset() noexcept {
    std::fill_n(m_delay.get(), static_cast<std::size_t>(m_delayFrames) * static_cast<std::size_t>(m_channels), 0.0f);
    m_delayPos = 0;
    for (TruePeakDetector& d : m_detectors) {
        d.reset();
    }
    m_dequeHead = 0;
    m_dequeSize = 0;
    std::fill_n(m_box.get(), static_cast<std::size_t>(m_boxLength), 1.0);
    m_boxPos = 0;
    m_boxSum = m_boxLength;
    m_sinceResync = 0;
    m_index = 0;
    m_gain = 1.0;
}

void Limiter::pushRequired(double required) noexcept {
    // Sliding minimum over the last m_holdLength required gains.
    while (m_dequeSize > 0) {
        const int back = (m_dequeHead + m_dequeSize - 1) % m_dequeCapacity;
        if (m_dequeValue[static_cast<std::size_t>(back)] < required) {
            break;
        }
        --m_dequeSize;
    }
    const int slot = (m_dequeHead + m_dequeSize) % m_dequeCapacity;
    m_dequeValue[static_cast<std::size_t>(slot)] = required;
    m_dequeIndex[static_cast<std::size_t>(slot)] = m_index;
    ++m_dequeSize;
    while (m_dequeIndex[static_cast<std::size_t>(m_dequeHead)] <= m_index - m_holdLength) {
        m_dequeHead = (m_dequeHead + 1) % m_dequeCapacity;
        --m_dequeSize;
    }
    const double held = m_dequeValue[static_cast<std::size_t>(m_dequeHead)];

    // Box filter: the average of the last m_boxLength held values.
    m_boxSum += held - m_box[static_cast<std::size_t>(m_boxPos)];
    m_box[static_cast<std::size_t>(m_boxPos)] = held;
    m_boxPos = (m_boxPos + 1) % m_boxLength;
    if (++m_sinceResync >= 65536) { // keep the running sum from drifting
        m_sinceResync = 0;
        m_boxSum = 0.0;
        for (int i = 0; i < m_boxLength; ++i) {
            m_boxSum += m_box[static_cast<std::size_t>(i)];
        }
    }
    const double smoothed = std::min(m_boxSum / m_boxLength, 1.0);

    // Attack is the box ramp; release is exponential and never above the box (lower gain is always safe).
    m_gain = smoothed < m_gain ? smoothed : smoothed + m_release * (m_gain - smoothed);
    ++m_index;
}

void Limiter::process(float* interleaved, int frames) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto ceiling = static_cast<float>(m_ceiling);
    for (int i = 0; i < frames; ++i) {
        float* frame = interleaved + static_cast<std::size_t>(i) * ch;
        float peak = 0.0f;
        for (std::size_t c = 0; c < ch; ++c) {
            peak = std::max(peak, m_detectors[c].push(frame[c]));
        }
        pushRequired(peak > m_target ? m_target / peak : 1.0);

        float* delayed = m_delay.get() + static_cast<std::size_t>(m_delayPos) * ch;
        const auto g = static_cast<float>(m_gain);
        for (std::size_t c = 0; c < ch; ++c) {
            const float in = frame[c];
            frame[c] = std::clamp(delayed[c] * g, -ceiling, ceiling);
            delayed[c] = in;
        }
        m_delayPos = (m_delayPos + 1) % m_delayFrames;
    }
}

} // namespace wabcut::audio::dsp
