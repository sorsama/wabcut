#pragma once

// Logging hook for the media module.
//
// The media module never writes to stdout/stderr. Its own diagnostics and everything FFmpeg logs through
// av_log() are routed to a single sink that the application installs (typically forwarding to spdlog). Until
// a sink is installed, messages are discarded.

#include <functional>
#include <string_view>

namespace wabcut::media {

enum class LogLevel { Trace, Debug, Info, Warning, Error };

/// Receives one complete log line (no trailing newline). It is called from whichever thread logged: decoder
/// threads, job workers and FFmpeg's internal worker threads, possibly concurrently. It must be thread-safe,
/// must not throw, and must not call back into the media module.
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

/// Installs the sink; an empty function discards all messages. Thread-safe.
void setLogSink(LogSink sink);

/// Messages below `minLevel` are dropped before they are formatted. The default is LogLevel::Warning.
/// Thread-safe.
void setLogLevel(LogLevel minLevel);
LogLevel logLevel();

} // namespace wabcut::media
