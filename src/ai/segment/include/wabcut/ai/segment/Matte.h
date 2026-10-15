#pragma once

// AI object mattes (AI-14; docs/ai.md "AI Object Mask", docs/masks.md "Object masks"): what a segmentation model made
// of one object of one media stream, frame by frame, and how the renderer gets it back.
//
//   MatteImage   one frame's 8-bit alpha at the track's matte size (the stored frame, its display aspect, 512 px on the
//                long side by default), run-length encoded (value, length) for storage: an object's matte is mostly
//                runs of 0 and 255 with a soft edge, so a 512 x 288 frame takes a few kilobytes.
//   MatteTrack   the frames (media time, confidence, present, bounding box, RLE), the prompts that made them, the
//                media's fingerprint and stream, the model. Stored in the analysis cache (AnalysisCache, JSON with the
//                RLE as base64) under analyzer "matte", keyed by the media's fingerprint, the stream, the model version
//                and a hash of the prompts and the span: its identity, which a matte mask (MaskMatte::track) names.
//   signedDistance  a matte as the renderer's distance field (exact Euclidean distance transform of the binarised
//                matte, in frame heights); CacheMatteSource (MatteCache.h, full builds) serves them to the compositor.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::segment {

inline constexpr int kDefaultMatteSide = 512;   ///< the long side of a matte
inline constexpr int kMaxMatteSide = 2048;
inline constexpr std::size_t kMaxMatteFrames = 216'000; ///< two hours at 30 fps
inline constexpr std::size_t kMaxPrompts = 64;
inline constexpr std::size_t kMaxPromptPoints = 32;      ///< per prompt
inline constexpr std::string_view kMatteAnalyzer = "matte";

struct MatteImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha; ///< row-major, top row first

    bool valid() const { return width > 0 && height > 0 && alpha.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
};

/// Run-length encoding: pairs of (value byte, run length as an unsigned LEB128 varint).
std::vector<std::uint8_t> encodeRle(const MatteImage& image);
/// Errors: ParseError (truncated, too long or too short for width x height).
std::expected<MatteImage, Error> decodeRle(std::span<const std::uint8_t> rle, int width, int height);

std::string toBase64(std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> fromBase64(std::string_view text);

/// A box in normalised stored-frame coordinates.
struct MatteBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const MatteBox&) const = default;
};
/// The bounding box of the matte's pixels >= `threshold`; nullopt when it has none.
std::optional<MatteBox> boxOf(const MatteImage& image, std::uint8_t threshold = 128);
/// Intersection over union of the two mattes' pixels >= 128 (same size); 1 when both are empty.
double iouOf(const MatteImage& a, const MatteImage& b);
/// Bilinear resampling to `width` x `height`.
MatteImage resized(const MatteImage& image, int width, int height);
/// The matte size for a frame of display aspect `aspect` (width / height): `longSide` on its longer side.
std::pair<int, int> matteSize(double aspect, int longSide = kDefaultMatteSide);

/// The signed distance (frame heights, + inside) of every texel centre to the matte's edge (alpha 128), for a frame of
/// display aspect `aspect`: render::MatteField::distance. An empty matte is far outside everywhere.
std::vector<float> signedDistance(const MatteImage& image, double aspect);

// ---- Prompts and tracks -----------------------------------------------------------------------------------------

struct PromptPoint {
    double x = 0.0; ///< normalised stored-frame coordinates
    double y = 0.0;
    bool positive = true;

    bool operator==(const PromptPoint&) const = default;
};

/// What the user pointed at on one frame: clicks and / or a box.
struct Prompt {
    Ticks time{}; ///< media time
    std::vector<PromptPoint> points;
    std::optional<MatteBox> box;

    bool operator==(const Prompt&) const = default;
};

struct MatteFrame {
    Ticks time{};            ///< media time of the frame
    float confidence = 0.0f; ///< 0..1: the model's predicted IoU, times its certainty that the object is there
    bool present = false;    ///< the model sees the object (else the matte is empty)
    std::optional<MatteBox> box;
    std::vector<std::uint8_t> rle;
};

struct MatteTrack {
    std::uint64_t fingerprint = 0; ///< the media's (media::sourceFingerprint)
    int stream = 0;                ///< video stream ordinal
    std::string model;             ///< "sam2.1-tiny-video-aa7a8542" (file-name safe)
    std::string params;            ///< 16 hex digits: the hash of the prompts and the span
    int width = 0;                 ///< matte size
    int height = 0;
    double aspect = 16.0 / 9.0;    ///< the frame's display aspect
    Ticks frameDuration{};         ///< the media's frame interval
    std::vector<Prompt> prompts;
    std::vector<MatteFrame> frames; ///< by time
    std::vector<Ticks> cuts;        ///< shot cuts where the propagation stopped (media time)

    /// "<fingerprint, 16 hex>/<stream>/<model>/<params>": what MaskMatte::track holds.
    std::string identity() const;
    /// The frame at `time` (within `tolerance`), else null.
    const MatteFrame* frameAt(Ticks time, Ticks tolerance) const;
    std::expected<MatteImage, Error> imageOf(const MatteFrame& frame) const;
    /// The media span the frames cover (first frame to the end of the last).
    TimeRange span() const;
    /// Merges `other`'s frames (same media and size) over this track's: a frame at the same time is replaced.
    void merge(const MatteTrack& other);
};

/// A track identity's parts. Errors: ParseError.
struct MatteTrackId {
    std::uint64_t fingerprint = 0;
    int stream = 0;
    std::string model;
    std::string params;
};
std::expected<MatteTrackId, Error> parseTrackId(std::string_view identity);

/// The hash of the prompts, the span and how the run went (`run`: direction, across shots, the track it refines):
/// `MatteTrack::params`.
std::string promptsHash(std::span<const Prompt> prompts, TimeRange span, int width, int height, std::string_view run = {});

nlohmann::json trackToJson(const MatteTrack& track);
/// Bounded (cache files are not trusted further than the project). Errors: ParseError.
std::expected<MatteTrack, Error> trackFromJson(const nlohmann::json& json);

/// Matte tracks in the analysis cache.
class MatteStore {
public:
    explicit MatteStore(std::filesystem::path cacheRoot);
    std::expected<void, Error> put(const MatteTrack& track) const;
    /// Nullopt when this computer has no such track (never made here, or the cache was cleared).
    std::expected<std::optional<MatteTrack>, Error> get(std::string_view identity) const;
    const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_root;
};

} // namespace wabcut::ai::segment
