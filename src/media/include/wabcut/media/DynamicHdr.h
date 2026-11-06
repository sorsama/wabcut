#pragma once

// Dynamic HDR metadata in a video file (0.7, card "Dolby Vision 8.4 and HDR10+ dynamic metadata"): what a file carries
// besides the static HDR10 values.
//
// * Dolby Vision: the decoder configuration record the container declares ('dvcC' / 'dvvC' in MP4 and MOV, Matroska's
//   BlockAdditionMapping), read from the stream's coded side data (libavutil/dovi_meta.h): profile, level, base layer
//   compatibility (8.1 HDR10, 8.2 SDR, 8.4 HLG), whether RPUs are present. media::probe stores it on the stream.
//   The RPUs themselves (the per-frame metadata NAL units) are parsed by FFmpeg's HEVC decoder when the decoder is asked to
//   export "enhancements": scanDynamicHdr() decodes the first frames and reads their level 1 values (min / max / average
//   PQ -> nits).
// * HDR10+: SMPTE ST 2094-40 in a user data registered ITU-T T.35 SEI message of every HEVC access unit; FFmpeg's decoder
//   parses it into AV_FRAME_DATA_DYNAMIC_HDR_PLUS (libavutil/hdr_dynamic_metadata.h). scanDynamicHdr() decodes the first
//   frames and summarises them (application version, the largest maxscl, the mean of average_maxrgb, the targeted display,
//   whether a tone mapping curve is sent).
//
// scanDynamicHdr() opens the file and decodes at most `maxFrames` frames of one video stream: call it off the GUI thread.
// The probe runs it with a handful of frames for HEVC streams that are PQ (the only ones that can carry HDR10+).

#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>

#include <expected>
#include <optional>
#include <stop_token>
#include <string>

namespace wabcut::media {

struct DynamicHdrReport {
    int framesScanned = 0;

    // ---- HDR10+ (ST 2094-40) ----
    int hdr10PlusFrames = 0;               ///< frames of the sample that carry it
    int hdr10PlusApplicationVersion = -1;  ///< of the first such frame (1 for HDR10+), -1 when none
    double hdr10PlusPeakNits = 0.0;        ///< the largest maxscl component of the sample, in nits (x 10000)
    double hdr10PlusMeanAverageNits = 0.0; ///< the mean of average_maxrgb over those frames, in nits
    double targetedDisplayNits = 0.0;      ///< targeted_system_display_maximum_luminance of the first such frame
    bool hdr10PlusToneCurve = false;       ///< some frame sends a Bezier tone mapping curve (tone_mapping_flag)

    // ---- Dolby Vision ----
    std::optional<DolbyVisionInfo> dolbyVision; ///< the container's configuration
    int rpuFrames = 0;                          ///< frames whose RPU the decoder parsed
    int rpuProfile = -1;                        ///< vdr_rpu_profile of the first RPU (1 for profile 8), -1 when none
    double rpuMinNits = 0.0;                    ///< level 1 over the sample: the lowest min_pq, the highest max_pq, the mean avg_pq
    double rpuMaxNits = 0.0;
    double rpuMeanNits = 0.0;
    bool rpuLevel1 = false;                     ///< the RPUs carry level 1 (per-frame brightness) blocks
    int rpuLevel2Blocks = 0;                    ///< trim blocks (level 2 and 8) of the first RPU: the target displays it has trims for

    bool hasHdr10Plus() const { return hdr10PlusFrames > 0; }
    bool hasDolbyVision() const { return dolbyVision.has_value() || rpuFrames > 0; }
};

/// SMPTE ST 2084 (PQ) code value of `bits` bits (12 for Dolby Vision's PQ fields) to nits.
double pqToNits(unsigned code, int bits);
/// The inverse: nits to the PQ code value of `bits` bits.
unsigned nitsToPq(double nits, int bits);

/// The Dolby Vision configuration the container of video stream `streamIndex` (-1: the first) declares, without decoding.
/// nullopt when the stream has none.
std::expected<std::optional<DolbyVisionInfo>, Error> readDolbyVisionConfig(const std::string& utf8Path, int streamIndex = -1);

/// Decodes up to `maxFrames` frames of video stream `streamIndex` (-1: the first) and reports the dynamic metadata they
/// carry. Errors: NotFound / Unsupported (the file or stream), Cancelled (`stop`). A stream without any is a report with zero
/// frames of each kind, not an error.
std::expected<DynamicHdrReport, Error> scanDynamicHdr(const std::string& utf8Path, int streamIndex = -1, int maxFrames = 24,
                                                      std::stop_token stop = {});

} // namespace wabcut::media
