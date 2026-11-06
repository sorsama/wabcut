#pragma once

// The editing lock of a cloud project (docs/cloud.md, "Locking"): one editor at a time, found in `<project>/lock.json` on the
// project's storage. Acquiring a lock is a create-if-absent write; renewing it every minute is a compare-and-swap write with the
// ETag of the last write, so a lock somebody took over cannot be renewed by its old holder (that session goes read-only at once).
// A lock that has not been renewed for 15 minutes is stale: the next editor may take it over, and is told whose it was and since
// when. The lock is advisory (it keeps well-behaved editors apart); the compare-and-swap of head.json is what protects the data.
//
// Clock skew: the age of a lock is measured with the reader's clock against the writer's heartbeat time, so computers whose clocks
// differ by many minutes can judge a live lock stale; docs/cloud.md says to keep the clocks right (any time service does).

#include <wabcut/cloud/RemoteStore.h>

#include <optional>
#include <string>

namespace wabcut::cloud {

inline constexpr std::int64_t kLockStaleSeconds = 15 * 60;
inline constexpr std::int64_t kLockRefreshSeconds = 60;
inline constexpr std::uint64_t kMaxLockBytes = 64 * 1024;

struct LockInfo {
    std::string session;     ///< 16 hex digits: one program start
    std::string user;
    std::string host;
    std::string since;       ///< ISO-8601 UTC: when this session first held it
    std::string heartbeat;   ///< ISO-8601 UTC: the last renewal
    std::string appVersion;
    std::string tookOverFrom; ///< "user on host, last seen 2026-11-05T09:12:00Z", set when this lock replaced a stale one
    bool operator==(const LockInfo&) const = default;
};

std::string toJson(const LockInfo& info);
/// Nullopt when the text is not a lock (also for oversized or hostile text).
std::optional<LockInfo> parseLock(std::string_view text);

struct LockOutcome {
    enum class Kind : std::uint8_t {
        Acquired,    ///< this session holds the lock now
        TookOver,    ///< ... after a stale one (see `warning` and `other`)
        HeldByOther, ///< somebody else is editing: `other` says who and since when
    };
    Kind kind = Kind::Acquired;
    LockInfo other;         ///< the other holder, for HeldByOther and TookOver
    std::string warning;    ///< for TookOver: the sentence to show
    std::int64_t ageSeconds = 0; ///< how long ago `other` last renewed
};

class CloudLock {
public:
    CloudLock(RemoteStore& store, std::string projectFolder, Identity who, std::string appVersion = {}, UnixClock clock = systemClock());

    /// Takes the lock, renews our own, or takes over a stale one; HeldByOther when a live editor has it.
    StoreResult<LockOutcome> acquire(std::stop_token stop = {});
    /// Renews the heartbeat. PreconditionFailed when the lock is no longer ours (somebody took it over): the caller makes the
    /// project read-only.
    StoreResult<void> refresh(std::stop_token stop = {});
    /// Gives the lock up (only when it is still ours).
    StoreResult<void> release(std::stop_token stop = {});
    /// Reads the lock without changing it: nullopt when there is none.
    StoreResult<std::optional<LockInfo>> peek(std::stop_token stop = {});

    bool held() const { return m_held; }
    const std::string& path() const { return m_path; }

private:
    LockInfo mine(const std::string& since, const std::string& tookOver) const;

    RemoteStore& m_store;
    std::string m_path;
    Identity m_who;
    std::string m_appVersion;
    UnixClock m_clock;
    std::string m_etag; ///< of our last write
    std::string m_since;
    bool m_held = false;
};

} // namespace wabcut::cloud
