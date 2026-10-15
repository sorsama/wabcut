#pragma once

// HttpDownloadTransport: ModelStore's IDownloadTransport (ModelStore.h, Local-AI engineer) on top of the AI
// platform's IHttpTransport, so model downloads go through the same Qt network stack as the cloud providers
// (proxies, TLS, cancellation, retries with Retry-After, the redacted request log) instead of a second HTTP client.
//
//   net::NetworkStack network(log);                 // or any IHttpTransport
//   HttpDownloadTransport downloads(network.transport());
//   store.ensure(model, &downloads, progress, stop);

#include <wabcut/ai/Http.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Redirects.h>

namespace wabcut::ai {

class HttpDownloadTransport final : public IDownloadTransport {
public:
    /// `redirects`: where downloads may be redirected (review wave 4, P1-4): Hugging Face and GitHub to their CDNs.
    explicit HttpDownloadTransport(IHttpTransport& transport, RedirectAllowList redirects = RedirectAllowList::modelDownloads())
        : m_transport(transport), m_redirects(std::move(redirects)) {}

    /// GET with "Range: bytes=<offset>-" when offset > 0, following redirects within the allow-list. The total size
    /// comes from Content-Range (a 206 answer) or Content-Length (a 200 answer). Errors: IoError (network, a body that
    /// broke off, a redirect to a host the list does not allow), Cancelled.
    std::expected<Response, Error> get(const std::string& url, std::uint64_t offset,
                                       const std::function<bool(const Response&)>& onResponse,
                                       const std::function<bool(std::span<const std::byte>)>& onData,
                                       std::stop_token stop) override;

private:
    IHttpTransport& m_transport;
    RedirectAllowList m_redirects;
};

} // namespace wabcut::ai
