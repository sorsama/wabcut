#pragma once

// Timeline export (Export.h): a project's sequence -> IrDocument (describeForExport) -> one writer per format. The
// describe step is the one place that knows the model; it reports what the IR cannot hold. Each writer reports what
// it writes: Carried, or Approximated / Dropped where its format has no exact place for it.

#include "Ir.h"

#include <wabcut/interchange/Export.h>

#include <expected>
#include <string>
#include <vector>

namespace wabcut::interchange::detail {

/// A marker's comment for formats without a highlight field (C.15): the comment, then "[highlight 0.87 high: kill
/// streak; mic peak]" when the marker has a highlight.
std::string markerCommentForExport(const Marker& marker);

struct ExportContext {
    /// File label of report locations.
    std::string fileLabel;
    const ExportOptions* options = nullptr;
    ImportReport* report = nullptr;

    void add(ReportOutcome outcome, ReportCategory category, std::string reason, const SourceLocation& where,
             const std::string& sequence) const {
        report->add(outcome, category, std::move(reason), where, sequence);
    }
};

struct ExportDocument {
    /// sequences[0] is the exported sequence; then the sequences it plays, in the order the timeline first meets them.
    IrDocument ir;
    /// The model id of each IR sequence (parallel to ir.sequences).
    std::vector<SequenceId> sequenceIds;
};

/// `sequence` and every sequence it plays (nested and multicam sources, recursively) as an IrDocument. Media gets
/// absolute paths (MediaLocator::path, else relativePath against the project's folder). Things the IR cannot hold
/// (graphic clips, clips of unknown sources) are reported Dropped. Errors: NotFound.
std::expected<ExportDocument, Error> describeForExport(const ProjectState& project, SequenceId sequence,
                                                       const ExportContext& context);

/// "'Main' V1 'Interview' at 00:00:12:05": where an element is, for report locations.
std::string exportLocation(const IrSequence& sequence, TrackKind kind, std::size_t track, const std::string& what,
                           Ticks at);

std::string writeFcp7Xml(const IrDocument& document, const ExportContext& context);
std::string writeFcpXml(const IrDocument& document, const ExportContext& context);
/// OpenTimelineIO JSON; metadata.wabcut on the timeline, tracks, clips, transitions and markers carries what OTIO
/// has no field for, exactly (ticks, curves, ...), so OtioImporter.cpp brings a Wabcut export back unchanged.
std::string writeOtio(const IrDocument& document, const ExportContext& context);
std::string writeEdl(const IrDocument& document, const ExportContext& context);

} // namespace wabcut::interchange::detail
