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
#include <optional>
#include <span>
#include <vector>

namespace wabcut::editor {

enum class ImportFileKind : std::uint8_t {
    Media,       ///< anything else: media::probe decides whether it can be read
    Interchange, ///< a timeline interchange::ImportRouter recognises (supported or not)
    Project,     ///< a .wabcut project: open it instead
    Captions,    ///< a caption file (interchange/CaptionFiles.h, P.2): becomes a caption track of the sequence
};

/// What a chosen or dropped file is: interchange formats are detected by content and extension (also .fcpxmld /
/// .otiod bundle folders), .wabcut by extension, caption files by extension (.srt .vtt .scc .mcc .stl .ttml .dfxp
/// .itt) or, for .xml, by a Timed Text root; everything else is treated as media.
ImportFileKind classifyImportFile(const std::filesystem::path& file);

/// A MediaItem for `file` (not yet in any project): probed MediaInfo, the file name as its name, and a locator
/// with the absolute path, the path relative to `projectFile` (none while the project is untitled) and the
/// fingerprint. Errors are media::probe's (NotFound, Unsupported, IoError) or the fingerprint's.
std::expected<MediaItem, Error> describeMediaFile(const std::filesystem::path& file,
                                                  const std::filesystem::path& projectFile = {});

/// 0.2, Premiere's Import with "Image Sequence" ticked: a MediaItem (not yet in any project) for the numbered stills
/// starting at `firstFrame` (model/ImageSequence.h: that file and the ones numbered after it without a gap), one video
/// stream at `frameRate`, named after its first file. Errors are findImageSequence's, media::probe's or the
/// fingerprint's.
std::expected<MediaItem, Error> describeImageSequence(const std::filesystem::path& firstFrame, Rate frameRate,
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
    /// P.2: Premiere's Preferences > Media > Default Media Scaling for the new items (MediaItem::scaling): how source
    /// edits size their video clips in sequences of another frame size.
    MediaScaling scaling = MediaScaling::None;
    /// P.2: Preferences > Timeline > Still Image Default Duration for new still images (MediaItem::stillDuration);
    /// nullopt: the built-in 5 seconds.
    std::optional<Ticks> stillDuration;
    /// 0.2 (P.3): fill the new items' clip metadata (Scene, Shot, Take, Log Note, Good, ...) from their files' XMP, as
    /// Premiere links those columns (interchange/Xmp.h): sidecars and MP4 / MOV boxes always, other files scanned only
    /// up to 32 MB. Read on the probing threads.
    bool readXmp = true;
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
