#pragma once

// Private: reads a marker list written by another tool or a spreadsheet, for marker.import (a sequence's markers) and
// source.importMarkers (a clip's). Three shapes are recognised from the content:
//   * csv: a table with a header row of column names (comma, semicolon or tab separated; quoted fields may hold
//     commas and line breaks). Premiere's own marker export ("Marker Name, Description, In, Out, Duration, Marker Type")
//     reads back, and so does Wabcut's (the Color column too). Other common names work: Name / Title / Label,
//     Comment / Notes, Start / Time / Timecode / Position, End, Length, Type, Colour. Without a header row the columns
//     are time, name, comment.
//   * txt: one marker per line, "<time> <name>": YouTube chapters ("0:00 Intro", "1:02:30 Outro"), "00:01:23:10 -
//     Name", a tab-separated "<time><TAB><name><TAB><comment>", and Audacity labels ("12.5<TAB>15.0<TAB>Name").
//   * json: an array of marker objects, or an object with a "markers" array (marker.list's output reads back). Each
//     object has a time ("time", "start", "in" or "position": seconds as a number, text as below, or an object with
//     "ticks" or "timecode"), optionally "duration" (or "out"), "name", "comment", "color" and "type".
// A time is "HH:MM:SS:FF" at the timebase given (frames; ';' before the frames for drop-frame), "H:MM:SS.fff",
// "M:SS" (a clock, as YouTube writes chapters), seconds ("12.5" or "12.5s"), or "1500ms".
// Untrusted input: the text is at most kMaxMarkerImportBytes, a line that cannot be read is skipped and reported (the
// first kMaxMarkerImportWarnings of them), names and comments are cut at the marker limit.

#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::command::detail {

inline constexpr std::size_t kMaxMarkerImportBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxMarkerImportWarnings = 20;

struct ParsedMarker {
    /// Start and duration in ticks as the file says (no offset yet); the text cleaned and cut to the marker limit.
    Marker marker;
    /// 1-based line (csv, txt) or entry (json) the marker came from.
    std::size_t where = 0;
};

struct ParsedMarkerList {
    /// "csv", "txt" or "json".
    std::string format;
    std::vector<ParsedMarker> markers;
    /// Why lines were skipped or values replaced ("line 4: no time"), the first kMaxMarkerImportWarnings only.
    std::vector<std::string> warnings;
    /// How many lines or entries gave no marker.
    std::size_t skipped = 0;
};

/// The text of a marker file as UTF-8: a byte order mark and carriage returns removed, and bytes that are not UTF-8
/// (a spreadsheet's Windows-1252 export) converted.
std::string markerFileText(std::string bytes);

/// Reads `text` (markerFileText's output). `format` is "csv", "txt", "json" or "" to detect it. `rate` is the
/// timebase of "HH:MM:SS:FF" times (the sequence's, or the clip's own); `dropFrame` for 29.97 / 59.94 drop-frame
/// counts. Fails (with the reason) when the text holds no marker at all, so a wrong file is not a silent no-op.
std::expected<ParsedMarkerList, std::string> parseMarkerList(std::string_view text, std::string_view format, Rate rate,
                                                             bool dropFrame);

} // namespace wabcut::command::detail
