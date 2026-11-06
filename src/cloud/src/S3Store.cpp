// S3Store (HttpStores.h): AWS Signature Version 4 and the S3 REST calls the cloud library needs.
// AWS documents the signing process at https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html and
// https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-query-string-auth.html; conditional writes (If-None-Match: *,
// If-Match) at https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-writes.html. ListObjectsV2:
// https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjectsV2.html.

#include <wabcut/cloud/HttpStores.h>

#include "FileUtil.h"
#include "HttpCommon.h"

#include <wabcut/model/MediaLocator.h>

#include <pugixml.hpp>

#include <algorithm>
#include <cstdlib>

namespace wabcut::cloud {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxSinglePut = 4500ull * 1024ull * 1024ull; // 4.5 GB: past it S3 needs multipart upload
constexpr std::size_t kMaxListed = 100000;
constexpr const char* kEmptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::string compactDate(std::int64_t unixSeconds) {
    const std::string iso = formatIsoUtc(unixSeconds); // 2013-05-24T00:00:00Z
    std::string out;
    for (const char c : iso) {
        if (c != '-' && c != ':') {
            out += c;
        }
    }
    return out; // 20130524T000000Z
}

std::string trimmed(std::string_view text) {
    std::size_t a = 0;
    std::size_t b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) {
        ++a;
    }
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) {
        --b;
    }
    return std::string(text.substr(a, b - a));
}

} // namespace

SigV4Result signV4(const SigV4Request& r) {
    auto headers = r.headers;
    std::sort(headers.begin(), headers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    SigV4Result out;
    std::string canonicalHeaders;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        canonicalHeaders += headers[i].first + ":" + trimmed(headers[i].second) + "\n";
        out.signedHeaders += (i ? ";" : "") + headers[i].first;
    }
    out.canonicalRequest = r.method + "\n" + r.canonicalUri + "\n" + r.canonicalQuery + "\n" + canonicalHeaders + "\n" +
                           out.signedHeaders + "\n" + r.payloadHash;
    const std::string date = r.amzDate.substr(0, 8);
    const std::string scope = date + "/" + r.region + "/" + r.service + "/aws4_request";
    out.stringToSign = "AWS4-HMAC-SHA256\n" + r.amzDate + "\n" + scope + "\n" + sha256Hex(out.canonicalRequest);
    const std::string kDate = hmacSha256("AWS4" + r.secretKey, date);
    const std::string kRegion = hmacSha256(kDate, r.region);
    const std::string kService = hmacSha256(kRegion, r.service);
    const std::string kSigning = hmacSha256(kService, "aws4_request");
    out.signature = toHex(hmacSha256(kSigning, out.stringToSign));
    out.authorization = "AWS4-HMAC-SHA256 Credential=" + r.accessKey + "/" + scope + ", SignedHeaders=" + out.signedHeaders +
                        ", Signature=" + out.signature;
    return out;
}

struct S3Store::Reply : detail::Raw {};

S3Store::S3Store(ai::IHttpTransport& transport, S3Config config, ai::Secret accessKeyId, ai::Secret secretAccessKey, UnixClock clock)
    : m_transport(transport),
      m_config(std::move(config)),
      m_accessKey(std::move(accessKeyId)),
      m_secretKey(std::move(secretAccessKey)),
      m_clock(std::move(clock)) {
    m_problem = endpointProblem(m_config.endpoint, m_config.allowInsecure);
    if (m_problem.empty()) {
        m_endpoint = *ai::parseHttpUrl(m_config.endpoint);
        if (!m_endpoint.path.empty() && m_endpoint.path != "/") {
            m_problem = "The endpoint must be the service address without a path (the bucket is a separate setting).";
        }
    }
    if (m_problem.empty() && (m_config.bucket.empty() || m_config.bucket.find('/') != std::string::npos)) {
        m_problem = "Enter the bucket name.";
    }
    if (m_problem.empty() && (m_accessKey.empty() || m_secretKey.empty())) {
        m_problem = "Enter the access key and the secret key.";
    }
    if (m_problem.empty() && m_config.region.empty()) {
        m_problem = "Enter the region (use us-east-1 or auto when the service does not care).";
    }
    // The key prefix: segments joined, no leading or trailing slash.
    std::string prefix;
    std::size_t start = 0;
    const std::string& raw = m_config.prefix;
    while (start <= raw.size()) {
        const std::size_t slash = raw.find('/', start);
        const std::string part = raw.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!validSegment(part)) {
                m_problem = "The folder name in the bucket may only use letters, digits, '.', '_' and '-'.";
                break;
            }
            prefix += part + "/";
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    m_prefix = prefix;
}

