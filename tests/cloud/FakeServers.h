#pragma once

// Fake servers for the cloud tests: an S3-compatible service and a WebDAV server behind IHttpTransport. They check what a real
// server checks - the Signature V4 of every S3 request is recomputed from the secret, Basic credentials are compared, conditional
// writes are enforced - and record every request so tests can assert what was sent. No socket is ever opened.

#include <wabcut/ai/Http.h>
#include <wabcut/cloud/Common.h>
#include <wabcut/cloud/HttpStores.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace wabcut::cloud::test {

struct Recorded {
    std::string method;
    std::string url;
    ai::HttpHeaders headers;
    std::string body;
    bool hadBodyFile = false;
};

struct TempFolder {
    std::filesystem::path path;
    explicit TempFolder(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("wabcut-cloud-test-" + name + "-" + newToken(systemRandom(), 5) + "-" + std::to_string(counter()++));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    static int& counter() {
        static int n = 0;
        return n;
    }
};

inline std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline void writeAll(const std::filesystem::path& file, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out << data;
}

/// Delivers a canned answer the way the real transports do (a 2xx body to the handlers in Raw mode, other bodies buffered).
inline std::expected<ai::HttpResponse, ai::AiError> answer(const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers, int status,
                                                           ai::HttpHeaders headers, std::string body) {
    ai::HttpResponse response;
    response.status = status;
    response.headers = std::move(headers);
    if (response.ok() && request.responseMode == ai::BodyMode::Raw) {
        if (handlers.onHead && !handlers.onHead(ai::HttpResponseHead{status, response.headers})) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::Cancelled, "handler stopped"));
        }
        if (handlers.onData && !body.empty() &&
            !handlers.onData(std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()))) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::Cancelled, "handler stopped"));
        }
        return response;
    }
    response.body = std::move(body);
    return response;
}

inline std::string requestBody(const ai::HttpRequest& request) {
    return request.bodyFile.empty() ? request.body : readAll(request.bodyFile);
}

inline std::string headerOf(const ai::HttpHeaders& headers, std::string_view name) {
    const auto value = ai::findHeader(headers, name);
    return value ? std::string(*value) : std::string();
}

inline std::string etagOf(const std::string& data) { return "\"" + sha256Hex(data).substr(0, 32) + "\""; }

// ---- S3 ----

class FakeS3 final : public ai::IHttpTransport {
public:
    struct Object {
        std::string data;
        std::string etag;
    };

    FakeS3(std::string bucket, std::string accessKey, std::string secretKey, std::string region = "us-east-1", bool pathStyle = true)
        : m_bucket(std::move(bucket)), m_accessKey(std::move(accessKey)), m_secretKey(std::move(secretKey)),
          m_region(std::move(region)), m_pathStyle(pathStyle) {}

