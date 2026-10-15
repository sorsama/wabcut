#include <wabcut/ai/highlights/Features.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace wabcut::ai::highlights {

namespace {

constexpr std::int64_t kTicksPerFeature = kTicksPerSecond / kFeatureRate;
constexpr std::size_t kLevelHops = 4;      // 400 ms loudness window
constexpr std::size_t kBurstWindow = 100;  // 1 s of 10 ms energies
constexpr double kMinPitchHz = 70.0;
constexpr double kMaxPitchHz = 500.0;
constexpr double kVoicedCorrelation = 0.6;
constexpr double kFrameFloor = 1e-6; // mean square of a frame below which it is not analysed (-60 dBFS)

float toDb(double meanSquare) {
    if (!(meanSquare > 1e-10)) {
        return kSilenceDb;
    }
    return std::max(kSilenceDb, static_cast<float>(10.0 * std::log10(meanSquare)));
}

/// Pitch of one window by normalised autocorrelation (the first lag within 90 % of the best one, which avoids
/// octave-down errors); 0 when it is not periodic enough.
float pitchOf(std::span<const float> x, double rate) {
    const auto minLag = static_cast<std::size_t>(std::floor(rate / kMaxPitchHz));
    const auto maxLag = std::min(x.size() / 2, static_cast<std::size_t>(std::ceil(rate / kMinPitchHz)));
    if (minLag < 2 || maxLag <= minLag) {
        return 0.0f;
    }
    std::vector<double> r(maxLag + 1, 0.0);
    double best = 0.0;
    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        double xy = 0.0;
        double xx = 0.0;
        double yy = 0.0;
        const std::size_t n = x.size() - lag;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = x[i];
            const double b = x[i + lag];
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }
        r[lag] = xx > 0.0 && yy > 0.0 ? xy / std::sqrt(xx * yy) : 0.0;
        best = std::max(best, r[lag]);
    }
    if (best < kVoicedCorrelation) {
        return 0.0f;
    }
    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        const bool peak = (lag == minLag || r[lag] >= r[lag - 1]) && (lag == maxLag || r[lag] >= r[lag + 1]);
        if (peak && r[lag] >= 0.9 * best) {
            // Parabolic interpolation around the peak.
            double offset = 0.0;
            if (lag > minLag && lag < maxLag) {
                const double denominator = r[lag - 1] - 2.0 * r[lag] + r[lag + 1];
                if (std::abs(denominator) > 1e-12) {
                    offset = std::clamp(0.5 * (r[lag - 1] - r[lag + 1]) / denominator, -0.5, 0.5);
                }
            }
            return static_cast<float>(rate / (static_cast<double>(lag) + offset));
        }
    }
    return 0.0f;
}

/// How strongly a second of 10 ms energies pulses at 3-7 Hz (laughter, "ha-ha-ha"), 0..1.
float burstOf(const std::deque<float>& energies) {
    if (energies.size() < kBurstWindow) {
        return 0.0f;
    }
    std::vector<double> db(energies.size());
    for (std::size_t i = 0; i < energies.size(); ++i) {
        db[i] = toDb(energies[i]);
    }
    const auto [lo, hi] = std::minmax_element(db.begin(), db.end());
    const double depth = *hi - *lo;
    if (*hi < -45.0 || depth < 9.0) {
        return 0.0f;
    }
    const double mean = std::accumulate(db.begin(), db.end(), 0.0) / static_cast<double>(db.size());
    double power = 0.0;
    for (double& v : db) {
        v -= mean;
        power += v * v;
    }
    if (power <= 0.0) {
        return 0.0f;
    }
    double best = 0.0;
    for (std::size_t lag = 14; lag <= 33; ++lag) { // 100 Hz energies: 7.1 .. 3.0 pulses a second
        double sum = 0.0;
        for (std::size_t i = 0; i + lag < db.size(); ++i) {
            sum += db[i] * db[i + lag];
        }
        best = std::max(best, sum / power);
    }
    return static_cast<float>(std::clamp(best * std::min(1.0, depth / 18.0) * 1.6, 0.0, 1.0));
}

} // namespace

std::size_t featureIndex(Ticks t) {
    return t.value <= 0 ? 0 : static_cast<std::size_t>(t.value / kTicksPerFeature);
}

Ticks featureTime(std::size_t i) {
    return Ticks{static_cast<std::int64_t>(i) * kTicksPerFeature};
}

// --- Audio ---------------------------------------------------------------------------------------------------------

