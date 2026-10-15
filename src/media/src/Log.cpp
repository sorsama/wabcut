#include "FfmpegUtil.h"

#include <wabcut/media/Log.h>

extern "C" {
#include <libavutil/log.h>
}

#include <atomic>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <string>

namespace wabcut::media {

namespace {

struct LogState {
    std::mutex mutex;
    std::shared_ptr<const LogSink> sink;
    std::atomic<bool> hasSink{false};
    std::atomic<int> minLevel{static_cast<int>(LogLevel::Warning)};
};

// Never destroyed: FFmpeg worker threads and detached jobs may still log while the process exits.
LogState& state() {
    static auto* s = new LogState();
    return *s;
}

LogLevel fromAvLevel(int level) {
    if (level <= AV_LOG_ERROR) {
        return LogLevel::Error;
    }
    if (level <= AV_LOG_WARNING) {
        return LogLevel::Warning;
    }
    if (level <= AV_LOG_INFO) {
        return LogLevel::Info;
    }
    if (level <= AV_LOG_DEBUG) {
        return LogLevel::Debug;
    }
    return LogLevel::Trace;
}

int toAvLevel(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return AV_LOG_TRACE;
    case LogLevel::Debug:
        return AV_LOG_DEBUG;
    case LogLevel::Info:
        return AV_LOG_INFO;
    case LogLevel::Warning:
        return AV_LOG_WARNING;
    case LogLevel::Error:
        return AV_LOG_ERROR;
    }
    return AV_LOG_WARNING;
}

void avLogCallback(void* avcl, int level, const char* fmt, va_list vl) {
    if (level < 0) { // AV_LOG_QUIET
        return;
    }
    const LogLevel ours = fromAvLevel(level);
    if (!detail::logEnabled(ours)) {
        return;
    }
    // FFmpeg sometimes emits one line in several calls; assemble per thread until the newline.
    thread_local int printPrefix = 1;
    thread_local std::string pending;
    char piece[1024];
    va_list copy;
    va_copy(copy, vl);
    av_log_format_line2(avcl, level, fmt, copy, piece, static_cast<int>(sizeof piece), &printPrefix);
    va_end(copy);
    pending += piece;
    const bool complete = !pending.empty() && pending.back() == '\n';
    if (!complete && pending.size() < 4096) {
        return;
    }
    while (!pending.empty() && (pending.back() == '\n' || pending.back() == '\r')) {
        pending.pop_back();
    }
    if (!pending.empty()) {
        detail::logMessage(ours, "ffmpeg: " + pending);
    }
    pending.clear();
}

} // namespace

void setLogSink(LogSink sink) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.hasSink.store(static_cast<bool>(sink));
    s.sink = sink ? std::make_shared<const LogSink>(std::move(sink)) : nullptr;
}

void setLogLevel(LogLevel minLevel) {
    state().minLevel.store(static_cast<int>(minLevel));
    av_log_set_level(toAvLevel(minLevel));
}

LogLevel logLevel() {
    return static_cast<LogLevel>(state().minLevel.load());
}

namespace detail {

void ensureFfmpegLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(toAvLevel(logLevel()));
        av_log_set_callback(avLogCallback);
    });
}

bool logEnabled(LogLevel level) {
    const LogState& s = state();
    return s.hasSink.load(std::memory_order_relaxed) &&
           static_cast<int>(level) >= s.minLevel.load(std::memory_order_relaxed);
}

void logMessage(LogLevel level, std::string_view message) {
    std::shared_ptr<const LogSink> sink;
    {
        LogState& s = state();
        std::lock_guard lock(s.mutex);
        sink = s.sink;
    }
    if (sink && *sink) {
        (*sink)(level, message);
    }
}

} // namespace detail

} // namespace wabcut::media
