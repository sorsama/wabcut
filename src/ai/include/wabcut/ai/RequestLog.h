#pragma once

// The local, redacted request log (docs/ai.md: "a local redacted request log"): every HTTP exchange of the AI
// module and every completed AI call with its usage and cost, so users can audit what was sent where and what it
// cost. Nothing leaves the machine. Entries are kept in a bounded in-memory ring and optionally appended to a
// JSON-lines file. Every string in an entry has been through the Redactor before it gets here.

#include <wabcut/ai/Chat.h>
#include <wabcut/ai/ModelRegistry.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wabcut::ai {

struct RequestLogEntry {
    enum class Kind { Http, Call };

    Kind kind = Kind::Http;
    std::string time;          ///< ISO-8601 UTC
    // Http
    std::string method;
    std::string url;           ///< redacted
    int status = 0;            ///< 0 when no response
    std::int64_t bytesSent = 0;
    std::int64_t bytesReceived = 0;
    std::int64_t durationMs = 0;
    std::string requestSummary; ///< redacted, truncated request body ("<binary 12 MB>" for files)
    // Call
    std::string provider;
    std::string model;
    std::string capability;
    Usage usage;
    MicroUsd cost = 0;
    // Both
    std::string error;         ///< AiErrorCode name, empty on success
    std::string detail;        ///< redacted diagnostic

    Json toJson() const;
};

/// Thread-safe.
class RequestLog {
public:
    explicit RequestLog(std::size_t capacity = 1000);

    void add(RequestLogEntry entry);
    std::vector<RequestLogEntry> entries() const;
    void clear();

    /// Appends every new entry to `file` as one JSON line (empty path: memory only). The file is not truncated.
    void setFile(std::filesystem::path file);
    /// Called after each add(), outside the lock.
    void setOnAppended(std::function<void(const RequestLogEntry&)> callback);

    /// Longest request body excerpt kept in an entry.
    static constexpr std::size_t kMaxSummaryBytes = 2048;

private:
    mutable std::mutex m_mutex;
    std::size_t m_capacity;
    std::deque<RequestLogEntry> m_entries;
    std::filesystem::path m_file;
    std::function<void(const RequestLogEntry&)> m_onAppended;
};

/// The current time as ISO-8601 UTC with seconds ("2026-10-11T08:30:00Z").
std::string nowIso8601();

} // namespace wabcut::ai