std::string S3Store::describe() const {
    return "S3 bucket " + m_config.bucket + " at " + m_endpoint.host + (m_prefix.empty() ? "" : " (" + m_prefix + ")");
}

std::string S3Store::keyFor(std::string_view path) const { return m_prefix + std::string(path); }

S3Store::Target S3Store::target(std::string_view key) const {
    Target t;
    const std::string authority = detail::hostHeader(m_endpoint);
    std::string host = authority;
    std::string uri = "/";
    if (m_config.pathStyle) {
        uri += uriEncode(m_config.bucket, false) + (key.empty() ? "" : "/" + uriEncode(key, false));
    } else {
        host = m_config.bucket + "." + authority;
        uri += uriEncode(key, false);
    }
    t.host = host;
    t.canonicalUri = uri;
    t.url = m_endpoint.scheme + "://" + host + uri;
    return t;
}

ai::HttpRequest S3Store::signed_(std::string method, std::string_view key, const std::string& canonicalQuery,
                                 const std::string& payloadHash, std::string_view contentType, const Precondition& precondition) const {
    const Target t = target(key);
    const std::string amzDate = compactDate(m_clock());
    SigV4Request s;
    s.method = method;
    s.canonicalUri = t.canonicalUri;
    s.canonicalQuery = canonicalQuery;
    s.payloadHash = payloadHash;
    s.region = m_config.region;
    s.amzDate = amzDate;
    s.accessKey = std::string(m_accessKey.reveal());
    s.secretKey = std::string(m_secretKey.reveal());
    s.headers = {{"host", t.host}, {"x-amz-content-sha256", payloadHash}, {"x-amz-date", amzDate}};
    if (!contentType.empty()) {
        s.headers.emplace_back("content-type", std::string(contentType));
    }
    if (precondition.kind == Precondition::Kind::Absent) {
        s.headers.emplace_back("if-none-match", "*");
    } else if (precondition.kind == Precondition::Kind::Etag) {
        s.headers.emplace_back("if-match", precondition.etag);
    }
    const SigV4Result signature = signV4(s);
    ai::HttpRequest request;
    request.method = std::move(method);
    request.url = t.url + (canonicalQuery.empty() ? "" : "?" + canonicalQuery);
    for (const auto& [name, value] : s.headers) {
        if (name != "host") { // the transport sets Host from the URL; it equals the signed value
            request.headers.push_back({name, value});
        }
    }
    request.headers.push_back({"Authorization", signature.authorization});
    return request;
}

