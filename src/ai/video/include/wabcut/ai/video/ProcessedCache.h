#pragma once

// The per-clip cache of processed pictures (0.7, AI video effects): one file per source frame under
// <cache>/ai-video/<media>-s<stream>-<chain>/, so the pictures a chain made for a stream of a media file are shared by
// every clip of it with the same effects, survive a restart and are found again by the frame's source time. A frame is
// stored as the planes the compositor will read (YUV 4:4:4 at 10 bits, with alpha when the media has it, or RGBA8),
// byte-shuffled and deflated; writes are atomic (a temporary file renamed over the name), so a crash or a cancel never
// leaves a half picture. Decoded pictures are kept in a small LRU so scrubbing does not inflate the same file again.
//
// Pure C++: it stores bytes and metadata (StoredFrame); wabcut/ai/video/MediaFrames.h turns them into media frames.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai::video {

/// Which pictures: a media file's stream, processed by a chain with a model version.
struct CacheIdentity {
    std::uint64_t media = 0;     ///< the file's fingerprint, hashed (size + sample hash)
    int stream = 0;              ///< index in MediaInfo::video
    std::uint64_t chain = 0;     ///< aiChainHash()
    std::string modelVersion;    ///< modelVersion(): digests of the weights and the pipeline version

    bool valid() const { return media != 0 && chain != 0 && !modelVersion.empty(); }
    /// The folder name: file-name safe, 16 + 16 hex digits and the stream.
    std::string directoryName() const;
    bool operator==(const CacheIdentity&) const = default;
};

/// One processed picture as stored: the frame's timing and colour description (the media enums' values) and its planes,
/// each tightly packed.
struct StoredPlane {
    int width = 0;  ///< texels
    int height = 0;
    int bytesPerTexel = 0;
    std::vector<std::uint8_t> data; ///< width x height x bytesPerTexel
};
struct StoredFrame {
    int width = 0;  ///< display size in pixels
    int height = 0;
    std::int64_t pts = 0;      ///< source time (ticks) the frame starts at
    std::int64_t duration = 0;
    std::int64_t streamPts = 0;
    std::int32_t timeBaseNum = 1;
    std::int32_t timeBaseDen = 1;
    std::int32_t sampleNum = 1;
    std::int32_t sampleDen = 1;
    std::uint8_t format = 0;   ///< media::PixelFormat
    std::uint8_t matrix = 0;   ///< media::ColorMatrix, ColorRange, ColorPrimaries, TransferFunction, ChromaLocation
    std::uint8_t range = 0;
    std::uint8_t primaries = 0;
    std::uint8_t transfer = 0;
    std::uint8_t chroma = 0;
    std::uint8_t alpha = 0;    ///< media::AlphaMode
    std::vector<StoredPlane> planes;

    std::size_t bytes() const;
    bool valid() const;
};

/// Where a cache came from, for people and for "Render and replace" later: written next to the pictures when a run starts.
struct Provenance {
    std::string chain;         ///< "Restore 2x, Denoise"
    std::string settings;      ///< the stages' settings in words
    std::string modelVersion;
    std::string models;        ///< "Real-ESRGAN x2plus (BSD-3-Clause), ..." - names and licences
    std::string created;       ///< ISO 8601 UTC
    std::string app;           ///< "Wabcut 0.7.0"
};

class ProcessedCache {
public:
    /// `memoryBudget`: bytes of decoded pictures kept in memory.
    explicit ProcessedCache(std::filesystem::path root, std::size_t memoryBudget = std::size_t{192} << 20);
    /// <user cache>/ai-video
    static std::filesystem::path defaultRoot();

    const std::filesystem::path& root() const { return m_root; }
    std::filesystem::path directoryOf(const CacheIdentity& id) const;

    /// Stores a picture (atomically). Errors: InvalidArgument (not valid), IoError.
    std::expected<void, Error> put(const CacheIdentity& id, const StoredFrame& frame);
    /// The picture whose start is nearest to `pts` within `tolerance` ticks (a frame's own start, half a frame for a
    /// variable rate); null when there is none or the file is damaged (it is then removed). Thread-safe, cheap on a hit.
    std::shared_ptr<const StoredFrame> get(const CacheIdentity& id, Ticks pts, Ticks tolerance);
    /// A picture exists for `pts` within `tolerance`.
    bool contains(const CacheIdentity& id, Ticks pts, Ticks tolerance);
    /// The start times of the stored pictures, ascending.
    std::vector<Ticks> frames(const CacheIdentity& id);
    /// How many pictures start in [from, to).
    std::size_t countIn(const CacheIdentity& id, Ticks from, Ticks to);
    /// How many of `wanted` (ascending start times, each matched within `tolerance`) are stored.
    std::size_t countCovered(const CacheIdentity& id, const std::vector<Ticks>& wanted, Ticks tolerance);

    /// Bytes on disk of one cache, and of all of them.
    std::uint64_t diskBytes(const CacheIdentity& id);
    std::uint64_t totalDiskBytes();
    /// Deletes one cache / all of them (and forgets the decoded pictures).
    void remove(const CacheIdentity& id);
    void clear();
    /// Reads the folder again on the next lookup (another process may have written to it).
    void rescan(const CacheIdentity& id);

    /// Changes on every put, remove and clear: pollers (the badge, the Process button) know when to look again.
    std::uint64_t revision() const;

    std::expected<void, Error> writeProvenance(const CacheIdentity& id, const Provenance& provenance);
    std::optional<Provenance> provenance(const CacheIdentity& id) const;

    /// Measured speed of a model on a device, ms per input megapixel (the runner records it; estimates read it). 0 = none.
    void recordSpeed(const std::string& model, const std::string& device, double msPerMegapixel);
    double speed(const std::string& model, const std::string& device) const;

private:
    struct Directory {
        bool scanned = false;
        std::map<std::int64_t, std::uint64_t> files; ///< pts -> file size
        std::uint64_t bytes = 0;
    };
    Directory& directory(const CacheIdentity& id);
    std::filesystem::path fileFor(const CacheIdentity& id, std::int64_t pts) const;
    std::shared_ptr<const StoredFrame> load(const CacheIdentity& id, std::int64_t pts);
    void remember(const std::string& key, std::shared_ptr<const StoredFrame> frame);

    std::filesystem::path m_root;
    std::size_t m_memoryBudget;
    mutable std::mutex m_mutex;
    std::map<std::string, Directory> m_directories;
    struct Decoded {
        std::string key;
        std::shared_ptr<const StoredFrame> frame;
    };
    std::vector<Decoded> m_recent; ///< most recent first
    std::size_t m_recentBytes = 0;
    std::uint64_t m_revision = 1;
};

/// Serialises a frame as a cache file's bytes (header, byte-shuffled planes, deflate) and back. Exposed for the tests.
std::expected<std::vector<std::uint8_t>, Error> encodeFrame(const StoredFrame& frame);
std::expected<StoredFrame, Error> decodeFrame(const std::vector<std::uint8_t>& bytes);

} // namespace wabcut::ai::video
