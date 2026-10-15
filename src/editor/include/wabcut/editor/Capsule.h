#pragma once

// Portable project capsule (I.4, "Collect Files…", like Premiere's Project Manager): one folder holding a project
// and everything it needs, so it opens on another machine (or after the originals are gone) without a single
// offline clip.
//
//   <destination>/<name>/
//     <name>.wabcut          the project, media locators relative to it
//     Media/...              the media the sequences use: whole files, or trimmed to the used ranges + handles
//                            (stream copies, exporter::trimmedCopy; clip In points and keyframe curves follow the
//                            copy's time exactly)
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

struct CapsuleOptions {
    /// The folder the capsule folder is created in.
    std::filesystem::path destination;
    /// The capsule's folder and project name; empty = the project's name (or "Untitled").
    std::string name;
    /// Copy only the used ranges of each media file (MP4/MOV/MKV; others are copied whole) plus `handles`.
    bool trim = false;
    Ticks handles = Ticks::fromSeconds(1);
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
    /// Verification (CapsuleOptions::verify): reopened in a temporary folder, offline clips there, fingerprints
    /// that differ from the manifest.
    bool verified = false;
    int offlineAfterReopen = 0;
    int fingerprintMismatches = 0;
    std::vector<std::string> notes; ///< what was left out and why
};

/// Bytes the capsule will take (trimmed media estimated from the used share of each file), for the free-space
/// check and the dialog.
std::int64_t capsuleBytesNeeded(const ProjectState& state, const std::filesystem::path& projectFile,
                                const CapsuleOptions& options);

/// Collects `state` (the project as it would be saved, proxies included in its locators) of `projectFile` (empty when
/// untitled) into a capsule. Errors: InvalidArgument (the capsule folder exists), IoError (not enough disk space,
/// checked before anything is written; a copy failed), NotFound (offline media), Cancelled.
std::expected<CapsuleResult, Error> collectProject(const ProjectState& state, const std::filesystem::path& projectFile,
                                                   const CapsuleOptions& options, std::stop_token stop = {},
                                                   const std::function<void(const CapsuleProgress&)>& onProgress = {});

} // namespace wabcut::editor
