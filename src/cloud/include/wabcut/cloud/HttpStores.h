#pragma once

// WebDAV and S3-compatible stores over the app's network stack (IHttpTransport: proxies, TLS, the redacted request log, the
// offline gate). The credentials arrive as ai::Secret (read from the OS keychain by the app): they are used to build one
// header per request and are never stored in a message, a log line or a URL (S3 links are signed in the query string only
// by presignGet(), whose result the caller must treat as a secret).
//
// WebDAV (RFC 4918, as Nextcloud, ownCloud, Synology, Apache mod_dav and nginx-dav serve it): a collection URL; PROPFIND,
// GET, PUT, DELETE, MKCOL; HTTP Basic authentication; conditional writes with If-None-Match: * and If-Match.
//
// S3 (path-style or virtual-hosted): AWS Signature Version 4 (header authentication, the real payload hash), GET / PUT /
// DELETE, ListObjectsV2; conditional writes with If-None-Match: * and If-Match, which AWS S3, Cloudflare R2, MinIO and other
// current services honour. Files over 4.5 GB would need multipart upload and are refused for now.

#include <wabcut/ai/Http.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/cloud/RemoteStore.h>

#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::cloud {

/// Why an endpoint URL cannot be used, or empty: it must be an absolute http(s) URL without user info, query or fragment;
/// plain http only for this computer (loopback) or when `allowInsecure` (Basic authentication and the data then travel
/// unencrypted).
std::string endpointProblem(std::string_view url, bool allowInsecure);

// ---- AWS Signature Version 4 (exposed for tests: the published example signatures are checked) ----

struct SigV4Request {
    std::string method;
    std::string canonicalUri;   ///< the encoded path: "/examplebucket/test.txt"
    std::string canonicalQuery; ///< sorted, encoded "a=1&b=2" (empty for none)
    /// Lower-case names, trimmed values; every header that will be signed (host and x-amz-* at least).
    std::vector<std::pair<std::string, std::string>> headers;
    std::string payloadHash; ///< hex SHA-256 of the body, or "UNSIGNED-PAYLOAD"
    std::string region;
    std::string service = "s3";
    std::string amzDate;     ///< "20130524T000000Z"
    std::string accessKey;
    std::string secretKey;
};

struct SigV4Result {
    std::string signedHeaders;     ///< "host;range;x-amz-content-sha256;x-amz-date"
    std::string canonicalRequest;
    std::string stringToSign;
    std::string signature;         ///< hex
    std::string authorization;     ///< "AWS4-HMAC-SHA256 Credential=…, SignedHeaders=…, Signature=…"
};

SigV4Result signV4(const SigV4Request& request);

// ---- WebDAV ----

struct WebDavConfig {
    std::string url;     ///< the collection that is the store's root: "https://cloud.example.com/remote.php/dav/files/ann/Wabcut"
    std::string user;
    bool allowInsecure = false;
};

class WebDavStore final : public RemoteStore {
public:
    WebDavStore(ai::IHttpTransport& transport, WebDavConfig config, ai::Secret password);

    /// A problem with the settings ("" when they are usable).
    std::string problem() const { return m_problem; }
    std::string describe() const override;
    std::string kind() const override { return "webdav"; }
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

private:
    struct Reply;
    std::string urlFor(std::string_view path, bool collection) const;
    ai::HttpRequest request(std::string method, std::string url) const;
    StoreResult<Reply> send(ai::HttpRequest request, const ai::HttpStreamHandlers& handlers, std::stop_token stop) const;
    StoreResult<void> ensureParents(std::string_view path, std::stop_token stop);
    StoreResult<std::vector<StoreEntry>> propfind(std::string_view path, int depth, std::stop_token stop);
    StoreResult<std::string> write(std::string_view path, ai::HttpRequest put, const Precondition& precondition, std::stop_token stop);

    ai::IHttpTransport& m_transport;
    WebDavConfig m_config;
    std::string m_authorization; ///< "Basic …": a credential, never logged
    std::string m_base;          ///< the collection URL without a trailing slash
    std::string m_basePath;      ///< its path part
    std::string m_problem;
    std::set<std::string> m_folders; ///< collections known to exist
};

// ---- S3 ----

struct S3Config {
    std::string endpoint;      ///< "https://s3.eu-west-1.amazonaws.com", "https://<account>.r2.cloudflarestorage.com", "http://localhost:9000"
    std::string region = "us-east-1";
    std::string bucket;
    std::string prefix;        ///< objects live below this key prefix ("wabcut"); may be empty
    bool pathStyle = true;     ///< https://endpoint/bucket/key (false: https://bucket.endpoint/key)
    bool allowInsecure = false;
};

class S3Store final : public RemoteStore {
public:
    S3Store(ai::IHttpTransport& transport, S3Config config, ai::Secret accessKeyId, ai::Secret secretAccessKey,
            UnixClock clock = systemClock());

    std::string problem() const { return m_problem; }
    std::string describe() const override;
    std::string kind() const override { return "s3"; }
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
    StoreResult<void> makeFolder(std::string_view, std::stop_token = {}) override { return {}; }

    /// A link that lets anyone holding it GET `path` until `seconds` have passed (1 .. 604800), signed with the secret key.
    /// The link is a secret: the app puts it into the review page, never into a log.
    StoreResult<std::string> presignGet(std::string_view path, int seconds) const;

private:
    struct Reply;
    struct Target {
        std::string url;
        std::string host;
        std::string canonicalUri;
    };
    std::string keyFor(std::string_view path) const;
    Target target(std::string_view key) const;
    ai::HttpRequest signed_(std::string method, std::string_view key, const std::string& canonicalQuery, const std::string& payloadHash,
                            std::string_view contentType, const Precondition& precondition) const;
    StoreResult<Reply> send(ai::HttpRequest request, const ai::HttpStreamHandlers& handlers, std::stop_token stop) const;
    StoreResult<std::string> write(std::string_view path, std::string_view data, const std::filesystem::path& file,
                                   const Precondition& precondition, std::string_view contentType, const Progress& progress,
                                   std::stop_token stop);
    StoreResult<std::vector<StoreEntry>> listing(std::string_view prefixKey, bool delimit, std::size_t limit, std::stop_token stop);

    ai::IHttpTransport& m_transport;
    S3Config m_config;
    ai::Secret m_accessKey;
    ai::Secret m_secretKey;
    UnixClock m_clock;
    std::string m_prefix; ///< normalised key prefix: "" or "a/b/"
    ai::UrlParts m_endpoint;
    std::string m_problem;
};

} // namespace wabcut::cloud
