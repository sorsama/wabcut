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

private:
    void run(std::stop_token stop);

    EditSession& m_session;
    EditSession::ListenerId m_listener{};
    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_idle;
    std::uint64_t m_requested = 0; ///< pokes so far
    std::uint64_t m_served = 0;    ///< pokes the thread has answered
    std::size_t m_released = 0;
    std::jthread m_thread;         ///< last: started once everything above exists
};

} // namespace wabcut
