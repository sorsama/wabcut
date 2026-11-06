#include <wabcut/cloud/CloudLock.h>

#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

namespace wabcut::cloud {

namespace {

using json = nlohmann::json;

std::string bounded(const json& j, const char* key, std::size_t limit = 256) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    std::string text = it->get<std::string>();
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

constexpr std::string_view kLockFormat = "wabcut-cloud-lock";

} // namespace

std::string toJson(const LockInfo& info) {
    json j{{"format", std::string(kLockFormat)}, {"version", 1}, {"session", info.session}, {"user", info.user},
           {"host", info.host}, {"since", info.since}, {"heartbeat", info.heartbeat}, {"appVersion", info.appVersion}};
    if (!info.tookOverFrom.empty()) {
        j["tookOverFrom"] = info.tookOverFrom;
    }
    return j.dump(2) + "\n";
}

std::optional<LockInfo> parseLock(std::string_view text) {
    if (text.size() > kMaxLockBytes || !io::jsonNestingWithin(text, 8)) {
        return std::nullopt;
    }
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || bounded(j, "format") != kLockFormat) {
        return std::nullopt;
    }
    LockInfo info;
    info.session = bounded(j, "session", 64);
    info.user = bounded(j, "user");
    info.host = bounded(j, "host");
    info.since = bounded(j, "since", 64);
    info.heartbeat = bounded(j, "heartbeat", 64);
    info.appVersion = bounded(j, "appVersion", 64);
    info.tookOverFrom = bounded(j, "tookOverFrom", 512);
    if (info.session.empty()) {
        return std::nullopt;
    }
    return info;
}

CloudLock::CloudLock(RemoteStore& store, std::string projectFolder, Identity who, std::string appVersion, UnixClock clock)
    : m_store(store),
      m_path(joinPath(projectFolder, "lock.json")),
      m_who(std::move(who)),
      m_appVersion(std::move(appVersion)),
      m_clock(std::move(clock)) {}

LockInfo CloudLock::mine(const std::string& since, const std::string& tookOver) const {
    LockInfo info;
    info.session = m_who.session;
    info.user = m_who.user;
    info.host = m_who.host;
    info.since = since;
    info.heartbeat = formatIsoUtc(m_clock());
    info.appVersion = m_appVersion;
    info.tookOverFrom = tookOver;
    return info;
}

StoreResult<std::optional<LockInfo>> CloudLock::peek(std::stop_token stop) {
    auto got = m_store.get(m_path, kMaxLockBytes, stop);
    if (!got) {
        if (got.error().kind == StoreFailure::NotFound) {
            return std::optional<LockInfo>();
        }
        return std::unexpected(got.error());
    }
    return parseLock(got->data);
}

