#pragma once

// Private (0.2): what an export writes besides the picture and the sound - chapters from chapter markers, the start
// timecode, XMP (embedded in MP4 / MOV as Adobe's 'uuid' box, or a sidecar), a Broadcast WAV's time reference.

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter::detail {

struct ChapterMark {
    std::int64_t startMs = 0; ///< from the start of the exported range
    std::int64_t endMs = 0;
    std::string title;
};

/// The sequence's chapter markers inside `range` as chapters: each runs to the next one (or its own end when it has
/// a duration, or the range end); a chapter at the range start opens the file. Titles: the marker's name, else its
/// comment, else "Chapter N".
std::vector<ChapterMark> chapterMarks(const Sequence& sequence, TimeRange range);

/// The timecode of the export's first frame: Set Start Timecode, else the sequence's start timecode plus the range start,
/// in `rate` ("hh:mm:ss:ff", drop-frame ";ff" where the sequence uses it and the rate has it).
Ticks exportStartTimecode(const ExportSettings& settings, const Sequence& sequence, TimeRange range);
std::string timecodeText(Ticks t, Rate rate, bool dropFrame);

/// The XMP packet of an export: dc:title, dc:description, xmp:CreatorTool / CreateDate, xmpDM:startTimeCode and (with
/// ExportMetadata::xmpMarkers) the sequence markers in the range as xmpDM:Tracks / markers, times in output frames.
std::string xmpPacket(const ExportSettings& settings, const Sequence& sequence, TimeRange range, Rate rate,
                      std::string_view creatorTool);

/// Appends `xmp` to an MP4 / MOV file as Adobe's top-level XMP 'uuid' box (BE7ACFCB-97A9-42E8-9C71-999491E3AFAC).
std::expected<void, Error> appendXmpBox(const std::filesystem::path& file, const std::string& xmp);

/// The XMP sidecar of an output: "<name>.xmp" next to it.
std::filesystem::path xmpSidecarPath(const std::filesystem::path& output);

/// Writes `text` to `file` through a temporary name (replacing it). Errors: IoError.
std::expected<void, Error> writeTextFile(const std::filesystem::path& file, const std::string& text);

} // namespace wabcut::exporter::detail
