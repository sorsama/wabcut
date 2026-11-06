// FolderStore (RemoteStore.h): a folder on disk that the operating system's sync client (or a network share) mirrors.

#include <wabcut/cloud/RemoteStore.h>

#include "FileUtil.h"

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <system_error>

namespace wabcut::cloud {

namespace fs = std::filesystem;

namespace {

// One writer at a time inside this process; across computers the sync client decides (docs/cloud.md).
std::mutex& writeMutex() {
    static std::mutex mutex;
    return mutex;
}

constexpr std::uint64_t kHashLimit = 16u * 1024u * 1024u;

/// The ETag of a file: the hash of its content (files up to 16 MiB), else its size and time. Quoted like HTTP's.
std::string etagOfFile(const fs::path& file) {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(file, ec);
    if (ec) {
        return {};
    }
    if (size <= kHashLimit) {
        auto data = detail::readFileBounded(file, kHashLimit);
        if (data) {
            return "\"" + sha256Hex(*data).substr(0, 32) + "\"";
        }
    }
    const auto time = fs::last_write_time(file, ec);
    // An explicit int64: the file clock counts in __int128 on libc++, where std::to_string has no exact overload.
    const std::int64_t ticks = ec ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
    return "\"" + std::to_string(size) + "-" + std::to_string(ticks) + "\"";
}

std::int64_t unixSecondsOf(const fs::file_time_type& time) {
    const auto age = fs::file_time_type::clock::now() - time;
    const auto when = std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(age);
    return std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
}

bool junkName(const std::string& name) {
    return name.empty() || name.front() == '.' || name == "desktop.ini" || name == "Thumbs.db" || name == "$RECYCLE.BIN";
}

} // namespace

FolderStore::FolderStore(fs::path root) {
    std::error_code ec;
    m_root = fs::absolute(std::move(root), ec).lexically_normal();
}

std::string FolderStore::describe() const { return "Folder " + pathToUtf8(m_root); }

fs::path FolderStore::localPath(std::string_view path) const {
    if (path.empty()) {
        return m_root;
    }
    if (!validPath(path)) {
        return {};
    }
    fs::path out = m_root;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        out /= pathFromUtf8(std::string(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start)));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return out;
}

StoreResult<std::optional<StoreEntry>> FolderStore::stat(std::string_view path, std::stop_token) {
    const fs::path file = localPath(path);
    if (file.empty() || path.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::optional<StoreEntry>();
    }
    StoreEntry entry;
    entry.path = std::string(path);
    entry.size = fs::file_size(file, ec);
    entry.etag = etagOfFile(file);
    const auto time = fs::last_write_time(file, ec);
    entry.modified = ec ? 0 : unixSecondsOf(time);
    return std::optional<StoreEntry>(std::move(entry));
}

StoreResult<std::vector<StoreEntry>> FolderStore::list(std::string_view folder, std::stop_token) {
    const fs::path dir = localPath(folder);
    if (dir.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(folder)});
    }
    std::vector<StoreEntry> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = pathToUtf8(it->path().filename());
        if (!validSegment(name)) {
            continue; // not a name this library makes: never joined to a path
        }
        std::error_code entryEc;
        if (it->is_symlink(entryEc)) {
            continue; // links are never followed
        }
        StoreEntry entry;
        entry.path = joinPath(folder, name);
        if (it->is_directory(entryEc)) {
            entry.folder = true;
        } else if (it->is_regular_file(entryEc)) {
            entry.size = it->file_size(entryEc);
            entry.etag = etagOfFile(it->path());
            const auto time = it->last_write_time(entryEc);
            entry.modified = entryEc ? 0 : unixSecondsOf(time);
        } else {
            continue;
        }
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(), [](const StoreEntry& a, const StoreEntry& b) { return a.path < b.path; });
    return out;
}

