#include <wabcut/script/Permissions.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <array>
#include <fstream>
#include <sstream>

namespace wabcut::script {

namespace fs = std::filesystem;
using command::Capability;
using command::CapabilitySet;

namespace {

constexpr int kStoreVersion = 1;
constexpr std::size_t kMaxStoreBytes = 4 * 1024 * 1024;
constexpr std::array kAllCapabilities{Capability::Read, Capability::EditSequence, Capability::EditProject,
                                      Capability::MediaAnalysis, Capability::Network};

CapabilitySet intersect(CapabilitySet a, CapabilitySet b) { return {a.bits & b.bits}; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::string folderKey(const fs::path& project) {
    std::error_code ec;
    const fs::path folder = fs::weakly_canonical(project.parent_path(), ec);
    return pathToUtf8(ec ? project.parent_path() : folder);
}

/// An extension's own folder, as the grants remember it.
std::string extensionFolderKey(const fs::path& folder) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(folder, ec);
    return pathToUtf8(ec ? folder : canonical);
}

} // namespace

CapabilityInfo describe(Capability capability) {
    switch (capability) {
    case Capability::Read:
        return {capability, "Read your project",
                "See the project's sequences, clips, markers, media names, transcripts and captions."};
    case Capability::EditSequence:
        return {capability, "Edit your sequences",
                "Change clips, markers, captions, effects and audio inside sequences; each change is one undo step "
                "named after the extension."};
    case Capability::EditProject:
        return {capability, "Change the project's structure",
                "Add, change and remove tracks, bins, media, subclips and sequences; every change can be undone."};
    case Capability::MediaAnalysis:
        return {capability, "Analyse media and use AI features",
                "Run analysis and AI commands (transcription, search, translation, dubbing...). Cloud AI still asks "
                "for your consent and stays within your spend cap and no-cloud settings."};
    case Capability::Network:
        break;
    }
    return {capability, "Network", "Extensions are never given network access."};
}

std::string capabilityList(CapabilitySet set) {
    std::string out;
    for (const Capability c : kAllCapabilities) {
        if (set.has(c)) {
            out += out.empty() ? "" : ", ";
            out += command::toString(c);
        }
    }
    return out;
}

std::optional<CapabilitySet> parseCapabilityList(std::string_view text) {
    CapabilitySet set;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = std::min(text.find(',', start), text.size());
        std::string_view item = text.substr(start, comma - start);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            bool known = false;
            for (const Capability c : kAllCapabilities) {
                if (equalsIgnoringCase(item, command::toString(c))) {
                    set.bits |= CapabilitySet::of({c}).bits;
                    known = true;
                }
            }
            if (!known) {
                return std::nullopt;
            }
        }
        start = comma + 1;
    }
    return set;
}

PermissionStore PermissionStore::load(const fs::path& file) {
    PermissionStore empty;
    empty.m_file = file;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return empty;
    }
    std::string bytes;
    bool read = false;
    {
        std::ifstream in(file, std::ios::binary); // closed before the rename below (Windows renames no open file)
        std::stringstream text;
        text << in.rdbuf();
        bytes = text.str();
        read = static_cast<bool>(in);
    }
    std::optional<PermissionStore> loaded;
    if (read && bytes.size() <= kMaxStoreBytes && io::jsonNestingWithin(bytes, 16)) {
        const Json json = Json::parse(bytes, nullptr, false);
        if (auto parsed = fromJson(json)) {
            loaded = std::move(*parsed);
        }
    }
    if (!loaded) {
        // Never reuse a damaged file (it could grant something by accident): keep it aside and start empty.
        fs::path bad = file;
        bad += ".bad";
        fs::rename(file, bad, ec);
        return empty;
    }
    loaded->m_file = file;
    return std::move(*loaded);
}

std::expected<void, Error> PermissionStore::save() const {
    std::error_code ec;
    fs::create_directories(m_file.parent_path(), ec);
    return io::writeFileAtomically(m_file, toJson().dump(2));
}

const ExtensionGrant* PermissionStore::find(std::string_view id) const {
    const auto it = m_grants.find(id);
    return it == m_grants.end() ? nullptr : &it->second;
}

