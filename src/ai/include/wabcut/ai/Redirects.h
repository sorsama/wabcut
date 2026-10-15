#pragma once

// Following HTTP redirects on purpose (review wave 4, P1-4). The transports follow same-origin redirects only, so a
// download whose server answers with a 302 to its CDN (Hugging Face -> cas-bridge.xethub.hf.co, GitHub releases ->
// release-assets.githubusercontent.com) would fail. Callers that expect such hops send the request with
// HttpRequest::followRedirects = false and let sendFollowingRedirects() follow each hop itself:
//   - every hop is parsed strictly (parseHttpUrl) and must stay on HTTPS (unless the list allows http, for loopback
//     tests), carry no userinfo, and go either to the original host itself or to a host the allow-list names for the
//     original host, exactly (no wildcards);
//   - a hop to another origin (scheme, host or port) carries only Range and Accept: credentials (Authorization,
//     x-goog-api-key, cookies, any other header) are never forwarded to it;
//   - at most `maxHops` hops; 303 (and 301/302 of a non-GET) continue as GET without a body, 307/308 keep the method;
//   - the request's dispatch check runs before each hop.
// The integrity of downloaded files stays with their SHA-256 (ModelStore): the allow-list decides where requests may
// go, not what a body may contain.

#include <wabcut/ai/Http.h>

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai {

struct RedirectAllowList {
    struct Rule {
        std::string from;            ///< the original request's host
        std::vector<std::string> to; ///< hosts it may redirect to (exact, lower case)
    };
    std::vector<Rule> rules;
    int maxHops = 5;
    bool httpsOnly = true;           ///< false only for tests against a loopback http server

    /// Model and runtime downloads: huggingface.co to its Xet bridge and LFS CDNs, github.com to its release-asset hosts.
    static RedirectAllowList modelDownloads();
    /// Same host only.
    static RedirectAllowList sameHost();

    /// Whether a hop from a request to `original` may go to `next`.
    bool allows(const UrlParts& original, const UrlParts& next) const;
};

/// The absolute URL a Location header names, resolved against `base` (absolute, scheme-relative "//host/..." or an
/// absolute path "/..."), or nullopt when it is not one of those or not a valid http(s) URL.
std::optional<std::string> resolveLocation(std::string_view base, std::string_view location);

/// Sends `request` with followRedirects off and follows 301/302/303/307/308 answers within `allow`. The final
/// response (or error) is what the transport returned for the last hop; `hops` (optional) receives the hosts visited
/// after the first. Errors: ProviderError for a redirect without a usable Location, to a host the list does not allow,
/// or past maxHops (the message names hosts only, never the signed URL); the transport's otherwise.
std::expected<HttpResponse, AiError> sendFollowingRedirects(IHttpTransport& transport, HttpRequest request, const HttpStreamHandlers& handlers,
                                                            std::stop_token stop, const RedirectAllowList& allow,
                                                            std::vector<std::string>* hops = nullptr);

} // namespace wabcut::ai
