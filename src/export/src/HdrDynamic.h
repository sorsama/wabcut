#pragma once

// Dynamic HDR metadata of an export (internal): the HDR10+ plan built from the measured frames, its serialisation, and the
// packet filter that puts SEI / RPU NAL units into the encoder's HEVC output (see include/wabcut/export/DynamicMetadata.h).

#include <wabcut/export/DynamicMetadata.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/ProjectState.h>

extern "C" {
#include <libavcodec/packet.h>
}

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::exporter::detail {

/// The HDR10+ plan: one message per output frame and the report of the shots.
struct Hdr10PlusPlan {
    /// Per output frame: the ITU-T T.35 payload of the SEI message (country code .. application data, no SEI header).
    std::vector<std::vector<std::uint8_t>> messages;
    DynamicMetadataReport report;
};

/// The HDR10+ metadata of every frame, shot by shot: per shot the maxscl of the whole shot (so the display's mapping does not
/// pump inside a shot); per frame its own mean and brightness distribution. Tone mapping curves are not sent
/// (tone_mapping_flag 0: the display maps by the distribution, as Samsung's HDR10+ players do for content without a curve).
/// `shotStarts`: sorted first frames of the shots (0 is added when missing). `targetedDisplayNits`: the reference display the
/// metadata is for (HDR10+ content is conventionally 400 nits). Errors: InvalidArgument (no frames).
std::expected<Hdr10PlusPlan, Error> planHdr10Plus(const std::vector<FrameLightStats>& stats,
                                                  const std::vector<std::int64_t>& shotStarts,
                                                  double targetedDisplayNits = 400.0);

/// The light statistics (FrameLightStats) of a picture read back as RGB10A2 words (R in bits 0-9, G 10-19, B 20-29: PQ code
/// values of the export's Rec.2100 PQ output): the brightest red, green and blue, the mean of the per-pixel maximum of R, G, B
/// and its distribution at kHdr10PlusPercentages, all in nits (SMPTE ST 2084). `count` is the number of pixels.
FrameLightStats measureLightRgb10(const std::uint32_t* words, std::size_t count);

/// The output frames where the sequence's video edit points fall inside `range` (a clip start or end, a transition's edges),
/// as frame numbers of an export of `frameCount` frames from range.start at `rate`; sorted, always starting with 0.
std::vector<std::int64_t> shotStartsOf(const ProjectState& state, SequenceId sequence, TimeRange range, Rate rate,
                                       std::int64_t frameCount);

/// A complete SEI NAL unit in Annex B form (start code, the two header bytes of nal_unit_type 39, one sei_message of
/// payload type 4 (user_data_registered_itu_t_t35) carrying `t35`, the trailing bits, with emulation prevention).
std::vector<std::uint8_t> hevcPrefixSeiNal(std::span<const std::uint8_t> t35);

/// An Annex B NAL unit (start code added, emulation prevention applied) of `type` from `rbsp` (the bytes after the two NAL
/// header bytes, which this adds: `type`, layer 0, temporal id 0).
std::vector<std::uint8_t> hevcNalUnit(int type, std::span<const std::uint8_t> rbsp);

/// Escapes 00 00 0x (x <= 3) in `rbsp` with an emulation prevention byte.
std::vector<std::uint8_t> addEmulationPrevention(std::span<const std::uint8_t> rbsp);

/// Puts `before` NAL units (Annex B, start codes included) in front of the first VCL NAL unit of the access unit in `packet` and
/// `after` behind its last one. The packet is an Annex B HEVC access unit as the encoders write it (start codes). Returns false
/// when it is not one (no start code, no VCL unit): the packet is then left as it is.
bool insertNalUnits(AVPacket* packet, std::span<const std::vector<std::uint8_t>> before,
                    std::span<const std::vector<std::uint8_t>> after);

/// Per output frame: the NAL units to insert. Built from an HDR10+ plan, or from a registered Dolby Vision writer.
class NalInjector {
public:
    virtual ~NalInjector() = default;
    /// Units for access unit `frameIndex` (the packet's pts in frame periods).
    virtual std::expected<DolbyVisionWriter::Units, Error> unitsFor(std::int64_t frameIndex) = 0;
};

std::unique_ptr<NalInjector> makeHdr10PlusInjector(std::shared_ptr<const Hdr10PlusPlan> plan);

} // namespace wabcut::exporter::detail
