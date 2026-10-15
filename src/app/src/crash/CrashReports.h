#pragma once

// The next start's view of the crash folder (CrashHandler.h): which runs ended badly since the user last looked, and the
// short diagnostic summary the "Wabcut closed unexpectedly" notice copies. Qt-free; nothing here sends anything.
//
//   <folder>/wabcut-<stamp>-<pid>.json   a crash report (+ .dmp on Windows, .txt backtrace on macOS/Linux)
//   <folder>/sessions/<pid>.json         a run in progress, or one that never reached a clean exit (killed, power
//                                        loss, a hang ended from the task manager): no report, but still worth a notice
//   <folder>/acknowledged.txt            report and session file names the user has dismissed

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace wabcut::crash {

struct CrashReport {
    std::filesystem::path report;   ///< the .json (a crash report, or a stale session file)
    std::filesystem::path dump;     ///< the .dmp / backtrace beside it; empty when none was written
    std::string version;            ///< "0.1.0 (abc1234def)"
    std::string time;               ///< ISO 8601 UTC, when it crashed (or started, for an unexpected exit)
    std::string what;               ///< "access violation", ... ; empty for an unexpected exit
    std::map<std::string, std::string> diagnostics; ///< gpu, driver, graphicsApi, decode, os, ...
    bool unexpectedExit = false;    ///< a session that ended without a crash report or a clean exit
    std::uint32_t pid = 0;          ///< the process that ended
};

/// Runs that ended badly and that the user has not dismissed, newest first: crash reports, and session files of
/// processes no longer running (`currentPid`'s own session is skipped; a session with a crash report is reported once,
/// as the crash).
std::vector<CrashReport> pendingReports(const std::filesystem::path& folder, std::uint32_t currentPid);

/// Dismisses `reports` (pendingReports() afterwards no longer lists them). Stale session files are deleted; crash reports
/// and dumps stay for the user to share or delete.
void acknowledge(const std::filesystem::path& folder, const std::vector<CrashReport>& reports);

/// Keeps the newest `keep` crash reports (with their dumps) and deletes older ones, and session files older than
/// `staleDays` days that nothing reports any more. Called at startup.
void prune(const std::filesystem::path& folder, std::size_t keep = 10, int staleDays = 30);

/// The text "Copy diagnostic summary" puts on the clipboard: only the product and version, the operating system, the
/// GPU and its driver, and the graphics and decode backends, plus what happened and when. No paths, no log, no project.
std::string diagnosticSummary(const CrashReport& report);

/// Whether a process with id `pid` is running.
bool processRunning(std::uint32_t pid);

} // namespace wabcut::crash
