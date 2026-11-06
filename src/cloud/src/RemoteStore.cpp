// RemoteStore's shared helpers and the in-memory store (RemoteStore.h).

#include <wabcut/cloud/RemoteStore.h>

#include "FileUtil.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace wabcut::cloud {

namespace fs = std::filesystem;

StoreResult<std::vector<StoreEntry>> RemoteStore::listAll(std::string_view folder, std::size_t limit, std::stop_token stop) {
    std::vector<StoreEntry> out;
    std::vector<std::string> pending{std::string(folder)};
    while (!pending.empty()) {
        const std::string next = std::move(pending.back());
        pending.pop_back();
        auto children = list(next, stop);
        if (!children) {
            return std::unexpected(children.error());
        }
        for (StoreEntry& entry : *children) {
            if (entry.folder) {
                pending.push_back(entry.path);
                continue;
            }
            if (out.size() >= limit) {
                return std::unexpected(StoreError{StoreFailure::TooLarge, "more files than allowed in " + std::string(folder)});
            }
            out.push_back(std::move(entry));
        }
    }
    return out;
}

StoreResult<std::size_t> RemoteStore::removeTree(std::string_view folder, std::stop_token stop) {
    auto files = listAll(folder, 20000, stop);
    if (!files) {
        return std::unexpected(files.error());
    }
    std::size_t removed = 0;
    for (const StoreEntry& entry : *files) {
        if (auto done = remove(entry.path, stop); !done) {
            return std::unexpected(done.error());
        }
        ++removed;
    }
    return removed;
}

// ---- MemoryStore ----

namespace {

struct Object {
    std::string data;
    std::string etag;
    std::int64_t modified = 0;
};

std::string etagOf(std::string_view data) { return "\"" + sha256Hex(data).substr(0, 32) + "\""; }

struct Failure {
    std::string op;
    std::string path;
    StoreError error;
};

} // namespace

struct MemoryStore::Impl {
    mutable std::mutex mutex;
    std::map<std::string, Object, std::less<>> objects;
    std::map<std::string, std::size_t, std::less<>> calls;
    std::vector<Failure> failures;
    std::int64_t tick = 1;

    std::optional<StoreError> enter(std::string_view op, std::string_view path) {
        std::lock_guard lock(mutex);
        ++calls[std::string(op)];
        for (auto it = failures.begin(); it != failures.end(); ++it) {
            if (it->op == op && (it->path.empty() || it->path == path)) {
                StoreError error = it->error;
                failures.erase(it);
                return error;
            }
        }
        return std::nullopt;
    }
};

MemoryStore::MemoryStore() : m_impl(std::make_unique<Impl>()) {}
MemoryStore::~MemoryStore() = default;

void MemoryStore::failNext(std::string op, std::string path, StoreError error) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->failures.push_back({std::move(op), std::move(path), std::move(error)});
}

std::size_t MemoryStore::calls(std::string_view op) const {
    std::lock_guard lock(m_impl->mutex);
    const auto it = m_impl->calls.find(op);
    return it == m_impl->calls.end() ? 0 : it->second;
}

std::string MemoryStore::peek(std::string_view path) const {
    std::lock_guard lock(m_impl->mutex);
    const auto it = m_impl->objects.find(path);
    return it == m_impl->objects.end() ? std::string() : it->second.data;
}

StoreResult<std::optional<StoreEntry>> MemoryStore::stat(std::string_view path, std::stop_token) {
    if (auto failure = m_impl->enter("stat", path)) {
        return std::unexpected(*failure);
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::lock_guard lock(m_impl->mutex);
    const auto it = m_impl->objects.find(path);
    if (it == m_impl->objects.end()) {
        return std::optional<StoreEntry>();
    }
    return std::optional<StoreEntry>(StoreEntry{std::string(path), false, it->second.data.size(), it->second.etag, it->second.modified});
}

StoreResult<std::vector<StoreEntry>> MemoryStore::list(std::string_view folder, std::stop_token) {
    if (auto failure = m_impl->enter("list", folder)) {
        return std::unexpected(*failure);
    }
    if (!folder.empty() && !validPath(folder)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(folder)});
    }
    const std::string prefix = folder.empty() ? std::string() : std::string(folder) + "/";
    std::lock_guard lock(m_impl->mutex);
    std::vector<StoreEntry> out;
    std::set<std::string> folders;
    for (const auto& [path, object] : m_impl->objects) {
        if (path.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::string rest = path.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            out.push_back(StoreEntry{path, false, object.data.size(), object.etag, object.modified});
        } else {
            folders.insert(prefix + rest.substr(0, slash));
        }
    }
    for (const std::string& f : folders) {
        out.push_back(StoreEntry{f, true, 0, {}, 0});
    }
    return out;
}

