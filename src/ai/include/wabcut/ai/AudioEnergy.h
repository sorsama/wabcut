#pragma once

// AudioEnergyService: how loud (and how speech-like) stretches of a media audio stream are, as a background job
// with a cache (full builds; it decodes media). What silence detection (the transcript review's pause check) and
// the Clean-up review measure: one job for every candidate range instead of a decode per range on the UI thread.
//
//   request (media file, stream, ranges in media time) -> per range, from the in-memory cache or decoded at 16 kHz
//   mono with media::AudioDecoder: RMS and peak in dBFS (-inf for digital silence), and, when asked and the
//   wabcut-infer helper is built, the fraction of the range Silero VAD calls speech.
//   -> the analysis cache stores every request's result under the source fingerprint ("energy" analyzer).

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/TranscriptionService.h>

#include <wabcut/media/Job.h>
#include <wabcut/model/Time.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai {

struct EnergyRequest {
    std::string mediaPath;        ///< UTF-8
    int containerStream = -1;     ///< container index of the audio stream; -1 = the best one
    std::vector<TimeRange> ranges; ///< media time (the decoder's source time)
    /// Also measure the speech fraction of every range (Silero VAD in the helper; nullopt without it).
    bool speech = false;
};

struct EnergyMeasure {
    double rmsDb = 0.0;            ///< dBFS; -inf for digital silence or an empty range
    double peakDb = 0.0;           ///< dBFS of the largest sample; -inf likewise
    std::optional<float> speech;   ///< 0..1, the fraction of the range that is speech, when measured
    bool operator==(const EnergyMeasure&) const = default;
};

struct EnergyReport {
    std::uint64_t fingerprint = 0;
    int streamIndex = -1;          ///< the container stream measured
    std::vector<EnergyMeasure> ranges; ///< in request order
    bool fromCache = false;        ///< every range came from a cache
};

struct AudioEnergyServiceConfig {
    /// The analysis cache; empty = memory cache only.
    std::filesystem::path cacheRoot = AnalysisCache::defaultRoot();
    /// The helper for the speech fraction (inferExecutable empty = defaultInferExecutable(); absent = no speech).
    TranscriptionServiceConfig local;
    std::string vadModel = "silero"; ///< ModelStore id of the VAD model
    /// Temporary PCM for the VAD; <temp>/wabcut-energy when empty.
    std::filesystem::path workDir;
};

class AudioEnergyService {
public:
    explicit AudioEnergyService(AudioEnergyServiceConfig config = {});
    ~AudioEnergyService(); ///< cancels running jobs and waits for them
    AudioEnergyService(const AudioEnergyService&) = delete;
    AudioEnergyService& operator=(const AudioEnergyService&) = delete;

    /// Starts a job. Errors (in the job): NotFound (media or stream), DecoderError, IoError, Cancelled.
    media::Job<EnergyReport> measure(EnergyRequest request);
    /// The same, synchronously on the calling thread. `progress` takes 0..1.
    std::expected<EnergyReport, Error> run(const EnergyRequest& request, std::stop_token stop = {},
                                           const std::function<void(float)>& progress = {});

    /// One range from the memory cache only (what a prior measure() found), or nullopt.
    std::optional<EnergyMeasure> cached(const std::string& mediaPath, int containerStream, TimeRange range) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// RMS and peak of `samples` in dBFS (-inf for none or all zero). Exposed for tests.
EnergyMeasure measureSamples(std::span<const float> samples);

} // namespace wabcut::ai
