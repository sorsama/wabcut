#pragma once

// Camera cards and the Media Browser (0.2, Premiere's Media Browser panel): a folder listing that knows how cameras
// write their cards, so it shows clips instead of the card's folders, joins the files one recording was split into
// (spanned clips, model/SpannedClip.h) and imports a clip as one item.
//
// Cards and what a clip is on each:
//   * AVCHD (PRIVATE/AVCHD/BDMV, AVCHD/BDMV or BDMV): the STREAM/*.MTS files; a recording the camera split is the run of
//     play items its PLAYLIST/*.MPL joins seamlessly (connection condition 5 or 6).
//   * Canon XF (CONTENTS/CLIPS001/<clip>/): the clip folder's numbered MXF files (<clip>01.MXF, <clip>02.MXF, ...).
//   * Panasonic P2 (CONTENTS/VIDEO, CONTENTS/AUDIO, CONTENTS/CLIP): a clip is its video MXF and one MXF per audio
//     channel; clips of one shot (CLIP/*.XML GlobalShotID, in OffsetInShot order) are one spanned clip.
//   * Sony XDCAM EX (BPAV/CLPR/<clip>/<clip>.MP4), XDCAM HD (Clip/*.MXF) and XAVC (PRIVATE/M4ROOT/CLIP/*.MP4).
//   * GoPro (any folder): chapters GX01xxxx, GX02xxxx, ... (GH, GL, GP too; GOPRxxxx, GP01xxxx, ... on older cameras).
// Other folders list their media files one clip each.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor {

enum class CameraCard : std::uint8_t { None, Avchd, CanonXf, P2, XdcamEx, XdcamHd, Xavc };

/// "AVCHD", "Canon XF", "P2", "XDCAM EX", "XDCAM HD", "XAVC", or "" for None.
std::string_view displayName(CameraCard card);

struct CameraClip {
    std::string name;   ///< what the Media Browser shows: the clip's name on the card, or the file's base name
    CameraCard card = CameraCard::None;
    /// The picture's (or the clip's) files in playing order: one, or the parts of a spanned clip.
    std::vector<std::filesystem::path> files;
    /// P2: each audio channel's files, in the same order as `files` (empty elsewhere: the audio is in `files`).
    std::vector<std::vector<std::filesystem::path>> audio;

    bool spanned() const { return files.size() > 1; }
    std::uintmax_t bytes() const;
};

struct BrowserEntry {
    bool folder = false;
    std::string name;
    std::filesystem::path path; ///< the folder, or the clip's first file
    std::optional<CameraClip> clip;
};

/// The card `folder` belongs to - its root or any folder inside it - and the card's root; nullopt for other folders.
std::optional<std::pair<CameraCard, std::filesystem::path>> cameraCardAt(const std::filesystem::path& folder);

/// The clips of the card of `kind` at `root`, spanned clips joined, sorted by name.
std::vector<CameraClip> cameraCardClips(CameraCard kind, const std::filesystem::path& root);

/// The Media Browser's listing of `folder`: a camera card (or a folder inside one) lists the card's clips; any other
/// folder its subfolders (by name) and then its media files as clips (by name), GoPro chapters joined. At most
/// `limit` entries; hidden and system entries are left out.
std::vector<BrowserEntry> browseFolder(const std::filesystem::path& folder, std::size_t limit = 5000);

/// The clip `file` starts (as browseFolder lists it: a card's clip or GoPro chapters joined), or the file alone.
CameraClip cameraClipOf(const std::filesystem::path& file);

/// True for the extensions the Media Browser lists (video, audio, stills, layered and RAW files).
bool isBrowsableMedia(const std::filesystem::path& file);

/// The media items a clip imports as: one item (a spanned clip: one spanned item, its parts probed for their
/// durations), named after the clip. P2: the video item first, then one item per audio channel (the caller merges them,
/// edit::mergeClips). `projectFile` makes the project-relative paths.
std::expected<std::vector<MediaItem>, Error> describeCameraClip(const CameraClip& clip,
                                                               const std::filesystem::path& projectFile);

/// Premiere's Media Browser > Ingest (Copy): `clip`'s files copied into `destination`/<clip name>/ (names kept, so
/// spanned parts and P2 channels stay together; a file already there with the same size is not copied again), and the
/// clip of the copies. Errors: IoError (the folder cannot be made, a file cannot be copied).
std::expected<CameraClip, Error> copyCameraClip(const CameraClip& clip, const std::filesystem::path& destination);

/// The media path of files played one after another as one clip: the file itself, or a spanned clip's path.
std::expected<std::string, Error> spannedMediaPath(const std::vector<std::filesystem::path>& files);

} // namespace wabcut::editor
