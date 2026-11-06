#pragma once

// Portable project capsule (I.4, "Collect Files…", like Premiere's Project Manager): one folder holding a project
// and everything it needs, so it opens on another machine (or after the originals are gone) without a single
// offline clip.
//
//   <destination>/<name>/
//     <name>.wabcut          the project, media locators relative to it
//     Media/...              the media the sequences use, flat or (0.7, CapsuleLayout) in folders that mirror the
//                            project's bins or the folders the files came from: whole files, or trimmed to the used
//                            ranges + handles
//                            (stream copies, exporter::trimmedCopy; clip In points and keyframe curves follow the
//                            copy's time exactly, and so does everything else keyed to the media's own time: redactions,
//                            mask paths and parameters, Master Clip Effects, the item's markers, marks and subclip range,
//                            transcript edits; items that share redactions or master effects are cut at the same time, or
//                            all copied whole)
//     <name> Proxies/...     the proxies (optional; whole-file mode only, a trimmed copy has none of its own)
//     Fonts/, Analysis/...   extra files the caller adds (fonts used by titles and captions, analysis sidecars)
//     manifest.json          every file: path, role, size, fingerprint, source; the Wabcut version
//
// It is assembled in "<destination>/<name>.wabcut-partial-<n>" and renamed when complete: a cancel or a failure
// leaves nothing behind. Free space is checked before anything is written. With `verify`, the finished capsule is
// copied to a temporary folder and opened there: no clip may be offline and every fingerprint must match the
// manifest.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::editor {

struct CapsuleExtraFile {
    std::filesystem::path source;
    /// Where it goes inside the capsule, e.g. "Fonts/Kanit-Regular.ttf".
    std::filesystem::path relative;
    std::string role = "extra"; ///< manifest role: "font", "analysis", ...
};

/// 0.7 (card 076): where the media files go inside the capsule's Media folder.
enum class CapsuleLayout : std::uint8_t {
    Flat,          ///< every file in Media/ (the default; names made unique)
    Bins,          ///< the project's bins as folders: Media/<bin>/<sub-bin>/file (items in the root bin stay in Media/)
    SourceFolders, ///< the folders the files came from, below their common folder: Media/<folder>/<sub-folder>/file
};
/// "flat", "bins", "folders": the names the manifest, scripts and the dialog use.
const char* capsuleLayoutName(CapsuleLayout layout);
/// The layout named `name`, or Flat for anything else.
CapsuleLayout capsuleLayoutFromName(const std::string& name);

struct CapsuleOptions {
    /// The folder the capsule folder is created in.
    std::filesystem::path destination;
    /// The capsule's folder and project name; empty = the project's name (or "Untitled").
    std::string name;
    /// Copy only the used ranges of each media file (MP4/MOV/MKV; others are copied whole) plus `handles` before and
    /// after each range (card 003: the dialog's Handles, 0 to 10 s; a range never reaches past the file).
    bool trim = false;
    Ticks handles = Ticks::fromSeconds(1);
    /// Card 003: the sequences to collect (empty = all). The capsule holds these and the sequences they play (nested
    /// sequences, multicam sources, audition takes), nothing else: the media only other sequences use stays out (unless
    /// `excludeUnused` is off) and so do their review notes. The manifest lists them. Ids the project does not have are
    /// ignored; naming none that exists is an InvalidArgument.
    std::vector<SequenceId> sequences;
    /// Card 076: how the media files are laid out below Media/. Folder names come from bin names or from the folders the
    /// files were in (untrusted text): every name is made valid on every OS and can never leave Media/, names that differ
    /// only by case or Unicode form share one folder, and a path deeper than 8 folders or 120 characters keeps its last
    /// folders (noted in the result). Files of an image sequence or a spanned clip stay together in a folder of their own.
    CapsuleLayout layout = CapsuleLayout::Flat;
    /// Copy the proxies the project uses (whole-file mode).
    bool includeProxies = false;
    /// Leave out media items no sequence uses.
    bool excludeUnused = true;
    std::vector<CapsuleExtraFile> extras;
    /// Reopen the finished capsule in a temporary folder and check it.
    bool verify = true;
    /// Written into the manifest.
    std::string wabcutVersion;
};