StoreResult<LockOutcome> CloudLock::acquire(std::stop_token stop) {
    const std::string now = formatIsoUtc(m_clock());
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto got = m_store.get(m_path, kMaxLockBytes, stop);
        if (!got && got.error().kind != StoreFailure::NotFound) {
            return std::unexpected(got.error());
        }
        if (!got) {
            // No lock: create it, but only if nobody else does at the same time.
            const LockInfo info = mine(now, {});
            auto put = m_store.put(m_path, toJson(info), Precondition::absent(), "application/json", stop);
            if (!put) {
                if (put.error().kind == StoreFailure::PreconditionFailed) {
                    continue; // somebody was faster: look at their lock
                }
                return std::unexpected(put.error());
            }
            // Eventually consistent stores can accept two create-if-absent writes: read back whose lock stands.
            auto check = m_store.get(m_path, kMaxLockBytes, stop);
            if (!check) {
                return std::unexpected(check.error());
            }
            const auto stored = parseLock(check->data);
            if (!stored || stored->session != m_who.session) {
                continue;
            }
            m_etag = check->etag;
            m_since = now;
            m_held = true;
            return LockOutcome{};
        }
        const std::optional<LockInfo> theirs = parseLock(got->data);
        if (theirs && theirs->session == m_who.session) {
            // our own lock (this session asks again): renew it
            m_etag = got->etag;
            m_since = theirs->since.empty() ? now : theirs->since;
            m_held = true;
            if (auto renewed = refresh(stop); !renewed) {
                if (renewed.error().kind == StoreFailure::PreconditionFailed) {
                    continue;
                }
                return std::unexpected(renewed.error());
            }
            return LockOutcome{};
        }
        // somebody else's (or damaged) lock
        const std::optional<std::int64_t> beat = theirs ? parseIsoUtc(theirs->heartbeat) : std::nullopt;
        std::int64_t age = beat ? m_clock() - *beat : kLockStaleSeconds + 1;
        // A heartbeat ahead of this clock is the other computer's clock running fast (or a time zone slip), not a lock
        // nobody renews: it counts as just renewed. One more than a day ahead is no skew but a damaged file.
        constexpr std::int64_t kMaxClockSkewSeconds = 24 * 60 * 60;
        if (beat && age < 0) {
            age = -age <= kMaxClockSkewSeconds ? 0 : kLockStaleSeconds + 1;
        }
        // The same person on the same computer, another start of the program: the lock a crashed or closed Wabcut left. It
        // is not waited for (15 minutes of "held by you on this computer"); a live second start loses it on its next renewal.
        const bool myEarlierStart = theirs && !m_who.user.empty() && theirs->user == m_who.user && theirs->host == m_who.host;
        if (theirs && !myEarlierStart && age < kLockStaleSeconds) {
            LockOutcome out;
            out.kind = LockOutcome::Kind::HeldByOther;
            out.other = *theirs;
            out.ageSeconds = age;
            m_held = false;
            return out;
        }
        // stale (or damaged): take it over, once, with the ETag we saw
        const std::string last = theirs ? theirs->heartbeat : std::string("unknown");
        const std::string who = theirs ? theirs->user + " on " + theirs->host : std::string("someone");
        const LockInfo info = mine(now, who + ", last seen " + last);
        auto swapped = m_store.put(m_path, toJson(info), Precondition::matches(got->etag), "application/json", stop);
        if (!swapped) {
            if (swapped.error().kind == StoreFailure::PreconditionFailed) {
                continue; // somebody else took it over first
            }
            return std::unexpected(swapped.error());
        }
        m_etag = *swapped;
        m_since = now;
        m_held = true;
        LockOutcome out;
        out.kind = LockOutcome::Kind::TookOver;
        if (theirs) {
            out.other = *theirs;
        }
        out.ageSeconds = age;
        out.warning = "The editing lock of " + who + " had not been renewed since " + last +
                      ". It was taken over. If they are still working, their next save will be refused and nothing is lost; "
                      "the two copies are merged when they sync.";
        return out;
    }
    return std::unexpected(StoreError{StoreFailure::PreconditionFailed, "the lock keeps changing; try again in a moment"});
}

StoreResult<void> CloudLock::refresh(std::stop_token stop) {
    if (!m_held) {
        return std::unexpected(StoreError{StoreFailure::PreconditionFailed, "this session does not hold the lock"});
    }
    const LockInfo info = mine(m_since, {});
    auto put = m_store.put(m_path, toJson(info), Precondition::matches(m_etag), "application/json", stop);
    if (!put) {
        if (put.error().kind == StoreFailure::PreconditionFailed) {
            m_held = false; // taken over (or deleted): read-only from now on
        }
        return std::unexpected(put.error());
    }
    m_etag = *put;
    return {};
}

StoreResult<void> CloudLock::release(std::stop_token stop) {
    if (!m_held) {
        return {};
    }
    m_held = false;
    auto got = m_store.get(m_path, kMaxLockBytes, stop);
    if (!got) {
        return got.error().kind == StoreFailure::NotFound ? StoreResult<void>() : std::unexpected(got.error());
    }
    const auto theirs = parseLock(got->data);
    if (theirs && theirs->session == m_who.session) {
        return m_store.remove(m_path, stop);
    }
    return {};
}

} // namespace wabcut::cloud
