// Interchange (X.1): interchange.export writes a sequence as a timeline file other editors read
// (wabcut/interchange/Export.h). It names the file it writes, so it is a file command (CommandSpec::touchesFiles).

#include "Catalog.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/model/MediaLocator.h>

#include <string>

namespace wabcut::command::detail {

namespace {

Json reportOf(const interchange::ImportReport& report) {
    using interchange::ReportOutcome;
    Json entries = Json::array();
    for (const interchange::ReportEntry& e : report.entries()) {
        if (e.outcome == ReportOutcome::Carried) {
            continue; // counted; the approximated and dropped ones are what a person needs to read
        }
        entries.push_back(Json{{"outcome", std::string(interchange::toString(e.outcome))},
                               {"category", std::string(interchange::toString(e.category))},
                               {"reason", e.reason},
                               {"where", e.location.element}});
    }
    return Json{{"carried", report.count(ReportOutcome::Carried)},
                {"approximated", report.count(ReportOutcome::Approximated)},
                {"dropped", report.count(ReportOutcome::Dropped)},
                {"entries", std::move(entries)}};
}

} // namespace

void registerInterchangeCommands(CommandRegistry& registry) {
    Json path = schema::string("The file to write (UTF-8 path); an existing one is replaced.");
    path["maxLength"] = 4096;
    path["minLength"] = 1;
    addBuiltin(
        registry,
        command::touchesFiles(
            {"interchange.export", 1, "Export Timeline",
             "Writes the sequence (with the sequences it nests and its multicam sources) as a timeline file other editors "
             "read, at \"path\": Final Cut Pro 7 XML (\"fcp7-xml\": Premiere Pro, DaVinci Resolve), FCPXML (\"fcpxml\": "
             "Final Cut Pro, Resolve; \"fcpxmlVersion\" 1.10 or 1.11), OpenTimelineIO (\"otio\") or a CMX3600 EDL "
             "(\"edl\"). Returns the export report: how many elements were carried exactly, and every one approximated "
             "(rounded to frames, a transition the format has no name for, ...) or dropped (graphics and effects the "
             "format cannot describe). The project does not change; a dry run writes nothing.",
             Capability::Read,
             withSequence({{"format", schema::enumeration({"fcp7-xml", "fcpxml", "otio", "edl"}, "The file format.")},
                           {"path", path},
                           {"fcpxmlVersion", schema::enumeration({"1.10", "1.11"}, "FCPXML only (default 1.10).")}},
                          {"format", "path"}),
             Json{{"format", "fcp7-xml"}, {"path", "C:/Projects/Film (for Resolve).xml"}},
             {},
             [](ExecutionContext& c) {
                 const auto format = interchange::exportFormatFromName(c.string("format"));
                 if (!format) {
                     fail(ErrorCode::InvalidParams, "unknown format", Json{{"path", "/format"}});
                 }
                 interchange::ExportOptions options;
                 options.fcpxmlVersion = c.string("fcpxmlVersion", "1.10");
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 options.fileLabel = pathToUtf8(file.filename());
                 // A dry run reports what the export would carry and writes nothing (E5-P2-7).
                 auto result = c.dryRun() ? interchange::exportSequence(c.state(), c.sequenceId(), *format, options)
                                          : interchange::exportSequenceToFile(c.state(), c.sequenceId(), *format, file, options);
                 if (!result) {
                     fail(result.error().code == wabcut::ErrorCode::IoError ? ErrorCode::EditRejected : ErrorCode::InvalidParams,
                          result.error().message);
                 }
                 Json sequences = Json::array();
                 for (const SequenceId id : result->sequences) {
                     sequences.push_back(id.value);
                 }
                 CommandOutput out;
                 out.state = c.state();
                 out.data = Json{{"path", c.string("path")},
                                 {"format", std::string(interchange::toString(*format))},
                                 {"bytes", result->data.size()},
                                 {"written", !c.dryRun()},
                                 {"sequences", std::move(sequences)},
                                 {"report", reportOf(result->report)}};
                 return out;
             }},
            {"path"}));
}

} // namespace wabcut::command::detail
