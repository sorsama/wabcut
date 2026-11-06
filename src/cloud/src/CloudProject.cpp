// Cloud projects (CloudProject.h).

#include <wabcut/cloud/CloudProject.h>

#include "FileUtil.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectDiff.h>
#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>

namespace wabcut::cloud {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::string_view kHeadFormat = "wabcut-cloud-head";
constexpr std::string_view kRevisionFormat = "wabcut-cloud-revision";
constexpr std::string_view kLinkFormat = "wabcut-cloud-link";
constexpr std::size_t kMaxChain = 500;
constexpr std::size_t kMaxNewRevisions = 20;

std::string text(const json& j, const char* key, std::size_t limit) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    std::string value = it->get<std::string>();
    if (value.size() > limit) {
        value.resize(limit);
    }
    return value;
}

std::int64_t integer(const json& j, const char* key, std::int64_t fallback = 0) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<std::int64_t>() : fallback;
}

bool isHex(std::string_view s, std::size_t length) {
    return s.size() == length && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

StoreError fail(StoreFailure kind, std::string message) { return StoreError{kind, std::move(message)}; }

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += (out.empty() ? "" : "; ") + line;
    }
    return out;
}

} // namespace

std::string revisionStem(std::int64_t revision, std::string_view sha256) {
    char number[24];
    std::snprintf(number, sizeof number, "%08lld-", static_cast<long long>(revision));
    return std::string(number) + std::string(sha256.substr(0, 16));
}

std::string revisionJson(const RevisionMeta& m, bool asHead) {
    json j{{"format", std::string(asHead ? kHeadFormat : kRevisionFormat)},
           {"version", 1},
           {"revision", m.revision},
           {"sha256", m.sha256},
           {"parentSha", m.parentSha},
           {"parentRevision", m.parentRevision},
           {"file", m.file},
           {"bytes", m.bytes},
           {"savedBy", m.savedBy},
           {"savedAt", m.savedAt},
           {"appVersion", m.appVersion},
           {"schema", m.schema}};
    if (!m.name.empty()) {
        j["name"] = m.name;
    }
    if (!m.label.empty()) {
        j["label"] = m.label;
    }
    if (!m.secondParentSha.empty()) {
        j["secondParentSha"] = m.secondParentSha;
        j["secondParentRevision"] = m.secondParentRevision;
    }
    return j.dump(2) + "\n";
}

std::optional<RevisionMeta> parseRevisionJson(std::string_view data) {
    if (data.size() > kMaxHeadBytes || !io::jsonNestingWithin(data, 8)) {
        return std::nullopt;
    }
    const json j = json::parse(data, nullptr, false);
    if (!j.is_object()) {
        return std::nullopt;
    }
    const std::string format = text(j, "format", 64);
    if (format != kHeadFormat && format != kRevisionFormat) {
        return std::nullopt;
    }
    RevisionMeta m;
    m.revision = integer(j, "revision");
    m.sha256 = text(j, "sha256", 64);
    m.parentSha = text(j, "parentSha", 64);
    m.parentRevision = integer(j, "parentRevision");
    m.secondParentSha = text(j, "secondParentSha", 64);
    m.secondParentRevision = integer(j, "secondParentRevision");
    m.file = text(j, "file", 128);
    const std::int64_t bytes = integer(j, "bytes", -1);
    m.savedBy = text(j, "savedBy", 300);
    m.savedAt = text(j, "savedAt", 64);
    m.appVersion = text(j, "appVersion", 64);
    m.schema = static_cast<int>(integer(j, "schema"));
    m.label = text(j, "label", 256);
    m.name = text(j, "name", 256);
    if (m.revision < 1 || m.revision > kMaxRevisionNumber || !isHex(m.sha256, 64) || (!m.parentSha.empty() && !isHex(m.parentSha, 64)) || (!m.secondParentSha.empty() && !isHex(m.secondParentSha, 64)) ||
        bytes < 0 || static_cast<std::uint64_t>(bytes) > kMaxRevisionBytes || m.schema < 0 || m.schema > 1000 ||
        m.file != "revisions/" + revisionStem(m.revision, m.sha256) + ".wabcut") {
        return std::nullopt;
    }
    m.bytes = static_cast<std::uint64_t>(bytes);
    return m;
}

// ---- the link file ----

fs::path cloudLinkPath(const fs::path& projectFile) {
    fs::path p = projectFile;
    p += "-cloud"; // "Film.wabcut" -> "Film.wabcut-cloud"
    return p;
}

