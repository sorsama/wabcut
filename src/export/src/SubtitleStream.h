#pragma once

// Private (0.2): a soft subtitle track inside an export (SubtitleTrackExport): the sequence's caption track as 3GPP
// Timed Text (mov_text) in MP4 / MOV, SubRip in MKV. Kept apart from the caption sidecars and burn-in (CaptionSidecar)
// and from embedded CEA-608 captions, which are other options.

#include "Encode.h"

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

/// One subtitle: plain text (lines separated by '\n') from `start` to `end`, in milliseconds from the export's start.
struct SubtitleCue {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string text;
};

/// The cues of the caption track an export of `sequence` over `range` carries (CaptionExportSettings picks the track;
/// words the soundtrack bleeps or mutes replaced as in sidecars), cut to the range and shifted to start at 0. Cues
/// without visible text are left out. Empty when the sequence has no enabled caption track.
std::vector<SubtitleCue> subtitleCues(const ProjectState& project, const Sequence& sequence,
                                      const CaptionExportSettings& captions, TimeRange range);

/// The ISO 639-2 language of the subtitle track: SubtitleTrackExport::language, else the caption track's (ISO 639-1
/// mapped for the common languages), else "und".
std::string subtitleLanguage(const Sequence& sequence, const ExportSettings& settings);

/// An opened subtitle encoder and its stream.
class SubtitleStream {
public:
    /// Opens the encoder for `container` (mov_text for MP4 / MOV, subrip for MKV) and adds its stream to `muxer`
    /// (before Muxer::open), tagged with `language` (ISO 639-2; "und" when empty). Errors: Unsupported, EncoderError.
    static std::expected<SubtitleStream, Error> add(Container container, Muxer& muxer, const std::string& language);

    /// Encodes and writes every cue (after Muxer::open). Errors: EncoderError, IoError.
    std::expected<void, Error> write(const std::vector<SubtitleCue>& cues, Muxer& muxer);

    /// Writes the cues that start at or before `uptoMs`, in order, that earlier calls have not written (cues start in
    /// ascending order: SubtitleCue lists are sorted by startMs, as subtitleCues() returns them). The export calls it as
    /// the video reaches each frame, so the cue packets are muxed beside the picture they belong to (card 146: written all
    /// at the end they sat after hours of video, clustered at the file's tail in Matroska). Errors as write().
    std::expected<void, Error> writeUntil(const std::vector<SubtitleCue>& cues, std::int64_t uptoMs, Muxer& muxer);
    /// The cues writeUntil() has written so far (write() writes the rest).
    std::size_t written() const noexcept { return m_next; }

private:
    std::expected<void, Error> writeOne(const SubtitleCue& cue, Muxer& muxer, AVPacket* packet);

    CodecContextPtr m_ctx;
    AVStream* m_stream = nullptr;
    std::size_t m_next = 0; ///< the next cue to write
};

} // namespace wabcut::exporter::detail
