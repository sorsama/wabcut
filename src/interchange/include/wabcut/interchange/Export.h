#pragma once

// Timeline export (X.1): a sequence as an interchange file that other editors read, so a cut can go to Premiere
// Pro, DaVinci Resolve or Final Cut Pro (for the grade, the mix, a client) and come back.
//
//     auto result = interchange::exportSequence(project, sequenceId, interchange::Format::Fcp7Xml);
//     if (result) { save(result->data); show(result->report); }
//
// Formats:
//  * Final Cut Pro 7 XML (xmeml v5), what Premiere Pro and Resolve import best;
//  * FCPXML 1.10 / 1.11 (Final Cut Pro, Resolve);
//  * OpenTimelineIO JSON (.otio), with Wabcut's own data under metadata.wabcut so Wabcut -> OTIO -> Wabcut loses
//    nothing;
//  * CMX3600 EDL: one video track and its audio, within the format's limits.
// The nested and multicam sequences the sequence plays go with it. The output is deterministic (the same project
// gives the same bytes) and Wabcut's own importers read it back: what the report calls Carried comes back the same.
//
// The export report has the import report's model (ImportReport.h): every element is Carried (written exactly),
// Approximated (written, but changed to fit the format: rounded to whole frames, a wipe written as a dissolve, ...)
// or Dropped (the format has no place for it: a graphic clip, an effect it cannot name). Locations name the
// sequence, track and clip in Wabcut ("'Main' V1 'Interview' at 00:00:12:05").

#include <wabcut/interchange/Import.h>
#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

struct ExportOptions {
    /// FCPXML document version: "1.10" or "1.11".
    std::string fcpxmlVersion = "1.10";
    /// The project file. Media whose absolute path is missing is written from its project-relative path
    /// (MediaLocator::relativePath) resolved against the project's folder. Empty: absolute paths only.
    std::filesystem::path projectFile;
    /// The file name report locations name (the destination). Empty: "<sequence name><extension>".
    std::string fileLabel;
};

struct ExportResult {
    Format format = Format::Unknown;
    /// The file's content (UTF-8).
    std::string data;
    /// ".xml", ".fcpxml", ".otio" or ".edl".
    std::string extension;
    /// Carried / approximated / dropped, like an import's.
    ImportReport report;
    /// The sequences written: the exported one first, then the nested and multicam sources it plays.
    std::vector<SequenceId> sequences;
};

/// Whether exportSequence writes `format` (FCP7 XML, FCPXML, OTIO, EDL).
bool canExport(Format format);
/// The format for "fcp7-xml" (or "xml", "fcp7", "xmeml"), "fcpxml", "otio", "edl" (case-insensitive).
std::optional<Format> exportFormatFromName(std::string_view name);
/// The default file extension of an export format (".xml", ".fcpxml", ".otio", ".edl"); empty for others.
std::string_view exportExtension(Format format);

/// Writes `sequence` of `project` in `format`. Errors: NotFound (no such sequence), Unsupported (a format Wabcut
/// does not export), InvalidArgument (an FCPXML version other than 1.10 / 1.11).
std::expected<ExportResult, Error> exportSequence(const ProjectState& project, SequenceId sequence, Format format,
                                                  const ExportOptions& options = {});

/// exportSequence, then the result written to `file` atomically (a temporary renamed over it). The report's
/// locations name the file. Errors: those of exportSequence, IoError.
std::expected<ExportResult, Error> exportSequenceToFile(const ProjectState& project, SequenceId sequence, Format format,
                                                        const std::filesystem::path& file,
                                                        ExportOptions options = {});

} // namespace wabcut::interchange
