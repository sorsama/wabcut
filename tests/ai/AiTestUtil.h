#pragma once

// Shared helpers for the ai suite.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace wabcut::ai::test {

/// A fresh, empty directory under the test output directory.
inline std::filesystem::path freshDir(const std::string& name) {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai" /
                                (name + "-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace wabcut::ai::test