AudioFeatureExtractor::AudioFeatureExtractor(int sampleRate)
    : m_rate(std::clamp(sampleRate, 8000, 192000)),
      m_hopSamples(static_cast<std::size_t>(m_rate / kFeatureRate)),
      m_frameSamples(static_cast<std::size_t>(m_rate / 50)) {
    m_window.reserve(m_frameSamples * 2);
}

void AudioFeatureExtractor::add(std::span<const float> mono) {
    m_pending.insert(m_pending.end(), mono.begin(), mono.end());
    std::size_t used = 0;
    while (m_pending.size() - used >= m_hopSamples) {
        const std::span<const float> hopSamples(m_pending.data() + used, m_hopSamples);
        const std::size_t tenMs = m_hopSamples / 10;
        for (std::size_t i = 0; i < hopSamples.size(); ++i) {
            const double s = std::isfinite(hopSamples[i]) ? hopSamples[i] : 0.0;
            m_hopSum += s * s;
            m_tenMsSum += s * s;
            if (++m_tenMsCount == tenMs) {
                m_energy10ms.push_back(static_cast<float>(m_tenMsSum / static_cast<double>(tenMs)));
                if (m_energy10ms.size() > kBurstWindow) {
                    m_energy10ms.pop_front();
                }
                m_tenMsSum = 0.0;
                m_tenMsCount = 0;
            }
        }
        m_hopCount += hopSamples.size();
        for (std::size_t f = 0; f + m_frameSamples <= hopSamples.size(); f += m_frameSamples) {
            frame(hopSamples.subspan(f, m_frameSamples));
        }
        hop();
        used += m_hopSamples;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(used));
}

void AudioFeatureExtractor::frame(std::span<const float> samples) {
    // A 40 ms window: the previous 20 ms frame and this one.
    if (m_window.size() >= m_frameSamples * 2) {
        m_window.erase(m_window.begin(), m_window.begin() + static_cast<std::ptrdiff_t>(m_frameSamples));
    }
    m_window.insert(m_window.end(), samples.begin(), samples.end());
    ++m_hopFrames;
    if (m_window.size() < m_frameSamples * 2) {
        return;
    }
    double energy = 0.0;
    for (float s : m_window) {
        energy += static_cast<double>(s) * s;
    }
    if (energy / static_cast<double>(m_window.size()) < kFrameFloor) {
        return;
    }
    // Decimate to ~8 kHz (pairwise means: a crude low-pass) - enough for 70-500 Hz and four times cheaper.
    const std::size_t step = static_cast<std::size_t>(std::max(1, m_rate / 8000));
    m_decimated.clear();
    for (std::size_t i = 0; i + step <= m_window.size(); i += step) {
        float sum = 0.0f;
        for (std::size_t k = 0; k < step; ++k) {
            sum += m_window[i + k];
        }
        m_decimated.push_back(sum / static_cast<float>(step));
    }
    const float pitch = pitchOf(m_decimated, static_cast<double>(m_rate) / static_cast<double>(step));
    if (pitch > 0.0f) {
        ++m_hopVoiced;
        m_hopPitches.push_back(pitch);
    }
}

void AudioFeatureExtractor::hop() {
    m_hopEnergy.push_back(m_hopSum);
    m_hopCounts.push_back(m_hopCount);
    if (m_hopEnergy.size() > kLevelHops) {
        m_hopEnergy.pop_front();
        m_hopCounts.pop_front();
    }
    const double sum = std::accumulate(m_hopEnergy.begin(), m_hopEnergy.end(), 0.0);
    const std::size_t count = std::accumulate(m_hopCounts.begin(), m_hopCounts.end(), std::size_t{0});
    m_out.levelDb.push_back(count > 0 ? toDb(sum / static_cast<double>(count)) : kSilenceDb);
    m_out.voiced.push_back(m_hopFrames > 0 ? static_cast<float>(m_hopVoiced) / static_cast<float>(m_hopFrames) : 0.0f);
    float pitch = 0.0f;
    if (!m_hopPitches.empty()) {
        auto middle = m_hopPitches.begin() + static_cast<std::ptrdiff_t>(m_hopPitches.size() / 2);
        std::nth_element(m_hopPitches.begin(), middle, m_hopPitches.end());
        pitch = *middle;
    }
    m_out.pitchHz.push_back(pitch);
    m_out.burst.push_back(burstOf(m_energy10ms));
    m_hopSum = 0.0;
    m_hopCount = 0;
    m_hopFrames = 0;
    m_hopVoiced = 0;
    m_hopPitches.clear();
}