struct CapsuleProgress {
    std::string phase; ///< "checking", "copying", "trimming", "saving", "verifying"
    std::string current; ///< the file being copied
    std::int64_t bytesDone = 0;
    std::int64_t bytesTotal = 0;
    int filesDone = 0;
    int filesTotal = 0;
};

struct CapsuleFile {
    std::filesystem::path relative;
    std::string role; ///< "project", "media", "proxy", or the extra file's role
    std::int64_t bytes = 0;
    std::string sampleHash; ///< SourceFingerprint::sampleHash
    std::string source;     ///< where it came from (UTF-8)
    bool trimmed = false;
};

struct CapsuleResult {
    std::filesystem::path folder;
    std::filesystem::path projectFile;
    std::filesystem::path manifest;
    std::vector<CapsuleFile> files;
    std::int64_t bytes = 0;
    int mediaFiles = 0;
    int trimmedFiles = 0;
    int lutFiles = 0; ///< LUT assets copied into "LUTs/" (I.5)
    /// Card 003: the names of the sequences the capsule holds (the chosen ones and what they play).
    std::vector<std::string> sequences;
    /// Verification (CapsuleOptions::verify): reopened in a temporary folder, offline clips there, fingerprints
    /// that differ from the manifest.
    bool verified = false;
    int offlineAfterReopen = 0;
    int fingerprintMismatches = 0;
    std::vector<std::string> notes; ///< what was left out and why
};

/// Card 003: the sequences a capsule of `chosen` holds: those that exist, and every sequence they play (nested, multicam
/// source, audition take), again and again; sorted by id. Empty when none of `chosen` exists.
std::vector<SequenceId> capsuleSequences(const ProjectState& state, const std::vector<SequenceId>& chosen);

/// The media items a capsule of `options` copies: the media of the chosen sequences (and what they play) that clips use,
/// plus unused media when `excludeUnused` is off - the set collectProject copies, for whatever is written per media file
/// (analysis sidecars). Sorted by id. Reads the disk (resolves each file); empty when the plan fails (offline media).
std::vector<MediaId> capsuleMedia(const ProjectState& state, const std::filesystem::path& projectFile,
                                  const CapsuleOptions& options);

/// Bytes the capsule will take (trimmed media estimated from the used share of each file), for the free-space
/// check and the dialog.
std::int64_t capsuleBytesNeeded(const ProjectState& state, const std::filesystem::path& projectFile,
                                const CapsuleOptions& options);

namespace detail {
/// 0.7 (card #90): copies `in` to `out` in 4 MiB chunks and reports the bytes written to `onChunk` (progress). It
/// succeeds only when the whole of `in` was read and written: `in` must end with a plain end of file (no I/O error)
/// after exactly `expectedBytes` bytes, and `out` must accept every byte. A read that stops early - a flaky drive, a
/// share that drops, a file that shrinks or grows while it is copied - is an IoError, never a shorter copy reported
/// as success. Cancelled when `stop` is requested. Exposed for tests.
std::expected<std::int64_t, Error> copyChecked(std::istream& in, std::ostream& out, std::int64_t expectedBytes,
                                               std::stop_token stop, const std::function<void(std::int64_t)>& onChunk = {});
} // namespace detail

/// Collects `state` (the project as it would be saved, proxies included in its locators) of `projectFile` (empty when
/// untitled) into a capsule. Errors: InvalidArgument (the capsule folder exists), IoError (not enough disk space,
/// checked before anything is written; a copy failed), NotFound (offline media), Cancelled.
std::expected<CapsuleResult, Error> collectProject(const ProjectState& state, const std::filesystem::path& projectFile,
                                                   const CapsuleOptions& options, std::stop_token stop = {},
                                                   const std::function<void(const CapsuleProgress&)>& onProgress = {});

} // namespace wabcut::editor
