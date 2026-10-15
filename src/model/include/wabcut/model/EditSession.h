#pragma once

// EditSession: the current project snapshot plus undo/redo history.
//
// Threading: snapshot() may be called from any thread at any time (an atomic shared_ptr load; acquire it
// once per job, never on the audio callback). releasePending() may run on any thread (a worker or the
// job queue). Every other member (apply, commit, undo, redo, gestures, reset, listeners) belongs to one
// thread, the GUI thread. Undo/redo swap whole snapshots, so there are no inverse operations to get wrong.
//
// Ids: the session remembers the highest id ever handed out, outside the undo history, and starts every
// edit from it. Undoing an edit and then editing again therefore never reuses the ids of the undone
// branch. Save stateForSave(), which carries that high-water mark, rather than snapshot().
//
// Memory: the undo history is capped by entry count and by an estimate of the memory its snapshots
// keep alive (only the nodes not shared with the next revision count). Snapshots the session lets go
// (evicted undo steps, a dropped redo branch, intermediate gesture states) are queued for
// releasePending() instead of being freed on the GUI thread, when deferRelease is on.

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/platform/AtomicSharedPtr.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace wabcut {

using ProjectSnapshot = std::shared_ptr<const ProjectState>;

#ifdef NDEBUG
inline constexpr bool kValidateEditsByDefault = false;
#else
inline constexpr bool kValidateEditsByDefault = true;
#endif

struct EditSessionOptions {
    /// Maximum number of undo steps kept; the oldest are dropped first.
    std::size_t undoLimit = 500;
    /// Approximate bytes the undo history may keep alive; the oldest steps are dropped first (the most
    /// recent step is always kept).
    std::size_t undoMemoryBudget = std::size_t{512} * 1024 * 1024;
    /// Run validate() on every committed state and reject states that break an invariant.
    bool validateEdits = kValidateEditsByDefault;
    /// Queue snapshots the session lets go for releasePending() instead of freeing them in place.
    bool deferRelease = true;
};

enum class ChangeKind { Edit, Undo, Redo, Reset, GestureCancelled };

struct ChangeEvent {
    ChangeKind kind = ChangeKind::Edit;
    /// Label of the edit that was applied, undone or redone (the gesture label inside a gesture; empty
    /// for Reset).
    std::string_view label;
    /// The new current snapshot.
    ProjectSnapshot state;
    /// What changed from the previous snapshot.
    const ChangeSet& changes;
};

namespace detail {
inline const ProjectState& editedState(const ProjectState& state) { return state; }
template <class Result>
    requires requires(const Result& r) { r.state; }
const ProjectState& editedState(const Result& result) {
    return result.state;
}
} // namespace detail

class EditSession {
public:
    explicit EditSession(ProjectState initial = {}, EditSessionOptions options = {});
    ~EditSession();
    EditSession(const EditSession&) = delete;
    EditSession& operator=(const EditSession&) = delete;

    /// The current snapshot. Thread-safe; the returned state stays valid as long as it is held.
    ProjectSnapshot snapshot() const noexcept;

    /// Increases by one each time the current snapshot changes (edit, undo, redo, reset, cancelled
    /// gesture). Thread-safe. External clients (commands, agents) compare it to detect a stale view.
    std::uint64_t revision() const noexcept { return m_revision.load(std::memory_order_acquire); }

    /// Runs `fn(state)` and commits the state it returns. `fn` returns std::expected<T, Error> where T is
    /// ProjectState or has a `state` member (EditResult, Created<Id>); that outcome is returned (an
    /// EditResult carries its ChangeSet). `state` is the current snapshot with its id generator raised
    /// to the session's high-water mark. A failed or no-op edit leaves the session untouched.
    ///
    /// Each apply is one undo step, except inside a gesture (all applies form one step) or when
    /// `mergeKey` matches the previous apply's (slider drags): those coalesce until endMerge(),
    /// another key, undo, redo or reset.
    template <class Fn>
    auto apply(std::string_view label, Fn&& fn, std::string_view mergeKey = {})
        -> std::invoke_result_t<Fn&, const ProjectState&> {
        const ProjectSnapshot before = snapshot();
        const ProjectState* input = before.get();
        ProjectState raised;
        if (before->ids.next < m_idHighWater) {
            raised = *before;
            raised.ids.next = m_idHighWater;
            input = &raised;
        }
        auto outcome = std::invoke(fn, *input);
        if (!outcome) {
            return outcome;
        }
        if (auto committed = commitFrom(*input, label, detail::editedState(*outcome), mergeKey); !committed) {
            return std::unexpected(std::move(committed.error()));
        }
        return outcome;
    }

    /// Makes `next` the current state as one undo step (same rules as apply()) and returns what
    /// changed. Fails, changing nothing, when validation is on and `next` breaks an invariant.
    std::expected<ChangeSet, Error> commit(std::string_view label, const ProjectState& next,
                                           std::string_view mergeKey = {});

    bool canUndo() const noexcept { return !m_undo.empty(); }
    bool canRedo() const noexcept { return !m_redo.empty(); }
    /// Steps back; returns what changed, or nullopt when there is nothing to undo. Ends an open gesture.
    std::optional<ChangeSet> undo();
    std::optional<ChangeSet> redo();
    std::optional<std::string> undoLabel() const;
    std::optional<std::string> redoLabel() const;
    std::size_t undoCount() const noexcept { return m_undo.size(); }
    std::size_t redoCount() const noexcept { return m_redo.size(); }
    /// Estimated bytes the undo history keeps alive.
    std::size_t undoMemoryBytes() const noexcept { return m_undoBytes; }

    /// Ends the current merge group: the next edit starts a new undo step even with the same key.
    void endMerge() noexcept { m_mergeOpen = false; }

    /// Starts a gesture (a drag, a trim, a slider scrub): every apply until endGesture() becomes one undo
    /// step named `label`. Nested begin/end pairs join the outer gesture.
    void beginGesture(std::string label);
    void endGesture();
    /// Ends the gesture and returns to the state before it, without an undo step. Returns what changed.
    std::optional<ChangeSet> cancelGesture();
    bool inGesture() const noexcept { return m_gestureDepth > 0; }

    /// Replaces the state (new/opened project) and clears the history.
    void reset(ProjectState state);

    /// The lowest id the next edit may hand out (the high-water mark, never lowered by undo).
    std::uint64_t idHighWater() const noexcept { return m_idHighWater; }
    /// The current state with its id generator at the high-water mark: what to save.
    ProjectState stateForSave() const;

    /// Snapshots waiting to be freed. Thread-safe.
    std::size_t pendingReleases() const;
    /// Frees the queued snapshots on the calling thread; returns how many. Thread-safe: call it from a
    /// worker or the job queue so large snapshots are not torn down on the GUI thread.
    std::size_t releasePending();

    using Listener = std::function<void(const ChangeEvent&)>;
    using ListenerId = std::uint64_t;
    /// Listeners run on the mutating thread after the new snapshot is published, in the order added.
    ListenerId addListener(Listener listener);
    void removeListener(ListenerId id);

private:
    struct Step {
        std::string label;
        ProjectSnapshot state; // undo stack: state before the step; redo stack: state after it
        std::string mergeKey;
        std::size_t bytes = 0; // undo stack: estimated bytes `state` keeps alive beyond its successor
    };

    std::expected<ChangeSet, Error> commitFrom(const ProjectState& base, std::string_view label,
                                               const ProjectState& next, std::string_view mergeKey);
    void publish(ProjectSnapshot state, ChangeKind kind, std::string_view label, const ChangeSet& changes);
    void pushUndo(Step step, const ProjectState& successor);
    void enforceBudget();
    void retire(ProjectSnapshot snapshot);
    void raiseIdHighWater(const ProjectState& state) noexcept;

    platform::AtomicSharedPtr<const ProjectState> m_current;
    std::atomic<std::uint64_t> m_revision{0};
    EditSessionOptions m_options;
    std::deque<Step> m_undo;
    std::vector<Step> m_redo;
    std::size_t m_undoBytes = 0;
    bool m_mergeOpen = false;
    int m_gestureDepth = 0;
    std::string m_gestureLabel;
    bool m_gestureStarted = false;
    std::uint64_t m_idHighWater = 1;
    std::vector<std::pair<ListenerId, Listener>> m_listeners;
    ListenerId m_nextListenerId = 1;
    mutable std::mutex m_releaseMutex;
    std::vector<ProjectSnapshot> m_releaseQueue;
};

/// Estimated heap bytes `older` keeps alive that `newer` does not share (sequences, tracks, clip chunks,
/// media items that differ by pointer). Used for the undo memory budget.
std::size_t estimateExclusiveBytes(const ProjectState& older, const ProjectState& newer);

} // namespace wabcut
