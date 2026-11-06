#pragma once

// Private (0.2): smart rendering. Frames an export can take as they are, without rendering and encoding them.
//
//   * Intra-only exports (ProRes, DNxHR / DNxHD): Use Previews (rendered preview segments that still show what the
//     sequence shows, when the export's format is the previews') and Smart Render (a single source clip shown untouched:
//     full frame, no Motion, Opacity, effects, transitions, redactions or colour conversion, its own size and rate).
//     Their packets are copied from the preview file or the source file into the export, which must then encode with no
//     frame delay (slice threads), so copied and encoded packets interleave in order.
//
//   * 0.7 (card 049), Long-GOP exports (H.264 and HEVC, MP4 / MOV / MKV): whole closed GOPs of a source clip that is
//     shown untouched and one frame after the other are copied, packet by packet, without decoding; everything else -
//     the pictures up to the next IDR after a cut, effects, transitions, titles, retimed clips, a source whose headers
//     are not the export's - is encoded again, in segments of its own. A GOP is copied only when it starts at an IDR
//     picture, no later picture of the stream's previous GOP needs it and it needs nothing from before it, its pictures
//     are one frame period apart, and the source's parameter sets (SPS / PPS / VPS) are exactly the ones the export's
//     encoder writes into the file (so the copied pictures decode under the file's own headers: LongGop.h). Each
//     re-encoded segment is made by an encoder of its own (opened with the same settings, so it writes the same headers),
//     which starts with an IDR and is drained at the segment's end, so every GOP of the file is closed. Packet times are
//     set so decode order stays monotonic across the joins (dts = first frame + decode index - delay).
//
// The plan only copies what it can prove: the packet's codec parameters equal the export encoder's, and the file's
// packets are what the plan read. Everything else renders. It also says why (notes()).

#include "Encode.h"
#include "LongGop.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

/// Where a copied frame comes from.
struct CopyFrame {
    int file = -1;              ///< index into SmartRenderPlan::files(); -1: the frame renders
    int stream = 0;             ///< the container stream
    std::int64_t timestamp = 0; ///< the packet's pts, in its stream's time base
    bool preview = false;       ///< from a preview file (Use Previews), else a source file (Smart Render)
    int gop = -1;               ///< 0.7: >= 0: the frame belongs to copied GOP `gop` (SmartRenderPlan::gops())
};

/// 0.7 (card 049): a whole closed GOP of a source file, copied packet by packet.
struct CopyGop {
    int file = -1;
    int stream = 0;
    int lengthSize = 4;            ///< the NAL length prefix of the source's packets
    bool annexB = true;            ///< the packets go out as Annex B (the export's stream has Annex B headers: the muxer
                                   ///< converts every packet it is given), else as they are
    std::int64_t keyDts = 0;       ///< the IDR packet's dts: where to seek
    std::vector<std::int64_t> pts; ///< every packet's pts, decode order (the stream's time base)
    std::int64_t minPts = 0;       ///< the first picture shown
    std::int64_t period = 1;       ///< stream ticks per frame
    std::int64_t firstFrame = 0;   ///< the output frame of the first picture shown
    std::int64_t reorder = 0;      ///< frames the decode order trails the display order
    std::size_t frames() const { return pts.size(); }
};

/// Whether an export can smart render at all: ProRes or DNxHR / DNxHD (intra), or H.264 / HEVC in MP4 or MOV
/// (long GOP, 0.7); no picture effects, burned-in or embedded captions or alpha, and Smart Render (intra: or Use
/// Previews) on.
bool smartRenderEligible(const ExportSettings& settings);
/// The intra-only kind: the encoder runs with no frame delay.
bool smartRenderIntra(const ExportSettings& settings);
/// The long-GOP kind (H.264 / HEVC).
bool smartRenderLongGop(const ExportSettings& settings);

class SmartRenderPlan {
public:
    struct Inputs {
        const ProjectState* project = nullptr;  ///< locators at the original files (what renders)
        const ProjectState* snapshot = nullptr; ///< the project as the preview cache hashes it (Use Previews)
        const Sequence* sequence = nullptr;
        const ResolvedExport* resolved = nullptr;
        const ExportSettings* settings = nullptr;
        const AVCodecParameters* encoder = nullptr; ///< the opened export encoder's parameters
        PreviewCache* previews = nullptr;           ///< Use Previews (may be null)
        std::filesystem::path projectFile;
    };

