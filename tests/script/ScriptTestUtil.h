#pragma once

// Helpers for the extension tests: extension folders written from a manifest and sources, and a host delegate that
// records what the extension says and lets a test pump until something happened.

#include <wabcut/script/Host.h>
#include <wabcut/script/Manifest.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wabcut::script::test {

namespace fs = std::filesystem;

/// A fresh folder under the test output: <output>/script/<name>.
inline fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "script" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

inline void writeFile(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

inline Json manifest(const std::string& id, const std::string& name, const std::vector<std::string>& capabilities,
                     const std::string& main = "main.js") {
    return Json{{"manifestVersion", 1}, {"id", id},           {"name", name},
                {"version", "1.0.0"},   {"author", "Tests"}, {"licence", "MIT"},
                {"minWabcutVersion", "0.1.0"}, {"capabilities", capabilities}, {"main", main}};
}

/// Writes an extension folder: the manifest and `files` (path -> source).
inline fs::path writeExtension(const fs::path& folder, const Json& manifestJson,
                               const std::map<std::string, std::string>& files) {
    writeFile(folder / "wabcut-extension.json", manifestJson.dump(2));
    for (const auto& [path, source] : files) {
        writeFile(folder / path, source);
    }
    return folder;
}

/// Records everything; wait() returns when the host woke it (or after `max`).
class RecordingDelegate : public HostDelegate {
public:
    void wake() override {
        {
            std::lock_guard lock(m_mutex);
            m_woken = true;
        }
        m_cv.notify_all();
    }
    void notify(const Notification& n) override { notifications.push_back(n); }
    void prompt(const PromptRequest& p) override { prompts.push_back(p); }
    void promptClosed(std::uint64_t id) override { closedPrompts.push_back(id); }
    void commandsChanged() override { ++commandsChangedCount; }
    void stopped(const StopReport& r) override { stops.push_back(r); }
    void log(const LogLine& l) override { logs.push_back(l); }
    bool runLongCommand(LongCommand job) override {
        if (!acceptLongCommands) {
            return false;
        }
        longCommands.push_back(std::move(job));
        return true;
    }

    void wait(std::chrono::milliseconds max) {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, max, [this] { return m_woken; });
        m_woken = false;
    }

    std::vector<Notification> notifications;
    std::vector<PromptRequest> prompts;
    std::vector<std::uint64_t> closedPrompts;
    int commandsChangedCount = 0;
    std::vector<StopReport> stops;
    std::vector<LogLine> logs;
    bool acceptLongCommands = false; ///< take model commands (as the app's job worker does) instead of refusing them
    std::vector<LongCommand> longCommands;

    bool logged(const std::string& part) const {
        for (const LogLine& l : logs) {
            if (l.message.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_woken = false;
};

/// Pumps `host` until `done()` holds or `timeout` passes; true when it held.
inline bool pumpUntil(ExtensionHost& host, RecordingDelegate& delegate, const std::function<bool()>& done,
                      std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        delegate.wait(std::chrono::milliseconds(20));
        host.pump();
    }
    return true;
}

/// Runs a registered command and waits for its result.
inline CallResult call(ExtensionHost& host, RecordingDelegate& delegate, const std::string& command,
                       Json args = Json::object(), std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    std::optional<CallResult> result;
    host.invoke(command, std::move(args), [&](const CallResult& r) { result = r; });
    if (!pumpUntil(host, delegate, [&] { return result.has_value(); }, timeout)) {
        FAIL("the call to " << command << " did not finish in time");
    }
    return *result;
}

} // namespace wabcut::script::test