StoreResult<S3Store::Reply> S3Store::send(ai::HttpRequest request, const ai::HttpStreamHandlers&, std::stop_token stop) const {
    auto reply = detail::sendBuffered(m_transport, std::move(request), 8u * 1024u * 1024u, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    return Reply{std::move(*reply)};
}

StoreResult<std::string> S3Store::presignGet(std::string_view path, int seconds) const {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path) || seconds < 1 || seconds > 604800) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "a link lasts 1 second to 7 days"});
    }
    const Target t = target(keyFor(path));
    const std::string amzDate = compactDate(m_clock());
    const std::string scope = amzDate.substr(0, 8) + "/" + m_config.region + "/s3/aws4_request";
    const std::string query = "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=" +
                              uriEncode(std::string(m_accessKey.reveal()) + "/" + scope, true) + "&X-Amz-Date=" + amzDate +
                              "&X-Amz-Expires=" + std::to_string(seconds) + "&X-Amz-SignedHeaders=host";
    SigV4Request s;
    s.method = "GET";
    s.canonicalUri = t.canonicalUri;
    s.canonicalQuery = query;
    s.headers = {{"host", t.host}};
    s.payloadHash = "UNSIGNED-PAYLOAD";
    s.region = m_config.region;
    s.amzDate = amzDate;
    s.accessKey = std::string(m_accessKey.reveal());
    s.secretKey = std::string(m_secretKey.reveal());
    return t.url + "?" + query + "&X-Amz-Signature=" + signV4(s).signature;
}

StoreResult<std::vector<StoreEntry>> S3Store::listing(std::string_view prefixKey, bool delimit, std::size_t limit, std::stop_token stop) {
    std::vector<StoreEntry> out;
    std::string token;
    for (;;) {
        std::string query;
        if (!token.empty()) {
            query += "continuation-token=" + uriEncode(token, true) + "&";
        }
        if (delimit) {
            query += "delimiter=%2F&";
        }
        query += "list-type=2&prefix=" + uriEncode(prefixKey, true);
        auto reply = send(signed_("GET", "", query, kEmptyHash, "", Precondition::none()), {}, stop);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        if (!reply->ok()) {
            return std::unexpected(fromStatus(reply->status, "listing the bucket"));
        }
        pugi::xml_document doc;
        if (auto problem = detail::parseXml(reply->body, doc)) {
            return std::unexpected(StoreError{StoreFailure::Other, *problem});
        }
        const pugi::xml_node root = doc.first_child();
        for (pugi::xml_node c = root.child("Contents"); c; c = c.next_sibling("Contents")) {
            const std::string key = c.child("Key").text().as_string();
            if (key.size() <= m_prefix.size() || key.compare(0, m_prefix.size(), m_prefix) != 0) {
                continue;
            }
            const std::string path = key.substr(m_prefix.size());
            if (!validPath(path)) {
                continue; // a name this library never makes
            }
            StoreEntry entry;
            entry.path = path;
            entry.size = std::strtoull(c.child("Size").text().as_string("0"), nullptr, 10);
            entry.etag = c.child("ETag").text().as_string();
            if (const auto modified = parseIsoUtc(std::string(c.child("LastModified").text().as_string()).substr(0, 19) + "Z")) {
                entry.modified = *modified;
            }
            out.push_back(std::move(entry));
            if (out.size() > limit) {
                return std::unexpected(StoreError{StoreFailure::TooLarge, "more objects than allowed in the listing"});
            }
        }
        for (pugi::xml_node c = root.child("CommonPrefixes"); c; c = c.next_sibling("CommonPrefixes")) {
            std::string key = c.child("Prefix").text().as_string();
            if (key.size() <= m_prefix.size() || key.compare(0, m_prefix.size(), m_prefix) != 0) {
                continue;
            }
            std::string path = key.substr(m_prefix.size());
            while (!path.empty() && path.back() == '/') {
                path.pop_back();
            }
            if (!validPath(path)) {
                continue;
            }
            StoreEntry entry;
            entry.path = std::move(path);
            entry.folder = true;
            out.push_back(std::move(entry));
        }
        const bool truncated = std::string_view(root.child("IsTruncated").text().as_string()) == "true";
        token = root.child("NextContinuationToken").text().as_string();
        if (!truncated || token.empty()) {
            break;
        }
    }
    return out;
}

StoreResult<std::optional<StoreEntry>> S3Store::stat(std::string_view path, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto found = listing(keyFor(path), false, 1000, stop); // a HEAD would do, but custom HEAD requests are not reliable in Qt
    if (!found) {
        return std::unexpected(found.error());
    }
    for (StoreEntry& entry : *found) {
        if (entry.path == path && !entry.folder) {
            return std::optional<StoreEntry>(std::move(entry));
        }
    }
    return std::optional<StoreEntry>();
}