    /// Decides frame by frame (cheap for intra codecs: evaluation and file indexes, nothing is decoded; long GOP reads the
    /// packets of the clips it may copy, a few seconds of video per GOP). Never fails: what it cannot prove renders.
    static SmartRenderPlan build(const Inputs& inputs, const std::stop_token& stop);

    std::int64_t copiedFrames() const { return m_copied; }
    std::int64_t previewFrames() const { return m_previewCopied; }
    /// The output frames that render, in order.
    std::vector<std::int64_t> renderedFrames() const;
    /// The copy of output frame k, or null when it renders.
    const CopyFrame* copyFor(std::int64_t k) const;
    const std::vector<std::filesystem::path>& files() const { return m_files; }

    /// 0.7: long-GOP smart rendering is planned (even if no GOP could be copied).
    bool longGop() const { return m_longGop; }
    const std::vector<CopyGop>& gops() const { return m_gops; }
    /// The most any copied GOP's decode order trails its display order, in frames.
    std::int64_t maxReorder() const { return m_maxReorder; }
    /// Why frames render, most frames first: the notes of the export details (the timecodes are filled in by the job).
    const std::vector<SmartRenderNote>& notes() const { return m_notes; }

private:
    std::vector<CopyFrame> m_frames; ///< one per output frame
    std::vector<std::filesystem::path> m_files;
    std::vector<CopyGop> m_gops;
    std::vector<SmartRenderNote> m_notes;
    std::int64_t m_copied = 0;
    std::int64_t m_previewCopied = 0;
    std::int64_t m_maxReorder = 0;
    bool m_longGop = false;

    /// 0.7: the long-GOP kind of build().
    static void buildLongGop(const Inputs& inputs, const std::stop_token& stop, SmartRenderPlan& plan);
};

/// Reads the planned packets (each file opened once, read forward; a jump seeks).
class PacketCopier {
public:
    explicit PacketCopier(const SmartRenderPlan& plan);
    ~PacketCopier();
    PacketCopier(const PacketCopier&) = delete;
    PacketCopier& operator=(const PacketCopier&) = delete;

    /// The packet of `copy`, stamped as output frame `k` (pts = dts = k, duration 1, key frame) in `timeBase`.
    /// Errors: IoError, DecoderError (the packet is not in the file).
    std::expected<PacketPtr, Error> packet(const CopyFrame& copy, std::int64_t k);

    /// 0.7: writes GOP `gop` of the plan to `muxer` (stream `stream`, packets in `timeBase` = the encoder's, one tick per
    /// frame): the packets as the source has them, in decode order, with pts = first frame + (pts - minPts) / period and
    /// dts = first frame + decode index - `delay`. Errors: IoError, DecoderError (the file no longer holds what the plan
    /// read), Cancelled.
    std::expected<void, Error> copyGop(const CopyGop& gop, Muxer& muxer, AVStream* stream, AVRational timeBase,
                                       std::int64_t delay, const std::stop_token& stop);

private:
    struct Input;
    Input& input(int file, std::string& path, std::expected<void, Error>& status);
    const SmartRenderPlan& m_plan;
    std::vector<std::unique_ptr<Input>> m_inputs;
};

/// 0.7: the numbering of one re-encoded segment's packets. The packet that comes out i-th gets dts = start + i - delay
/// (its pts is the frame number it was fed with), so the dts of the whole file counts up by one per frame across copied
/// GOPs and segments alike.
struct SegmentStamp {
    std::int64_t start = 0; ///< the output frame the segment began at
    std::int64_t index = 0; ///< packets written so far
    std::int64_t delay = 0; ///< frames of reordering delay of the whole file
};

/// drainEncoder with the segment's numbering. Errors: EncoderError (a picture shown before it is decoded: the delay is too
/// small for this encoder), IoError.
std::expected<void, Error> drainSegment(AVCodecContext* encoder, AVPacket* packet, Muxer& muxer, AVStream* stream,
                                        bool flushing, SegmentStamp& stamp);

/// The frames of reordering delay an encoder needs between decode and display order (B-frames and their pyramid), with a
/// frame to spare: dts = pts - this for the first picture.
std::int64_t encoderReorderDelay(const AVCodecContext* encoder);

} // namespace wabcut::exporter::detail
