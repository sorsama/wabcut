#pragma once

// Local crash safety (I.7, docs/release.md "Crash reports"). Nothing is ever uploaded: a crash leaves a report in the
// app data folder for the next start to mention, and for the user to share or delete.
//
//   install()        once, first thing in main(): from then on a crash (access violation, stack overflow, abort,
//                    std::terminate, an invalid CRT parameter, a fatal signal on macOS/Linux) writes
//                      <folder>/wabcut-<UTC yyyymmdd-hhmmss>-<pid>.dmp    Windows minidump (MiniDumpWriteDump: stacks and
//                                                                         modules only; stack memory filtered to what
//                                                                         unwinding needs, module paths cut to file names)
//                      <folder>/wabcut-<UTC yyyymmdd-hhmmss>-<pid>.json   what happened, the version, the session's
//                                                                         diagnostics and the last log lines (redacted)
//                    and a session file (<folder>/sessions/<pid>.json) records that this process is running.
//   log()            every log line (the app's Qt message handler) into a fixed ring the report includes, redacted on
//                    the way in: the crash path never formats or allocates.
//   setDiagnostic()  facts for the diagnostic summary (GPU, driver, graphics API, decode backend), kept in the session
//                    file so even a crash that writes no dump has them.
//   markCleanExit()  on a normal exit: the session file goes, so the next start shows no notice for this run.
//
// The writer runs on its own thread, started by install(), which the crashing thread wakes and waits for: it has a
// whole stack even when the crash is a stack overflow. Everything the crash path writes is formatted into buffers
// allocated at install time. CrashReports.h reads the folder on the next start.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace wabcut::crash {

struct CrashOptions {
    /// Where reports and session files go, e.g. "%LOCALAPPDATA%\Wabcut\Wabcut\crashes". Created when missing.
    std::filesystem::path folder;
    /// "0.1.0 (abc1234def)".
    std::string version;
    /// How many recent log lines a report keeps (each up to kMaxLogLine bytes).
    std::size_t logLines = 200;
};

inline constexpr std::size_t kMaxLogLine = 400;

/// Installs the handlers. Idempotent: a second call only reports whether the first succeeded. Not thread-safe against
/// itself (call it from main() before other threads start).
bool install(const CrashOptions& options);
/// Whether install() succeeded in this process.
bool installed();
/// The folder install() writes to (empty before).
std::filesystem::path folder();

/// Appends one log line (redacted by redact()) to the ring a crash report includes. Thread-safe; cheap. Lines longer
/// than kMaxLogLine bytes are cut.
void log(std::string_view line);
/// Sets one diagnostic ("gpu", "driver", "graphicsApi", "decode", ...) of this session: shown in the next start's
/// diagnostic summary. Thread-safe; rewrites the session file.
void setDiagnostic(std::string_view key, std::string_view value);
/// Ends this session cleanly: removes its session file. The handlers stay installed.
void markCleanExit();

/// `line` without what must not leave this computer in a report: absolute paths (C:\..., \\server\share\..., /Users/,
/// /home/, ~/, file:// URLs) become "<path>" keeping only the extension, API keys and tokens (sk-..., Bearer ..., key=,
/// token=, api_key ..., AIza..., long hex or base64 runs) become "<redacted>", e-mail addresses "<email>", and the values
/// of environment variables whose names mention KEY, TOKEN, SECRET or PASSWORD (read at install) wherever they appear.
std::string redact(std::string_view line);
/// Secrets redact() replaces too, wherever they appear, before anything is cut (review wave 5, S5-P2-10): the app's
/// AI controller passes its key ring's (ai::Redactor::global()). Thread-safe; null removes it. Called by log() and
/// setDiagnostic(), never on the crash path.
void setSecretRedaction(std::function<std::string(std::string_view)> redactor);

namespace detail {
/// Tests: the crash path without crashing (writes a report for a synthetic exception code, as the handler would).
bool writeReportForTesting(unsigned long code, const char* what);
} // namespace detail

} // namespace wabcut::crash
