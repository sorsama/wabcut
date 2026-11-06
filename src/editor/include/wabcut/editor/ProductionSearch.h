#pragma once

// Search across all projects of a production (0.7, card 094; Premiere's search in the Productions panel).
//
//   * A production is a folder of projects. scanProduction() reads every project of it (a project open in this session is
//     taken as it is in memory, unsaved changes included; the others are read from their files, off the calling thread's
//     business: the caller runs it on a worker, with progress and a stop token) into a ProductionIndex: for each sequence
//     and media item the text a user would look for it by - its name, comment, description, clip metadata (scene, shot,
//     take, log note, client, status, camera fields), tape name, file path, bin and marker texts. A project whose file
//     did not change since the earlier scan (same size and time) is not read again, so typing a second query, or
//     searching again after a project was saved, costs a stat per project.
//   * searchProduction() finds items in the index: every word of the query must be found (in any searched field, or the one
//     field asked for), quoted words are a phrase, and the comparison ignores case (Unicode simple case folding, the
//     Find and Replace rules, TextFind.h; accents count). The best matches come first: a name that is the query, then one that starts with
//     it, then a word of the name, then the rest of the name, the file name or path, and the other fields. The same
//     "field" and "type" values as a Search Bin (BinSearch) narrow it.
//   * Nothing here writes anything or opens a project for editing: a hit names the project, the item's id in it and what
//     matched; the Production panel browses the project read only and imports the item as a copy (edit::importProjectItems).
//
// Pure C++, no Qt. Project files are untrusted: sizes are bounded, a project that cannot be read is reported and skipped.

#include <wabcut/editor/Production.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor {

/// Bounds: a production is a folder anyone can put files in; projects are untrusted.
inline constexpr std::size_t kMaxIndexedItemsPerProject = 100000;
inline constexpr std::size_t kMaxIndexedFieldBytes = 4096;  ///< one field of one item (the rest is not searched)
inline constexpr std::size_t kMaxSearchTextBytes = 1024;    ///< the query
inline constexpr std::size_t kMaxSearchHits = 1000;         ///< SearchQuery::limit is cut to this

/// SearchQuery::field values besides "" (every field): a Search Bin's fields and the rest of the Library's Find bar, plus
/// "marker" (marker names and comments) and "bin" (the bins an item is in).
inline constexpr std::string_view kProductionSearchFields[] = {
    "name",       "comment",     "description",   "scene",     "shot",        "take",          "logNote",
    "client",     "tapeName",    "status",        "cameraAngle", "cameraLabel", "cameraModel",  "shotDate",
    "shotLocation", "path",      "marker",        "bin"};
/// SearchQuery::type values besides "" (any): the Library's kinds of sequence and media.
inline constexpr std::string_view kProductionSearchTypes[] = {"sequence", "video", "audio", "still", "title", "subclip"};

/// One searchable sequence or media item of a project.
struct SearchItem {
    struct Field {
        std::string field;  ///< one of kProductionSearchFields
        std::string text;   ///< as written
        std::string folded; ///< Unicode simple case folded, what is compared
    };
    bool sequence = false;
    std::uint64_t id = 0;     ///< SequenceId / MediaId value in its project
    std::string kind;         ///< one of kProductionSearchTypes
    std::string name;
    std::string nameFolded;   ///< name, Unicode simple case folded (the order of equal scores)
    std::string location;     ///< the bins it is in, "Footage / Day 1"; empty at the top
    int width = 0;            ///< what a result row shows under the name: a sequence's or a picture's size,
    int height = 0;
    Ticks duration{};         ///< its length (0 for a still or a title),
    Rate frameRate{};         ///< its frame rate (invalid for audio),
    bool offline = false;     ///< and whether its file is missing
    std::vector<Field> fields; ///< only the fields that have text
};

/// A project as the search sees it.
struct ProjectIndex {
    std::filesystem::path file;  ///< absolute
    std::string name;            ///< the file name without extension
    std::string relative;        ///< from the production folder, '/' separators
    std::int64_t size = -1;      ///< the file's size when it was read (-1: unknown)
    long long modified = 0;      ///< the file's write time (platform ticks: only compared with itself)
    bool open = false;           ///< read from memory: this session has the project open
    std::string error;           ///< why it could not be read (then it has no items)
    bool truncated = false;      ///< it had more than kMaxIndexedItemsPerProject items
    std::vector<SearchItem> items;
};

struct ProductionIndex {
    std::filesystem::path folder;
    std::vector<ProjectIndex> projects;
};

/// The items of `state` (a project in memory or just loaded) as the search sees them; `file`, `name`, `relative` and the
/// stat fields are left for the caller.
ProjectIndex indexProject(const ProjectState& state);

struct ScanOptions {
    /// The project open in this session at `file`, as it is in memory; null for one that is not open.
    std::function<std::shared_ptr<const ProjectState>(const std::filesystem::path& file)> openState;
    /// The earlier scan: a project whose file has the size and write time it recorded (and was not read from memory) is
    /// taken from it, not read again.
    const ProductionIndex* previous = nullptr;
    /// Called before each project: how many are done, how many there are, the one being read ("Interview.wabcut").
    std::function<void(int done, int total, const std::string& current)> progress;
};

/// Reads every project of `production` into an index. Stops at the next project when `stop` is requested (the index then
/// holds the projects read so far: check `stop` before trusting it). Never throws; a project that cannot be read is in
/// the index with its `error`.
ProductionIndex scanProduction(const Production& production, const ScanOptions& options, std::stop_token stop = {});

struct SearchQuery {
    std::string text;              ///< words; "double quotes" make a phrase; empty with `type` set lists that type
    std::string field;             ///< "" or one of kProductionSearchFields
    std::string type;              ///< "" or one of kProductionSearchTypes
    std::string project;           ///< "" or one project (its `relative` path) to search in
    std::size_t limit = 200;       ///< at most this many hits come back (cut to kMaxSearchHits)
};

struct SearchHit {
    std::filesystem::path project;
    std::string projectName;
    std::string projectRelative;
    bool projectOpen = false;
    std::uint64_t id = 0;
    bool sequence = false;
    std::string kind;
    std::string name;
    std::string location;
    int width = 0;
    int height = 0;
    Ticks duration{};
    Rate frameRate{};
    bool offline = false;
    std::vector<std::string> matched; ///< the fields the words were found in ("name", "comment", ...)
    int score = 0;
};

struct SearchResult {
    std::vector<SearchHit> hits; ///< best first, at most `limit`
    std::size_t total = 0;       ///< how many items matched
    int projects = 0;            ///< projects searched
    std::vector<std::string> unreadable; ///< "Name.wabcut: why" for each project that could not be read
};

/// Why `query` cannot be run (text too long or not UTF-8, an unknown field or type), or nullopt.
std::optional<std::string> searchProblem(const SearchQuery& query);

/// The items of `index` that match `query`. An empty query (no text and no type) matches nothing.
SearchResult searchProduction(const ProductionIndex& index, const SearchQuery& query);

} // namespace wabcut::editor
