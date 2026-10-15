// Timeline export (Export.h): describe the sequence, then the format's writer.

#include "Exporters.h"
#include "TextUtil.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <cstdio>

namespace wabcut::interchange {

namespace detail {

std::string markerCommentForExport(const Marker& marker) {
    if (!marker.highlight) {
        return marker.comment;
    }
    char score[32];
    std::snprintf(score, sizeof score, "%.2f", marker.highlight->score);
    std::string text = "[highlight " + std::string(score) + " " + std::string(toString(marker.highlight->confidence));
    for (std::size_t i = 0; i < marker.highlight->reasons.size(); ++i) {
        text += (i == 0 ? ": " : "; ") + marker.highlight->reasons[i];
    }
    text += "]";
    return marker.comment.empty() ? text : marker.comment + " " + text;
}

} // namespace detail

using namespace detail;

bool canExport(Format format) {
    return format == Format::Fcp7Xml || format == Format::FcpXml || format == Format::Otio || format == Format::Edl;
}

std::optional<Format> exportFormatFromName(std::string_view name) {
    const std::string n = toLowerAscii(trim(name));
    if (n == "fcp7-xml" || n == "fcp7" || n == "xml" || n == "xmeml" || n == "fcpxml7") {
        return Format::Fcp7Xml;
    }
    if (n == "fcpxml") {
        return Format::FcpXml;
    }
    if (n == "otio") {
        return Format::Otio;
    }
    if (n == "edl" || n == "cmx3600") {
        return Format::Edl;
    }
    return std::nullopt;
}

std::string_view exportExtension(Format format) {
    switch (format) {
    case Format::Fcp7Xml:
        return ".xml";
    case Format::FcpXml:
        return ".fcpxml";
    case Format::Otio:
        return ".otio";
    case Format::Edl:
        return ".edl";
    default:
        return {};
    }
}

std::expected<ExportResult, Error> exportSequence(const ProjectState& project, SequenceId sequence, Format format,
                                                  const ExportOptions& options) {
    if (!canExport(format)) {
        return std::unexpected(Error{ErrorCode::Unsupported, std::string(displayName(format)) + " cannot be exported"});
    }
    if (format == Format::FcpXml && options.fcpxmlVersion != "1.10" && options.fcpxmlVersion != "1.11") {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "FCPXML version " + options.fcpxmlVersion + " is not written (1.10 or 1.11)"});
    }
    const Sequence* seq = project.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(sequence.value) + " does not exist"});
    }
    ExportResult result;
    result.format = format;
    result.extension = std::string(exportExtension(format));
    ExportContext context;
    context.fileLabel = options.fileLabel.empty() ? seq->name + result.extension : options.fileLabel;
    context.options = &options;
    context.report = &result.report;
    auto described = describeForExport(project, sequence, context);
    if (!described) {
        return std::unexpected(described.error());
    }
    result.sequences = described->sequenceIds;
    switch (format) {
    case Format::Fcp7Xml:
        result.data = writeFcp7Xml(described->ir, context);
        break;
    case Format::FcpXml:
        result.data = writeFcpXml(described->ir, context);
        break;
    case Format::Otio:
        result.data = writeOtio(described->ir, context);
        break;
    case Format::Edl:
        result.data = writeEdl(described->ir, context);
        break;
    default:
        break;
    }
    return result;
}

std::expected<ExportResult, Error> exportSequenceToFile(const ProjectState& project, SequenceId sequence, Format format,
                                                        const std::filesystem::path& file, ExportOptions options) {
    if (options.fileLabel.empty()) {
        options.fileLabel = pathToUtf8(file.filename());
    }
    auto result = exportSequence(project, sequence, format, options);
    if (!result) {
        return result;
    }
    if (auto written = io::writeFileAtomically(file, result->data); !written) {
        return std::unexpected(written.error());
    }
    return result;
}

} // namespace wabcut::interchange
