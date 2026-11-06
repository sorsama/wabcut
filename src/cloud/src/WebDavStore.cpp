// WebDavStore (HttpStores.h): RFC 4918 over the app's network stack.

#include <wabcut/cloud/HttpStores.h>

#include "FileUtil.h"
#include "HttpCommon.h"

#include <wabcut/model/MediaLocator.h>

#include <pugixml.hpp>

#include <algorithm>
#include <cstdlib>

namespace wabcut::cloud {

namespace fs = std::filesystem;

struct WebDavStore::Reply : detail::Raw {};

namespace {

constexpr std::size_t kMaxEntries = 100000;

const char* const kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/><d:getcontentlength/>"
    "<d:getlastmodified/><d:resourcetype/></d:prop></d:propfind>";

bool statusIs200(const std::string& status) {
    // "HTTP/1.1 200 OK"
    const std::size_t space = status.find(' ');
    return space != std::string::npos && status.compare(space + 1, 3, "200") == 0;
}

} // namespace

WebDavStore::WebDavStore(ai::IHttpTransport& transport, WebDavConfig config, ai::Secret password)
    : m_transport(transport), m_config(std::move(config)) {
    m_problem = endpointProblem(m_config.url, m_config.allowInsecure);
    if (m_problem.empty() && m_config.user.empty()) {
        m_problem = "Enter the user name.";
    }
    if (m_problem.empty() && password.empty()) {
        m_problem = "Enter the password.";
    }
    if (m_problem.empty()) {
        std::string url = m_config.url;
        while (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
        m_base = url;
        const auto parts = ai::parseHttpUrl(url);
        m_basePath = parts ? parts->path : std::string();
        m_authorization = "Basic " + base64Encode(m_config.user + ":" + std::string(password.reveal()));
    }
}

std::string WebDavStore::describe() const { return "WebDAV " + m_base; }

std::string WebDavStore::urlFor(std::string_view path, bool collection) const {
    std::string url = m_base;
    if (!path.empty()) {
        url += "/" + uriEncode(path, false);
    }
    if (collection) {
        url += "/";
    }
    return url;
}

ai::HttpRequest WebDavStore::request(std::string method, std::string url) const {
    ai::HttpRequest r;
    r.method = std::move(method);
    r.url = std::move(url);
    r.headers.push_back({"Authorization", m_authorization});
    return r;
}

StoreResult<WebDavStore::Reply> WebDavStore::send(ai::HttpRequest request, const ai::HttpStreamHandlers&, std::stop_token stop) const {
    auto reply = detail::sendBuffered(m_transport, std::move(request), 8u * 1024u * 1024u, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    return Reply{std::move(*reply)};
}

StoreResult<void> WebDavStore::ensureParents(std::string_view path, std::stop_token stop) {
    std::string folder;
    std::size_t start = 0;
    const std::string_view parent = parentOf(path);
    while (!parent.empty() && start <= parent.size()) {
        const std::size_t slash = parent.find('/', start);
        const std::string_view part = parent.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        folder = joinPath(folder, part);
        if (!m_folders.contains(folder)) {
            auto reply = send(request("MKCOL", urlFor(folder, true)), {}, stop);
            if (!reply) {
                return std::unexpected(reply.error());
            }
            // 201 created; 405 already exists (RFC 4918 9.3.1); 301 when a server redirects to the collection's own URL
            if (!reply->ok() && reply->status != 405 && reply->status != 301) {
                return std::unexpected(fromStatus(reply->status, "creating the folder " + folder));
            }
            m_folders.insert(folder);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return {};
}

StoreResult<std::vector<StoreEntry>> WebDavStore::propfind(std::string_view path, int depth, std::stop_token stop) {
    ai::HttpRequest r = request("PROPFIND", urlFor(path, depth > 0));
    r.headers.push_back({"Depth", std::to_string(depth)});
    r.headers.push_back({"Content-Type", "application/xml; charset=utf-8"});
    r.body = kPropfindBody;
    auto reply = send(std::move(r), {}, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (reply->status == 404) {
        return std::vector<StoreEntry>();
    }
    if (reply->status != 207 && !reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "PROPFIND " + std::string(path)));
    }
    pugi::xml_document doc;
    if (auto problem = detail::parseXml(reply->body, doc)) {
        return std::unexpected(StoreError{StoreFailure::Other, *problem});
    }
    std::vector<StoreEntry> out;
    const pugi::xml_node multistatus = doc.first_child();
    for (pugi::xml_node response = multistatus.first_child(); response; response = response.next_sibling()) {
        if (response.type() != pugi::node_element) {
            continue;
        }
        std::string href = detail::localText(response, "href");
        if (href.empty()) {
            continue;
        }
        // The href is an absolute URL or an absolute path; only its path counts, percent-decoded, below the collection's own.
        if (const auto parts = ai::parseHttpUrl(href)) {
            href = parts->path;
        }
        const std::size_t cut = href.find_first_of("?#");
        if (cut != std::string::npos) {
            href.resize(cut);
        }
        auto decoded = uriDecode(href);
        if (!decoded) {
            continue;
        }
        auto basePath = uriDecode(m_basePath);
        const std::string base = basePath ? *basePath : m_basePath;
        std::string full = *decoded;
        while (full.size() > 1 && full.back() == '/') {
            full.pop_back();
        }
        std::string root = base;
        while (root.size() > 1 && root.back() == '/') {
            root.pop_back();
        }
        if (full == root) {
            continue; // the collection itself
        }
        const std::string requested = std::string(path);
        if (full.compare(0, root.size() + 1, root + "/") != 0) {
            continue; // outside the store's root: never followed
        }
        const std::string relative = full.substr(root.size() + 1);
        if (!validPath(relative) || relative == requested) {
            continue; // not a name of ours, or the folder that was asked about
        }
        StoreEntry entry;
        entry.path = relative;
        for (pugi::xml_node propstat = response.first_child(); propstat; propstat = propstat.next_sibling()) {
            if (propstat.type() != pugi::node_element) {
                continue;
            }
            std::string_view name = propstat.name();
            if (const std::size_t colon = name.rfind(':'); colon != std::string_view::npos) {
                name.remove_prefix(colon + 1);
            }
            if (name != "propstat") {
                continue;
            }
            if (!statusIs200(detail::localText(propstat, "status"))) {
                continue;
            }
            const pugi::xml_node prop = detail::localChild(propstat, "prop");
            entry.etag = detail::localText(prop, "getetag");
            const std::string length = detail::localText(prop, "getcontentlength");
            entry.size = length.empty() ? 0 : std::strtoull(length.c_str(), nullptr, 10);
            entry.folder = static_cast<bool>(detail::localChild(detail::localChild(prop, "resourcetype"), "collection"));
        }
        out.push_back(std::move(entry));
        if (out.size() > kMaxEntries) {
            return std::unexpected(StoreError{StoreFailure::TooLarge, "more entries than allowed in the listing"});
        }
    }
    return out;
}

StoreResult<std::optional<StoreEntry>> WebDavStore::stat(std::string_view path, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    // Depth 0 answers with the resource itself, which the listing code skips as "the collection itself": ask for the
    // parent's children instead and pick the name.
    auto children = propfind(parentOf(path), 1, stop);
    if (!children) {
        return std::unexpected(children.error());
    }
    for (StoreEntry& entry : *children) {
        if (entry.path == path && !entry.folder) {
            return std::optional<StoreEntry>(std::move(entry));
        }
    }
    return std::optional<StoreEntry>();
}

StoreResult<std::vector<StoreEntry>> WebDavStore::list(std::string_view folder, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!folder.empty() && !validPath(folder)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(folder)});
    }
    return propfind(folder, 1, stop);
}

StoreResult<Fetched> WebDavStore::get(std::string_view path, std::uint64_t maxBytes, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto reply = detail::sendBuffered(m_transport, request("GET", urlFor(path, false)), maxBytes, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "GET " + std::string(path)));
    }
    return Fetched{std::move(reply->body), detail::headerOr(reply->headers, "etag")};
}

