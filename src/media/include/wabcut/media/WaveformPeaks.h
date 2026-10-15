#pragma once

// Waveform overviews: min/max peak pyramids per channel, computed in the background and cached on disk.

#include <wabcut/media/Job.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::media {

struct WaveformOptions {
    int stream = -1; ///< Container audio stream index, -1 = best.
    /// Samples per bin of each level, finest first. Each must be a multiple of the previous one (and >= 16).
    std::vector<int> samplesPerBin{256, 2048, 16384};
};

/// One level of detail. Peaks are 16-bit (full scale = 32767); values beyond full scale are clipped.
struct PeakLevel {
    int samplesPerBin = 0;
    std::int64_t binCount = 0;
    /// Interleaved as [bin][channel][min, max]: size = binCount * channels * 2.
    std::vector<std::int16_t> peaks;

    std::int16_t minAt(std::int64_t bin, int channel, int channels) const {
        return peaks[static_cast<std::size_t>((bin * channels + channel) * 2)];
    }
    std::int16_t maxAt(std::int64_t bin, int channel, int channels) const {
        return peaks[static_cast<std::size_t>((bin * channels + channel) * 2 + 1)];
    }
};

/// Peaks of one audio stream at its native sample rate and channel count. Bin b of a level covers samples
/// [b * samplesPerBin, (b + 1) * samplesPerBin) counted from the media origin (see AudioDecoder); the last
/// bin of each level may be partial.
struct WaveformPeaks {
    Rate sampleRate{48000, 1};
    int channels = 0;
    std::int64_t sampleCount = 0;  ///< Samples per channel that were analysed.
    std::vector<PeakLevel> levels; ///< Same order as WaveformOptions::samplesPerBin.
    bool fromCache = false;

    /// The coarsest level whose bins are not wider than `samplesPerPixel` (falls back to the finest level).
    const PeakLevel* levelFor(double samplesPerPixel) const;
};

/// Snapshots of a peak computation in progress, so the waveform of a long recording (a 2-hour VOD decodes for tens
/// of seconds) can be drawn while it is still being decoded.
struct WaveformPartials {
    /// Receives each snapshot, in order, on the computing thread. A snapshot is an exact prefix of the final result:
    /// sampleCount is a whole number of finest bins, every level holds only its complete bins (binCount =
    /// sampleCount / samplesPerBin, rounded down), and each of those bins equals the final result's. The final result
    /// itself is not delivered here, and a stream decoded within one interval delivers none. Empty: no snapshots
    /// (and nothing is copied).
    std::function<void(std::shared_ptr<const WaveformPeaks>)> onPartial;
    /// Wall-clock time from the start to the first snapshot and between two snapshots. Zero: one after every
    /// decoded block (finest samplesPerBin x 256 samples per channel, 65536 with the default levels), for tests.
    std::chrono::milliseconds interval{250};
};

/// Decodes the whole stream synchronously and computes the pyramid (no cache). Deterministic: the result does not
/// depend on `partials`. Checks `stop` regularly (ErrorCode::Cancelled); `progress` (optional) receives values in
/// [0, 1]; `partials` (optional) receives snapshots while it decodes.
std::expected<WaveformPeaks, Error> computeWaveformPeaks(const std::string& path,
                                                         const WaveformOptions& options = {},
                                                         std::stop_token stop = {},
                                                         const std::function<void(float)>& progress = {},
                                                         const WaveformPartials& partials = {});

/// Background peak computation with a disk cache in `<cacheDir>/peaks/` (binary file with a small versioned
/// header, keyed by the source fingerprint and options). Same threading and caching rules as
/// ThumbnailService.
///
/// Requests run in submission order on `workerThreads` workers, each decoding its stream on its own. The streams of
/// one file therefore build at the same time when there are enough workers: each pass reads the whole file (a
/// Matroska recording interleaves every stream), and passes that run together read it once from disk and share the
/// OS file cache instead of reading it once per stream, one after the other.
class WaveformPeaksService {
public:
    using Callback = std::function<void(const std::expected<WaveformPeaks, Error>&)>;

    explicit WaveformPeaksService(std::filesystem::path cacheDir, int workerThreads = 1);
    ~WaveformPeaksService();
    WaveformPeaksService(const WaveformPeaksService&) = delete;
    WaveformPeaksService& operator=(const WaveformPeaksService&) = delete;

    /// Queues the computation (a cache hit completes without decoding). `partials` (optional) receives snapshots on
    /// the worker while it decodes, all before `onDone`; after Job::cancel() at most one more may still arrive. The
    /// cache file is written from the final result only.
    Job<WaveformPeaks> request(const std::string& path, const WaveformOptions& options = {},
                               Callback onDone = {}, WaveformPartials partials = {});

    std::expected<WaveformPeaks, Error> getOrCompute(const std::string& path,
                                                     const WaveformOptions& options = {},
                                                     std::stop_token stop = {},
                                                     const std::function<void(float)>& progress = {},
                                                     const WaveformPartials& partials = {});

    std::filesystem::path cacheFileFor(const std::string& path, const WaveformOptions& options) const;
    const std::filesystem::path& cacheDir() const { return m_cacheDir; }

private:
    struct Workers;
    std::filesystem::path m_cacheDir;
    std::unique_ptr<Workers> m_workers;
};

} // namespace wabcut::media
