#pragma once

// Version history ("time travel", C.9): a bounded record of a project's revisions in a sidecar folder next to the
// project file ("<file>.wabcut.history"), for restoring an earlier cut, naming takes ("Client cut v2") and comparing
// versions (ProjectDiff.h).
//
// Storage. A revision is the project as content-addressed chunks (ProjectChunks: the project root with its media list,
// each sequence, each track), each stored once as objects/<2 hex>/<SHA-256 of its text>: zlib-compressed with a small
// header ("WCH1", the text's length) and checked against its name when read. Revisions share every unchanged sequence
// and track, and a checkpoint does not even serialise the sequences and tracks it stored before (the snapshot nodes are
// immutable, so the same node is the same chunk). index.json lists the revisions.
//
// Crash safety. A checkpoint writes its new objects first (each to a temporary file, flushed and renamed into place, so
// an object is whole or absent), then a new index the same way. Interrupted at any point, the folder still holds the
// previous index and every object it names; leftovers (temporary files, objects no revision names) are cleaned up by
// open() and prune().
//
// Pruning. Named revisions stay. Of the others the newest `keepRecent` stay; then, while the folder is over `maxBytes`,
// the oldest of the rest go - but never the newest revision. Objects no remaining revision reaches are deleted.
//
// Untrusted input. A history folder may come from anywhere: the index, the revisions' fields and the objects are bounded
// (kMaxHistoryRevisions, kMaxHistoryLabelBytes, kMaxHistoryObjectBytes), keys must be 64 hex digits, and a revision
// loads through the project file's checks (ProjectIO).
//
// When to checkpoint is the host's decision: on save, every few minutes of editing, before an AI agent applies a plan,
// on restore, and when the user names a version (HistoryKind).

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wabcut {

/// What made a checkpoint.
enum class HistoryKind : std::uint8_t { Save, Auto, Agent, Manual, Restore };

std::string_view toString(HistoryKind kind);
std::optional<HistoryKind> historyKindFromString(std::string_view name);

inline constexpr std::size_t kMaxHistoryRevisions = 10'000;
inline constexpr std::size_t kMaxHistoryLabelBytes = 256;
/// Bound on one object's text (decompressed) and on the index.
inline constexpr std::uint64_t kMaxHistoryObjectBytes = std::uint64_t{1} << 30;
/// Bound on index.json (10,000 revisions take a few MB).
inline constexpr std::uint64_t kMaxHistoryIndexBytes = std::uint64_t{64} << 20;
/// Revision ids stay below 2^53 (exact in every JSON reader).
inline constexpr std::uint64_t kMaxHistoryRevisionId = std::uint64_t{1} << 53;

struct HistoryRevision {
    std::uint64_t id = 0;     ///< increasing; never reused in a folder
    std::string createdAt;    ///< ISO-8601 UTC
    HistoryKind kind = HistoryKind::Auto;
    std::string label;        ///< a user's name for it ("Client cut v2"); named revisions are kept
    std::string root;         ///< the root chunk's key
    std::uint64_t addedBytes = 0; ///< bytes of the objects this checkpoint added (compressed)
    std::uint64_t sequences = 0;
    std::uint64_t clips = 0;

    bool named() const { return !label.empty(); }
    bool operator==(const HistoryRevision&) const = default;
};

struct HistoryOptions {
    /// The folder's size budget (objects and index).
    std::uint64_t maxBytes = std::uint64_t{1} << 30;
    /// Unnamed revisions always kept (the newest ones).
    std::size_t keepRecent = 50;
    /// prune() after every checkpoint.
    bool pruneOnCheckpoint = true;
    /// Tests: a checkpoint stops after writing its objects, before the index (as if the process died).
    bool failBeforeIndexForTesting = false;
};

struct HistoryPruneReport {
    std::size_t revisionsRemoved = 0;
    std::size_t objectsRemoved = 0;
    std::uint64_t bytesFreed = 0;
};

class ProjectHistory {
public:
    /// The sidecar folder of a project file: "<file>.history".
    static std::filesystem::path folderFor(const std::filesystem::path& projectFile);
    /// Opens (or starts, when there is none) the history in `folder`. Fails on a damaged index.
    static std::expected<ProjectHistory, Error> open(std::filesystem::path folder, HistoryOptions options = {});

    const std::filesystem::path& folder() const { return m_folder; }
    /// Oldest first.
    const std::vector<HistoryRevision>& revisions() const { return m_revisions; }
    const HistoryRevision* find(std::uint64_t id) const;

    /// Records `state`. When it is the same as the newest revision (same chunks) and gets no label, no revision is added
    /// and the newest one is returned. `createdAt` defaults to now (UTC).
    std::expected<HistoryRevision, Error> checkpoint(const ProjectState& state, HistoryKind kind, std::string label = {},
                                                     std::string createdAt = {});
    /// Names a revision (a non-empty label keeps it from pruning); an empty label unnames it.
    std::expected<void, Error> name(std::uint64_t id, std::string label);
    /// The project as it was at a revision.
    std::expected<ProjectState, Error> load(std::uint64_t id) const;
    std::expected<HistoryPruneReport, Error> prune();

    /// Bytes on disk (objects and index).
    std::uint64_t diskBytes() const;
    std::size_t objectCount() const { return m_objects.size(); }

private:
    friend class HistorySink;
    std::expected<void, Error> writeIndex() const;
    std::expected<std::string, Error> readObject(const std::string& key) const;
    std::filesystem::path objectPath(const std::string& key) const;

    std::filesystem::path m_folder;
    HistoryOptions m_options;
    std::vector<HistoryRevision> m_revisions;
    std::uint64_t m_nextRevision = 1;
    /// Objects on disk: key -> file size.
    std::unordered_map<std::string, std::uint64_t> m_objects;
    /// Snapshot nodes (sequences, tracks) already stored: node -> key. Holds the nodes, so an address is never reused.
    std::unordered_map<const void*, std::pair<std::shared_ptr<const void>, std::string>> m_known;
    /// The chunks a root or sequence chunk names (content-addressed, so never stale).
    std::unordered_map<std::string, std::vector<std::string>> m_refs;
};

} // namespace wabcut
