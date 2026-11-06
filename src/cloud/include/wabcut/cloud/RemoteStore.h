#pragma once

// RemoteStore: the one storage seam of the cloud library. A store is a folder tree on storage the user owns: a folder on
// disk that a sync client (OneDrive, Dropbox, Google Drive, a network share) mirrors, a WebDAV collection, or an
// S3-compatible bucket. Everything above it - cloud project locks and revisions, review links - talks to it only through
// this interface, so it is tested with an in-memory store and with fake transports (no test reaches the internet).
//
// Paths are relative to the store's root, '/'-separated, every segment valid (Common.h validSegment): a name that came
// from a remote listing is never joined to a local path without that check. Writes can carry a Precondition - the object
// must not exist (create), or must still have the ETag the caller read (compare-and-swap) - which is how locks and
// head.json stay safe when two editors write at once. A folder store cannot do that atomically across computers (it
// checks right before the atomic rename; the sync client decides the rest) - docs/cloud.md says so.

#include <wabcut/cloud/Common.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::cloud {

struct StoreEntry {
    std::string path;          ///< relative to the store's root
    bool folder = false;
    std::uint64_t size = 0;    ///< bytes, files only
    std::string etag;          ///< opaque; compare for equality only (HTTP stores keep the quotes)
    std::int64_t modified = 0; ///< unix seconds, 0 when unknown
};

/// What a write expects to find.
struct Precondition {
    enum class Kind : std::uint8_t { None, Absent, Etag };
    Kind kind = Kind::None;
    std::string etag;

    static Precondition none() { return {}; }
    /// The object must not exist yet.
    static Precondition absent() { return {Kind::Absent, {}}; }
    /// The object must still have this ETag.
    static Precondition matches(std::string etag) { return {Kind::Etag, std::move(etag)}; }
};

struct Fetched {
    std::string data;
    std::string etag;
};

/// Transfer progress; returning false cancels the transfer.
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

class RemoteStore {
public:
    virtual ~RemoteStore() = default;

    /// For the UI: "WebDAV https://cloud.example.com/dav/wabcut", "S3 bucket reviews (eu-west-1)", "Folder D:/Dropbox/Wabcut".
    virtual std::string describe() const = 0;
    /// "folder", "webdav" or "s3".
    virtual std::string kind() const = 0;

    /// The object at `path`, nullopt when there is none.
    virtual StoreResult<std::optional<StoreEntry>> stat(std::string_view path, std::stop_token stop = {}) = 0;
    /// The direct children of folder `folder` ("" = the root), files and folders; none when it does not exist.
    virtual StoreResult<std::vector<StoreEntry>> list(std::string_view folder, std::stop_token stop = {}) = 0;
    /// The object's bytes (at most `maxBytes`, else TooLarge) and ETag. NotFound when it does not exist.
    virtual StoreResult<Fetched> get(std::string_view path, std::uint64_t maxBytes, std::stop_token stop = {}) = 0;
    /// Downloads to `file` (a temporary file in its folder, renamed into place when complete). Returns the ETag.
    virtual StoreResult<std::string> getToFile(std::string_view path, const std::filesystem::path& file, std::uint64_t maxBytes,
                                               const Progress& progress = {}, std::stop_token stop = {}) = 0;
    /// Writes `data`, creating missing folders. Returns the new ETag. PreconditionFailed when `precondition` does not hold.
    virtual StoreResult<std::string> put(std::string_view path, std::string_view data, const Precondition& precondition,
                                         std::string_view contentType = "application/octet-stream", std::stop_token stop = {}) = 0;
    /// Uploads a local file (streamed; never read into memory whole).
    virtual StoreResult<std::string> putFile(std::string_view path, const std::filesystem::path& file,
                                             const Precondition& precondition, std::string_view contentType = "application/octet-stream",
                                             const Progress& progress = {}, std::stop_token stop = {}) = 0;
    /// Deletes an object; a missing one is not an error.
    virtual StoreResult<void> remove(std::string_view path, std::stop_token stop = {}) = 0;
    /// Makes a folder (and its parents). Stores without folders (S3) do nothing.
    virtual StoreResult<void> makeFolder(std::string_view path, std::stop_token stop = {}) = 0;