StoreResult<Fetched> MemoryStore::get(std::string_view path, std::uint64_t maxBytes, std::stop_token) {
    if (auto failure = m_impl->enter("get", path)) {
        return std::unexpected(*failure);
    }
    std::lock_guard lock(m_impl->mutex);
    const auto it = m_impl->objects.find(path);
    if (it == m_impl->objects.end()) {
        return std::unexpected(StoreError{StoreFailure::NotFound, "no such object: " + std::string(path), 404});
    }
    if (it->second.data.size() > maxBytes) {
        return std::unexpected(StoreError{StoreFailure::TooLarge, std::string(path) + " is larger than allowed"});
    }
    return Fetched{it->second.data, it->second.etag};
}

StoreResult<std::string> MemoryStore::getToFile(std::string_view path, const fs::path& file, std::uint64_t maxBytes,
                                                const Progress& progress, std::stop_token stop) {
    auto fetched = get(path, maxBytes, stop);
    if (!fetched) {
        return std::unexpected(fetched.error());
    }
    if (progress && !progress(fetched->data.size(), fetched->data.size())) {
        return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
    }
    auto out = detail::AtomicFile::create(file);
    if (!out) {
        return std::unexpected(out.error());
    }
    if (!(*out)->write(fetched->data.data(), fetched->data.size())) {
        return std::unexpected(StoreError{StoreFailure::Other, "the file could not be written"});
    }
    if (auto done = (*out)->commit(); !done) {
        return std::unexpected(done.error());
    }
    return fetched->etag;
}

StoreResult<std::string> MemoryStore::put(std::string_view path, std::string_view data, const Precondition& precondition,
                                          std::string_view, std::stop_token) {
    if (auto failure = m_impl->enter("put", path)) {
        return std::unexpected(*failure);
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    if (beforeWrite) {
        beforeWrite("put", path);
    }
    std::lock_guard lock(m_impl->mutex);
    const auto it = m_impl->objects.find(path);
    if (precondition.kind == Precondition::Kind::Absent && it != m_impl->objects.end()) {
        return std::unexpected(StoreError{StoreFailure::PreconditionFailed, std::string(path) + " already exists", 412});
    }
    if (precondition.kind == Precondition::Kind::Etag && (it == m_impl->objects.end() || it->second.etag != precondition.etag)) {
        return std::unexpected(StoreError{StoreFailure::PreconditionFailed, std::string(path) + " has changed", 412});
    }
    Object object{std::string(data), etagOf(data), m_impl->tick++};
    const std::string etag = object.etag;
    m_impl->objects[std::string(path)] = std::move(object);
    return etag;
}

StoreResult<std::string> MemoryStore::putFile(std::string_view path, const fs::path& file, const Precondition& precondition,
                                              std::string_view contentType, const Progress& progress, std::stop_token stop) {
    auto data = detail::readFileBounded(file, std::uint64_t{1} << 30);
    if (!data) {
        return std::unexpected(data.error());
    }
    if (progress && !progress(data->size(), data->size())) {
        return std::unexpected(StoreError{StoreFailure::Cancelled, "cancelled"});
    }
    return put(path, *data, precondition, contentType, stop);
}

StoreResult<void> MemoryStore::remove(std::string_view path, std::stop_token) {
    if (auto failure = m_impl->enter("remove", path)) {
        return std::unexpected(*failure);
    }
    if (beforeWrite) {
        beforeWrite("remove", path);
    }
    std::lock_guard lock(m_impl->mutex);
    if (const auto it = m_impl->objects.find(path); it != m_impl->objects.end()) {
        m_impl->objects.erase(it);
    }
    return {};
}

StoreResult<void> MemoryStore::makeFolder(std::string_view path, std::stop_token) {
    if (auto failure = m_impl->enter("makeFolder", path)) {
        return std::unexpected(*failure);
    }
    return {};
}

} // namespace wabcut::cloud
