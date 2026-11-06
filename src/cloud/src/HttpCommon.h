#pragma once

// Helpers the WebDAV and S3 stores share (private): sending a request and collecting or saving the body with a size
// limit, progress and cancellation, reading headers, and parsing the XML answers of a server (untrusted: bounded, no
// entity expansion beyond the five predefined ones, pugixml never reads external entities or DTDs).

#include <wabcut/ai/Http.h>
#include <wabcut/cloud/RemoteStore.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pugi {
class xml_document;
class xml_node;
} // namespace pugi

namespace wabcut::cloud::detail {

struct Raw {
    int status = 0;
    ai::HttpHeaders headers;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

/// Sends `request` with its response read as Raw bytes: a 2xx body is collected (at most `maxBytes`, else TooLarge), any
/// other status comes back with its (short) body. Transport failures become StoreErrors.
StoreResult<Raw> sendBuffered(ai::IHttpTransport& transport, ai::HttpRequest request, std::uint64_t maxBytes, std::stop_token stop);

/// Like sendBuffered, but a 2xx body is written to `file` (atomically; at most `maxBytes`). Returns the status and headers.
StoreResult<Raw> sendToFile(ai::IHttpTransport& transport, ai::HttpRequest request, const std::filesystem::path& file,
                            std::uint64_t maxBytes, const Progress& progress, std::stop_token stop);

/// Sends a request with an upload (body or bodyFile): upload progress is reported to `progress`, and a false return cancels.
StoreResult<Raw> sendUpload(ai::IHttpTransport& transport, ai::HttpRequest request, const Progress& progress, std::stop_token stop);

std::string headerOr(const ai::HttpHeaders& headers, std::string_view name);

/// Parses `text` (at most `maxBytes`) into `doc`; a message when it is not XML.
std::optional<std::string> parseXml(std::string_view text, pugi::xml_document& doc, std::size_t maxBytes = 8u * 1024u * 1024u);
/// The first child of `node` whose name without a namespace prefix is `local` (null node when none).
pugi::xml_node localChild(const pugi::xml_node& node, std::string_view local);
/// The text of that child, "" when there is none.
std::string localText(const pugi::xml_node& node, std::string_view local);

/// The authority of an endpoint as a Host header value: "host" or "host:port" (the port only when it is not the scheme's).
std::string hostHeader(const ai::UrlParts& parts);

} // namespace wabcut::cloud::detail
