#include "TextUtil.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/interchange/MediaRelinker.h>

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <tuple>

namespace wabcut::interchange {

namespace fs = std::filesystem;
using detail::pathFromUtf8;
using detail::toLowerAscii;
using detail::utf8FromPath;

namespace {

/// Longest tail of the original path tried under the project folder.
constexpr std::size_t kMaxTailComponents = 6;

bool isRegularFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

/// Path components without the root ("C:", "//server", "/") and without empty parts.
std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= path.size()) {
        const auto next = path.find_first_of("/\\", i);
        const std::string_view part =
            path.substr(i, next == std::string_view::npos ? std::string_view::npos : next - i);
        if (!part.empty() && part != ".") {
            out.emplace_back(part);
        }
        if (next == std::string_view::npos) {
            break;
        }
        i = next + 1;
    }
    if (!out.empty() && out.front().size() == 2 && out.front()[1] == ':') {
        out.erase(out.begin()); // drive letter
    }
    return out;
}

/// Number of trailing components two paths share (case-insensitive).
std::size_t sharedTail(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::size_t n = 0;
    while (n < a.size() && n < b.size() && detail::iequals(a[a.size() - 1 - n], b[b.size() - 1 - n])) {
        ++n;
    }
    return n;
}

bool durationsMatch(Ticks a, Ticks b) {
    const std::int64_t diff = std::llabs(a.value - b.value);
    const std::int64_t tolerance =
        std::max<std::int64_t>(kTicksPerSecond / 2, std::max(a.value, b.value) / 200);
    return diff <= tolerance;
}

} // namespace

std::string_view toString(RelinkMethod method) {
    switch (method) {
    case RelinkMethod::ExactPath:
        return "exact-path";
    case RelinkMethod::ProjectRelative:
        return "project-relative";
    case RelinkMethod::SearchRoot:
        return "search-root";
    case RelinkMethod::Unresolved:
        return "unresolved";
    }
    return "unknown";
}

MediaRelinker::MediaRelinker(RelinkOptions options) : m_options(std::move(options)) {}

std::size_t MediaRelinker::indexedFileCount() {
    buildIndex();
    return m_fileCount;
}

void MediaRelinker::buildIndex() {
    if (m_indexed) {
        return;
    }
    m_indexed = true;
    for (const fs::path& root : m_options.searchRoots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            if (it.depth() >= m_options.maxDepth) {
                it.disable_recursion_pending();
            }
            std::error_code statusError;
            if (it->is_regular_file(statusError)) {
                if (m_fileCount >= m_options.maxIndexedFiles) {
                    m_truncated = true;
                    return;
                }
                m_byName[toLowerAscii(utf8FromPath(it->path().filename()))].push_back(it->path());
                ++m_fileCount;
            }
            it.increment(ec);
            if (ec) {
                // An unreadable entry ends this root's walk but not the import.
                break;
            }
        }
    }
}

bool MediaRelinker::matchesKnownFacts(const MediaReference& reference, const fs::path& candidate,
                                      std::string& why) {
    if (reference.fileSize) {
        std::error_code ec;
        const auto size = fs::file_size(candidate, ec);
        if (!ec && static_cast<std::int64_t>(size) != *reference.fileSize) {
            why = "size differs";
            return false;
        }
    }
    if (reference.duration && m_options.inspector) {
        if (const auto facts = m_options.inspector(candidate);
            facts && facts->duration && !durationsMatch(*facts->duration, *reference.duration)) {
            why = "duration differs";
            return false;
        }
    }
    return true;
}

RelinkResult MediaRelinker::resolve(const MediaReference& reference) {
    RelinkResult result;
    const std::string& original = reference.path;

    auto alreadyTried = [&](const std::string& p) {
        return std::find(result.tried.begin(), result.tried.end(), p) != result.tried.end();
    };
    auto attempt = [&](const fs::path& candidate, RelinkMethod method, bool checkFacts) {
        const std::string label = utf8FromPath(candidate.lexically_normal());
        if (alreadyTried(label)) {
            return false;
        }
        result.tried.push_back(label);
        if (!(m_options.fileExists ? m_options.fileExists(candidate) : isRegularFile(candidate))) {
            return false;
        }
        std::string why;
        if (checkFacts && !matchesKnownFacts(reference, candidate, why)) {
            return false;
        }
        result.method = method;
        result.resolved = candidate.lexically_normal();
        return true;
    };

    if (!original.empty()) {
        const fs::path native = pathFromUtf8(original);
        // 1. The exact path, when it means something on this platform.
        if (native.is_absolute() && attempt(native, RelinkMethod::ExactPath, false)) {
            return result;
        }
        const fs::path& dir = m_options.projectDirectory;
        if (!dir.empty()) {
            // 2a. A relative reference is relative to the imported file.
            if (!isAbsoluteOnAnyPlatform(original) &&
                attempt(dir / native, RelinkMethod::ProjectRelative, true)) {
                return result;
            }
            // 2b. The original folder structure moved together with the imported file.
            const auto parts = components(original);
            std::vector<fs::path> bases{dir};
            if (dir.has_parent_path() && dir.parent_path() != dir) {
                bases.push_back(dir.parent_path());
            }
            for (const fs::path& base : bases) {
                for (std::size_t k = std::min(parts.size(), kMaxTailComponents); k >= 1; --k) {
                    fs::path candidate = base;
                    for (std::size_t i = parts.size() - k; i < parts.size(); ++i) {
                        candidate /= pathFromUtf8(parts[i]);
                    }
                    if (attempt(candidate, RelinkMethod::ProjectRelative, true)) {
                        return result;
                    }
                }
            }
        }
    }

    // 3. Search the roots by file name.
    const std::string name(!original.empty() ? fileNameOf(original) : std::string_view(reference.name));
    if (name.empty() || m_options.searchRoots.empty()) {
        return result;
    }
    buildIndex();
    const auto found = m_byName.find(toLowerAscii(name));
    if (found == m_byName.end()) {
        result.note = "no file named '" + name + "' under the search folders";
        if (m_truncated) {
            result.note += " (the search stopped after " + std::to_string(m_fileCount) + " files)";
        }
        return result;
    }
    const auto originalParts = components(original);
    struct Candidate {
        fs::path path;
        std::size_t shared = 0;
        bool exactCase = false;
    };
    std::vector<Candidate> matches;
    std::size_t rejected = 0;
    std::string lastWhy;
    for (const fs::path& p : found->second) {
        result.tried.push_back(utf8FromPath(p));
        std::string why;
        if (!matchesKnownFacts(reference, p, why)) {
            ++rejected;
            lastWhy = why;
            continue;
        }
        matches.push_back(
            {p, sharedTail(originalParts, components(utf8FromPath(p))), utf8FromPath(p.filename()) == name});
    }
    if (matches.empty()) {
        result.note = std::to_string(rejected) + " file(s) named '" + name + "' found, but the " + lastWhy;
        return result;
    }
    std::sort(matches.begin(), matches.end(), [](const Candidate& a, const Candidate& b) {
        const auto aLen = a.path.native().size();
        const auto bLen = b.path.native().size();
        return std::tie(b.shared, b.exactCase, aLen, a.path) < std::tie(a.shared, a.exactCase, bLen, b.path);
    });
    result.method = RelinkMethod::SearchRoot;
    result.resolved = matches.front().path.lexically_normal();
    if (matches.size() > 1) {
        result.note = std::to_string(matches.size()) + " files named '" + name +
                      "' found; picked the one sharing the most folders with the original path";
    }
    return result;
}

} // namespace wabcut::interchange
