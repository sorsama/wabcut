#pragma once

// One-click import of timelines from other editors.
//
//     auto result = interchange::ImportRouter::importFile("cut.xml", {.mediaSearchRoots = {"D:/Footage"}});
//     if (result) { session.replace(result->project); show(result->report); }
//
// ImportRouter::detect() identifies a file by its content first and its extension second: FCP7 XML
// (xmeml, as Premiere Pro and Resolve export it), FCPXML 1.9-1.11 (also .fcpxmld bundles), OpenTimelineIO
// JSON (.otio, also .otiod bundles), CMX3600 EDL and Premiere .prproj. Formats Wabcut cannot read
// (Resolve .drp/.drt, AAF, FCP libraries, .otioz) are detected too, with guidance on what to export
// instead.
//
// importFile() builds a new, valid ProjectState through the model's own edit operations (or adds to
// `targetProject`), relinks media (MediaRelinker.h) and lists what was carried, approximated and dropped
// (ImportReport.h). Malformed input returns an Error; it never crashes or yields a half-built project.

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/interchange/MediaRelinker.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

enum class Format : std::uint8_t {
    Unknown,
    /// Final Cut Pro 7 XML (xmeml v1-v5): Premiere Pro's and Resolve's "FCP XML" export.
    Fcp7Xml,
    /// FCPXML 1.x (Final Cut Pro 10+, Resolve), .fcpxml or an .fcpxmld bundle.
    FcpXml,
    /// OpenTimelineIO JSON (.otio, or an .otiod bundle).
    Otio,
    /// CMX3600 edit decision list.
    Edl,
    /// Adobe Premiere Pro project (.prproj, gzip-compressed XML).
    PremiereProject,
    /// DaVinci Resolve project/timeline/archive (.drp, .drt, .dra): proprietary.
    ResolveProject,
    /// Advanced Authoring Format (.aaf): not supported yet.
    Aaf,
    /// Final Cut Pro library (.fcpbundle): proprietary.
    FinalCutLibrary,
    /// Zipped OpenTimelineIO bundle (.otioz): not supported yet.
    OtioArchive,
};

/// Stable lower-case id ("fcp7-xml", "fcpxml", "otio", ...).
std::string_view toString(Format format);
/// User-facing name ("Final Cut Pro 7 XML", ...).
std::string_view displayName(Format format);

struct Detection {
    Format format = Format::Unknown;
    /// importFile() can read the file (with the given options).
    bool supported = false;
    /// The file holding the timeline: Info.fcpxml inside an .fcpxmld bundle, content.otio inside an
    /// .otiod bundle, otherwise the path itself.
    std::filesystem::path contentPath;
    /// For unsupported or unrecognised files: what to do instead, for the user. Empty otherwise.
    std::string guidance;
};

struct ImportOptions {
    /// Folders searched by file name for media that is not at its original path.
    std::vector<std::filesystem::path> mediaSearchRoots;
    /// Import into this project (new bin, media and sequences are added to it) instead of a new one.
    std::optional<ProjectState> targetProject;
    /// EDLs carry no frame rate. Default: 30000/1001 for drop-frame EDLs, otherwise 24/1 (OTIO's default).
    std::optional<Rate> edlFrameRate;
    /// Optional media inspector for relinking (duration/size checks of same-name candidates).
    MediaInspector mediaInspector;
    /// Where the imported project will be saved, for MediaLocator::relativePath. Empty: the imported
    /// file's folder (the project is assumed to be saved next to it).
    std::filesystem::path projectFile;
    /// Import Premiere .prproj files with the experimental reader (see docs/interchange.md). Off by
    /// default: .prproj is then reported as unsupported with export guidance.
    bool experimentalPremiereProject = false;
    /// Replaces the relinker's file-existence check (RelinkOptions::fileExists); empty: the file system.
    std::function<bool(const std::filesystem::path&)> fileExists;
};

/// A media file that could not be found. Its MediaItem's locator keeps the original path and is marked
/// offline; the clips using it are in the timeline and come back online once the file is relinked.
struct UnresolvedMedia {
    MediaId media;
    std::string name;
    /// The reference as written in the imported file.
    std::string reference;
    /// The decoded original path (may be empty when the format only names the clip, e.g. EDL).
    std::string path;
    /// Paths that were checked.
    std::vector<std::string> tried;
    std::string note;
};

struct ImportResult {
    /// The new project state; passes validate(..., Validation::Strict).
    ProjectState project;
    Format format = Format::Unknown;
    /// Bin named after the imported file that holds the imported media and sequences.
    BinId bin;
    /// Imported sequences, in file order.
    std::vector<SequenceId> sequences;
    /// Imported media items, in file order.
    std::vector<MediaId> media;
    ImportReport report;
    std::vector<UnresolvedMedia> unresolvedMedia;
};

class ImportRouter {
public:
    /// Identifies a file (or an .fcpxmld/.otiod bundle folder) by content and extension. Reads at most
    /// the first 64 KiB. Missing/unreadable files give Format::Unknown with guidance.
    static Detection detect(const std::filesystem::path& path);

    /// Identifies in-memory content; `fileName` (optional) supplies the extension hint.
    static Detection detectData(std::string_view data, std::string_view fileName = {});

    /// Detects and imports a file. Errors: NotFound / IoError (cannot read), Unsupported (a detected
    /// format Wabcut cannot import; detect() has the guidance), ParseError (malformed or unrecognised).
    static std::expected<ImportResult, Error> importFile(const std::filesystem::path& path,
                                                         const ImportOptions& options = {});

    /// Imports in-memory content of a known format. `sourceName` is used for report locations and the
    /// bin name; relative media references resolve against `baseDirectory`.
    static std::expected<ImportResult, Error> importData(std::string_view data, Format format,
                                                         std::string_view sourceName,
                                                         const ImportOptions& options = {},
                                                         const std::filesystem::path& baseDirectory = {});
};

} // namespace wabcut::interchange
