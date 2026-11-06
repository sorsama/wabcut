#pragma once

// Dynamic HDR metadata of an export (0.7, card "Dolby Vision 8.4 and HDR10+ dynamic metadata").
//
// What this build can do, checked against its FFmpeg 9.0 and x265 4.3 (docs/color.md "Dynamic metadata"):
//   * HDR10+ (SMPTE ST 2094-40): DONE here. The frames are measured while they are rendered (VideoSourceFormat::frameStats),
//     a first pass over the range at half size; the metadata is shot-by-shot (the sequence's video edit points are the
//     cuts) and written as a user data registered ITU-T T.35 SEI message into every access unit of a PQ HEVC video by
//     detail::Hdr10PlusInjector, through the muxer's packet filter. x265 4.3 as bundled has no HDR10+ support of its own
//     (its ENABLE_HDR10_PLUS option is off) and FFmpeg's libx265 does not turn AV_FRAME_DATA_DYNAMIC_HDR_PLUS into SEI,
//     so the message is put into the encoder's output packets; the serialisation is FFmpeg's av_dynamic_hdr_plus_to_t35.
//   * Dolby Vision 8.1 / 8.4: NOT written by this build. Dolby licenses the format (RPU generation, the configuration box
//     and the profile 8 stream requirements are certified against Dolby's test suite), so the writer sits behind this
//     interface and the registry below: an export that asks for Dolby Vision without a licensed writer is refused with
//     unavailableReason(); HDR10 and HLG exports are not affected. FFmpeg 9.0's libx265 can write RPUs from
//     AV_FRAME_DATA_DOVI_METADATA (libavcodec/dovi_rpu.c) and x265 takes them, so a licensed build only needs to supply the
//     per-frame AVDOVIMetadata (level 1 from the analysis below, level 6 from the mastering values, trims from the
//     colourist) and the configuration record; libdovi (quietvoid/dovi_tool, MIT) is not in vcpkg.json and not needed for
//     reading: media::scanDynamicHdr parses RPUs with FFmpeg.
//   * Reading and keeping: media::probe stores the container's Dolby Vision configuration and whether the first frames carry
//     HDR10+; Get Properties shows both; Pre-export Check warns when an export drops what a clip carries.

#include <wabcut/export/ExportSettings.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::exporter {

/// One shot of the programme as the analysis saw it (frames are output frames of the export).
struct ShotReport {
    std::int64_t firstFrame = 0;
    std::int64_t frameCount = 0;
    double peakNits = 0.0;    ///< the brightest component of any pixel of the shot
    double averageNits = 0.0; ///< the mean of the frames' mean of max(R, G, B)
};

/// What an export wrote as dynamic metadata, for the result and the report in the export queue.
struct DynamicMetadataReport {
    DynamicMetadata kind = DynamicMetadata::None;
    bool written = false;        ///< the metadata is in the file
    std::int64_t frames = 0;     ///< frames that got a message
    double peakNits = 0.0;       ///< over the whole programme
    double averageNits = 0.0;
    double targetedDisplayNits = 0.0;
    std::vector<ShotReport> shots;
    std::string note;            ///< why nothing was written, or a caveat
};

/// The Dolby Vision metadata writer a licensed build registers. Everything Dolby-specific lives behind it.
class DolbyVisionWriter {
public:
    virtual ~DolbyVisionWriter() = default;
    /// "Dolby Vision 8.4 (licensed writer by ...)".
    virtual std::string name() const = 0;
    /// Whether `kind` (DolbyVision81 or DolbyVision84) can be written; otherwise why not.
    virtual std::expected<void, Error> supports(DynamicMetadata kind) const = 0;
    /// The data to put with access unit `frameIndex` of the output: NAL units (with their two-byte header, without start
    /// code) to place before the first slice (`before`) and after the last slice (`after`; the RPU of profile 8 goes there).
    struct Units {
        std::vector<std::vector<std::uint8_t>> before;
        std::vector<std::vector<std::uint8_t>> after;
    };
    /// Called once, before the first frame, with the analysis of the whole range (`stats`, one per output frame, 10-bit PQ
    /// luminance) and the shot starts. Returns the container configuration record (the 'dvcC' / 'dvvC' box payload).
    virtual std::expected<std::vector<std::uint8_t>, Error> begin(DynamicMetadata kind, const std::vector<FrameLightStats>& stats,
                                                                  const std::vector<std::int64_t>& shotStarts) = 0;
    virtual std::expected<Units, Error> unitsFor(std::int64_t frameIndex) = 0;
};

/// Where a build with a licence plugs its writer in. A build without one has none: Dolby Vision is unavailable and says why.
class DolbyVisionRegistry {
public:
    static void set(std::shared_ptr<DolbyVisionWriter> writer);
    static std::shared_ptr<DolbyVisionWriter> get();
    /// "" when a writer is registered and supports `kind`; otherwise the sentence the export dialog and the error show.
    static std::string unavailableReason(DynamicMetadata kind);
};

} // namespace wabcut::exporter
