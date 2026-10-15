#pragma once

// Importing files into a project (File > Import, drag and drop onto the Library, the e2e test):
//   * media files: media::probe + the model's source fingerprint -> a MediaItem with a MediaLocator;
//   * timelines from other editors (.xml / .fcpxml / .otio / .edl / .prproj, by content): the interchange
//     importer, whose ImportReport says what was carried, approximated and dropped.
// Every import is one undo step. Pure C++; the app's bridge only converts URLs and shows the report.

#include <wabcut/interchange/Import.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

namespace wabcut::editor {

enum class ImportFileKind : std::uint8_t {
    Media,       ///< anything else: media::probe decides whether it can be read
    Interchange, ///< a timeline interchange::ImportRouter recognises (supported or not)
    Project,     ///< a .wabcut project: open it instead
};

/// What a chosen or dropped file is: interchange formats are detected by content and extension (also .fcpxmld /
/// .otiod bundle folders), .wabcut by extension; everything else is treated as media.
ImportFileKind classifyImportFile(const std::filesystem::path& file);

/// A MediaItem for `file` (not yet in any project): probed MediaInfo, the file name as its name, and a locator
/// with the absolute path, the path relative to `projectFile` (none while the project is untitled) and the
/// fingerprint. Errors are media::probe's (NotFound, Unsupported, IoError) or the fingerprint's.
std::expected<MediaItem, Error> describeMediaFile(const std::filesystem::path& file,
                                                  const std::filesystem::path& projectFile = {});

struct FailedImport {
    std::filesystem::path file;
    Error error;
};

struct MediaImportOptions {
    /// The project file, for MediaLocator::relativePath (empty while untitled).
    std::filesystem::path projectFile;
    /// Bin for the new items (invalid = the root bin).
    BinId bin;
    /// Probe on this many threads; 0 = automatic (up to 4: probing is I/O bound).
    int threads = 0;
};

struct MediaImportResult {
    /// New media items, in the order the files were given.
    std::vector<MediaId> added;
    /// Files the project already has (same path and content): their existing items, not added again.
    std::vector<MediaId> alreadyInProject;
    std::vector<FailedImport> failed;
    /// Per file given, in order: its item (new or already in the project), invalid when it failed.
    std::vector<MediaId> byFile;
};

/// Probes `files` and adds every readable one to the session's project as one undo step ("Import"). Nothing is
/// committed when no file could be added.
MediaImportResult importMediaFiles(EditSession& session, std::span<const std::filesystem::path> files,
                                   const MediaImportOptions& options = {});

struct TimelineImportOptions {
    std::filesystem::path projectFile;
    /// Folders searched by file name for media that is not at its original path.
    std::vector<std::filesystem::path> mediaSearchRoots;
    bool experimentalPremiereProject = false;
    /// Sequences removed in the same undo step as the import (M5f: the empty first sequence of a project started
    /// for the import, so the new project holds just the imported sequences). Unknown ids are ignored.
    std::vector<SequenceId> dropSequences;
};

/// Imports a timeline file into the session's project (a new bin with its media and sequences) as one undo
/// step "Import <file name>". Media is relinked with media::probe as the inspector (sizes and durations are
/// checked); media that cannot be found is imported offline. The result carries the ImportReport.
std::expected<interchange::ImportResult, Error> importTimelineFile(EditSession& session,
                                                                   const std::filesystem::path& file,
                                                                   const TimelineImportOptions& options = {});

/// Facts about a candidate media file for interchange relinking (size, duration), from media::probe.
interchange::MediaInspector probeInspector();

} // namespace wabcut::editor
