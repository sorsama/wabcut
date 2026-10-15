#include "Importers.h"
#include "TextUtil.h"
#include "TimelineBuilder.h"
#include "XmlUtil.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/model/ProjectIO.h>

#include <array>
#include <system_error>

namespace wabcut::interchange {

namespace fs = std::filesystem;
using namespace detail;

namespace {

constexpr std::size_t kSniffBytes = 64 * 1024;

constexpr std::string_view kSupportedFormats =
    "Wabcut imports Final Cut Pro 7 XML (.xml, as Premiere Pro and DaVinci Resolve export it), FCPXML "
    "(.fcpxml/.fcpxmld), OpenTimelineIO (.otio) and CMX3600 EDL (.edl) files.";

constexpr std::string_view kPremiereGuidance =
    "Premiere Pro projects are not imported directly yet. In Premiere Pro, select the sequence in the "
    "Project "
    "panel and choose File > Export > Final Cut Pro XML..., then import the .xml file.";

constexpr std::string_view kResolveGuidance =
    "DaVinci Resolve project, timeline and archive files (.drp, .drt, .dra) are proprietary. In Resolve, "
    "select the timeline in the Media Pool and choose File > Export > Timeline..., pick \"FCPXML 1.10 "
    "Files\" "
    "(or \"OpenTimelineIO Files\"), then import that file.";

constexpr std::string_view kFinalCutGuidance =
    "Final Cut Pro libraries are proprietary. In Final Cut Pro, select the project and choose File > Export "
    "XML... (FCPXML 1.10 or later), then import the .fcpxml or .fcpxmld.";

constexpr std::string_view kAafGuidance =
    "AAF is not supported yet. From Premiere Pro or Resolve, export Final Cut Pro XML, FCPXML or "
    "OpenTimelineIO instead; from Avid Media Composer, export a CMX3600 EDL.";

constexpr std::string_view kOtiozGuidance = ".otioz bundles are not supported yet. An .otioz is a zip "
                                            "archive: extract it and import its content.otio, "
                                            "or export a plain .otio file.";

bool hasZipMagic(std::string_view d) {
    return d.starts_with(std::string_view("PK\x03\x04", 4));
}
bool hasCompoundFileMagic(std::string_view d) {
    return d.starts_with(std::string_view("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8));
}

bool isTimecodeWord(std::string_view w) {
    // HH:MM:SS:FF with any of : ; . , as separators.
    if (w.size() != 11) {
        return false;
    }
    for (std::size_t i = 0; i < w.size(); ++i) {
        const bool separator = i == 2 || i == 5 || i == 8;
        const char c = w[i];
        if (separator ? (c != ':' && c != ';' && c != '.' && c != ',') : (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

bool looksLikeEdl(std::string_view data) {
    int events = 0;
    int headers = 0;
    int lines = 0;
    for (const std::string_view raw :
         splitLines(data.substr(0, std::min<std::size_t>(data.size(), kSniffBytes)))) {
        const std::string_view line = trim(raw);
        if (line.empty()) {
            continue;
        }
        if (++lines > 200) {
            break;
        }
        if (istartsWith(line, "TITLE:") || istartsWith(line, "FCM:")) {
            ++headers;
            continue;
        }
        const auto words = splitWords(line);
        if (words.size() >= 7 && words[0].size() >= 3 &&
            words[0].find_first_not_of("0123456789") == std::string_view::npos &&
            isTimecodeWord(words[words.size() - 1]) && isTimecodeWord(words[words.size() - 2]) &&
            isTimecodeWord(words[words.size() - 3]) && isTimecodeWord(words[words.size() - 4])) {
            ++events;
        }
    }
    return events > 0 || headers >= 2;
}

bool looksLikeOtio(std::string_view data) {
    const std::string_view head = trim(data.substr(0, std::min<std::size_t>(data.size(), kSniffBytes)));
    return head.starts_with('{') && head.find("\"OTIO_SCHEMA\"") != std::string_view::npos;
}

Detection unknown(std::string guidance) {
    Detection d;
    d.guidance = std::move(guidance);
    return d;
}

Detection make(Format format, bool supported, std::string_view guidance = {}) {
    Detection d;
    d.format = format;
    d.supported = supported;
    d.guidance = std::string(guidance);
    return d;
}

std::string extensionOf(std::string_view fileName) {
    const std::string_view name = fileNameOf(fileName);
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? std::string() : toLowerAscii(name.substr(dot));
}

std::string stemOf(std::string_view fileName) {
    const std::string_view name = fileNameOf(fileName);
    const auto dot = name.rfind('.');
    return std::string(dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot));
}

} // namespace

std::string_view toString(Format format) {
    switch (format) {
    case Format::Unknown:
        return "unknown";
    case Format::Fcp7Xml:
        return "fcp7-xml";
    case Format::FcpXml:
        return "fcpxml";
    case Format::Otio:
        return "otio";
    case Format::Edl:
        return "edl";
    case Format::PremiereProject:
        return "prproj";
    case Format::ResolveProject:
        return "resolve-project";
    case Format::Aaf:
        return "aaf";
    case Format::FinalCutLibrary:
        return "fcp-library";
    case Format::OtioArchive:
        return "otioz";
    }
    return "unknown";
}

std::string_view displayName(Format format) {
    switch (format) {
    case Format::Unknown:
        return "Unknown format";
    case Format::Fcp7Xml:
        return "Final Cut Pro 7 XML";
    case Format::FcpXml:
        return "FCPXML";
    case Format::Otio:
        return "OpenTimelineIO";
    case Format::Edl:
        return "CMX3600 EDL";
    case Format::PremiereProject:
        return "Premiere Pro project";
    case Format::ResolveProject:
        return "DaVinci Resolve project";
    case Format::Aaf:
        return "AAF";
    case Format::FinalCutLibrary:
        return "Final Cut Pro library";
    case Format::OtioArchive:
        return "OpenTimelineIO archive (.otioz)";
    }
    return "Unknown format";
}

Detection ImportRouter::detectData(std::string_view data, std::string_view fileName) {
    const std::string ext = extensionOf(fileName);

    // Proprietary or not-yet-supported containers, recognised by extension (their content is opaque).
    if (ext == ".drp" || ext == ".drt" || ext == ".dra") {
        return make(Format::ResolveProject, false, kResolveGuidance);
    }
    if (ext == ".fcpbundle") {
        return make(Format::FinalCutLibrary, false, kFinalCutGuidance);
    }
    if (ext == ".otioz") {
        return make(Format::OtioArchive, false, kOtiozGuidance);
    }
    if (ext == ".aaf" || hasCompoundFileMagic(data)) {
        return make(Format::Aaf, false, kAafGuidance);
    }
    if (hasZipMagic(data)) {
        return unknown("This is a zip archive, not a timeline. " + std::string(kSupportedFormats));
    }

    if (hasGzipMagic(data)) {
        const auto inflated = gunzip(data, kSniffBytes, true);
        if (ext == ".prproj" || (inflated && rootElementName(*inflated) == "PremiereData")) {
            return make(Format::PremiereProject, false, kPremiereGuidance);
        }
        return unknown("This is a gzip-compressed file that is not a Premiere Pro project. " +
                       std::string(kSupportedFormats));
    }

    const std::string root = rootElementName(data);
    if (root == "xmeml") {
        return make(Format::Fcp7Xml, true);
    }
    if (root == "fcpxml") {
        return make(Format::FcpXml, true);
    }
    if (root == "PremiereData") {
        return make(Format::PremiereProject, false, kPremiereGuidance);
    }
    if (looksLikeOtio(data)) {
        return make(Format::Otio, true);
    }
    if (root.empty() && (ext == ".edl" || looksLikeEdl(data))) {
        return make(Format::Edl, true);
    }

    if (!root.empty()) {
        return unknown("This XML file (root element <" + root + ">) is not a timeline format Wabcut knows. " +
                       std::string(kSupportedFormats));
    }
    if (ext == ".otio") {
        return unknown("This .otio file is not OpenTimelineIO JSON (no OTIO_SCHEMA). " +
                       std::string(kSupportedFormats));
    }
    if (ext == ".prproj") {
        return unknown("This .prproj file is not a Premiere Pro project (it is not gzip-compressed XML).");
    }
    return unknown("Unrecognised file. " + std::string(kSupportedFormats));
}

Detection ImportRouter::detect(const fs::path& path) {
    std::error_code ec;
    const std::string ext = toLowerAscii(utf8FromPath(path.extension()));
    if (fs::is_directory(path, ec)) {
        fs::path content;
        if (ext == ".fcpxmld") {
            content = path / "Info.fcpxml";
        } else if (ext == ".otiod") {
            content = path / "content.otio";
        } else if (ext == ".fcpbundle") {
            return make(Format::FinalCutLibrary, false, kFinalCutGuidance);
        } else {
            return unknown("This is a folder, not a timeline file. " + std::string(kSupportedFormats));
        }
        if (!fs::is_regular_file(content, ec)) {
            return unknown("The bundle has no " + utf8FromPath(content.filename()) + " inside.");
        }
        Detection d = detect(content);
        d.contentPath = content;
        return d;
    }
    if (!fs::exists(path, ec)) {
        Detection d = unknown("The file does not exist.");
        d.contentPath = path;
        return d;
    }
    const auto prefix = readFilePrefix(path, kSniffBytes);
    if (!prefix) {
        Detection d = unknown("The file cannot be read.");
        d.contentPath = path;
        return d;
    }
    Detection d = detectData(*prefix, utf8FromPath(path.filename()));
    d.contentPath = path;
    return d;
}

std::expected<ImportResult, Error> ImportRouter::importFile(const fs::path& path,
                                                            const ImportOptions& options) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "file not found: " + utf8FromPath(path)});
    }
    const Detection detection = detect(path);
    const bool experimentalPrproj =
        detection.format == Format::PremiereProject && options.experimentalPremiereProject;
    if (detection.format == Format::Unknown) {
        return std::unexpected(parseError("unrecognised file: " + utf8FromPath(path)));
    }
    if (!detection.supported && !experimentalPrproj) {
        return std::unexpected(
            unsupported(std::string(displayName(detection.format)) + " files cannot be imported"));
    }
    auto data = readFileLimited(detection.contentPath);
    if (!data) {
        return std::unexpected(data.error());
    }
    // Relative media references resolve against the folder holding the timeline (inside a bundle, the
    // bundle itself; the relinker also tries the folder above).
    const fs::path base = detection.contentPath.parent_path();
    return importData(*data, detection.format, utf8FromPath(path.filename()), options, base);
}

std::expected<ImportResult, Error> ImportRouter::importData(std::string_view data, Format format,
                                                            std::string_view sourceName,
                                                            const ImportOptions& options,
                                                            const fs::path& baseDirectory) {
    ParseContext context{std::string(sourceName), &options};
    std::expected<IrDocument, Error> document = std::unexpected(Error{});
    switch (format) {
    case Format::Fcp7Xml:
        document = parseFcp7Xml(data, context);
        break;
    case Format::FcpXml:
        document = parseFcpXml(data, context);
        break;
    case Format::Otio:
        document = parseOtio(data, context);
        break;
    case Format::Edl:
        document = parseEdl(data, context);
        break;
    case Format::PremiereProject:
        if (!options.experimentalPremiereProject) {
            return std::unexpected(
                unsupported("Premiere Pro projects need ImportOptions::experimentalPremiereProject"));
        }
        document = parsePremiereProject(data, context);
        break;
    default:
        return std::unexpected(unsupported(std::string(displayName(format)) + " files cannot be imported"));
    }
    if (!document) {
        return std::unexpected(document.error());
    }
    BuildContext build;
    build.format = format;
    build.fileLabel = std::string(sourceName);
    build.binName = stemOf(sourceName);
    build.baseDirectory = baseDirectory;
    build.projectFile = !options.projectFile.empty()
                            ? options.projectFile
                            : baseDirectory / pathFromUtf8(stemOf(sourceName) + std::string(".wabcut"));
    build.options = &options;
    auto result = buildProject(std::move(*document), build);
    // Whatever was imported must also save: a name that is not UTF-8 would make every later save fail.
    if (result) {
        if (auto saved = io::toJson(result->project); !saved) {
            return std::unexpected(parseError("the file's text cannot be stored in a project: " + saved.error().message));
        }
    }
    return result;
}

} // namespace wabcut::interchange
