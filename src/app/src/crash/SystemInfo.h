#pragma once

// What the diagnostic summary says about this computer (CrashReports.h): the operating system, the CPU, and the primary
// GPU with its driver version. Nothing that identifies the user (no host or user names, no paths, no serial numbers).

#include <map>
#include <string>

namespace wabcut::crash {

/// {"os": "Windows 11 (10.0.26200)", "cpu": "16 threads", "gpu": "NVIDIA GeForce RTX 3060", "driver": "32.0.15.6094"};
/// keys whose value is unknown on this platform are left out (macOS/Linux report no driver version here).
std::map<std::string, std::string> systemDiagnostics();

} // namespace wabcut::crash