std::expected<CloudLink, Error> readCloudLink(const fs::path& projectFile) {
    const auto data = detail::readFileBounded(cloudLinkPath(projectFile), 64 * 1024);
    if (!data) {
        return std::unexpected(toError(data.error()));
    }
    const json j = json::parse(*data, nullptr, false);
    if (!j.is_object() || text(j, "format", 64) != kLinkFormat) {
        return std::unexpected(Error{ErrorCode::ParseError, "this is not a cloud project link"});
    }
    CloudLink link;
    link.location = text(j, "location", 128);
    link.projectId = text(j, "projectId", 128);
    link.name = text(j, "name", 256);
    link.baseRevision = integer(j, "baseRevision");
    link.baseSha = text(j, "baseSha", 64);
    link.baseNormSha = text(j, "baseNormSha", 64);
    link.syncedAt = text(j, "syncedAt", 64);
    link.autoSync = j.value("autoSync", false);
    if (!validSegment(link.projectId) || link.baseRevision < 0 || (!link.baseSha.empty() && !isHex(link.baseSha, 64))) {
        return std::unexpected(Error{ErrorCode::ParseError, "the cloud project link is damaged"});
    }
    return link;
}

std::expected<void, Error> writeCloudLink(const fs::path& projectFile, const CloudLink& link) {
    const json j{{"format", std::string(kLinkFormat)}, {"version", 1}, {"location", link.location}, {"projectId", link.projectId},
                 {"name", link.name}, {"baseRevision", link.baseRevision}, {"baseSha", link.baseSha},
                 {"baseNormSha", link.baseNormSha}, {"syncedAt", link.syncedAt}, {"autoSync", link.autoSync}};
    return io::writeFileAtomically(cloudLinkPath(projectFile), j.dump(2) + "\n");
}

// ---- what belongs to one computer ----