ExtensionReview PermissionStore::review(const ExtensionBundle& bundle, const fs::path& project) const {
    ExtensionReview r;
    r.manifest = bundle.manifest;
    r.scope = bundle.scope;
    r.folder = bundle.folder;
    r.contentHash = bundle.contentHash;
    const ExtensionGrant* grant = find(bundle.manifest.id);
    r.update = grant && !grant->reviewedVersion.empty();
    r.authorChanged = r.update && !grant->reviewedAuthor.empty() && grant->reviewedAuthor != bundle.manifest.author;
    for (const Capability c : bundle.manifest.capabilities) {
        // Someone else's extension under the same id starts from nothing: no box is ticked for the user.
        r.requests.push_back({describe(c), grant && !r.authorChanged && grant->granted.has(c)});
    }
    if (r.authorChanged) {
        r.notes.push_back("The author changed from \"" + grant->reviewedAuthor + "\" to \"" + bundle.manifest.author +
                          "\": review it as a new extension. It starts with empty storage.");
    } else if (r.update && !isReviewed(bundle)) {
        r.notes.push_back(grant->reviewedFolder == extensionFolderKey(bundle.folder)
                              ? "Its files changed since you reviewed it: it runs only once you review it again."
                              : "It is not the copy you reviewed (that one is in " + grant->reviewedFolder +
                                    "): it runs only once you review this one.");
    }
    if (bundle.scope == ExtensionScope::Project) {
        r.needsTrust = project.empty() || !isTrusted(project, bundle);
        r.notes.push_back("This extension comes with the project, not from you: run it only if you trust where the "
                          "project came from.");
    }
    if (r.update && grant->reviewedVersion != toString(bundle.manifest.version)) {
        r.notes.push_back("Updated from version " + grant->reviewedVersion + ".");
    }
    for (const std::string& w : bundle.manifest.warnings) {
        r.notes.push_back(w);
    }
    return r;
}

std::expected<void, Error> PermissionStore::grant(const ExtensionBundle& bundle, CapabilitySet chosen) {
    const Manifest& manifest = bundle.manifest;
    const CapabilitySet allowed = intersect(manifest.requested(), grantableCapabilities());
    if ((chosen.bits & ~allowed.bits) != 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     "can only grant what " + manifest.name + " asks for (" + capabilityList(allowed) +
                                         "), not " + capabilityList({chosen.bits & ~allowed.bits})});
    }
    ExtensionGrant& g = m_grants[manifest.id];
    g.granted = chosen;
    g.enabled = true;
    g.reviewedVersion = toString(manifest.version);
    g.reviewedHash = bundle.contentHash;
    g.reviewedAuthor = manifest.author;
    g.reviewedFolder = extensionFolderKey(bundle.folder);
    g.crashes = 0;
    g.disabledReason.clear();
    return {};
}

void PermissionStore::revoke(std::string_view id) {
    ExtensionGrant& g = m_grants[std::string(id)];
    g.granted = {};
    g.enabled = false;
    g.disabledReason = "permissions revoked";
}

void PermissionStore::setEnabled(std::string_view id, bool enabled) {
    ExtensionGrant& g = m_grants[std::string(id)];
    g.enabled = enabled;
    if (enabled) {
        g.crashes = 0;
        g.disabledReason.clear();
    }
}

void PermissionStore::forget(std::string_view id) {
    if (const auto it = m_grants.find(id); it != m_grants.end()) {
        m_grants.erase(it);
    }
    const std::string suffix = "|" + std::string(id);
    std::erase_if(m_trust, [&](const auto& entry) { return entry.first.ends_with(suffix); });
}

bool PermissionStore::isEnabled(std::string_view id) const {
    const ExtensionGrant* g = find(id);
    return g && g->enabled;
}

bool PermissionStore::isReviewed(const ExtensionBundle& bundle) const {
    const ExtensionGrant* g = find(bundle.manifest.id);
    return g && !g->reviewedVersion.empty() && !g->reviewedHash.empty() && g->reviewedHash == bundle.contentHash &&
           g->reviewedFolder == extensionFolderKey(bundle.folder);
}

CapabilitySet PermissionStore::effective(const ExtensionBundle& bundle) const {
    const ExtensionGrant* g = find(bundle.manifest.id);
    if (!g || !g->enabled || !isReviewed(bundle)) {
        return {};
    }
    return intersect(intersect(g->granted, bundle.manifest.requested()), grantableCapabilities());
}

bool PermissionStore::needsReview(const ExtensionBundle& bundle) const {
    const ExtensionGrant* g = find(bundle.manifest.id);
    if (!g || !isReviewed(bundle)) {
        return true;
    }
    return (bundle.manifest.requested().bits & ~g->granted.bits & grantableCapabilities().bits) != 0;
}

