#pragma once

// Private (0.7): the bitstream side of H.264 / HEVC smart rendering (card 049). A long-GOP export can take the encoded
// pictures of a source as they are when whole closed GOPs of the source line up with the frames the export shows:
// the GOP starts at an IDR picture, depends on nothing before it, and the source's parameter sets (SPS / PPS / VPS) are
// exactly the ones the export's encoder writes, so the copied pictures decode under the file's own headers.
//
// This file reads what that needs from encoded data without decoding anything: the parameter sets of an avcC / hvcC /
// Annex B extradata, the NAL units of a packet, and the GOPs of a stretch of a stream (their packets' times in decode
// order). SmartRender.cpp plans with them; ExportJob.cpp writes the GOPs back out.

#include "FfmpegCommon.h"

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

/// A parameter set NAL unit (with its header byte(s)), emulation prevention bytes as written.
using NalUnit = std::vector<std::uint8_t>;

/// Whether smart rendering copies this codec's pictures as long GOPs.
bool isLongGopCodec(AVCodecID id);

/// The SPS / PPS (HEVC: VPS too) NAL units in `extradata`: an avcC / hvcC record or an Annex B stream (start codes).
/// Sorted and without repeats, so two streams' sets compare with ==. Empty when none could be read.
std::vector<NalUnit> parameterSets(AVCodecID codec, std::span<const std::uint8_t> extradata);

/// The size of the NAL length prefix in packets (1..4) from an avcC / hvcC record; 0 when `extradata` is not one (an
/// Annex B stream, or nothing): such a stream is not copied.
int nalLengthSize(AVCodecID codec, std::span<const std::uint8_t> extradata);

/// A length-prefixed packet (an avcC / hvcC stream: `lengthSize` bytes before each NAL unit) as an Annex B one, every NAL
/// unit behind the four-byte start code 00 00 00 01 and otherwise untouched. The encoders give the muxers Annex B headers
/// and the MP4, MOV and MKV muxers then treat every packet of the stream as Annex B and write the lengths themselves
/// (libavformat movenc.c, matroskaenc.c): a copied packet reaches them in that form. Empty for a malformed packet.
std::vector<std::uint8_t> toAnnexB(int lengthSize, std::span<const std::uint8_t> data);

/// What a length-prefixed packet holds.
struct PacketNals {
    bool idr = false;            ///< an IDR picture (H.264 type 5; HEVC IDR_W_RADL / IDR_N_LP)
    bool parameterSets = false;  ///< SPS / PPS / VPS inside the packet
    bool bad = false;            ///< a NAL length that runs past the packet
    std::vector<NalUnit> sets;   ///< the parameter sets found in it
};
PacketNals inspectPacket(AVCodecID codec, int lengthSize, std::span<const std::uint8_t> data);

/// Why two parameter-set lists differ, in words for the user: "the profile (High vs Main)", "the level (4.2 vs 5.1)",
/// else "the encoder settings (the headers differ)". `source` and `export` are lists from parameterSets().
std::string describeHeaderDifference(AVCodecID codec, const std::vector<NalUnit>& source, const std::vector<NalUnit>& exported);

/// One GOP found in a stream: an IDR picture and the pictures that follow it up to the next IDR, in decode order.
struct SourceGop {
    std::int64_t keyDts = 0;               ///< dts of the IDR packet (stream time base): the seek target
    std::int64_t bytes = 0;                ///< the packets' size together
    std::vector<std::int64_t> pts;         ///< every packet's pts, decode order
    std::vector<std::int64_t> dts;         ///< every packet's dts, decode order
    std::int64_t minPts = 0;               ///< first picture shown
    std::int64_t maxPts = 0;               ///< last picture shown
    bool contiguous = false;               ///< shown pictures are one frame period apart, none missing or repeated
    bool headersOk = true;                 ///< no packet changes the parameter sets
    bool complete = false;                 ///< ended by the next IDR (or the end of the stream), not by the scan's limit
    /// The number of frames: pts.size().
    std::size_t frames() const { return pts.size(); }
    /// How many frame periods the decode order trails the display order: the most any packet is shown before the picture
    /// that would have its place without reordering (0 for a stream without B-frames).
    std::int64_t reorderFrames(std::int64_t period) const;
};

struct GopScanInputs {
    AVFormatContext* ctx = nullptr;
    int stream = -1;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int lengthSize = 4;
    std::int64_t period = 0;              ///< ticks of the stream's time base per frame
    std::int64_t from = 0;                ///< pts of the first frame wanted
    std::int64_t to = 0;                  ///< pts of the last frame wanted
    const std::vector<NalUnit>* accepted = nullptr; ///< parameter sets a packet may carry (the export's)
};

/// The GOPs that cover [from, to] (pts of frames in the stream's time base): from the IDR at or before `from` through the
/// GOP holding `to`. A leading stretch before the first IDR is not returned. Reads the packets (no decoding): a few
/// seconds of video per GOP. Errors: IoError; Cancelled.
std::expected<std::vector<SourceGop>, Error> scanGops(const GopScanInputs& in, const std::stop_token& stop);

} // namespace wabcut::exporter::detail
