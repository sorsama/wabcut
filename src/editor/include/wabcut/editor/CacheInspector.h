#pragma once

// CacheInspector: disk usage of the regenerable data Wabcut keeps (proxies, thumbnails, waveform peaks, analysis
// results), per-cache size limits and "clean regenerable data". Files in use (the current project's proxies)
// are never deleted; everything else here can be rebuilt from the originals.
//
// Plain filesystem work: scan() and clean() may take a while on large caches, so callers run them off the GUI
// thread. Thread-safe as long as one instance is used by one thread at a time.

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace wabcut::editor {

struct CacheLocation {
    std::string id;   ///< stable: "proxies", "thumbnails", "peaks", "analysis"
    std::string name; ///< English UI name
    std::filesystem::path dir;
    /// Bytes the cache may keep; 0 = unlimited. enforceLimits() deletes the least recently written files beyond it.
    std::int64_t limitBytes = 0;
    /// Only files with one of these extensions belong to the cache (empty = every file under `dir`).
    std::vector<std::string> extensions;
};

struct CacheUsage {
    std::string id;
    std::string name;
    std::filesystem::path dir;
    std::int64_t bytes = 0;
    std::int64_t files = 0;
    std::int64_t inUseBytes = 0; ///< of `bytes`, files the project uses (never cleaned)
    std::int64_t limitBytes = 0;
};

class CacheInspector {
public:
    void setLocations(std::vector<CacheLocation> locations) { m_locations = std::move(locations); }
    const std::vector<CacheLocation>& locations() const { return m_locations; }
    void setLimit(const std::string& id, std::int64_t bytes);

    /// Sizes of every cache. `inUse`: files that must survive cleaning (compared by canonical, case-folded path). A
    /// cache folder that is a link, and links inside one, are never listed or cleaned (a project decides where its
    /// proxy folder is).
    std::vector<CacheUsage> scan(const std::set<std::filesystem::path>& inUse = {}) const;
    /// Deletes every file of cache `id` ("" = all caches) except those in use. Returns the bytes freed.
    std::int64_t clean(const std::string& id, const std::set<std::filesystem::path>& inUse = {}) const;
    /// Deletes the least recently written files of every cache over its limit, never one in use. Returns the
    /// bytes freed.
    std::int64_t enforceLimits(const std::set<std::filesystem::path>& inUse = {}) const;

private:
    std::vector<CacheLocation> m_locations;
};

/// "1.2 GB", "340 MB", "12 KB", "0 B".
std::string formatBytes(std::int64_t bytes);

} // namespace wabcut::editor