    /// Every file below `folder`, recursively (at most `limit`; TooLarge past it).
    StoreResult<std::vector<StoreEntry>> listAll(std::string_view folder, std::size_t limit = 20000, std::stop_token stop = {});
    /// Deletes every file below `folder`. Returns how many.
    StoreResult<std::size_t> removeTree(std::string_view folder, std::stop_token stop = {});
};

// ---- the stores ----

/// A tree in memory (tests, and the "two editors" fixture). Thread-safe. Its ETags look like HTTP's (quoted).
class MemoryStore final : public RemoteStore {
public:
    MemoryStore();
    ~MemoryStore() override;

    std::string describe() const override { return "Memory"; }
    std::string kind() const override { return "memory"; }
    StoreResult<std::optional<StoreEntry>> stat(std::string_view path, std::stop_token stop = {}) override;
    StoreResult<std::vector<StoreEntry>> list(std::string_view folder, std::stop_token stop = {}) override;
    StoreResult<Fetched> get(std::string_view path, std::uint64_t maxBytes, std::stop_token stop = {}) override;
    StoreResult<std::string> getToFile(std::string_view path, const std::filesystem::path& file, std::uint64_t maxBytes,
                                       const Progress& progress = {}, std::stop_token stop = {}) override;
    StoreResult<std::string> put(std::string_view path, std::string_view data, const Precondition& precondition,
                                 std::string_view contentType = "application/octet-stream", std::stop_token stop = {}) override;
    StoreResult<std::string> putFile(std::string_view path, const std::filesystem::path& file, const Precondition& precondition,
                                     std::string_view contentType = "application/octet-stream", const Progress& progress = {},
                                     std::stop_token stop = {}) override;
    StoreResult<void> remove(std::string_view path, std::stop_token stop = {}) override;
    StoreResult<void> makeFolder(std::string_view path, std::stop_token stop = {}) override;

    /// Tests: runs inside put() / remove() before the precondition is looked at - "another editor wrote just now".
    std::function<void(std::string_view op, std::string_view path)> beforeWrite;
    /// Tests: a failure to return from the next calls of `op` ("get", "put", ...) for `path` ("" = any); consumed once.
    void failNext(std::string op, std::string path, StoreError error);
    /// Number of calls per operation, for assertions.
    std::size_t calls(std::string_view op) const;
    /// The text of an object ("" when missing), without going through the interface.
    std::string peek(std::string_view path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// A folder on disk. `root` is created when missing.
class FolderStore final : public RemoteStore {
public:
    explicit FolderStore(std::filesystem::path root);

    const std::filesystem::path& root() const { return m_root; }
    std::string describe() const override;
    std::string kind() const override { return "folder"; }
    StoreResult<std::optional<StoreEntry>> stat(std::string_view path, std::stop_token stop = {}) override;
    StoreResult<std::vector<StoreEntry>> list(std::string_view folder, std::stop_token stop = {}) override;
    StoreResult<Fetched> get(std::string_view path, std::uint64_t maxBytes, std::stop_token stop = {}) override;
    StoreResult<std::string> getToFile(std::string_view path, const std::filesystem::path& file, std::uint64_t maxBytes,
                                       const Progress& progress = {}, std::stop_token stop = {}) override;
    StoreResult<std::string> put(std::string_view path, std::string_view data, const Precondition& precondition,
                                 std::string_view contentType = "application/octet-stream", std::stop_token stop = {}) override;
    StoreResult<std::string> putFile(std::string_view path, const std::filesystem::path& file, const Precondition& precondition,
                                     std::string_view contentType = "application/octet-stream", const Progress& progress = {},
                                     std::stop_token stop = {}) override;
    StoreResult<void> remove(std::string_view path, std::stop_token stop = {}) override;
    StoreResult<void> makeFolder(std::string_view path, std::stop_token stop = {}) override;

    /// The local file or folder of a store path (checked: nothing outside the root); empty for an invalid path.
    std::filesystem::path localPath(std::string_view path) const;
    /// The names in `folder` that this library never makes - what a sync client leaves when two computers wrote the same
    /// file ("head (conflicted copy).json", "head-DESKTOP-4F2.json") - without the usual junk (dot files, desktop.ini).
    std::vector<std::string> oddNames(std::string_view folder) const;

private:
    std::filesystem::path m_root;
};

} // namespace wabcut::cloud
