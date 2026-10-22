#pragma once

// Private (0.2): Loudness Normalization of an export (LoudnessNormalization): the mix is measured first (ITU-R
// BS.1770-4 through libebur128: gated integrated loudness, 4x oversampled true peak), then rendered again through a
// gain and a lookahead true-peak limiter at Max True Peak Level (audio::dsp::Limiter), its delay compensated.

#include "Mixdown.h"

#include <wabcut/audio/dsp/Limiter.h>
#include <wabcut/export/ExportSettings.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

struct LoudnessMeasurement {
    double integratedLufs = 0.0; ///< -inf for silence
    double truePeakDbtp = 0.0;
    double targetLufs = 0.0;
    double gainDb = 0.0;      ///< what the export applies (0 within the tolerance or for silence)
    bool normalized = false;  ///< a gain was applied
};

/// Measures `samples` frames of `mix` (from its start) and decides the gain. Errors: Cancelled.
std::expected<LoudnessMeasurement, Error> measureLoudness(OfflineMix& mix, int channels, int sampleRate,
                                                          std::int64_t samples, const LoudnessNormalization& settings,
                                                          const std::stop_token& stop);

/// The mix through the gain and the true-peak limiter, sample-aligned with the mix (the limiter's lookahead is primed
/// from the mix itself).
class NormalizedMix {
public:
    NormalizedMix(OfflineMix& mix, int channels, int sampleRate, double gainDb, double ceilingDb);
    void render(std::span<float> interleaved);

private:
    OfflineMix& m_mix;
    int m_channels;
    float m_gain;
    std::unique_ptr<audio::dsp::Limiter> m_limiter;
};

/// The text of "<output>.loudness.txt" (Premiere's loudness log).
std::string loudnessLogText(const LoudnessMeasurement& m, const LoudnessNormalization& settings, std::string_view output);

} // namespace wabcut::exporter::detail