    std::expected<ai::HttpResponse, ai::AiError> send(const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers,
                                                      std::stop_token) override {
        std::lock_guard lock(m_mutex);
        const std::string body = requestBody(request);
        requests.push_back({request.method, request.url, request.headers, request.bodyFile.empty() ? body : std::string("<file>"),
                            !request.bodyFile.empty()});
        const auto parts = ai::parseHttpUrl(request.url);
        if (!parts) {
            return answer(request, handlers, 400, {}, "bad url");
        }
        std::string path = parts->path;
        std::string query;
        if (const std::size_t q = path.find('?'); q != std::string::npos) {
            query = path.substr(q + 1);
            path.resize(q);
        }
        std::string host = parts->host + (parts->port ? ":" + std::to_string(*parts->port) : std::string());
        // Signature check (header authentication)
        const std::string auth = headerOf(request.headers, "authorization");
        const std::size_t sh = auth.find("SignedHeaders=");
        const std::size_t sg = auth.find("Signature=");
        if (sh == std::string::npos || sg == std::string::npos) {
            return answer(request, handlers, 403, {}, "<Error><Code>AccessDenied</Code></Error>");
        }
        const std::string signedList = auth.substr(sh + 14, auth.find(',', sh) - (sh + 14));
        SigV4Request s;
        s.method = request.method;
        s.canonicalUri = path;
        s.canonicalQuery = canonicalQuery(query);
        std::size_t start = 0;
        while (start <= signedList.size()) {
            const std::size_t semi = signedList.find(';', start);
            const std::string name = signedList.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            s.headers.emplace_back(name, name == "host" ? host : headerOf(request.headers, name));
            if (semi == std::string::npos) {
                break;
            }
            start = semi + 1;
        }
        s.payloadHash = headerOf(request.headers, "x-amz-content-sha256");
        s.region = m_region;
        s.amzDate = headerOf(request.headers, "x-amz-date");
        s.accessKey = m_accessKey;
        s.secretKey = m_secretKey;
        if (signV4(s).signature != auth.substr(sg + 10)) {
            return answer(request, handlers, 403, {}, "<Error><Code>SignatureDoesNotMatch</Code></Error>");
        }
        if (s.payloadHash != sha256Hex(body)) {
            return answer(request, handlers, 400, {}, "<Error><Code>XAmzContentSHA256Mismatch</Code></Error>");
        }
        // Routing
        std::string key;
        if (m_pathStyle) {
            const std::string prefix = "/" + m_bucket;
            if (path != prefix && path.compare(0, prefix.size() + 1, prefix + "/") != 0) {
                return answer(request, handlers, 404, {}, "<Error><Code>NoSuchBucket</Code></Error>");
            }
            key = path.size() > prefix.size() ? path.substr(prefix.size() + 1) : std::string();
        } else {
            if (host.compare(0, m_bucket.size() + 1, m_bucket + ".") != 0) {
                return answer(request, handlers, 404, {}, "<Error><Code>NoSuchBucket</Code></Error>");
            }
            key = path.size() > 1 ? path.substr(1) : std::string();
        }
        key = decode(key);
        if (request.method == "GET" && key.empty()) {
            return list(request, handlers, query);
        }
        if (request.method == "GET") {
            const auto it = objects.find(key);
            if (it == objects.end()) {
                return answer(request, handlers, 404, {}, "<Error><Code>NoSuchKey</Code></Error>");
            }
            return answer(request, handlers, 200, {{"ETag", it->second.etag}, {"Content-Length", std::to_string(it->second.data.size())}}, it->second.data);
        }
        if (request.method == "PUT") {
            const auto it = objects.find(key);
            const std::string none = headerOf(request.headers, "if-none-match");
            const std::string match = headerOf(request.headers, "if-match");
            if (none == "*" && it != objects.end()) {
                return answer(request, handlers, 412, {}, "<Error><Code>PreconditionFailed</Code></Error>");
            }
            if (!match.empty() && (it == objects.end() || it->second.etag != match)) {
                return answer(request, handlers, 412, {}, "<Error><Code>PreconditionFailed</Code></Error>");
            }
            objects[key] = Object{body, etagOf(body)};
            return answer(request, handlers, 200, {{"ETag", etagOf(body)}}, {});
        }
        if (request.method == "DELETE") {
            objects.erase(key);
            return answer(request, handlers, 204, {}, {});
        }
        return answer(request, handlers, 405, {}, {});
    }

    std::map<std::string, Object> objects;
    std::vector<Recorded> requests;
    std::size_t pageSize = 1000; ///< keys per listing page (tests set it small to see the continuation)

    /// Everything the store sent, flattened: used to prove a secret never travelled anywhere but into the signature.
    std::string everythingSent() const {
        std::string all;
        for (const Recorded& r : requests) {
            all += r.method + " " + r.url + "\n";
            for (const auto& h : r.headers) {
                if (!equalsIgnoreCase(h.name, "authorization")) {
                    all += h.name + ": " + h.value + "\n";
                }
            }
            all += r.body;
        }
        return all;
    }

private:
    static bool equalsIgnoreCase(const std::string& a, const std::string& b) { return ai::equalsIgnoreCase(a, b); }

    static std::string decode(const std::string& text) {
        const auto decoded = uriDecode(text);
        return decoded ? *decoded : text;
    }

