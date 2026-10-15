#include <wabcut/ai/Redirects.h>

#include <algorithm>

namespace wabcut::ai {

namespace {

int effectivePort(const UrlParts& url) { return url.port ? *url.port : (url.scheme == "https" ? 443 : 80); }

bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

} // namespace

RedirectAllowList RedirectAllowList::modelDownloads() {
    RedirectAllowList list;
    // Hugging Face serves LFS / Xet files from these hosts after a 302 from /<repo>/resolve/<revision>/<file>. Since
    // 2026-10 it answers with its CDN's Xet bridge (us.aws.cdn.hf.co/xet-bridge-us/..., seen 2026-10-12; eu. for Europe).
    list.rules.push_back({"huggingface.co",
                          {"cas-bridge.xethub.hf.co", "transfer.xethub.hf.co", "us.aws.cdn.hf.co", "eu.aws.cdn.hf.co", "cdn-lfs.hf.co",
                           "cdn-lfs-us-1.hf.co", "cdn-lfs-eu-1.hf.co", "cdn-lfs.huggingface.co", "cdn-lfs-us-1.huggingface.co",
                           "cdn-lfs-eu-1.huggingface.co"}});
    // GitHub release assets (/releases/download/<tag>/<file>) answer with a 302 to their asset storage.
    list.rules.push_back({"github.com", {"objects.githubusercontent.com", "release-assets.githubusercontent.com"}});
    return list;
}

RedirectAllowList RedirectAllowList::sameHost() { return {}; }

bool RedirectAllowList::allows(const UrlParts& original, const UrlParts& next) const {
    if (!next.userinfo.empty() || (httpsOnly && next.scheme != "https")) {
        return false;
    }
    if (original.scheme == "https" && next.scheme != "https") {
        return false; // never downgrade
    }
    if (next.host == original.host && effectivePort(next) == effectivePort(original)) {
        return true;
    }
    for (const Rule& rule : rules) {
        if (rule.from == original.host && std::ranges::find(rule.to, next.host) != rule.to.end()) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> resolveLocation(std::string_view base, std::string_view location) {
    while (!location.empty() && (location.front() == ' ' || location.front() == '\t')) {
        location.remove_prefix(1);
    }
    while (!location.empty() && (location.back() == ' ' || location.back() == '\t' || location.back() == '\r')) {
        location.remove_suffix(1);
    }
    const auto baseParts = parseHttpUrl(base);
    if (!baseParts || location.empty()) {
        return std::nullopt;
    }
    std::string resolved;
    if (location.starts_with("//")) {
        resolved = baseParts->scheme + ":" + std::string(location);
    } else if (location.starts_with('/')) {
        resolved = baseParts->scheme + "://" + (baseParts->host.find(':') != std::string::npos ? "[" + baseParts->host + "]" : baseParts->host) +
                   (baseParts->port ? ":" + std::to_string(*baseParts->port) : std::string()) + std::string(location);
    } else {
        resolved = std::string(location);
    }
    if (!parseHttpUrl(resolved)) {
        return std::nullopt;
    }
    return resolved;
}

std::expected<HttpResponse, AiError> sendFollowingRedirects(IHttpTransport& transport, HttpRequest request, const HttpStreamHandlers& handlers,
                                                            std::stop_token stop, const RedirectAllowList& allow,
                                                            std::vector<std::string>* hops) {
    const auto original = parseHttpUrl(request.url);
    if (!original) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not an http(s) URL"));
    }
    request.followRedirects = false;
    for (int hop = 0;; ++hop) {
        auto response = transport.send(request, handlers, stop);
        if (!response || !isRedirect(response->status)) {
            return response;
        }
        if (hop >= allow.maxHops) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "too many redirects from " + original->host));
        }
        const auto location = findHeader(response->headers, "location");
        const auto next = location ? resolveLocation(request.url, *location) : std::nullopt;
        const auto parts = next ? parseHttpUrl(*next) : std::nullopt;
        if (!parts) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, original->host + " redirected without a usable location"));
        }
        if (!allow.allows(*original, *parts)) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, original->host + " redirected to " + parts->host +
                                                                           ", which is not an approved download host"));
        }
        if (parts->host != original->host || parts->scheme != original->scheme || effectivePort(*parts) != effectivePort(*original)) {
            // Credentials and everything else stay with the original origin (scheme, host and port).
            HttpHeaders kept;
            for (const HttpHeader& h : request.headers) {
                if (equalsIgnoreCase(h.name, "range") || equalsIgnoreCase(h.name, "accept")) {
                    kept.push_back(h);
                }
            }
            request.headers = std::move(kept);
        }
        if (response->status == 303 || ((response->status == 301 || response->status == 302) && request.method != "GET" && request.method != "HEAD")) {
            request.method = "GET";
            request.body.clear();
            request.bodyFile.clear();
            request.multipart.clear();
        }
        request.url = *next;
        if (hops) {
            hops->push_back(parts->host);
        }
    }
}

} // namespace wabcut::ai