StoreResult<std::string> WebDavStore::getToFile(std::string_view path, const fs::path& file, std::uint64_t maxBytes,
                                                const Progress& progress, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto reply = detail::sendToFile(m_transport, request("GET", urlFor(path, false)), file, maxBytes, progress, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "GET " + std::string(path)));
    }
    return detail::headerOr(reply->headers, "etag");
}

StoreResult<std::string> WebDavStore::write(std::string_view path, ai::HttpRequest put, const Precondition& precondition,
                                            std::stop_token stop) {
    if (precondition.kind == Precondition::Kind::Absent) {
        put.headers.push_back({"If-None-Match", "*"});
    } else if (precondition.kind == Precondition::Kind::Etag) {
        put.headers.push_back({"If-Match", precondition.etag});
    }
    if (auto parents = ensureParents(path, stop); !parents) {
        return std::unexpected(parents.error());
    }
    auto reply = detail::sendUpload(m_transport, std::move(put), {}, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "PUT " + std::string(path)));
    }
    std::string etag = detail::headerOr(reply->headers, "etag");
    if (etag.empty()) { // some servers do not send it: ask
        if (auto now = stat(path, stop); now && *now) {
            etag = (*now)->etag;
        }
    }
    return etag;
}

StoreResult<std::string> WebDavStore::put(std::string_view path, std::string_view data, const Precondition& precondition,
                                          std::string_view contentType, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    ai::HttpRequest r = request("PUT", urlFor(path, false));
    r.headers.push_back({"Content-Type", std::string(contentType)});
    r.body = std::string(data);
    return write(path, std::move(r), precondition, stop);
}

StoreResult<std::string> WebDavStore::putFile(std::string_view path, const fs::path& file, const Precondition& precondition,
                                              std::string_view contentType, const Progress& progress, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    ai::HttpRequest r = request("PUT", urlFor(path, false));
    r.headers.push_back({"Content-Type", std::string(contentType)});
    r.bodyFile = file;
    if (precondition.kind == Precondition::Kind::Absent) {
        r.headers.push_back({"If-None-Match", "*"});
    } else if (precondition.kind == Precondition::Kind::Etag) {
        r.headers.push_back({"If-Match", precondition.etag});
    }
    if (auto parents = ensureParents(path, stop); !parents) {
        return std::unexpected(parents.error());
    }
    auto reply = detail::sendUpload(m_transport, std::move(r), progress, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "PUT " + std::string(path)));
    }
    return detail::headerOr(reply->headers, "etag");
}

StoreResult<void> WebDavStore::remove(std::string_view path, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto reply = send(request("DELETE", urlFor(path, false)), {}, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok() && reply->status != 404) {
        return std::unexpected(fromStatus(reply->status, "DELETE " + std::string(path)));
    }
    return {};
}

StoreResult<void> WebDavStore::makeFolder(std::string_view path, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    return ensureParents(std::string(path) + "/x", stop);
}

} // namespace wabcut::cloud
