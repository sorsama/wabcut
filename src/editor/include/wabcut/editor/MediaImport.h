#pragma once

// Importing files into a project (File > Import, drag and drop onto the Library, the e2e test):
//   * media files: media::probe + the model's source fingerprint -> a MediaItem with a MediaLocator;
//   * timelines from other editors (.xml / .fcpxml / .otio / .edl / .prproj, by content): the interchange
//     importer, whose ImportReport says what was carried, approximated and dropped;
//   * 0.4 folders (File > Import Folder…, a folder dropped on the Library or picked in the Media Browser):
//     collectImportFolder lists their media, and importMediaFiles puts each file in a bin named after its folder
//     (subfolders nested), all in the same one undo step.
// Every import is one undo step. Pure C++; the app's bridge only converts URLs and shows the report.
// 0.4 also lists the game capture tools' folders (Game Bar, ShadowPlay, OBS) for the Media Browser's places.

#include <wabcut/interchange/Import.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::editor {

enum class ImportFileKind : std::uint8_t {
    Media,       ///< anything else: media::probe decides whether it can be read
    Interchange, ///< a timeline interchange::ImportRouter recognises (supported or not)
    Project,     ///< a .wabcut project: open it instead
    Captions,    ///< a caption file (interchange/CaptionFiles.h, P.2): becomes a caption track of the sequence
    Folder,      ///< 0.4: a folder that is not a timeline bundle: Import Folder expands it (collectImportFolder)
};

/// What a chosen or dropped file is: interchange formats are detected by content and extension (also .fcpxmld /
/// .otiod bundle folders), .wabcut by extension, caption files by extension (.srt .vtt .scc .mcc .stl .ttml .dfxp
/// .itt) or, for .xml, by a Timed Text root; any other folder is a Folder; everything else is treated as media.
ImportFileKind classifyImportFile(const std::filesystem::path& file);

// ---- 0.4: Import Folder ----

struct FolderImportLimits {
    std::size_t maxFiles = 1000; ///< files looked at (HomeBridge's kMaxFolderFiles)
    int maxDepth = 8;            ///< folder levels below the one given (HomeBridge's kMaxFolderDepth)
};

/// Why a file or folder of an imported folder was left out.
enum class FolderSkip : std::uint8_t {
    NotMedia,       ///< not a media file by its extension (timelines, captions, projects, Lottie, documents, ...)
    Hidden,         ///< its name starts with "."
    Link,           ///< a symbolic link or junction (never followed)
    TimelineBundle, ///< a .fcpxmld / .otiod / .fcpbundle folder
    CameraCard,     ///< a camera card (AVCHD, Canon XF, P2, XDCAM, XAVC): the Media Browser joins its clips
    TooDeep,        ///< a folder deeper than FolderImportLimits::maxDepth
};

struct FolderImportFile {
    std::filesystem::path file;
    /// The bins it goes in, outermost first: the imported folder's name, then each subfolder's.
    std::vector<std::string> bins;
};

struct FolderImportSkipped {
    std::filesystem::path path;
    FolderSkip reason = FolderSkip::NotMedia;
};

struct FolderImportPlan {
    /// By folder: a folder's own files (by name, case-insensitive) before its subfolders' (by name).
    std::vector<FolderImportFile> files;
    std::vector<FolderImportSkipped> skipped;
    /// More than FolderImportLimits::maxFiles files: only the first ones were looked at.
    bool truncated = false;
};

/// What Import Folder takes from `folder`: its media files (isBrowsableMedia: video, audio, stills, PSD / PSB, RAW) and
/// its subfolders' (nested bins), at most `limits.maxFiles` files looked at and `limits.maxDepth` levels deep. Left out
/// (in `skipped`): other files, hidden files and folders, links, timeline bundles, camera cards (`folder` itself being
/// one too) and folders too deep. Never follows links; reads no file contents. A folder that cannot be listed gives an
/// empty plan.
FolderImportPlan collectImportFolder(const std::filesystem::path& folder, const FolderImportLimits& limits = {});

// ---- 0.4: game capture folders (the Media Browser's places) ----

enum class CaptureTool : std::uint8_t { GameBar, ShadowPlay, Obs };

struct CaptureFolder {
    CaptureTool tool = CaptureTool::GameBar;
    std::string profile; ///< OBS: the profile's folder name ("Untitled"); "" for the others
    std::filesystem::path path;
};

/// Game capture folders that exist: <videos>/Captures (Game Bar), <videos>/NVIDIA (ShadowPlay), then OBS Studio's
/// recording folders from <obsConfig>/basic/profiles/*/basic.ini ([SimpleOutput] FilePath, [AdvOut] RecFilePath; the
/// one [Output] Mode uses first), deduplicated by normalised path, in that order. basic.ini is read by hand (UTF-8,
/// optional BOM, both slash kinds), not as QSettings, which reads backslashes as escapes.
std::vector<CaptureFolder> captureFolders(const std::filesystem::path& videos, const std::filesystem::path& obsConfig);

/// OBS Studio's config folder: %APPDATA%/obs-studio (Windows), ~/Library/Application Support/obs-studio (macOS),
/// $XDG_CONFIG_HOME (else ~/.config) /obs-studio (Linux); empty when the variable is unset. Read at each call (tests
/// change the environment).
std::filesystem::path obsConfigFolder();

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
    /// Bin for the new items (invalid = the root bin; 0.4: an unknown one too).
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
    /// 0.4 (Import Folder): per file given, the bins under `bin` it goes in, outermost first (empty, or shorter than
    /// `files`: `bin` itself). A child bin of that name (not a search bin) is reused, else made in the same undo step;
    /// bins are made only for items actually added.
    std::vector<std::vector<std::string>> binPaths;
    /// 0.4: the undo step's name ("" = "Import"; Import Folder sends "Import Folder").
    std::string undoLabel;
};

struct MediaImportResult {
    /// New media items, in the order the files were given.
    std::vector<MediaId> added;
    /// Files the project already has (same path and content): their existing items, not added again.
    std::vector<MediaId> alreadyInProject;
    std::vector<FailedImport> failed;
    /// Per file given, in order: its item (new or already in the project), invalid when it failed.
    std::vector<MediaId> byFile;
    /// 0.4: the bins the import made (MediaImportOptions::binPaths), outermost first.
    std::vector<BinId> createdBins;
};

/// Probes `files` and adds every readable one to the session's project as one undo step (`options.undoLabel`, else
/// "Import"), each in its bin (`options.bin` and `options.binPaths`). Nothing is committed when no file could be added.
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