StoreResult<std::vector<StoreEntry>> S3Store::list(std::string_view folder, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!folder.empty() && !validPath(folder)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(folder)});
    }
    return listing(m_prefix + (folder.empty() ? std::string() : std::string(folder) + "/"), true, kMaxListed, stop);
}

StoreResult<Fetched> S3Store::get(std::string_view path, std::uint64_t maxBytes, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto reply = detail::sendBuffered(m_transport, signed_("GET", keyFor(path), "", kEmptyHash, "", Precondition::none()), maxBytes, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "GET " + std::string(path)));
    }
    return Fetched{std::move(reply->body), detail::headerOr(reply->headers, "etag")};
}

StoreResult<std::string> S3Store::getToFile(std::string_view path, const fs::path& file, std::uint64_t maxBytes,
                                            const Progress& progress, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto reply = detail::sendToFile(m_transport, signed_("GET", keyFor(path), "", kEmptyHash, "", Precondition::none()), file, maxBytes,
                                    progress, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "GET " + std::string(path)));
    }
    return detail::headerOr(reply->headers, "etag");
}

StoreResult<std::string> S3Store::write(std::string_view path, std::string_view data, const fs::path& file,
                                        const Precondition& precondition, std::string_view contentType, const Progress& progress,
                                        std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    std::string payloadHash;
    std::uint64_t size = data.size();
    if (!file.empty()) {
        std::error_code ec;
        size = fs::file_size(file, ec);
        if (ec) {
            return std::unexpected(StoreError{StoreFailure::NotFound, "cannot open " + pathToUtf8(file)});
        }
        if (size > kMaxSinglePut) {
            return std::unexpected(StoreError{StoreFailure::TooLarge, "files over 4.5 GB need multipart upload, which is not supported yet"});
        }
        auto hash = detail::sha256File(file, progress ? Progress([&](std::uint64_t, std::uint64_t) { return !stop.stop_requested(); }) : Progress());
        if (!hash) {
            return std::unexpected(hash.error());
        }
        payloadHash = *hash;
    } else {
        payloadHash = sha256Hex(data);
    }
    ai::HttpRequest request = signed_("PUT", keyFor(path), "", payloadHash, contentType, precondition);
    if (!file.empty()) {
        request.bodyFile = file;
    } else {
        request.body = std::string(data);
    }
    request.retrySafe = true;
    auto reply = detail::sendUpload(m_transport, std::move(request), progress, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok()) {
        return std::unexpected(fromStatus(reply->status, "PUT " + std::string(path)));
    }
    return detail::headerOr(reply->headers, "etag");
}

StoreResult<std::string> S3Store::put(std::string_view path, std::string_view data, const Precondition& precondition,
                                      std::string_view contentType, std::stop_token stop) {
    return write(path, data, {}, precondition, contentType, {}, stop);
}

StoreResult<std::string> S3Store::putFile(std::string_view path, const fs::path& file, const Precondition& precondition,
                                          std::string_view contentType, const Progress& progress, std::stop_token stop) {
    return write(path, {}, file, precondition, contentType, progress, stop);
}

StoreResult<void> S3Store::remove(std::string_view path, std::stop_token stop) {
    if (!m_problem.empty()) {
        return std::unexpected(StoreError{StoreFailure::Invalid, m_problem});
    }
    if (!validPath(path)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "not a valid path: " + std::string(path)});
    }
    auto reply = detail::sendBuffered(m_transport, signed_("DELETE", keyFor(path), "", kEmptyHash, "", Precondition::none()), 1u << 20, stop);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!reply->ok() && reply->status != 404) {
        return std::unexpected(fromStatus(reply->status, "DELETE " + std::string(path)));
    }
    return {};
}

} // namespace wabcut::cloud
