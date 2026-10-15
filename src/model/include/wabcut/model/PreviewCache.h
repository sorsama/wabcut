#pragma once

// Preview render cache (C.15): Premiere's render bar and preview files. The model side decides what needs rendering,
// keys what was rendered by content and drops it exactly when that content changes; the render owner renders segments
// into files and plays them back (PreviewRenderer; docs/internal/c15-parity-handoff.md, "Preview render").
//
// * The render bar (renderSegments): a sequence is cut at every video edit point (clip starts and ends, transition
//   region edges). Each segment gets a status from a cost model (effectRenderCost): Red when it holds a nested or
//   multicam clip or an adjustment layer (always rendered, like Premiere's red bar), or costs more than
//   kRealTimeBudget per frame; Yellow when it has effects, transitions, blend modes or track mattes within the budget;
//   None (no bar) otherwise. Rendered (green) when the cache holds a file whose content hash matches.
// * Content hash (renderContentHash): FNV-1a 64 of io::renderKeyJson - everything that decides the segment's pixels,
//   times relative to the segment's start. Equal hash = the file still shows the right pixels.
// * Invalidation (PreviewCache::invalidate): after every edit, undo or redo the host passes the ChangeSet. Entries of
//   sequences the edit cannot have touched (not changed, no changed nested sequence, no changed media or redactions)
//   stay without hashing; entries the edit may have touched (their range meets a changed track range, or a flag without
//   a range changed) are re-hashed and dropped (file deleted) when the hash differs. So an edit outside a segment keeps
//   it and an edit inside invalidates it; an edit that changes nothing visible (a rename) keeps it too.
// * Storage: one folder per project (the host picks it, e.g. "<project> Preview Files" next to the project file), the
//   index "previews.json" in it (untrusted on load: bounded, plain file names only), a size budget with least recently
//   used eviction. Files are only ever deleted by name inside the folder.
//
// Thread safety: a PreviewCache may be used from several threads (playback lookups while the GUI edits).

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

enum class RenderStatus : std::uint8_t { None, Yellow, Red, Rendered };

std::string_view toString(RenderStatus status);

/// The cost model: per frame, relative to drawing one plain HD layer (1.0). Effects not listed cost 1.0.
double effectRenderCost(std::string_view effectId);
/// Cost per frame that plays in real time on the reference machine (above it a segment is Red).
inline constexpr double kRealTimeBudget = 6.0;

struct RenderSegment {
    TimeRange range;
    RenderStatus status = RenderStatus::None;
    double cost = 0.0;  ///< estimated cost per frame (cost model)
    std::string reason; ///< why it is Red or Yellow, e.g. "adjustment layer", "effects: Ultra Key, Gaussian Blur"

    bool operator==(const RenderSegment&) const = default;
};

/// The render bar of `sequence` over `within` (default: [0, its duration)), segments in time order, without cache
/// knowledge (no Rendered). Empty for an unknown sequence.
std::vector<RenderSegment> renderSegments(const ProjectState& state, SequenceId sequence,
                                          std::optional<TimeRange> within = {});

/// The content hash of what `sequence` shows over `range`: 16 lowercase hex digits ("" for an unknown sequence).
std::string renderContentHash(const ProjectState& state, SequenceId sequence, TimeRange range);

struct PreviewEntry {
    SequenceId sequence;
    TimeRange range;
    std::string hash;         ///< renderContentHash when it was rendered
    std::string file;         ///< a plain file name inside the cache folder
    std::int64_t bytes = 0;
    std::int64_t lastUse = 0; ///< a use counter: higher = used more recently

    bool operator==(const PreviewEntry&) const = default;
};

/// Renders preview files (implemented by the render owner).
class PreviewRenderer {
public:
    virtual ~PreviewRenderer() = default;
    /// Renders what `sequence` of `state` shows over `range` (its frame size and rate, every frame in [start, end))
    /// into `file` (created or replaced; a format of the renderer's choice). `progress` gets 0..1 and returns false to
    /// cancel (then fail with ErrorCode::Cancelled and leave no file). Returns the file's size in bytes.
    virtual std::expected<std::int64_t, Error> render(const ProjectState& state, SequenceId sequence, TimeRange range,
                                                      const std::filesystem::path& file,
                                                      const std::function<bool(double)>& progress) = 0;
};

