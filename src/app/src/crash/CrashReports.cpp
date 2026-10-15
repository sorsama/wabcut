#include "CrashReports.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif

namespace wabcut::crash {

namespace {

namespace fs = std::filesystem;

// UTF-8 text of a file name (wabcut_crash does not link the model's pathToUtf8): path::string() throws on Windows
// for a name outside the code page.
std::string nameOf(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::set<std::string> acknowledged(const fs::path& folder) {
    std::set<std::string> out;
    std::ifstream in(folder / "acknowledged.txt");
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            out.insert(line);
        }
    }
    return out;
}

std::optional<nlohmann::json> readJson(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::string stringOf(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::map<std::string, std::string> diagnosticsOf(const nlohmann::json& j) {
    std::map<std::string, std::string> out;
    if (const auto it = j.find("diagnostics"); it != j.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (value.is_string()) {
                out[key] = value.get<std::string>();
            }
        }
    }
    return out;
}

bool isReportName(const std::string& name) {
    return name.starts_with("wabcut-") && name.ends_with(".json");
}

} // namespace

bool processRunning(std::uint32_t pid) {
#ifdef _WIN32
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED; // it exists, just not ours to open
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::vector<CrashReport> pendingReports(const fs::path& folder, std::uint32_t currentPid) {
    std::vector<CrashReport> out;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        return out;
    }
    const std::set<std::string> seen = acknowledged(folder);
    std::set<std::uint32_t> crashedPids;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        const std::string name = nameOf(entry.path().filename());
        if (!entry.is_regular_file(ec) || !isReportName(name)) {
            continue;
        }
        const auto json = readJson(entry.path());
        if (!json || !json->is_object()) {
            continue;
        }
        const auto pid = json->value("pid", std::uint32_t{0});
        crashedPids.insert(pid);
        if (seen.contains(name)) {
            continue;
        }
        CrashReport r;
        r.report = entry.path();
        r.pid = pid;
        if (const std::string dump = stringOf(*json, "dump"); !dump.empty() && dump.find_first_of("/\\") == std::string::npos) {
            if (fs::exists(folder / dump, ec)) {
                r.dump = folder / dump;
            }
        }
        r.version = stringOf(*json, "version");
        r.time = stringOf(*json, "time");
        r.what = stringOf(*json, "what");
        r.diagnostics = diagnosticsOf(*json);
        out.push_back(std::move(r));
    }
    for (const auto& entry : fs::directory_iterator(folder / "sessions", ec)) {
        const std::string name = nameOf(entry.path().filename());
        if (!entry.is_regular_file(ec) || !name.ends_with(".json") || seen.contains("sessions/" + name)) {
            continue;
        }
        std::uint32_t pid = 0;
        try {
            pid = static_cast<std::uint32_t>(std::stoul(nameOf(entry.path().stem())));
        } catch (const std::exception&) {
            continue;
        }
        if (pid == currentPid || crashedPids.contains(pid) || processRunning(pid)) {
            continue; // this run, a crash already reported, or a run still going (another window)
        }
        const auto json = readJson(entry.path());
        CrashReport r;
        r.report = entry.path();
        r.unexpectedExit = true;
        r.pid = pid;
        if (json && json->is_object()) {
            r.version = stringOf(*json, "version");
            r.time = stringOf(*json, "started");
            r.diagnostics = diagnosticsOf(*json);
        }
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const CrashReport& a, const CrashReport& b) { return a.time > b.time; });
    return out;
}

void acknowledge(const fs::path& folder, const std::vector<CrashReport>& reports) {
    std::ofstream out(folder / "acknowledged.txt", std::ios::app);
    for (const CrashReport& r : reports) {
        std::error_code ec;
        if (r.unexpectedExit) {
            fs::remove(r.report, ec); // nothing to keep: a session file
            continue;
        }
        out << nameOf(r.report.filename()) << "\n";
        fs::remove(folder / "sessions" / (std::to_string(r.pid) + ".json"), ec); // the crashed run's session file
    }
}

void prune(const fs::path& folder, std::size_t keep, int staleDays) {
    std::error_code ec;
    std::vector<fs::path> reports;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && isReportName(nameOf(entry.path().filename()))) {
            reports.push_back(entry.path());
        }
    }
    // Names carry a UTC stamp: sorting by name sorts by time.
    std::sort(reports.begin(), reports.end(), std::greater<>());
    for (std::size_t i = keep; i < reports.size(); ++i) {
        fs::path stem = reports[i];
        stem.replace_extension();
        for (const char* ext : {".json", ".dmp", ".txt"}) {
            fs::remove(fs::path(stem).concat(ext), ec);
        }
    }
    const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * staleDays);
    for (const auto& entry : fs::directory_iterator(folder / "sessions", ec)) {
        if (entry.is_regular_file(ec) && entry.last_write_time(ec) < limit) {
            fs::remove(entry.path(), ec);
        }
    }
}

std::string diagnosticSummary(const CrashReport& report) {
    std::ostringstream s;
    s << "Wabcut " << (report.version.empty() ? std::string("(unknown version)") : report.version) << "\n";
    if (report.unexpectedExit) {
        s << "Closed unexpectedly (no crash report: ended from outside, power loss or a hang)";
    } else {
        s << "Crashed: " << (report.what.empty() ? std::string("unknown") : report.what);
    }
    if (!report.time.empty()) {
        s << " - " << report.time;
    }
    s << "\n";
    // Only these: the system, the GPU and the backends. Never paths, logs or project details.
    static constexpr std::pair<const char*, const char*> kFields[] = {
        {"os", "OS"}, {"cpu", "CPU"}, {"gpu", "GPU"}, {"driver", "Driver"}, {"graphicsApi", "Graphics API"},
        {"decode", "Decode"}, {"encoders", "Encoders"}, {"audio", "Audio backend"}, {"qt", "Qt"}};
    for (const auto& [key, label] : kFields) {
        if (const auto it = report.diagnostics.find(key); it != report.diagnostics.end() && !it->second.empty()) {
            s << label << ": " << it->second << "\n";
        }
    }
    return s.str();
}

} // namespace wabcut::crash
