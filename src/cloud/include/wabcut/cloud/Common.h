#pragma once

// Shared pieces of the cloud library (0.7, card "shared review links, cloud projects on your own storage, stock media",
// docs/cloud.md): how a storage call fails, hashes and signatures, random tokens, names that are safe to put in a remote
// path, and a clock the tests can set.

#include <wabcut/ai/AiError.h>
#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::cloud {

/// Why a storage call failed.
enum class StoreFailure : std::uint8_t {
    Other,
    NotFound,
    PreconditionFailed, ///< the object is not what the write expected (someone else wrote it), or it exists and was not to
    Unauthorized,       ///< wrong or missing credentials, or no right to do this
    Offline,            ///< offline mode or the project's no-cloud lock blocked the request
    Network,
    TooLarge,
    Cancelled,
    Invalid, ///< the request itself is wrong (a name that is not allowed, a bad setting)
};

struct StoreError {
    StoreFailure kind = StoreFailure::Other;
    std::string message; ///< English, for logs and for the UI to wrap; never holds a secret
    int status = 0;      ///< the HTTP status, 0 when there was none

    StoreError() = default;
    StoreError(StoreFailure k, std::string m, int s = 0) : kind(k), message(std::move(m)), status(s) {}
};

template <class T>
using StoreResult = std::expected<T, StoreError>;

/// The shared error type for callers that speak wabcut::Error.
Error toError(const StoreError& error);
/// A transport failure as a storage failure.
StoreError fromTransport(const ai::AiError& error);
/// A failed HTTP answer as a storage failure (401/403 Unauthorized, 404 NotFound, 409/412 PreconditionFailed, 413
/// TooLarge, the rest Other). `what` names the action ("PUT head.json").
StoreError fromStatus(int status, std::string_view what);

// ---- hashes and encodings ----

std::string sha256Hex(std::string_view data);
/// HMAC-SHA-256 (RFC 2104): the 32 raw bytes.
std::string hmacSha256(std::string_view key, std::string_view data);
/// Lower-case hex of raw bytes.
std::string toHex(std::string_view raw);
std::string base64Encode(std::string_view raw);
/// RFC 4648 base32 without padding, lower case.
std::string base32Lower(std::string_view raw);
/// RFC 3986 percent-encoding: A-Z a-z 0-9 - . _ ~ stay; '/' stays unless `encodeSlash`; the rest is %XX (upper case).
std::string uriEncode(std::string_view text, bool encodeSlash);
/// Decodes %XX; nullopt on a bad escape or a decoded NUL.
std::optional<std::string> uriDecode(std::string_view text);

// ---- randomness ----

/// `count` random bytes. The default draws from the operating system's generator (std::random_device); tests pass a fixed one.
using RandomBytes = std::function<std::string(std::size_t count)>;
RandomBytes systemRandom();
/// A token of `bytes` random bytes in base32: 16 bytes = 128 bits = 26 characters. Used for review links and project ids.
std::string newToken(const RandomBytes& random, std::size_t bytes = 16);

// ---- names ----

/// A path segment a remote object may have: 1..128 of [A-Za-z0-9._-], not "." or "..", not starting with '.'.
bool validSegment(std::string_view segment);
/// A '/'-separated relative path of valid segments (at most 8 of them).
bool validPath(std::string_view path);
/// "a" + "b" = "a/b"; either side may be empty.
std::string joinPath(std::string_view a, std::string_view b);
/// The last segment.
std::string_view baseName(std::string_view path);
/// Everything before the last '/', "" for a single segment.
std::string_view parentOf(std::string_view path);

// ---- time ----

/// Seconds since 1970-01-01 UTC. Injectable so locks and expiry can be tested without waiting.
using UnixClock = std::function<std::int64_t()>;
UnixClock systemClock();
/// "2026-11-05T09:30:00Z".
std::string formatIsoUtc(std::int64_t unixSeconds);
/// The seconds of "YYYY-MM-DDTHH:MM:SSZ" (also without the 'T' and 'Z' variants of a date only: "YYYY-MM-DD" = midnight).
std::optional<std::int64_t> parseIsoUtc(std::string_view text);

/// The name of the person and computer holding a lock or writing a revision ("user", "host"), from the environment.
struct Identity {
    std::string user;
    std::string host;
    std::string session; ///< 16 hex digits, different for each program start
};
Identity currentIdentity(const RandomBytes& random = systemRandom());

} // namespace wabcut::cloud