    static std::string canonicalQuery(const std::string& query) {
        std::vector<std::string> pairs;
        std::size_t start = 0;
        while (start <= query.size() && !query.empty()) {
            const std::size_t amp = query.find('&', start);
            pairs.push_back(query.substr(start, amp == std::string::npos ? std::string::npos : amp - start));
            if (amp == std::string::npos) {
                break;
            }
            start = amp + 1;
        }
        std::sort(pairs.begin(), pairs.end());
        std::string out;
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            out += (i ? "&" : "") + pairs[i];
        }
        return out;
    }

    static std::string queryValue(const std::string& query, const std::string& name) {
        std::size_t start = 0;
        while (start <= query.size()) {
            const std::size_t amp = query.find('&', start);
            const std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (pair.compare(0, name.size() + 1, name + "=") == 0) {
                return decode(pair.substr(name.size() + 1));
            }
            if (amp == std::string::npos) {
                break;
            }
            start = amp + 1;
        }
        return {};
    }

    std::expected<ai::HttpResponse, ai::AiError> list(const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers,
                                                      const std::string& query) {
        const std::string prefix = queryValue(query, "prefix");
        const bool delimited = !queryValue(query, "delimiter").empty();
        const std::string after = queryValue(query, "continuation-token");
        std::vector<std::string> keys;
        std::set<std::string> folders;
        for (const auto& [key, object] : objects) {
            if (key.compare(0, prefix.size(), prefix) != 0 || key <= after) {
                continue;
            }
            const std::string rest = key.substr(prefix.size());
            const std::size_t slash = delimited ? rest.find('/') : std::string::npos;
            if (slash != std::string::npos) {
                folders.insert(prefix + rest.substr(0, slash + 1));
            } else {
                keys.push_back(key);
            }
        }
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
        const bool truncated = keys.size() > pageSize;
        if (truncated) {
            keys.resize(pageSize);
        }
        xml += std::string("<IsTruncated>") + (truncated ? "true" : "false") + "</IsTruncated>";
        if (truncated) {
            xml += "<NextContinuationToken>" + keys.back() + "</NextContinuationToken>";
        }
        for (const std::string& key : keys) {
            const Object& object = objects.at(key);
            xml += "<Contents><Key>" + key + "</Key><LastModified>2026-11-05T09:30:00.000Z</LastModified><ETag>&quot;" +
                   object.etag.substr(1, object.etag.size() - 2) + "&quot;</ETag><Size>" + std::to_string(object.data.size()) +
                   "</Size></Contents>";
        }
        for (const std::string& f : folders) {
            xml += "<CommonPrefixes><Prefix>" + f + "</Prefix></CommonPrefixes>";
        }
        xml += "</ListBucketResult>";
        return answer(request, handlers, 200, {{"Content-Type", "application/xml"}}, xml);
    }

    std::mutex m_mutex;
    std::string m_bucket;
    std::string m_accessKey;
    std::string m_secretKey;
    std::string m_region;
    bool m_pathStyle;
};

// ---- WebDAV ----

class FakeDav final : public ai::IHttpTransport {
public:
    struct Object {
        std::string data;
        std::string etag;
    };

    FakeDav(std::string basePath, std::string user, std::string password)
        : m_base(std::move(basePath)), m_auth("Basic " + base64Encode(user + ":" + password)) {
        folders.insert("");
    }

