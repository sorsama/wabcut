#pragma once

#include <wabcut/model/Error.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace wabcut::ai::detail {

// Atomically reserve a directory before extraction starts. Separate processes cannot claim the same directory;
// every file written by a job lives inside it and is removed on success, cancellation or extraction failure.
class TempWorkspace {
public:
    static std::expected<std::unique_ptr<TempWorkspace>, Error> create(const std::filesystem::path& root) {
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot create transcription work directory: " + ec.message()});
        }
        static std::atomic<std::uint64_t> counter{0};
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto path = root / ("job-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
            if (std::filesystem::create_directory(path, ec)) {
                auto workspace = std::unique_ptr<TempWorkspace>(new TempWorkspace(path));
#ifndef _WIN32
                std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace, ec);
                if (ec) {
                    return std::unexpected(Error{ErrorCode::IoError, "cannot restrict transcription work directory: " + ec.message()});
                }
#endif
                return workspace;
            }
            if (ec && ec != std::errc::file_exists) {
                return std::unexpected(Error{ErrorCode::IoError, "cannot reserve transcription work directory: " + ec.message()});
            }
        }
        return std::unexpected(Error{ErrorCode::IoError, "cannot reserve a unique transcription work directory"});
    }

    ~TempWorkspace() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;
    const std::filesystem::path& path() const { return m_path; }

private:
    explicit TempWorkspace(std::filesystem::path path) : m_path(std::move(path)) {}
    std::filesystem::path m_path;
};

} // namespace wabcut::ai::detail
