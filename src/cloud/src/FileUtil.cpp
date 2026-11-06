#include "FileUtil.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <atomic>
#include <fstream>
#include <system_error>

namespace wabcut::cloud::detail {

namespace fs = std::filesystem;

namespace {
std::atomic<std::uint64_t> g_counter{0};
}

StoreResult<std::unique_ptr<AtomicFile>> AtomicFile::create(const fs::path& target) {
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return std::unexpected(StoreError{StoreFailure::Other, "cannot make the folder for " + pathToUtf8(target)});
        }
    }
    std::unique_ptr<AtomicFile> file(new AtomicFile());
    file->m_target = target;
    file->m_temp = target;
    file->m_temp += ".part" + std::to_string(g_counter.fetch_add(1));
#ifdef _WIN32
    file->m_file = _wfopen(file->m_temp.c_str(), L"wb");
#else
    file->m_file = std::fopen(file->m_temp.c_str(), "wb");
#endif
    if (!file->m_file) {
        return std::unexpected(StoreError{StoreFailure::Other, "cannot write " + pathToUtf8(target)});
    }
    return file;
}

AtomicFile::~AtomicFile() {
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
        std::error_code ec;
        fs::remove(m_temp, ec);
    }
}

bool AtomicFile::write(const char* data, std::size_t size) {
    if (!m_file || m_failed) {
        return false;
    }
    if (size > 0 && std::fwrite(data, 1, size, m_file) != size) {
        m_failed = true;
        return false;
    }
    m_written += size;
    return true;
}

StoreResult<void> AtomicFile::commit(bool createOnly) {
    if (!m_file || m_failed) {
        return std::unexpected(StoreError{StoreFailure::Other, "the file could not be written (is the disk full?)"});
    }
    const bool flushed = std::fflush(m_file) == 0;
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    if (!flushed || !closed) {
        std::error_code ec;
        fs::remove(m_temp, ec);
        return std::unexpected(StoreError{StoreFailure::Other, "the file could not be written (is the disk full?)"});
    }
    std::error_code ec;
    if (createOnly) {
        // A hard link of the finished file is an atomic "create if absent" on local disks and SMB shares: it fails when the
        // target exists. Where links are not supported (FAT, exFAT, some NAS) it falls back to check and rename.
        fs::create_hard_link(m_temp, m_target, ec);
        if (!ec) {
            std::error_code ignored;
            fs::remove(m_temp, ignored);
            return {};
        }
        if (ec == std::errc::file_exists) {
            std::error_code ignored;
            fs::remove(m_temp, ignored);
            return std::unexpected(StoreError{StoreFailure::PreconditionFailed, pathToUtf8(m_target.filename()) + " already exists", 412});
        }
        if (fs::exists(m_target, ec)) {
            std::error_code ignored;
            fs::remove(m_temp, ignored);
            return std::unexpected(StoreError{StoreFailure::PreconditionFailed, pathToUtf8(m_target.filename()) + " already exists", 412});
        }
        ec.clear();
    }
    fs::rename(m_temp, m_target, ec);
    if (ec) {
        fs::remove(m_temp, ec);
        return std::unexpected(StoreError{StoreFailure::Other, "cannot move the file into place: " + pathToUtf8(m_target)});
    }
    return {};
}

StoreResult<std::string> readFileBounded(const fs::path& file, std::uint64_t maxBytes) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::unexpected(StoreError{StoreFailure::NotFound, "no such file: " + pathToUtf8(file)});
    }
    const std::uint64_t size = fs::file_size(file, ec);
    if (ec) {
        return std::unexpected(StoreError{StoreFailure::Other, "cannot read " + pathToUtf8(file)});
    }
    if (size > maxBytes) {
        return std::unexpected(StoreError{StoreFailure::TooLarge, pathToUtf8(file.filename()) + " is larger than allowed"});
    }
    std::ifstream in(file, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !in.read(data.data(), static_cast<std::streamsize>(size))) {
        return std::unexpected(StoreError{StoreFailure::Other, "cannot read " + pathToUtf8(file)});
    }
    return data;
}

StoreResult<std::string> sha256File(const fs::path& file, const Progress& progress) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(StoreError{StoreFailure::NotFound, "cannot open " + pathToUtf8(file)});
    }
    std::error_code ec;
    const std::uint64_t total = fs::file_size(file, ec);
    ai::Sha256 hash;
    std::string buffer(1u << 20, '\0');
    std::uint64_t done = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(got)));
        done += static_cast<std::uint64_t>(got);
        if (progress && !progress(done, total)) {
            return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
        }
    }
    return hash.finishHex();
}

} // namespace wabcut::cloud::detail
