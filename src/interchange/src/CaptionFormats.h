#pragma once

// Private: the caption file formats (CaptionFiles.h). Each reader produces RawCaptions - cues in file time with what
// it noticed - and finishImport() turns them into model cues (timecode offset, order, overlaps, text rules). Each writer
// gets cues already cut to the export range and shifted to start at 0.

#include <wabcut/interchange/CaptionFiles.h>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail::captions {

struct RawCue {
    Ticks start;
    Ticks end;
    std::string text; ///< UTF-8, lines joined by '\n' (any markup already removed)
    std::optional<CaptionPlacement> placement;
    std::optional<CaptionAlign> alignment;
    std::string where; ///< report location ("cue 12", "line 40")
};

struct RawCaptions {
    std::vector<RawCue> cues;
    std::string language;
    std::string title;
    std::optional<Rate> frameRate;
    bool dropFrame = false;
    /// Times are timecodes (subtract the sequence's start timecode).
    bool timecodeBased = false;
    ImportReport report;
};

/// Shared reporting helper: one entry per kind of approximation (counted), instead of one per cue.
class Notes {
public:
    void add(ReportOutcome outcome, std::string reason) { ++m_counts[{outcome, std::move(reason)}]; }
    void flush(ImportReport& report, std::string_view fileLabel) const;

private:
    std::map<std::pair<ReportOutcome, std::string>, std::size_t> m_counts;
};

std::expected<RawCaptions, Error> readSrt(std::string_view data, std::string_view fileLabel);
std::expected<RawCaptions, Error> readVtt(std::string_view data, std::string_view fileLabel);
std::expected<RawCaptions, Error> readScc(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel);
std::expected<RawCaptions, Error> readMcc(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel);
std::expected<RawCaptions, Error> readStl(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel);
std::expected<RawCaptions, Error> readTtml(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel);

std::string writeSrt(const std::vector<CaptionCue>& cues);
std::string writeVtt(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options);
std::expected<std::string, Error> writeScc(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options, Notes& notes);
std::expected<std::string, Error> writeMcc(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options, Notes& notes);
std::expected<std::string, Error> writeStl(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options, Notes& notes);
std::string writeTtml(const std::vector<CaptionCue>& cues, CaptionFileFormat flavour, const CaptionWriteOptions& options,
                      Notes& notes);

/// Cue text lines (CR LF, CR and LF all end a line), blank ones dropped.
std::vector<std::string> textLines(std::string_view text);

/// CEA-608 (SCC / MCC): the byte pairs of `cues` (rewrapped to 32 columns and 4 rows) keyed by frame (absolute: the
/// start timecode added), with notes of what changed.
std::map<std::int64_t, std::pair<std::uint8_t, std::uint8_t>> cea608Pairs(const std::vector<CaptionCue>& cues,
                                                                          const CaptionWriteOptions& options, Rate rate,
                                                                          Notes& notes);
/// Captions decoded from 608 byte pairs keyed by frame at `rate`.
void cea608Cues(const std::vector<std::pair<std::int64_t, std::pair<std::uint8_t, std::uint8_t>>>& pairs, Rate rate,
                RawCaptions& out, Notes& notes);

/// Milliseconds -> "hh:mm:ss<sep>mmm".
std::string clockTime(Ticks t, char separator);

} // namespace wabcut::interchange::detail::captions