StoreResult<Fetched> FolderStore::get(std::string_view path, std::uint64_t maxBytes, std::stop_token) {
    const fs::path file = localPath(path);
    if (file.empty() || path.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto data = detail::readFileBounded(file, maxBytes);
    if (!data) {
        return std::unexpected(data.error());
    }
    return Fetched{std::move(*data), etagOfFile(file)};
}

StoreResult<std::string> FolderStore::getToFile(std::string_view path, const fs::path& target, std::uint64_t maxBytes,
                                                const Progress& progress, std::stop_token stop) {
    const fs::path file = localPath(path);
    if (file.empty() || path.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::unexpected(StoreError{StoreFailure::NotFound, "no such object: " + std::string(path), 404});
    }
    const std::uint64_t total = fs::file_size(file, ec);
    if (total > maxBytes) {
        return std::unexpected(StoreError{StoreFailure::TooLarge, std::string(path) + " is larger than allowed"});
    }
    std::ifstream in(file, std::ios::binary);
    auto out = detail::AtomicFile::create(target);
    if (!in || !out) {
        return std::unexpected(out ? StoreError{StoreFailure::Other, "cannot read " + std::string(path)} : out.error());
    }
    std::string buffer(1u << 20, '\0');
    std::uint64_t done = 0;
    while (in) {
        if (stop.stop_requested()) {
            return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        if (!(*out)->write(buffer.data(), got)) {
            return std::unexpected(StoreError{StoreFailure::Other, "the file could not be written (is the disk full?)"});
        }
        done += got;
        if (progress && !progress(done, total)) {
            return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
        }
    }
    if (auto committed = (*out)->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return etagOfFile(file);
}

namespace {

StoreResult<void> checkPrecondition(const fs::path& file, const Precondition& precondition, std::string_view path) {
    if (precondition.kind == Precondition::Kind::None) {
        return {};
    }
    std::error_code ec;
    const bool exists = fs::is_regular_file(file, ec);
    if (precondition.kind == Precondition::Kind::Absent && exists) {
        return std::unexpected(StoreError{StoreFailure::PreconditionFailed, std::string(path) + " already exists", 412});
    }
    if (precondition.kind == Precondition::Kind::Etag && (!exists || etagOfFile(file) != precondition.etag)) {
        return std::unexpected(StoreError{StoreFailure::PreconditionFailed, std::string(path) + " has changed", 412});
    }
    return {};
}

} // namespace

StoreResult<std::string> FolderStore::put(std::string_view path, std::string_view data, const Precondition& precondition,
                                          std::string_view, std::stop_token) {
    const fs::path file = localPath(path);
    if (file.empty() || path.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::lock_guard lock(writeMutex());
    if (auto ok = checkPrecondition(file, precondition, path); !ok) {
        return std::unexpected(ok.error());
    }
    auto out = detail::AtomicFile::create(file);
    if (!out) {
        return std::unexpected(out.error());
    }
    if (!(*out)->write(data.data(), data.size())) {
        return std::unexpected(StoreError{StoreFailure::Other, "the file could not be written (is the disk full?)"});
    }
    if (auto committed = (*out)->commit(precondition.kind == Precondition::Kind::Absent); !committed) {
        return std::unexpected(committed.error());
    }
    return etagOfFile(file);
}

StoreResult<std::string> FolderStore::putFile(std::string_view path, const fs::path& source, const Precondition& precondition,
                                              std::string_view, const Progress& progress, std::stop_token stop) {
    const fs::path file = localPath(path);
    if (file.empty() || path.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        return std::unexpected(StoreError{StoreFailure::NotFound, "cannot open " + pathToUtf8(source)});
    }
    std::error_code ec;
    const std::uint64_t total = fs::file_size(source, ec);
    std::lock_guard lock(writeMutex());
    if (auto ok = checkPrecondition(file, precondition, path); !ok) {
        return std::unexpected(ok.error());
    }
    auto out = detail::AtomicFile::create(file);
    if (!out) {
        return std::unexpected(out.error());
    }
    std::string buffer(1u << 20, '\0');
    std::uint64_t done = 0;
    while (in) {
        if (stop.stop_requested()) {
            return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        if (!(*out)->write(buffer.data(), got)) {
            return std::unexpected(StoreError{StoreFailure::Other, "the file could not be written (is the disk full?)"});
        }
        done += got;
        if (progress && !progress(done, total)) {
            return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
        }
    }
    if (auto committed = (*out)->commit(precondition.kind == Precondition::Kind::Absent); !committed) {
        return std::unexpected(committed.error());
    }
    return etagOfFile(file);
}

StoreResult<void> FolderStore::remove(std::string_view path, std::stop_token) {
    const fs::path file = localPath(path);
    if (file.empty() || path.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::lock_guard lock(writeMutex());
    std::error_code ec;
    if (fs::is_symlink(file, ec) || !fs::is_regular_file(file, ec)) {
        return {};
    }
    if (!fs::remove(file, ec) || ec) {
        return std::unexpected(StoreError{StoreFailure::Other, "cannot delete " + std::string(path)});
    }
    // A folder that is left empty goes too (up to, not including, the root): a revoked review link leaves no folder.
    for (fs::path dir = file.parent_path(); dir != m_root && dir.has_parent_path() && fs::is_empty(dir, ec) && !ec; dir = dir.parent_path()) {
        fs::remove(dir, ec);
        if (ec) {
            break;
        }
    }
    return {};
}

StoreResult<void> FolderStore::makeFolder(std::string_view path, std::stop_token) {
    const fs::path dir = localPath(path);
    if (dir.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return std::unexpected(StoreError{StoreFailure::Other, "cannot make the folder " + pathToUtf8(dir)});
    }
    return {};
}

std::vector<std::string> FolderStore::oddNames(std::string_view folder) const {
    std::vector<std::string> out;
    const fs::path dir = localPath(folder);
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return out;
    }
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = pathToUtf8(it->path().filename());
        if (!validSegment(name) && !junkName(name)) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace wabcut::cloud
