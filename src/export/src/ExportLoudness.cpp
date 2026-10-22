#include "ExportLoudness.h"

#include <wabcut/audio/dsp/Loudness.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace wabcut::exporter::detail {

std::expected<LoudnessMeasurement, Error> measureLoudness(OfflineMix& mix, int channels, int sampleRate,
                                                          std::int64_t samples, const LoudnessNormalization& settings,
                                                          const std::stop_token& stop) {
    audio::dsp::LoudnessMeter meter(channels, sampleRate);
    std::vector<float> buffer;
    for (std::int64_t done = 0; done < samples;) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"});
        }
        const std::int64_t n = std::min<std::int64_t>(samples - done, 8192);
        buffer.resize(static_cast<std::size_t>(n * channels));
        mix.render(buffer);
        meter.add(buffer.data(), static_cast<std::size_t>(n));
        done += n;
    }
    const audio::dsp::LoudnessResult r = meter.result();
    LoudnessMeasurement m;
    m.integratedLufs = r.integratedLufs;
    m.truePeakDbtp = r.truePeakDbtp;
    m.targetLufs = settings.effectiveTarget();
    if (r.hasIntegrated() && std::abs(r.integratedLufs - m.targetLufs) > settings.toleranceLu) {
        m.gainDb = std::clamp(m.targetLufs - r.integratedLufs, -60.0, 60.0);
        m.normalized = true;
    }
    return m;
}

NormalizedMix::NormalizedMix(OfflineMix& mix, int channels, int sampleRate, double gainDb, double ceilingDb)
    : m_mix(mix), m_channels(channels), m_gain(static_cast<float>(std::pow(10.0, gainDb / 20.0))),
      m_limiter(std::make_unique<audio::dsp::Limiter>(channels, static_cast<double>(sampleRate), 5.0)) {
    m_limiter->setSettings(ceilingDb, 50.0);
    // Prime the lookahead: the first latency frames of the mix go in and their (delayed, silent) output is dropped, so
    // every later output frame is the input frame of the same index.
    const int latency = m_limiter->latencyFrames();
    if (latency > 0) {
        std::vector<float> prime(static_cast<std::size_t>(latency * channels));
        m_mix.render(prime);
        for (float& s : prime) {
            s *= m_gain;
        }
        m_limiter->process(prime.data(), latency);
    }
}

void NormalizedMix::render(std::span<float> interleaved) {
    m_mix.render(interleaved);
    for (float& s : interleaved) {
        s *= m_gain;
    }
    m_limiter->process(interleaved.data(), static_cast<int>(interleaved.size() / static_cast<std::size_t>(m_channels)));
}

std::string loudnessLogText(const LoudnessMeasurement& m, const LoudnessNormalization& s, std::string_view output) {
    const auto level = [](double v, const char* unit) {
        return std::isfinite(v) ? std::format("{:.1f} {}", v, unit) : std::string("silence");
    };
    std::string text = std::format("Loudness Normalization: {}\n", output);
    text += std::format("Loudness standard: {} (ITU-R BS.1770-4 measurement)\n",
                        s.standard == LoudnessStandard::EbuR128   ? "EBU R 128"
                        : s.standard == LoudnessStandard::AtscA85 ? "ATSC A/85"
                                                                  : "custom");
    text += std::format("Target loudness: {:.1f} LUFS, tolerance {:.1f} LU, max true peak {:.1f} dBTP\n", s.effectiveTarget(),
                        s.toleranceLu, s.effectiveTruePeak());
    text += "Measured loudness: " + level(m.integratedLufs, "LUFS") + "\n";
    text += "Measured true peak: " + level(m.truePeakDbtp, "dBTP") + "\n";
    text += m.normalized ? std::format("Gain applied: {:+.2f} dB, then the true-peak limiter\n", m.gainDb)
                         : std::string("Within tolerance (or silent): no gain; the true-peak limiter only\n");
    return text;
}

} // namespace wabcut::exporter::detail
