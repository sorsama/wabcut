#pragma once

// SnapshotReleaser: frees the snapshots an EditSession lets go (EditSession::releasePending(): evicted undo steps, a
// dropped redo branch, merged slider states, the whole previous project on reset) on a thread of its own, soon after
// each change. Without one, a session with deferRelease (the default) keeps them until it is destroyed: memory grows
// with every edit beyond the undo limit and every project opened (P.1). With one, the GUI thread never pays for
// freeing a whole project either.
//
// Construct it on the session's thread after the session and destroy it before (it is a listener of the session, and
// its destructor frees what is still queued and joins its thread).

#include <wabcut/model/EditSession.h>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

namespace wabcut {

class SnapshotReleaser {
public:
    explicit SnapshotReleaser(EditSession& session);
    ~SnapshotReleaser();
    SnapshotReleaser(const SnapshotReleaser&) = delete;
    SnapshotReleaser& operator=(const SnapshotReleaser&) = delete;

    /// Something may be queued: free it soon. Any thread; the session's changes call it already.
    void poke();
    /// Blocks until everything queued before the call is freed (tests, shutdown).
    void flush();
    /// Snapshots freed so far.
    std::size_t released() const;

    /// F2P (0.6): frees what is queued now, then gives the heaps' freed memory back to the system (trimFreedMemory),
    /// on the releaser's thread. For after a project was closed or replaced. Any thread; returns at once.
    void releaseAndTrim();
    /// Trims releaseAndTrim() has run so far.
    std::size_t trims() const;
    /// Gives freed heap memory back to the system. Freeing a big project returns hundreds of MB of snapshot nodes to
    /// the process heaps, which keep those pages committed for reuse. Windows: HeapOptimizeResources on every heap (the
    /// low-fragmentation heap's caches) and _heapmin (the CRT heap's free ranges); glibc: malloc_trim; macOS: the
    /// malloc zones' pressure relief. Only wholly free pages go back: a page still holding a live block stays
    /// committed (docs/performance.md, "Memory after closing a project"). Any thread.
    static void trimFreedMemory();

private:
    void run(std::stop_token stop);

    EditSession& m_session;
    EditSession::ListenerId m_listener{};
    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_idle;
    std::uint64_t m_requested = 0; ///< pokes so far
    std::uint64_t m_served = 0;    ///< pokes the thread has answered
    std::uint64_t m_trimAfter = 0; ///< the poke whose pass ends with a trim (0: none asked)
    std::size_t m_released = 0;
    std::size_t m_trims = 0;
    std::jthread m_thread;         ///< last: started once everything above exists
};

} // namespace wabcut
