#include <wabcut/ai/HttpDownload.h>

#include <charconv>

namespace wabcut::ai {

namespace {

std::optional<std::uint64_t> parseNumber(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    std::uint64_t value = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || p == text.data()) {
        return std::nullopt;
    }
    return value;
}

/// Size of the whole resource: "Content-Range: bytes 100-199/1234" -> 1234, else Content-Length + offset.
std::optional<std::uint64_t> totalSize(const HttpHeaders& headers, int status, std::uint64_t offset) {
    if (status == 206) {
        if (auto range = findHeader(headers, "content-range")) {
            if (const auto slash = range->rfind('/'); slash != std::string_view::npos) {
                return parseNumber(range->substr(slash + 1)); // "*" (unknown) does not parse
            }
        }
        if (auto length = findHeader(headers, "content-length")) {
            if (auto n = parseNumber(*length)) {
                return *n + offset;
            }
        }
        return std::nullopt;
    }
    if (auto length = findHeader(headers, "content-length")) {
        return parseNumber(*length);
    }
    return std::nullopt;
}

} // namespace

std::expected<IDownloadTransport::Response, Error>
HttpDownloadTransport::get(const std::string& url, std::uint64_t offset,
                           const std::function<bool(const Response&)>& onResponse,
                           const std::function<bool(std::span<const std::byte>)>& onData, std::stop_token stop) {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.responseMode = BodyMode::Raw;
    request.timeout = std::chrono::hours(6); // large weights over slow links
    request.idleTimeout = std::chrono::seconds(60);
    if (offset > 0) {
        request.headers.push_back({"Range", "bytes=" + std::to_string(offset) + "-"});
    }

    Response response;
    bool announced = false;
    bool abortedByCaller = false;
    HttpStreamHandlers handlers;
    handlers.onHead = [&](const HttpResponseHead& head) {
        response.status = head.status;
        response.totalSize = totalSize(head.headers, head.status, offset);
        announced = true;
        if (onResponse && !onResponse(response)) {
            abortedByCaller = true;
            return false;
        }
        return true;
    };
    handlers.onData = [&](std::span<const std::byte> bytes) {
        if (onData && !onData(bytes)) {
            abortedByCaller = true;
            return false;
        }
        return true;
    };

    auto result = sendFollowingRedirects(m_transport, request, handlers, stop, m_redirects);
    if (!result) {
        const bool cancelled = abortedByCaller || result.error().code == AiErrorCode::Cancelled;
        return std::unexpected(Error(cancelled ? ErrorCode::Cancelled : ErrorCode::IoError, result.error().message));
    }
    if (!announced) {
        // Error statuses arrive buffered: report the status once, before (no) body, as the contract asks.
        response.status = result->status;
        response.totalSize = totalSize(result->headers, result->status, offset);
        if (onResponse && !onResponse(response)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "download aborted"));
        }
    }
    return response;
}

} // namespace wabcut::ai
