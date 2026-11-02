#include <wabcut/editor/CacheInspector.h>

#include <wabcut/editor/ProxyManager.h>
#include <wabcut/model/MediaLocator.h>

#include "PathSafety.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>

namespace wabcut::editor {

namespace {

struct CacheFile {
    std::filesystem::path path;
    std::int64_t bytes = 0;
    std::filesystem::file_time_type written;
    bool inUse = false;
};

bool matches(const CacheLocation& location, const std::filesystem::path& file) {
    if (location.extensions.empty()) {
        return true;
    }
    std::string ext = pathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::find(location.extensions, ext) != location.extensions.end();
}

/// Fix F2C: `file` is an entry file of the analysis cache at `root` (<root>/v<n>/<source>/<stream>/<analyser>/<x>.json,
/// AnalysisCache.h) of the kind the cache lists: one that can be rebuilt (Analysis) or one that cannot (KeptAnalysis).
bool isAnalysisEntry(const std::filesystem::path& root, const std::filesystem::path& file, CacheKind kind) {
    std::vector<std::string> parts;
    for (const std::filesystem::path& part : file.lexically_relative(root)) {
        parts.push_back(pathToUtf8(part));
    }
    if (parts.size() != 5 || parts[0].size() < 2 || parts[0][0] != 'v' ||
        !std::ranges::all_of(parts[0].substr(1), [](char c) { return c >= '0' && c <= '9'; }) ||
        pathToUtf8(file.extension()) != ".json") {
        return false;
    }
    const bool kept = std::ranges::find(kUnrebuildableAnalyzers, std::string_view(parts[3])) != kUnrebuildableAnalyzers.end();
    return kind == CacheKind::KeptAnalysis ? kept : !kept;
}

/// The files of a cache. A cache folder can come from a project's location (its proxy folder), so it is untrusted
/// (Codex wave-3 audit): a folder that is a link (symlink, junction) has no files here, nothing is listed through a
/// link inside it, and files in use are recognised by their canonical, case-folded path, however they are spelled.
/// Fix F2C: only the files Wabcut wrote there, by the cache's kind (CacheKind); nothing else is ever listed, so nothing
/// else is ever measured, cleaned or cut by a limit.
std::vector<CacheFile> filesOf(const CacheLocation& location, const std::set<std::filesystem::path>& inUse) {
    std::vector<CacheFile> out;
    std::error_code ec;
    if (location.dir.empty() || paths::isLink(location.dir) || !std::filesystem::is_directory(location.dir, ec)) {
        return out;
    }
    std::set<std::filesystem::path::string_type> used;
    for (const std::filesystem::path& p : inUse) {
        used.insert(paths::key(p));
    }
    const auto add = [&](const std::filesystem::directory_entry& entry) {
        CacheFile f;
        f.path = entry.path();
        f.bytes = static_cast<std::int64_t>(entry.file_size(ec));
        f.written = entry.last_write_time(ec);
        f.inUse = used.contains(paths::key(f.path));
        out.push_back(std::move(f));
    };
    if (location.kind == CacheKind::WabcutProxies) {
        // Wabcut writes its proxies directly into the folder and records each in the folder's index: a file is
        // Wabcut's when the index names it (by the file system's name comparison) and it has the proxy shape.
        std::set<std::string> ours;
        for (const std::string& name : indexedProxyFiles(location.dir)) {
            ours.insert(paths::foldName(name));
        }
        if (ours.empty()) {
            return out;
        }
        for (auto it = std::filesystem::directory_iterator(location.dir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::string name = pathToUtf8(it->path().filename());
            if (!paths::isLink(it->path()) && it->is_regular_file(ec) && isProxyFileName(name) &&
                ours.contains(paths::foldName(name))) {
                add(*it);
            }
        }
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
             location.dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (paths::isLink(it->path())) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const bool analysis = location.kind == CacheKind::Analysis || location.kind == CacheKind::KeptAnalysis;
        if (analysis ? !isAnalysisEntry(location.dir, it->path(), location.kind) : !matches(location, it->path())) {
            continue;
        }
        add(*it);
    }
    return out;
}

} // namespace

void CacheInspector::setLimit(const std::string& id, std::int64_t bytes) {
    for (CacheLocation& l : m_locations) {
        if (l.id == id) {
            l.limitBytes = std::max<std::int64_t>(0, bytes);
        }
    }
}

std::vector<CacheUsage> CacheInspector::scan(const std::set<std::filesystem::path>& inUse) const {
    std::vector<CacheUsage> out;
    for (const CacheLocation& l : m_locations) {
        CacheUsage u{l.id, l.name, l.dir, 0, 0, 0, l.limitBytes, l.kind, 0};
        for (const CacheFile& f : filesOf(l, inUse)) {
            u.bytes += f.bytes;
            ++u.files;
            u.inUseBytes += f.inUse ? f.bytes : 0;
            u.inUseFiles += f.inUse ? 1 : 0;
        }
        out.push_back(std::move(u));
    }
    return out;
}

std::int64_t CacheInspector::clean(const std::string& id, const std::set<std::filesystem::path>& inUse) const {
    std::int64_t freed = 0;
    for (const CacheLocation& l : m_locations) {
        // Fix F2C: what cannot be rebuilt is never part of "clean everything"; only its own Clean (by id) deletes it.
        if (id.empty() ? l.kind == CacheKind::KeptAnalysis : l.id != id) {
            continue;
        }
        for (const CacheFile& f : filesOf(l, inUse)) {
            std::error_code ec;
            if (!f.inUse && std::filesystem::remove(f.path, ec)) {
                freed += f.bytes;
            }
        }
    }
    return freed;
}

std::int64_t CacheInspector::enforceLimits(const std::set<std::filesystem::path>& inUse) const {
    std::int64_t freed = 0;
    for (const CacheLocation& l : m_locations) {
        if (l.limitBytes <= 0) {
            continue;
        }
        std::vector<CacheFile> files = filesOf(l, inUse);
        std::int64_t total = 0;
        for (const CacheFile& f : files) {
            total += f.bytes;
        }
        std::ranges::sort(files, [](const CacheFile& a, const CacheFile& b) { return a.written < b.written; });
        for (const CacheFile& f : files) {
            if (total <= l.limitBytes) {
                break;
            }
            std::error_code ec;
            if (!f.inUse && std::filesystem::remove(f.path, ec)) {
                total -= f.bytes;
                freed += f.bytes;
            }
        }
    }
    return freed;
}

std::string formatBytes(std::int64_t bytes) {
    constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(std::max<std::int64_t>(0, bytes));
    int unit = 0;
    while (v >= 1000.0 && unit < 4) {
        v /= 1000.0;
        ++unit;
    }
    if (unit == 0) {
        return std::format("{} B", bytes);
    }
    return v < 10.0 ? std::format("{:.1f} {}", v, kUnits[unit]) : std::format("{:.0f} {}", v, kUnits[unit]);
}

} // namespace wabcut::editor
