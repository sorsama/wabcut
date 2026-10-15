#pragma once

// Finds the media files an imported timeline refers to.
//
// Interchange files name media by the path they had on the machine that exported them. MediaRelinker
// looks for each file, in order:
//  1. the exact path (when it is absolute on this platform);
//  2. project-relative: a relative reference resolved against the imported file's folder, then the
//     trailing components of the original path under that folder and its parent ("/Volumes/X/Proj/
//     Media/a.mov" -> "<dir>/Media/a.mov", "<dir>/a.mov", "<dir>/../Media/a.mov", ...);
//  3. a file-name search under the search roots (case-insensitive), keeping only candidates whose size
//     and duration match when those are known, and preferring the one whose path shares the most
//     trailing folders with the original.
// It only reads directory listings and file sizes (plus whatever the optional inspector does); it
// never opens, moves or modifies media files.

#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wabcut::interchange {

/// What is known about a media file on disk (from an inspector, e.g. media::probe in the app).
struct MediaFacts {
    std::optional<std::int64_t> fileSize;
    std::optional<Ticks> duration;
    /// Embedded start timecode as a position (01:00:00:00 = 3600 s); lets EDL source timecodes be
    /// converted to offsets from the media's first frame.
    std::optional<Ticks> startTimecode;
};

/// Optional hook to inspect a candidate file; returns nullopt when it cannot be read.
using MediaInspector = std::function<std::optional<MediaFacts>(const std::filesystem::path&)>;

/// A media file as an interchange file refers to it.
struct MediaReference {
    /// Display name (clip/file name).
    std::string name;
    /// The reference exactly as written (URL, path or clip name).
    std::string original;
    /// Decoded path, UTF-8 with '/' separators (see mediaReferenceToPath); may be relative or empty.
    std::string path;
    /// Known size of the original file, used to reject same-name candidates.
    std::optional<std::int64_t> fileSize;
    /// Known duration of the original media; checked against candidates when an inspector is set.
    std::optional<Ticks> duration;
};

enum class RelinkMethod : std::uint8_t { ExactPath, ProjectRelative, SearchRoot, Unresolved };

std::string_view toString(RelinkMethod method);

struct RelinkResult {
    RelinkMethod method = RelinkMethod::Unresolved;
    /// The file found; empty when unresolved.
    std::filesystem::path resolved;
    /// Every path checked, in order (UTF-8), for the report.
    std::vector<std::string> tried;
    /// Explanation of a non-obvious choice or failure, e.g. "2 files named a.mov; picked the one whose
    /// folders match the original path". Empty when there is nothing to add.
    std::string note;

    bool found() const { return method != RelinkMethod::Unresolved; }
};

struct RelinkOptions {
    /// Folder of the imported file; relative references and project-relative guesses start here.
    std::filesystem::path projectDirectory;
    /// Folders searched recursively by file name.
    std::vector<std::filesystem::path> searchRoots;
    /// Optional; used to compare durations (and sizes) of same-name candidates.
    MediaInspector inspector;
    /// Search index limits (a search root like "C:/" must not hang the import).
    std::size_t maxIndexedFiles = 500'000;
    int maxDepth = 24;
    /// Whether a candidate is an existing file; empty: the file system. The fuzz harnesses replace it so the paths in
    /// an untrusted file are never looked up on disk (or on a network share).
    std::function<bool(const std::filesystem::path&)> fileExists;
};

class MediaRelinker {
public:
    explicit MediaRelinker(RelinkOptions options);

    /// Looks for the file. The search-root index is built on first use and reused.
    RelinkResult resolve(const MediaReference& reference);

    /// Number of files indexed under the search roots (builds the index).
    std::size_t indexedFileCount();
    /// True when the index stopped at maxIndexedFiles.
    bool indexTruncated() const { return m_truncated; }

private:
    void buildIndex();
    bool matchesKnownFacts(const MediaReference& reference, const std::filesystem::path& candidate,
                           std::string& why);

    RelinkOptions m_options;
    bool m_indexed = false;
    bool m_truncated = false;
    /// Lower-cased file name -> files with that name.
    std::unordered_map<std::string, std::vector<std::filesystem::path>> m_byName;
    std::size_t m_fileCount = 0;
};

} // namespace wabcut::interchange