std::string PermissionStore::trustKey(const fs::path& project, std::string_view id) {
    return folderKey(project) + "|" + std::string(id);
}

void PermissionStore::trust(const fs::path& project, const ExtensionBundle& bundle) {
    m_trust[trustKey(project, bundle.manifest.id)] = bundle.contentHash;
}

void PermissionStore::distrust(const fs::path& project, std::string_view id) {
    if (const auto it = m_trust.find(trustKey(project, id)); it != m_trust.end()) {
        m_trust.erase(it);
    }
}

bool PermissionStore::isTrusted(const fs::path& project, const ExtensionBundle& bundle) const {
    const auto it = m_trust.find(trustKey(project, bundle.manifest.id));
    return it != m_trust.end() && !bundle.contentHash.empty() && it->second == bundle.contentHash;
}

int PermissionStore::recordCrash(std::string_view id, std::string_view why) {
    ExtensionGrant& g = m_grants[std::string(id)];
    ++g.crashes;
    if (g.crashes >= kMaxCrashes && g.enabled) {
        g.enabled = false;
        g.disabledReason = "crashed " + std::to_string(g.crashes) + " times in a row (last: " + std::string(why) +
                           "); enable it again to retry";
    }
    return g.crashes;
}

void PermissionStore::recordCleanRun(std::string_view id) {
    if (const auto it = m_grants.find(id); it != m_grants.end()) {
        it->second.crashes = 0;
    }
}

Json PermissionStore::toJson() const {
    Json extensions = Json::object();
    for (const auto& [id, g] : m_grants) {
        Json granted = Json::array();
        for (const Capability c : kAllCapabilities) {
            if (g.granted.has(c)) {
                granted.push_back(command::toString(c));
            }
        }
        extensions[id] = {{"enabled", g.enabled},
                          {"granted", granted},
                          {"reviewedVersion", g.reviewedVersion},
                          {"reviewedHash", g.reviewedHash},
                          {"reviewedAuthor", g.reviewedAuthor},
                          {"reviewedFolder", g.reviewedFolder},
                          {"crashes", g.crashes},
                          {"disabledReason", g.disabledReason}};
    }
    Json trust = Json::array();
    for (const auto& [key, hash] : m_trust) {
        const std::size_t bar = key.rfind('|');
        trust.push_back({{"project", key.substr(0, bar)}, {"extension", key.substr(bar + 1)}, {"contentHash", hash}});
    }
    return {{"format", "wabcut-extension-permissions"}, {"version", kStoreVersion}, {"extensions", extensions},
            {"trustedProjectExtensions", trust}};
}

std::expected<PermissionStore, std::string> PermissionStore::fromJson(const Json& json) {
    try {
        if (!json.is_object() || json.value("format", "") != "wabcut-extension-permissions" ||
            json.value("version", 0) != kStoreVersion) {
            return std::unexpected(std::string("not a version 1 permission file"));
        }
        PermissionStore store;
        for (const auto& [id, g] : json.at("extensions").items()) {
            if (!isValidExtensionId(id)) {
                return std::unexpected("bad extension id " + id);
            }
            ExtensionGrant grant;
            grant.enabled = g.at("enabled").get<bool>();
            for (const Json& name : g.at("granted")) {
                const auto c = command::capabilityFromString(name.get<std::string>());
                if (!c || !grantableCapabilities().has(*c)) {
                    return std::unexpected(std::string("bad capability"));
                }
                grant.granted.bits |= CapabilitySet::of({*c}).bits;
            }
            grant.reviewedVersion = g.value("reviewedVersion", "");
            // A grant from before S5-P2-4 has none of these: the extension is reviewed again before it runs.
            grant.reviewedHash = g.value("reviewedHash", "");
            grant.reviewedAuthor = g.value("reviewedAuthor", "");
            grant.reviewedFolder = g.value("reviewedFolder", "");
            grant.crashes = g.value("crashes", 0);
            grant.disabledReason = g.value("disabledReason", "");
            store.m_grants[id] = std::move(grant);
        }
        for (const Json& t : json.at("trustedProjectExtensions")) {
            const std::string id = t.at("extension").get<std::string>();
            if (!isValidExtensionId(id)) {
                return std::unexpected("bad extension id " + id);
            }
            store.m_trust[t.at("project").get<std::string>() + "|" + id] = t.at("contentHash").get<std::string>();
        }
        return store;
    } catch (const Json::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
}

} // namespace wabcut::script
