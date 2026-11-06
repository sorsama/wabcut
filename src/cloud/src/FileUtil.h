#pragma once

// File helpers of the cloud library (private): an atomic file writer for downloads and copies, and bounded reads.

#include <wabcut/cloud/RemoteStore.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace wabcut::cloud::detail {

/// Writes `target` through a temporary file in the same folder that is renamed into place by commit(); a writer that is
/// destroyed without commit() removes its temporary file, so a failed or cancelled download leaves nothing behind.
class AtomicFile {
public:
    static StoreResult<std::unique_ptr<AtomicFile>> create(const std::filesystem::path& target);
    ~AtomicFile();
    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;

    bool write(const char* data, std::size_t size);
    std::uint64_t written() const { return m_written; }
    /// Renames the finished file into place. `createOnly`: fails with PreconditionFailed when the target exists (atomically,
    /// where the file system can link).
    StoreResult<void> commit(bool createOnly = false);

private:
    AtomicFile() = default;
    std::filesystem::path m_target;
    std::filesystem::path m_temp;
    std::FILE* m_file = nullptr;
    std::uint64_t m_written = 0;
    bool m_failed = false;
};

/// The file's bytes, at most `maxBytes` (TooLarge past it), NotFound when it does not exist.
StoreResult<std::string> readFileBounded(const std::filesystem::path& file, std::uint64_t maxBytes);
/// SHA-256 of a file, streamed; calls `progress` (may be empty) every few MiB.
StoreResult<std::string> sha256File(const std::filesystem::path& file, const Progress& progress = {});

} // namespace wabcut::cloud::detail