class PreviewCache {
public:
    static constexpr std::string_view kIndexFile = "previews.json";
    static constexpr std::size_t kMaxEntries = 100'000;
    static constexpr std::int64_t kDefaultBudget = std::int64_t{20} * 1024 * 1024 * 1024; ///< 20 GiB

    explicit PreviewCache(std::filesystem::path folder, std::int64_t budgetBytes = kDefaultBudget);

    const std::filesystem::path& folder() const noexcept { return m_folder; }
    std::int64_t budget() const;
    /// A new budget; evicts (deleting files) down to it. Returns the evicted entries.
    std::vector<PreviewEntry> setBudget(std::int64_t bytes);

    /// Reads the folder's index. A missing index is an empty cache; a malformed one is an error (the cache stays empty).
    std::expected<void, Error> load();
    /// Writes the index (atomically).
    std::expected<void, Error> save() const;
    /// The index file format (untrusted: at most kMaxEntries entries, plain file names, sorted hex hashes, ranges and
    /// sizes in range). Fuzzed.
    static std::expected<std::vector<PreviewEntry>, Error> parseIndex(std::string_view json);
    static std::string indexJson(const std::vector<PreviewEntry>& entries);
    /// A plain file name: 1..128 of [A-Za-z0-9._-], not starting with '.'.
    static bool isPlainFileName(std::string_view name);

    std::vector<PreviewEntry> entries() const;
    std::int64_t totalBytes() const;

    /// The rendered segment of `sequence` holding `t`, if any (marks it used). Entries stay valid through invalidate().
    std::optional<PreviewEntry> lookup(SequenceId sequence, Ticks t);
    /// C.16, what playback switches to: the rendered segment of `sequence` holding `t` whose file still shows what
    /// `state` shows there (its hash is the current one), if any. Hashes the segment: callers memoize per state.
    std::optional<PreviewEntry> validAt(const ProjectState& state, SequenceId sequence, Ticks t);
    /// renderSegments() with Rendered where an entry covers the whole segment and its hash is the current one.
    std::vector<RenderSegment> renderBar(const ProjectState& state, SequenceId sequence,
                                         std::optional<TimeRange> within = {}) const;
    /// The file name for a new segment: "<sequence>-<start>-<hash>.wpv".
    static std::string fileNameFor(SequenceId sequence, TimeRange range, std::string_view hash);

    /// Records a rendered segment (its file already in the folder): replaces entries of the sequence it overlaps
    /// (deleting their files when the name differs), then evicts the least recently used entries above the budget.
    /// Returns the replaced and evicted entries.
    std::vector<PreviewEntry> record(PreviewEntry entry);
    /// After an edit, undo or redo from `before`... to `after` (see the file comment). Returns the dropped entries.
    std::vector<PreviewEntry> invalidate(const ProjectState& after, const ChangeSet& changes);
    /// Removes the entries of `sequence` overlapping `range` (the whole sequence when nullopt; every sequence when
    /// `sequence` is invalid) and deletes their files. Returns them.
    std::vector<PreviewEntry> remove(SequenceId sequence, std::optional<TimeRange> range = {});
    /// C.16: deletes the files a delete could not remove earlier (on Windows a file the monitor's decoder still has open
    /// cannot be deleted). Every record, invalidate, remove, setBudget and save retries them too, and load() also sweeps
    /// preview files the index does not name. Returns how many are still there.
    std::size_t retryDeletes();

private:
    std::vector<PreviewEntry> evictLocked();
    void deleteFile(const PreviewEntry& entry) const;
    void deleteName(const std::string& name) const;
    void retryDeletesLocked() const;

    std::filesystem::path m_folder;
    mutable std::mutex m_mutex;
    std::int64_t m_budget;
    std::vector<PreviewEntry> m_entries; ///< sorted by (sequence, start)
    std::int64_t m_useCounter = 0;
    mutable std::vector<std::string> m_undeleted; ///< files a delete could not remove yet (retried)
};

} // namespace wabcut
