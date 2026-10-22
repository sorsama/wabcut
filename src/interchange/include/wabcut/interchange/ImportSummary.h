#pragma once

// Human- and machine-readable views of an ImportResult (used by `wabcut-cli import` and the tests) and of an
// ExportResult or an AAF / OMF AudioPostResult (`wabcut-cli export-timeline`, the interchange.export command).

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/Import.h>

#include <cstddef>
#include <string>

namespace wabcut::interchange {

struct SummaryOptions {
    /// List every clip of every track (start/end timecode, name, source In, flags).
    bool listClips = true;
    /// List every Carried report entry too (otherwise they are only counted per category).
    bool listCarried = false;
};

/// Plain-text summary: sequences (settings, tracks, clips), media (resolved/unresolved) and the report
/// (counts per outcome and category, then every Approximated and Dropped entry with its location).
std::string formatSummary(const ImportResult& result, const SummaryOptions& options = {});

/// The same information as JSON (UTF-8, pretty-printed). Times are integer ticks plus timecode strings;
/// report entries are listed in full, including Carried ones.
std::string toJson(const ImportResult& result);

/// The report alone as text: counts per outcome and category, then every Approximated and Dropped entry (and the
/// Carried ones with `listCarried`) with its location.
std::string formatReport(const ImportReport& report, bool listCarried = false);

/// An export: the format, the sequences written, the size, then the report (formatReport).
std::string formatSummary(const ExportResult& result, const SummaryOptions& options = {});
/// The same as JSON: {"format", "extension", "bytes", "sequences": [ids], "report": {"counts", "entries"}}.
std::string toJson(const ExportResult& result);

/// An AAF or OMF export (`format`: "AAF" / "OMF"): the file, its tracks and clips, the audio files and reference video
/// written, then the report.
std::string formatSummary(const AudioPostResult& result, std::string_view format, const SummaryOptions& options = {});
/// {"format", "path", "bytes", "tracks", "clips", "audioFiles": [paths], "referenceVideo", "report"}.
std::string toJson(const AudioPostResult& result, std::string_view format);

} // namespace wabcut::interchange
