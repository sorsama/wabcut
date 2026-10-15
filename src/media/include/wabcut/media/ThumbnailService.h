#pragma once

// Filmstrip thumbnails: N evenly spaced frames, scaled to a target height as RGBA8, cached on disk.

#include <wabcut/media/Job.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::media {

struct ThumbnailOptions {
    int count = 10;  ///< Number of thumbnails, 1..1000.
    int height = 90; ///< Output height in pixels, 8..2160; width follows the display aspect ratio.
    int stream = -1; ///< Container video stream index, -1 = best.
    /// Thumbnails default to software decoding to leave the GPU decoder to playback.
    bool allowHardware = false;
};

/// One RGBA8 thumbnail, rows tightly packed (stride = width * 4), display rotation already applied.
struct Thumbnail {
    Ticks time{}; ///< Source time of the frame shown (its pts).
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct ThumbnailStrip {
    std::vector<Thumbnail> thumbnails; ///< In time order. Thumbnail i shows the frame containing the centre
                                       ///< of the i-th of `count` equal slices of the stream's duration.
    bool fromCache = false;            ///< True when loaded from the disk cache instead of decoded.
};

/// Decodes the strip synchronously (no cache). Deterministic: the same file and options give identical
/// pixels. Checks `stop` between thumbnails (ErrorCode::Cancelled). `progress` (optional) receives values in
/// [0, 1].
std::expected<ThumbnailStrip, Error> extractThumbnails(const std::string& path,
                                                       const ThumbnailOptions& options,
                                                       std::stop_token stop = {},
                                                       const std::function<void(float)>& progress = {});

/// Background thumbnail extraction with a disk cache.
///
/// Cache entries live in `<cacheDir>/thumbnails/` and are keyed by the source fingerprint
/// (sourceFingerprint(): size, modification time, first/last 64 KiB) plus the options. A moved or renamed
/// file keeps its cache; an edited, replaced or touched file is re-extracted. Files are written atomically
/// (temp file + rename), so several services or processes may share one cache directory. Unreadable or stale
/// entries are ignored and replaced.
///
/// Threading: all member functions are thread-safe. Jobs run on the service's own worker threads; the
/// completion callback runs on a worker thread. The destructor cancels queued and running jobs and joins the
/// workers.
class ThumbnailService {
public:
    using Callback = std::function<void(const std::expected<ThumbnailStrip, Error>&)>;

    explicit ThumbnailService(std::filesystem::path cacheDir, int workerThreads = 1);
    ~ThumbnailService();
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;

    /// Queues extraction (or a cache load) of `path` and returns immediately.
    Job<ThumbnailStrip> request(const std::string& path, const ThumbnailOptions& options = {},
                                Callback onDone = {});

    /// Synchronous variant of request(): returns the cached strip or extracts and caches it on the calling
    /// thread.
    std::expected<ThumbnailStrip, Error> getOrExtract(const std::string& path,
                                                      const ThumbnailOptions& options = {},
                                                      std::stop_token stop = {},
                                                      const std::function<void(float)>& progress = {});

    /// Cache file that holds (or would hold) the strip; empty if the source file cannot be stat'ed.
    std::filesystem::path cacheFileFor(const std::string& path, const ThumbnailOptions& options) const;
    const std::filesystem::path& cacheDir() const { return m_cacheDir; }

private:
    struct Workers;
    std::filesystem::path m_cacheDir;
    std::unique_ptr<Workers> m_workers;
};

} // namespace wabcut::media