AudioFeatures AudioFeatureExtractor::finish() {
    if (m_pending.size() >= m_frameSamples) {
        std::vector<float> rest(m_pending);
        rest.resize(m_hopSamples, 0.0f); // the tail padded with silence: its level reads a little low
        m_pending.clear();
        add(rest);
    }
    m_pending.clear();
    AudioFeatures out = std::move(m_out);
    m_out = {};
    return out;
}

// --- Video ---------------------------------------------------------------------------------------------------------

void VisualFeatureExtractor::closeHopsBefore(std::size_t index) {
    while (m_hop < index) {
        const float luma = m_lumaCount > 0 ? static_cast<float>(m_lumaSum / m_lumaCount)
                                           : (m_out.luma.empty() ? 0.0f : m_out.luma.back());
        float flash = 0.0f;
        if (m_lumaCount > 0 && !m_recentLuma.empty()) {
            const double before = std::accumulate(m_recentLuma.begin(), m_recentLuma.end(), 0.0) / static_cast<double>(m_recentLuma.size());
            const double jump = m_lumaMax - before;
            if (jump > 0.12) {
                flash = static_cast<float>(std::clamp((jump - 0.12) / 0.3 + 0.3, 0.0, 1.0));
            }
        }
        m_out.motion.push_back(m_motionCount > 0 ? static_cast<float>(m_motionSum / m_motionCount) : 0.0f);
        m_out.luma.push_back(luma);
        m_out.flash.push_back(flash);
        m_out.cut.push_back(m_cut ? 1.0f : 0.0f);
        m_recentLuma.push_back(luma);
        if (m_recentLuma.size() > 5) {
            m_recentLuma.pop_front();
        }
        m_motionSum = 0.0;
        m_motionCount = 0;
        m_lumaSum = 0.0;
        m_lumaMax = 0.0;
        m_lumaCount = 0;
        m_cut = false;
        ++m_hop;
    }
}

void VisualFeatureExtractor::add(const analysis::AnalysisFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.rgb.size() < static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3) {
        return;
    }
    const std::size_t index = featureIndex(frame.pts);
    if (!m_started) {
        m_started = true;
        m_hop = 0;
    }
    if (index < m_hop) {
        return; // out of order: ignore
    }
    closeHopsBefore(index);

    // Luma, at most 64 px wide (box-sampled), and its 16-bin histogram.
    const int step = std::max(1, (frame.width + 63) / 64);
    const int w = frame.width / step;
    const int h = frame.height / step;
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<int> histogram(16, 0);
    double sum = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y * step) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x * step)) * 3;
            const int value = (299 * frame.rgb[at] + 587 * frame.rgb[at + 1] + 114 * frame.rgb[at + 2]) / 1000;
            luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(value);
            ++histogram[static_cast<std::size_t>(value / 16)];
            sum += value;
        }
    }
    const double pixels = static_cast<double>(luma.size());
    const double mean = pixels > 0 ? sum / pixels / 255.0 : 0.0;
    m_lumaSum += mean;
    m_lumaMax = std::max(m_lumaMax, mean);
    ++m_lumaCount;
    if (w == m_prevWidth && h == m_prevHeight && !m_previous.empty()) {
        std::int64_t diff = 0;
        for (std::size_t i = 0; i < luma.size(); ++i) {
            diff += std::abs(static_cast<int>(luma[i]) - static_cast<int>(m_previous[i]));
        }
        const double motion = static_cast<double>(diff) / pixels / 255.0;
        m_motionSum += motion;
        ++m_motionCount;
        int moved = 0;
        for (std::size_t b = 0; b < histogram.size(); ++b) {
            moved += std::abs(histogram[b] - m_prevHistogram[b]);
        }
        if (static_cast<double>(moved) / (2.0 * pixels) > 0.5 && motion > 0.15) {
            m_cut = true;
        }
    }
    m_previous = std::move(luma);
    m_prevHistogram = std::move(histogram);
    m_prevWidth = w;
    m_prevHeight = h;
}

VisualFeatures VisualFeatureExtractor::finish() {
    if (m_started && (m_lumaCount > 0 || m_motionCount > 0)) {
        closeHopsBefore(m_hop + 1);
    }
    VisualFeatures out = std::move(m_out);
    *this = VisualFeatureExtractor{};
    return out;
}

} // namespace wabcut::ai::highlights
