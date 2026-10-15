#include <wabcut/ai/RequestLog.h>

#include <chrono>
#include <cstdio>
#include <fstream>

namespace wabcut::ai {

std::string nowIso8601() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto day = std::chrono::floor<std::chrono::days>(now);
    const std::chrono::year_month_day ymd(day);
    const std::chrono::hh_mm_ss hms(now - day);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                  static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()));
    return buf;
}

Json RequestLogEntry::toJson() const {
    Json j = {{"kind", kind == Kind::Http ? "http" : "call"}, {"time", time}};
    if (kind == Kind::Http) {
        j["method"] = method;
        j["url"] = url;
        j["status"] = status;
        j["bytesSent"] = bytesSent;
        j["bytesReceived"] = bytesReceived;
        j["durationMs"] = durationMs;
        j["request"] = requestSummary;
    } else {
        j["provider"] = provider;
        j["model"] = model;
        j["capability"] = capability;
        j["durationMs"] = durationMs;
        j["usage"] = {{"inputTokens", usage.inputTokens},         {"outputTokens", usage.outputTokens},
                      {"cacheReadTokens", usage.cacheReadTokens}, {"cacheWriteTokens", usage.cacheWriteTokens},
                      {"reasoningTokens", usage.reasoningTokens}, {"audioMilliseconds", usage.audioMilliseconds},
                      {"characters", usage.characters}};
        j["costMicroUsd"] = cost;
        j["cost"] = formatUsd(cost);
    }
    if (!error.empty()) {
        j["error"] = error;
    }
    if (!detail.empty()) {
        j["detail"] = detail;
    }
    return j;
}

RequestLog::RequestLog(std::size_t capacity) : m_capacity(capacity == 0 ? 1 : capacity) {}

void RequestLog::add(RequestLogEntry entry) {
    std::function<void(const RequestLogEntry&)> callback;
    {
        std::lock_guard lock(m_mutex);
        if (!m_file.empty()) {
            std::ofstream out(m_file, std::ios::binary | std::ios::app);
            if (out) {
                out << entry.toJson().dump() << '\n';
            }
        }
        m_entries.push_back(entry);
        while (m_entries.size() > m_capacity) {
            m_entries.pop_front();
        }
        callback = m_onAppended;
    }
    if (callback) {
        callback(entry);
    }
}

std::vector<RequestLogEntry> RequestLog::entries() const {
    std::lock_guard lock(m_mutex);
    return {m_entries.begin(), m_entries.end()};
}

void RequestLog::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

void RequestLog::setFile(std::filesystem::path file) {
    std::lock_guard lock(m_mutex);
    m_file = std::move(file);
    if (!m_file.empty() && m_file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(m_file.parent_path(), ec);
    }
}

void RequestLog::setOnAppended(std::function<void(const RequestLogEntry&)> callback) {
    std::lock_guard lock(m_mutex);
    m_onAppended = std::move(callback);
}

} // namespace wabcut::ai
