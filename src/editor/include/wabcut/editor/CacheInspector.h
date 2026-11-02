#pragma once

// CacheInspector: disk usage of the data Wabcut keeps (proxies, thumbnails, waveform peaks, analysis results),
// per-cache size limits and "clean regenerable data". Files in use (the current project's proxies and originals) are
// never deleted.
//
// Fix F2C (data safety): a cache only ever lists, and so only ever cleans, files Wabcut itself wrote there (CacheKind):
// a proxy folder may hold a user's own video or another project's proxies, so only the proxies its index records as
// Wabcut's (ProxyManager.h, indexedProxyFiles) count; the analysis cache lists only its own entry files. Analysis
// results that cannot be rebuilt from the media (AI object mask mattes, imported chat logs) are a cache of their own
// (CacheKind::KeptAnalysis) that "clean everything" leaves alone: it is cleaned only when asked for by id.
//
// Plain filesystem work: scan() and clean() may take a while on large caches, so callers run them off the GUI
// thread. Thread-safe as long as one instance is used by one thread at a time.

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor {

/// What a cache folder holds and therefore which of its files a cache may list, measure and delete (fix F2C).
enum class CacheKind {
    /// Every file under `dir` with one of `extensions` (thumbnails, waveforms: folders only Wabcut writes to).
    Files,
    /// Only the proxies Wabcut made: direct children of `dir` of the proxy file shape that the folder's index records
    /// as written by Wabcut's generator (indexedProxyFiles). Anything else there, a user's video included, is ignored.
    WabcutProxies,
    /// Analysis cache entries (<dir>/v<n>/<source>/<stream>/<analyser>/<entry>.json) that can be rebuilt from the media.
    Analysis,
    /// Analysis cache entries that cannot be rebuilt (kUnrebuildableAnalyzers). Never part of clean("").
    KeptAnalysis,
};

/// Analysers whose results cannot be made again from the original media (fix F2C): AI object mask mattes (tracked from
/// the user's prompts; ai::segment::kMatteAnalyzer) and imported chat logs (the highlights' chat analyser).
inline constexpr std::array<std::string_view, 2> kUnrebuildableAnalyzers{"matte", "highlights.chat"};

struct CacheLocation {
    std::string id;   ///< stable: "proxies", "thumbnails", "peaks", "analysis"
    std::string name; ///< English UI name
    std::filesystem::path dir;
    /// Bytes the cache may keep; 0 = unlimited. enforceLimits() deletes the least recently written files beyond it.
    std::int64_t limitBytes = 0;
    /// Only files with one of these extensions belong to the cache (empty = every file under `dir`).
    std::vector<std::string> extensions;
    CacheKind kind = CacheKind::Files;
};

struct CacheUsage {
    std::string id;
    std::string name;
    std::filesystem::path dir;
    std::int64_t bytes = 0;
    std::int64_t files = 0;
    std::int64_t inUseBytes = 0; ///< of `bytes`, files the project uses (never cleaned)
    std::int64_t limitBytes = 0;
    CacheKind kind = CacheKind::Files;
    std::int64_t inUseFiles = 0; ///< of `files`, those in use: clean(id) deletes `files - inUseFiles`
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
    /// Deletes every file of cache `id` except those in use. "" = every cache that can be rebuilt: a KeptAnalysis cache
    /// is cleaned only when named. Only the files the cache lists (CacheKind) are ever deleted. Returns the bytes freed.
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