namespace {

void normalizeLocator(MediaLocator& locator) {
    locator.path.clear();
    locator.relativePath.clear();
    locator.proxy.reset();
    locator.offline = false;
    locator.fingerprint.mtimeNs = 0;
}

/// The path with one kind of separator: Windows takes "/" and "\" alike, so "/\evil\share" names a share too.
std::string withBackslashes(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

bool startsWithNoCase(const std::string& text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

/// A path on a network share: "\host\share", "//host/share", the mixed forms and "\?\UNC\host\share". The other
/// extended-length prefix ("\?\C:\x", "\.\C:\x") names a local device, not a share.
bool foreignShare(const std::string& raw) {
    const std::string path = withBackslashes(raw);
    if (path.rfind("\\\\", 0) != 0) { // two separators
        return false;
    }
    const std::string rest = path.substr(2);
    if (rest.size() >= 2 && (rest[0] == '?' || rest[0] == '.') && rest[1] == '\\') {
        return startsWithNoCase(rest.substr(2), "UNC\\");
    }
    return true;
}

/// A relative path that is not one: rooted, on a drive, a share, or climbing out of the project's folder. Joined to the
/// project folder, a rooted or UNC right-hand side replaces the left.
bool unsafeRelative(const std::string& raw) {
    if (raw.empty()) {
        return false;
    }
    const std::string path = withBackslashes(raw);
    if (path.front() == '\\' || (path.size() >= 2 && path[1] == ':')) {
        return true;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t cut = path.find('\\', start);
        const std::string part = path.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
        if (part == "..") {
            return true;
        }
        if (cut == std::string::npos) {
            break;
        }
        start = cut + 1;
    }
    return false;
}

std::string lastComponent(const std::string& path) {
    const std::size_t cut = path.find_last_of("\\/");
    std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
    return name.empty() || name == "." || name == ".." ? std::string("media") : name;
}

bool quarantineLocator(MediaLocator& locator, const fs::path& projectFolder) {
    bool changed = false;
    if (unsafeRelative(locator.relativePath)) {
        locator.relativePath.clear(); // a pulled revision never decides where the project folder's media is looked for
        locator.offline = true;
        changed = true;
    }
    if (foreignShare(locator.path)) {
        locator.path = pathToUtf8(projectFolder / "Media" / pathFromUtf8(lastComponent(locator.path)));
        locator.relativePath = "Media/" + lastComponent(locator.path);
        locator.offline = true;
        changed = true;
    }
    if (locator.proxy && foreignShare(locator.proxy->path)) {
        locator.proxy.reset();
        changed = true;
    }
    return changed;
}

} // namespace

ProjectState normalizeForSync(const ProjectState& state) {
    ProjectState out = state;
    for (MediaPtr& item : out.media) {
        auto copy = std::make_shared<MediaItem>(*item);
        normalizeLocator(copy->locator);
        item = std::move(copy);
    }
    for (ProjectAsset& asset : out.assets) {
        normalizeLocator(asset.locator);
    }
    return out;
}

std::size_t quarantineForeignPaths(ProjectState& state, const fs::path& projectFolder) {
    std::size_t changed = 0;
    for (MediaPtr& item : state.media) {
        if (!foreignShare(item->locator.path) && !unsafeRelative(item->locator.relativePath) &&
            !(item->locator.proxy && foreignShare(item->locator.proxy->path))) {
            continue;
        }
        auto copy = std::make_shared<MediaItem>(*item);
        if (quarantineLocator(copy->locator, projectFolder)) {
            ++changed;
        }
        item = std::move(copy);
    }
    for (ProjectAsset& asset : state.assets) {
        if (quarantineLocator(asset.locator, projectFolder)) {
            ++changed;
        }
    }
    return changed;
}

ProjectState restoreLocalState(ProjectState merged, const ProjectState& mine, const ProjectState& theirs) {
    for (MediaPtr& item : merged.media) {
        if (item->generator) {
            continue;
        }
        const MediaItem* local = mine.findMedia(item->id);
        const MediaItem* remote = theirs.findMedia(item->id);
        auto copy = std::make_shared<MediaItem>(*item);
        if (local) {
            copy->locator = local->locator;
            if (!local->locator.fingerprint.sameContentAs(item->locator.fingerprint)) {
                copy->locator.fingerprint = item->locator.fingerprint; // the cloud relinked it to another file
                copy->locator.offline = true;
                copy->locator.proxy.reset();
            }
        } else if (remote) {
            copy->locator = remote->locator;
            copy->locator.offline = true; // not checked on this computer yet: relink finds it
        }
        item = std::move(copy);
    }
    for (ProjectAsset& asset : merged.assets) {
        if (const ProjectAsset* local = mine.findAsset(asset.id)) {
            asset.locator = local->locator;
        } else if (const ProjectAsset* remote = theirs.findAsset(asset.id)) {
            asset.locator = remote->locator;
            asset.locator.offline = true;
        }
    }
    merged.aiNoCloud = mine.aiNoCloud || theirs.aiNoCloud; // a pull can only turn the lock on
    return merged;
}

// ---- the store check ----

StoreCheck checkStore(RemoteStore& store, const RandomBytes& random, std::stop_token stop) {
    StoreCheck out;
    const std::string folder = "check-" + newToken(random, 8);
    const std::string path = folder + "/probe.json";
    auto first = store.put(path, "1", Precondition::none(), "application/json", stop);
    if (!first) {
        out.problems.push_back("Could not write to the storage: " + first.error().message);
        return out;
    }
    out.reachable = true;
    if (first->empty() || first->rfind("W/", 0) == 0) {
        out.problems.push_back("The storage does not return a usable version tag (ETag) when a file is written.");
    }
    if (auto again = store.put(path, "2", Precondition::absent(), "application/json", stop); again) {
        out.problems.push_back("The storage overwrote a file that was meant to be created only if it did not exist (If-None-Match).");
    } else if (again.error().kind != StoreFailure::PreconditionFailed) {
        out.problems.push_back("Writing 'only if absent' failed in an unexpected way: " + again.error().message);
    }
    if (auto stale = store.put(path, "3", Precondition::matches("\"not-the-current-tag\""), "application/json", stop); stale) {
        out.problems.push_back("The storage overwrote a file although its version tag was out of date (If-Match).");
    } else if (stale.error().kind != StoreFailure::PreconditionFailed) {
        out.problems.push_back("Writing with a version tag failed in an unexpected way: " + stale.error().message);
    }
    if (!first->empty()) {
        if (auto good = store.put(path, "4", Precondition::matches(*first), "application/json", stop); !good) {
            out.problems.push_back("The storage refused a write with the current version tag: " + good.error().message);
        }
    }
    (void)store.remove(path, stop);
    out.conditionalWrites = out.problems.empty();
    return out;
}

// ---- CloudProjects ----

CloudProjects::CloudProjects(RemoteStore& store, Identity who, UnixClock clock, RandomBytes random, std::string appVersion)
    : m_store(store), m_who(std::move(who)), m_clock(std::move(clock)), m_random(std::move(random)), m_appVersion(std::move(appVersion)) {}

std::string CloudProjects::folderOf(std::string_view projectId) const { return std::string(projectId); }

StoreResult<std::vector<RemoteProject>> CloudProjects::list(std::stop_token stop) {
    auto folders = m_store.list("", stop);
    if (!folders) {
        return std::unexpected(folders.error());
    }
    std::vector<RemoteProject> out;
    for (const StoreEntry& entry : *folders) {
        if (!entry.folder || out.size() >= 200) {
            continue;
        }
        auto meta = head(entry.path, nullptr, stop);
        if (meta) {
            out.push_back({entry.path, std::move(*meta)});
        } else if (meta.error().kind == StoreFailure::Cancelled || meta.error().kind == StoreFailure::Unauthorized ||
                   meta.error().kind == StoreFailure::Offline) {
            return std::unexpected(meta.error());
        }
    }
    std::sort(out.begin(), out.end(), [](const RemoteProject& a, const RemoteProject& b) { return a.head.savedAt > b.head.savedAt; });
    return out;
}

StoreResult<RevisionMeta> CloudProjects::head(std::string_view projectId, std::string* etag, std::stop_token stop) {
    if (!validSegment(projectId)) {
        return std::unexpected(fail(StoreFailure::Invalid, "not a project id"));
    }
    auto got = m_store.get(joinPath(folderOf(projectId), "head.json"), kMaxHeadBytes, stop);
    if (!got) {
        if (got.error().kind == StoreFailure::NotFound) {
            // revisions without a head: damaged, not "new"
            auto revs = m_store.list(joinPath(folderOf(projectId), "revisions"), stop);
            if (revs && !revs->empty()) {
                return std::unexpected(fail(StoreFailure::Other,
                                            "The cloud project's head.json is missing although its revisions are there. Restore head.json from "
                                            "your storage's recycle bin or version history; nothing was changed."));
            }
        }
        return std::unexpected(got.error());
    }
    auto meta = parseRevisionJson(got->data);
    if (!meta) {
        return std::unexpected(fail(StoreFailure::Other, "The cloud project's head.json is damaged."));
    }
    if (etag) {
        *etag = got->etag;
    }
    return *meta;
}

StoreResult<std::optional<RevisionMeta>> CloudProjects::metaOf(std::string_view projectId, std::string_view sha256, std::int64_t revision,
                                                               std::stop_token stop) {
    if (revision < 1 || !isHex(sha256, 64)) {
        return std::optional<RevisionMeta>();
    }
    auto got = m_store.get(joinPath(folderOf(projectId), "revisions/" + revisionStem(revision, sha256) + ".json"), kMaxHeadBytes, stop);
    if (!got) {
        if (got.error().kind == StoreFailure::NotFound) {
            return std::optional<RevisionMeta>();
        }
        return std::unexpected(got.error());
    }
    auto meta = parseRevisionJson(got->data);
    if (!meta || meta->sha256 != sha256 || meta->revision != revision) {
        return std::optional<RevisionMeta>();
    }
    return std::optional<RevisionMeta>(std::move(*meta));
}

StoreResult<std::vector<RevisionMeta>> CloudProjects::revisions(std::string_view projectId, std::size_t limit, std::stop_token stop) {
    auto listed = m_store.list(joinPath(folderOf(projectId), "revisions"), stop);
    if (!listed) {
        return std::unexpected(listed.error());
    }
    struct Named {
        std::int64_t revision;
        std::string sha16;
    };
    std::vector<Named> names;
    for (const StoreEntry& e : *listed) {
        const std::string_view base = baseName(e.path);
        // "00000007-1a2b3c4d5e6f7a8b.json"
        if (e.folder || base.size() != 8 + 1 + 16 + 5 || base.substr(base.size() - 5) != ".json" || base[8] != '-') {
            continue;
        }
        std::int64_t number = 0;
        bool digits = true;
        for (std::size_t i = 0; i < 8; ++i) {
            digits = digits && base[i] >= '0' && base[i] <= '9';
            number = number * 10 + (base[i] - '0');
        }
        const std::string sha16(base.substr(9, 16));
        if (digits && number >= 1 && isHex(sha16, 16)) {
            names.push_back({number, sha16});
        }
    }
    std::sort(names.begin(), names.end(), [](const Named& a, const Named& b) { return a.revision != b.revision ? a.revision > b.revision : a.sha16 < b.sha16; });
    std::vector<RevisionMeta> out;
    for (const Named& n : names) {
        if (out.size() >= limit) {
            break;
        }
        auto got = m_store.get(joinPath(folderOf(projectId), "revisions/" + revisionStem(n.revision, n.sha16) + ".json"), kMaxHeadBytes, stop);
        if (!got) {
            if (got.error().kind == StoreFailure::NotFound) {
                continue;
            }
            return std::unexpected(got.error());
        }
        auto meta = parseRevisionJson(got->data);
        if (meta && meta->revision == n.revision && meta->sha256.compare(0, 16, n.sha16) == 0) {
            out.push_back(std::move(*meta));
        }
    }
    return out;
}

StoreResult<std::vector<RevisionMeta>> CloudProjects::unmerged(std::string_view projectId, std::size_t limit, std::stop_token stop) {
    auto current = head(projectId, nullptr, stop);
    if (!current) {
        return std::unexpected(current.error());
    }
    auto all = revisions(projectId, 200, stop);
    if (!all) {
        return std::unexpected(all.error());
    }
    std::map<std::string, const RevisionMeta*> bySha;
    for (const RevisionMeta& m : *all) {
        bySha[m.sha256] = &m;
    }
    std::set<std::string> reached;
    std::vector<std::string> pending{current->sha256};
    for (std::size_t step = 0; step < kMaxChain * 2 && !pending.empty(); ++step) {
        const std::string at = std::move(pending.back());
        pending.pop_back();
        if (at.empty() || !reached.insert(at).second) {
            continue;
        }
        if (const auto it = bySha.find(at); it != bySha.end()) {
            pending.push_back(it->second->parentSha);
            pending.push_back(it->second->secondParentSha);
        }
    }
    std::vector<RevisionMeta> out;
    for (const RevisionMeta& m : *all) {
        if (!reached.contains(m.sha256) && out.size() < limit) {
            out.push_back(m);
        }
    }
    return out;
}

StoreResult<ProjectState> CloudProjects::fetch(std::string_view projectId, const RevisionMeta& meta, const fs::path& cacheFolder,
                                               const Progress& progress, std::stop_token stop) {
    if (meta.schema > io::kSchemaVersion) {
        return std::unexpected(fail(StoreFailure::Invalid,
                                    "This cloud project was saved by a newer Wabcut (project format " + std::to_string(meta.schema) +
                                        "). Update Wabcut to open it."));
    }
    if (meta.bytes > kMaxRevisionBytes) {
        return std::unexpected(fail(StoreFailure::TooLarge, "The cloud copy of the project is larger than allowed."));
    }
    const fs::path cached = cacheFolder / pathFromUtf8(std::string(projectId) + "-" + meta.sha256.substr(0, 16) + ".wabcut");
    std::string content;
    bool fromCache = false;
    {
        std::error_code ec;
        if (fs::is_regular_file(cached, ec)) {
            if (auto data = detail::readFileBounded(cached, kMaxRevisionBytes); data && sha256Hex(*data) == meta.sha256) {
                content = std::move(*data);
                fromCache = true;
            }
        }
    }
    if (!fromCache) {
        fs::path temp = cached;
        temp += ".download";
        auto got = m_store.getToFile(joinPath(folderOf(projectId), meta.file), temp, meta.bytes, progress, stop);
        if (!got) {
            return std::unexpected(got.error());
        }
        auto data = detail::readFileBounded(temp, kMaxRevisionBytes);
        std::error_code ec;
        if (!data) {
            fs::remove(temp, ec);
            return std::unexpected(data.error());
        }
        if (data->size() != meta.bytes || sha256Hex(*data) != meta.sha256) {
            fs::remove(temp, ec);
            return std::unexpected(fail(StoreFailure::Other, "The downloaded project does not match its checksum; nothing was changed."));
        }
        fs::rename(temp, cached, ec);
        if (ec) {
            fs::remove(temp, ec);
        }
        content = std::move(*data);
    }
    auto state = io::fromJson(content);
    if (!state) {
        return std::unexpected(fail(StoreFailure::Other, "The cloud copy of the project cannot be read: " + state.error().message));
    }
    return std::move(*state);
}

StoreResult<RevisionMeta> CloudProjects::push(const ProjectState& state, const CloudLink& link, const RevisionMeta* parent,
                                              std::string_view headEtag, std::string_view label, const fs::path& cacheFolder,
                                              const Progress& progress, std::stop_token stop, std::string_view secondParentSha,
                                              std::int64_t secondParentRevision) {
    auto json = io::toJson(state);
    if (!json) {
        return std::unexpected(fail(StoreFailure::Invalid, "The project cannot be saved: " + json.error().message));
    }
    if (json->size() > kMaxRevisionBytes) {
        return std::unexpected(fail(StoreFailure::TooLarge, "The project file is larger than a cloud project may be (512 MB)."));
    }
    // The next number is one past the highest on the storage, so a revision a lost race left behind is never reused.
    std::int64_t highest = parent ? parent->revision : 0;
    if (auto listed = m_store.list(joinPath(folderOf(link.projectId), "revisions"), stop)) {
        for (const StoreEntry& e : *listed) {
            const std::string_view base = baseName(e.path);
            if (base.size() > 9 && base[8] == '-') {
                std::int64_t n = 0;
                bool digits = true;
                for (std::size_t i = 0; i < 8; ++i) {
                    digits = digits && base[i] >= '0' && base[i] <= '9';
                    n = n * 10 + (base[i] - '0');
                }
                if (digits) {
                    highest = std::max(highest, n);
                }
            }
        }
    }
    if (highest + 1 > kMaxRevisionNumber) {
        return std::unexpected(fail(StoreFailure::Other, "This cloud project has reached its revision limit."));
    }
    RevisionMeta meta;
    meta.revision = highest + 1;
    meta.sha256 = sha256Hex(*json);
    meta.parentSha = parent ? parent->sha256 : std::string();
    meta.parentRevision = parent ? parent->revision : 0;
    meta.secondParentSha = std::string(secondParentSha);
    meta.secondParentRevision = secondParentSha.empty() ? 0 : secondParentRevision;
    meta.file = "revisions/" + revisionStem(meta.revision, meta.sha256) + ".wabcut";
    meta.bytes = json->size();
    meta.savedBy = m_who.user + " on " + m_who.host;
    meta.savedAt = formatIsoUtc(m_clock());
    meta.appVersion = m_appVersion;
    meta.schema = io::kSchemaVersion;
    meta.name = link.name;
    meta.label = std::string(label);

    // The file is written next to the cache copy first and uploaded from there (a file is never put in the request log).
    const fs::path source = cacheFolder / pathFromUtf8(link.projectId + "-" + meta.sha256.substr(0, 16) + ".wabcut");
    if (auto written = io::writeFileAtomically(source, *json); !written) {
        return std::unexpected(fail(StoreFailure::Other, "Cannot write the working copy: " + written.error().message));
    }
    const std::string folder = folderOf(link.projectId);
    if (auto up = m_store.putFile(joinPath(folder, meta.file), source, Precondition::absent(), "application/octet-stream", progress, stop);
        !up && up.error().kind != StoreFailure::PreconditionFailed) {
        return std::unexpected(up.error());
    }
    if (auto up = m_store.put(joinPath(folder, "revisions/" + revisionStem(meta.revision, meta.sha256) + ".json"), revisionJson(meta, false),
                              Precondition::absent(), "application/json", stop);
        !up && up.error().kind != StoreFailure::PreconditionFailed) {
        return std::unexpected(up.error());
    }
    const Precondition swap = headEtag.empty() ? Precondition::absent() : Precondition::matches(std::string(headEtag));
    auto swapped = m_store.put(joinPath(folder, "head.json"), revisionJson(meta, true), swap, "application/json", stop);
    if (!swapped) {
        if (swapped.error().kind == StoreFailure::PreconditionFailed) {
            // Maybe our own earlier attempt landed (the answer was lost): then head is already this revision.
            if (auto now = head(link.projectId, nullptr, stop); now && now->sha256 == meta.sha256) {
                return meta;
            }
        }
        return std::unexpected(swapped.error());
    }
    return meta;
}

StoreResult<CreatedProject> CloudProjects::create(const ProjectState& state, std::string name, std::string location,
                                                  const fs::path& cacheFolder, const Progress& progress, std::stop_token stop) {
    CloudLink link;
    link.location = std::move(location);
    link.projectId = newToken(m_random);
    link.name = std::move(name);
    auto pushed = push(state, link, nullptr, {}, {}, cacheFolder, progress, stop);
    if (!pushed) {
        return std::unexpected(pushed.error());
    }
    auto normalized = io::toJson(normalizeForSync(state));
    link.baseRevision = pushed->revision;
    link.baseSha = pushed->sha256;
    link.baseNormSha = normalized ? sha256Hex(*normalized) : std::string();
    link.syncedAt = formatIsoUtc(m_clock());
    return CreatedProject{std::move(link), std::move(*pushed)};
}

StoreResult<std::pair<ProjectState, CloudLink>> CloudProjects::open(std::string_view projectId, std::string location,
                                                                    const fs::path& projectFolder, const fs::path& cacheFolder,
                                                                    std::size_t* quarantined, const Progress& progress,
                                                                    std::stop_token stop) {
    auto current = head(projectId, nullptr, stop);
    if (!current) {
        return std::unexpected(current.error());
    }
    auto state = fetch(projectId, *current, cacheFolder, progress, stop);
    if (!state) {
        return std::unexpected(state.error());
    }
    const std::size_t foreign = quarantineForeignPaths(*state, projectFolder);
    if (quarantined) {
        *quarantined = foreign;
    }
    for (MediaPtr& item : state->media) { // media from another computer is not checked here yet
        if (!item->generator && !item->locator.offline) {
            auto copy = std::make_shared<MediaItem>(*item);
            copy->locator.offline = true;
            item = std::move(copy);
        }
    }
    CloudLink link;
    link.location = std::move(location);
    link.projectId = std::string(projectId);
    link.name = current->name.empty() ? std::string(projectId) : current->name;
    link.baseRevision = current->revision;
    link.baseSha = current->sha256;
    auto normalized = io::toJson(normalizeForSync(*state));
    link.baseNormSha = normalized ? sha256Hex(*normalized) : std::string();
    link.syncedAt = formatIsoUtc(m_clock());
    return std::make_pair(std::move(*state), std::move(link));
}

StoreResult<std::size_t> CloudProjects::remove(std::string_view projectId, std::stop_token stop) {
    if (!validSegment(projectId)) {
        return std::unexpected(fail(StoreFailure::Invalid, "not a project id"));
    }
    return m_store.removeTree(folderOf(projectId), stop);
}

StoreResult<SyncResult> CloudProjects::sync(const SyncRequest& request, const Progress& progress, std::stop_token stop) {
    // A push that loses the race for head.json starts again from what the other editor wrote (at most three times).
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool raced = false;
        auto result = syncOnce(request, progress, stop, raced);
        if (result || !raced) {
            return result;
        }
    }
    return std::unexpected(fail(StoreFailure::PreconditionFailed, "Somebody else keeps saving this cloud project; try again in a moment."));
}

StoreResult<SyncResult> CloudProjects::syncOnce(const SyncRequest& request, const Progress& progress, std::stop_token stop, bool& raced) {
    const CloudLink& link = request.link;
    std::string headEtag;
    auto remote = head(link.projectId, &headEtag, stop);
    if (!remote) {
        return std::unexpected(remote.error());
    }
    if (remote->schema > io::kSchemaVersion) {
        return std::unexpected(fail(StoreFailure::Invalid, "This cloud project was saved by a newer Wabcut (project format " +
                                                               std::to_string(remote->schema) + "). Update Wabcut to sync it."));
    }
    SyncResult out;
    out.link = link;
    out.remoteHead = *remote;

    const ProjectState localNormalized = normalizeForSync(request.local);
    auto localText = io::toJson(localNormalized);
    if (!localText) {
        return std::unexpected(fail(StoreFailure::Invalid, "The project cannot be read: " + localText.error().message));
    }
    const std::string localNormSha = sha256Hex(*localText);
    const bool changedLocally = localNormSha != link.baseNormSha;
    const bool remoteChanged = remote->revision != link.baseRevision || remote->sha256 != link.baseSha;
    const std::string now = formatIsoUtc(m_clock());

    const auto afterPush = [&](const RevisionMeta& pushed, const std::string& normSha) {
        out.link.baseRevision = pushed.revision;
        out.link.baseSha = pushed.sha256;
        out.link.baseNormSha = normSha;
        out.link.syncedAt = now;
        out.pushed = pushed;
    };

    if (!remoteChanged) {
        if (!changedLocally) {
            out.kind = SyncKind::UpToDate;
            return out;
        }
        if (!request.mayPush) {
            out.kind = SyncKind::PushBlocked;
            out.notes.push_back(request.pushBlockedBecause);
            return out;
        }
        auto pushed = push(request.local, link, &*remote, headEtag, request.label, request.cacheFolder, progress, stop);
        if (!pushed) {
            raced = pushed.error().kind == StoreFailure::PreconditionFailed;
            return std::unexpected(pushed.error());
        }
        afterPush(*pushed, localNormSha);
        out.kind = SyncKind::Pushed;
        return out;
    }

    // The cloud moved (or diverged): bring its head down.
    auto theirsFetched = fetch(link.projectId, *remote, request.cacheFolder, progress, stop);
    if (!theirsFetched) {
        return std::unexpected(theirsFetched.error());
    }
    ProjectState theirs = std::move(*theirsFetched);
    if (const std::size_t foreign = quarantineForeignPaths(theirs, request.projectFolder)) {
        out.notes.push_back(std::to_string(foreign) + (foreign == 1 ? " media item" : " media items") +
                            " pointed at a network share of another computer. It is not used; relink it to a file on this computer.");
    }
    const ProjectState theirsNormalized = normalizeForSync(theirs);
    auto theirsText = io::toJson(theirsNormalized);
    const std::string theirsNormSha = theirsText ? sha256Hex(*theirsText) : std::string();

    // The revisions that came with the pull (for the version history), newest first until our base.
    std::vector<RevisionMeta> chain;
    {
        RevisionMeta at = *remote;
        for (std::size_t step = 0; step < kMaxChain; ++step) {
            chain.push_back(at);
            if (at.sha256 == link.baseSha || at.parentSha.empty()) {
                break;
            }
            auto parent = metaOf(link.projectId, at.parentSha, at.parentRevision, stop);
            if (!parent) {
                return std::unexpected(parent.error());
            }
            if (!*parent) {
                break; // pruned or missing
            }
            at = std::move(**parent);
        }
    }
    for (std::size_t i = std::min(chain.size(), kMaxNewRevisions); i > 0; --i) {
        if (chain[i - 1].sha256 != link.baseSha) {
            out.newRevisions.push_back(chain[i - 1]);
        }
    }

    const auto finishPull = [&](SyncKind kind, ProjectState state) {
        out.kind = kind;
        out.changes = joinLines(diffProjects(request.local, state).summary());
        out.state = std::move(state);
        out.link.baseRevision = remote->revision;
        out.link.baseSha = remote->sha256;
        out.link.baseNormSha = theirsNormSha;
        out.link.syncedAt = now;
    };

    // Does the cloud's head come from the revision this computer last synced? (If not, the lines diverged: two computers wrote
    // the same revision, or the head went back. Then even an unchanged project must be merged, never replaced.)
    const bool descends = std::any_of(chain.begin(), chain.end(), [&](const RevisionMeta& m) { return m.sha256 == link.baseSha; });
    if (!changedLocally && descends) {
        finishPull(SyncKind::FastForwarded, restoreLocalState(theirsNormalized, request.local, theirs));
        return out;
    }

    // Both changed (or diverged): merge against the common ancestor.
    std::optional<RevisionMeta> ancestor;
    for (const RevisionMeta& m : chain) {
        if (m.sha256 == link.baseSha) {
            ancestor = m;
            break;
        }
    }
    if (!ancestor) {
        // Diverged (two computers wrote the same number, or the head went back): walk our own line to a revision both reach.
        std::set<std::string> ours;
        std::string at = link.baseSha;
        std::int64_t atRevision = link.baseRevision;
        for (std::size_t step = 0; step < kMaxChain && !at.empty(); ++step) {
            ours.insert(at);
            auto meta = metaOf(link.projectId, at, atRevision, stop);
            if (!meta) {
                return std::unexpected(meta.error());
            }
            if (!*meta) {
                break;
            }
            at = (*meta)->parentSha;
            atRevision = (*meta)->parentRevision;
        }
        for (const RevisionMeta& m : chain) {
            if (ours.contains(m.sha256)) {
                ancestor = m;
                break;
            }
        }
        // `chain` stops at the first missing meta; extend it from its last element if our line was not reached yet.
        for (RevisionMeta at2 = chain.back(); !ancestor && !at2.parentSha.empty() && chain.size() < kMaxChain;) {
            auto parent = metaOf(link.projectId, at2.parentSha, at2.parentRevision, stop);
            if (!parent) {
                return std::unexpected(parent.error());
            }
            if (!*parent) {
                break;
            }
            at2 = std::move(**parent);
            chain.push_back(at2);
            if (ours.contains(at2.sha256)) {
                ancestor = at2;
            }
        }
    }
    if (!ancestor) {
        return std::unexpected(fail(StoreFailure::Other,
                                    "This project and the cloud copy have no common history (was the cloud project recreated?). Open the "
                                    "cloud copy as a separate project instead."));
    }
    auto baseFetched = fetch(link.projectId, *ancestor, request.cacheFolder, progress, stop);
    if (!baseFetched) {
        return std::unexpected(baseFetched.error());
    }
    const ProjectState baseNormalized = normalizeForSync(*baseFetched);

    std::map<std::string, MergeChoice> choices = request.choices;
    auto merged = mergeProjects(baseNormalized, localNormalized, theirsNormalized, choices);
    if (!merged) {
        return std::unexpected(fail(StoreFailure::Other, "The two copies cannot be merged: " + merged.error().message));
    }
    const auto unresolved = [&](const MergeResult& r) {
        std::size_t n = 0;
        for (const MergeConflict& c : r.conflicts) {
            n += choices.contains(c.id) ? 0 : 1;
        }
        return n;
    };
    if (unresolved(*merged) > 0 && request.takeAllTheirs) {
        for (const MergeConflict& c : merged->conflicts) {
            choices.emplace(c.id, MergeChoice::Theirs);
        }
        merged = mergeProjects(baseNormalized, localNormalized, theirsNormalized, choices);
        if (!merged) {
            return std::unexpected(fail(StoreFailure::Other, "The two copies cannot be merged: " + merged.error().message));
        }
    }
    if (merged->invalid || (unresolved(*merged) > 0 && !request.resolveRemaining)) {
        out.kind = SyncKind::NeedsChoices;
        out.conflicts = merged->conflicts;
        if (merged->invalid) {
            out.notes.push_back("These choices do not give a valid project: " + *merged->invalid);
        }
        return out;
    }
    for (const std::string& adjustment : merged->adjustments) {
        out.notes.push_back(adjustment);
    }
    ProjectState result = restoreLocalState(std::move(merged->merged), request.local, theirs);
    if (!request.mayPush) {
        finishPull(SyncKind::Merged, std::move(result));
        if (!request.pushBlockedBecause.empty()) {
            out.notes.push_back(request.pushBlockedBecause);
        }
        return out;
    }
    auto pushed = push(result, link, &*remote, headEtag, request.label, request.cacheFolder, progress, stop,
                       descends ? std::string_view() : std::string_view(link.baseSha), link.baseRevision);
    if (!pushed) {
        raced = pushed.error().kind == StoreFailure::PreconditionFailed;
        return std::unexpected(pushed.error());
    }
    out.changes = joinLines(diffProjects(request.local, result).summary());
    auto resultText = io::toJson(normalizeForSync(result));
    out.state = std::move(result);
    afterPush(*pushed, resultText ? sha256Hex(*resultText) : std::string());
    out.kind = SyncKind::MergedAndPushed;
    return out;
}

} // namespace wabcut::cloud