    std::expected<ai::HttpResponse, ai::AiError> send(const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers,
                                                      std::stop_token) override {
        std::lock_guard lock(m_mutex);
        const std::string body = requestBody(request);
        requests.push_back({request.method, request.url, request.headers, request.bodyFile.empty() ? body : std::string("<file>"),
                            !request.bodyFile.empty()});
        if (headerOf(request.headers, "authorization") != m_auth) {
            return answer(request, handlers, 401, {{"WWW-Authenticate", "Basic realm=\"dav\""}}, {});
        }
        const auto parts = ai::parseHttpUrl(request.url);
        if (!parts) {
            return answer(request, handlers, 400, {}, {});
        }
        std::string path = decode(parts->path);
        if (path.compare(0, m_base.size(), m_base) != 0) {
            return answer(request, handlers, 404, {}, {});
        }
        std::string rel = path.substr(m_base.size());
        while (!rel.empty() && rel.front() == '/') {
            rel.erase(rel.begin());
        }
        const bool trailing = !rel.empty() && rel.back() == '/';
        while (!rel.empty() && rel.back() == '/') {
            rel.pop_back();
        }
        const std::string parent = rel.find('/') == std::string::npos ? std::string() : rel.substr(0, rel.rfind('/'));
        if (request.method == "MKCOL") {
            if (folders.contains(rel) || files.contains(rel)) {
                return answer(request, handlers, 405, {}, {});
            }
            if (!folders.contains(parent)) {
                return answer(request, handlers, 409, {}, {});
            }
            folders.insert(rel);
            return answer(request, handlers, 201, {}, {});
        }
        if (request.method == "PUT") {
            if (!folders.contains(parent)) {
                return answer(request, handlers, 409, {}, {});
            }
            const auto it = files.find(rel);
            const std::string none = headerOf(request.headers, "if-none-match");
            const std::string match = headerOf(request.headers, "if-match");
            if ((none == "*" && it != files.end()) || (!match.empty() && (it == files.end() || it->second.etag != match))) {
                return answer(request, handlers, 412, {}, {});
            }
            const bool existed = it != files.end();
            files[rel] = Object{body, etagOf(body)};
            return answer(request, handlers, existed ? 204 : 201, includeEtag ? ai::HttpHeaders{{"ETag", etagOf(body)}} : ai::HttpHeaders{}, {});
        }
        if (request.method == "GET") {
            const auto it = files.find(rel);
            if (it == files.end()) {
                return answer(request, handlers, 404, {}, {});
            }
            return answer(request, handlers, 200, {{"ETag", it->second.etag}, {"Content-Length", std::to_string(it->second.data.size())}}, it->second.data);
        }
        if (request.method == "DELETE") {
            files.erase(rel);
            return answer(request, handlers, 204, {}, {});
        }
        if (request.method == "PROPFIND") {
            (void)trailing;
            if (!folders.contains(rel) && !files.contains(rel)) {
                return answer(request, handlers, 404, {}, {});
            }
            std::string xml = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">";
            xml += entry(rel, folders.contains(rel));
            if (headerOf(request.headers, "depth") == "1" && folders.contains(rel)) {
                for (const std::string& f : folders) {
                    if (!f.empty() && f != rel && (f.find('/') == std::string::npos ? std::string() : f.substr(0, f.rfind('/'))) == rel) {
                        xml += entry(f, true);
                    }
                }
                for (const auto& [file, object] : files) {
                    if ((file.find('/') == std::string::npos ? std::string() : file.substr(0, file.rfind('/'))) == rel) {
                        xml += entry(file, false);
                    }
                }
            }
            xml += extraEntries;
            xml += "</d:multistatus>";
            return answer(request, handlers, 207, {{"Content-Type", "application/xml"}}, xml);
        }
        return answer(request, handlers, 405, {}, {});
    }

    std::map<std::string, Object> files;
    std::set<std::string> folders;
    std::vector<Recorded> requests;
    bool includeEtag = true;
    std::string extraEntries; ///< raw <d:response> elements appended to every PROPFIND answer (hostile hrefs)

private:
    static std::string decode(const std::string& text) {
        const auto decoded = uriDecode(text);
        return decoded ? *decoded : text;
    }

    std::string entry(const std::string& rel, bool folder) const {
        std::string href = m_base + (rel.empty() ? "" : "/" + uriEncode(rel, false)) + (folder ? "/" : "");
        std::string xml = "<d:response><d:href>" + href + "</d:href><d:propstat><d:prop>";
        if (folder) {
            xml += "<d:resourcetype><d:collection/></d:resourcetype>";
        } else {
            const Object& object = files.at(rel);
            xml += "<d:getetag>" + object.etag + "</d:getetag><d:getcontentlength>" + std::to_string(object.data.size()) +
                   "</d:getcontentlength><d:resourcetype/>";
        }
        xml += "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
        return xml;
    }

    std::mutex m_mutex;
    std::string m_base;
    std::string m_auth;
};

} // namespace wabcut::cloud::test
