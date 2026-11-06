#include "HttpCommon.h"

#include "FileUtil.h"

#include <wabcut/cloud/HttpStores.h>

#include <pugixml.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>

namespace wabcut::cloud {

std::string endpointProblem(std::string_view url, bool allowInsecure) {
    const auto parts = ai::parseHttpUrl(url);
    if (!parts) {
        return "The address must start with https:// (or http://) and have a host name.";
    }
    if (!parts->userinfo.empty()) {
        return "Do not put a user name or password in the address; they are asked for separately.";
    }
    if (url.find('?') != std::string_view::npos || url.find('#') != std::string_view::npos) {
        return "The address must not contain a query or a fragment.";
    }
    if (parts->scheme == "http") {
        const bool loopback = parts->host == "localhost" || parts->host == "127.0.0.1" || parts->host == "::1";
        if (!loopback && !allowInsecure) {
            return "This address is not encrypted (http://). Use https://, or allow an unencrypted connection on purpose.";
        }
    }
    return {};
}

} // namespace wabcut::cloud

namespace wabcut::cloud::detail {

namespace {

/// Wraps the caller's stop token with one a progress callback can trigger.
struct LinkedStop {
    std::stop_source source;
    std::optional<std::stop_callback<std::function<void()>>> forward;
    explicit LinkedStop(std::stop_token outer) {
        if (outer.stop_possible()) {
            forward.emplace(outer, std::function<void()>([this] { source.request_stop(); }));
        }
    }
};

StoreError failed(const ai::AiError& error, bool tooLarge, std::stop_token stop) {
    if (tooLarge) {
        return StoreError{StoreFailure::TooLarge, "the answer is larger than allowed"};
    }
    if (stop.stop_requested()) {
        return StoreError{StoreFailure::Cancelled, "cancelled"};
    }
    return fromTransport(error);
}

} // namespace

StoreResult<Raw> sendBuffered(ai::IHttpTransport& transport, ai::HttpRequest request, std::uint64_t maxBytes, std::stop_token stop) {
    request.responseMode = ai::BodyMode::Raw;
    Raw out;
    bool tooLarge = false;
    ai::HttpStreamHandlers handlers;
    handlers.onHead = [&](const ai::HttpResponseHead& head) {
        const std::string length = headerOr(head.headers, "content-length");
        if (!length.empty()) {
            char* end = nullptr;
            const unsigned long long declared = std::strtoull(length.c_str(), &end, 10);
            if (end != length.c_str() && declared > maxBytes) {
                tooLarge = true;
                return false;
            }
        }
        return true;
    };
    handlers.onData = [&](std::span<const std::byte> bytes) {
        if (out.body.size() + bytes.size() > maxBytes) {
            tooLarge = true;
            return false;
        }
        out.body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    };
    auto response = transport.send(request, handlers, stop);
    if (!response) {
        return std::unexpected(failed(response.error(), tooLarge, stop));
    }
    out.status = response->status;
    out.headers = std::move(response->headers);
    if (!response->ok()) {
        out.body = std::move(response->body);
        if (out.body.size() > 64 * 1024) {
            out.body.resize(64 * 1024);
        }
    }
    return out;
}

StoreResult<Raw> sendToFile(ai::IHttpTransport& transport, ai::HttpRequest request, const std::filesystem::path& file,
                            std::uint64_t maxBytes, const Progress& progress, std::stop_token stop) {
    request.responseMode = ai::BodyMode::Raw;
    request.timeout = std::chrono::hours(6);
    request.idleTimeout = std::chrono::seconds(120);
    LinkedStop linked(stop);
    std::unique_ptr<AtomicFile> out;
    StoreError writeFailure;
    bool failedWrite = false;
    bool tooLarge = false;
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    ai::HttpStreamHandlers handlers;
    handlers.onHead = [&](const ai::HttpResponseHead& head) {
        const std::string length = headerOr(head.headers, "content-length");
        if (!length.empty()) {
            char* end = nullptr;
            total = std::strtoull(length.c_str(), &end, 10);
            if (end != length.c_str() && total > maxBytes) {
                tooLarge = true;
                return false;
            }
        }
        auto created = AtomicFile::create(file);
        if (!created) {
            writeFailure = created.error();
            failedWrite = true;
            return false;
        }
        out = std::move(*created);
        return true;
    };
    handlers.onData = [&](std::span<const std::byte> bytes) {
        done += bytes.size();
        if (done > maxBytes) {
            tooLarge = true;
            return false;
        }
        if (!out || !out->write(reinterpret_cast<const char*>(bytes.data()), bytes.size())) {
            writeFailure = StoreError{StoreFailure::Other, "the file could not be written (is the disk full?)"};
            failedWrite = true;
            return false;
        }
        if (progress && !progress(done, total)) {
            linked.source.request_stop();
            return false;
        }
        return true;
    };
    auto response = transport.send(request, handlers, linked.source.get_token());
    if (failedWrite) {
        return std::unexpected(writeFailure);
    }
    if (!response) {
        return std::unexpected(failed(response.error(), tooLarge, linked.source.get_token()));
    }
    Raw result;
    result.status = response->status;
    result.headers = std::move(response->headers);
    if (!response->ok()) {
        result.body = std::move(response->body);
        if (result.body.size() > 64 * 1024) {
            result.body.resize(64 * 1024);
        }
        return result;
    }
    if (!out) { // an empty 2xx body still makes the file
        auto created = AtomicFile::create(file);
        if (!created) {
            return std::unexpected(created.error());
        }
        out = std::move(*created);
    }
    if (auto committed = out->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return result;
}

StoreResult<Raw> sendUpload(ai::IHttpTransport& transport, ai::HttpRequest request, const Progress& progress, std::stop_token stop) {
    request.responseMode = ai::BodyMode::Raw;
    request.timeout = std::chrono::hours(6);
    request.idleTimeout = std::chrono::seconds(120);
    LinkedStop linked(stop);
    ai::HttpStreamHandlers handlers;
    handlers.onData = [](std::span<const std::byte>) { return true; };
    handlers.onUploadProgress = [&](std::uint64_t sent, std::uint64_t total) {
        if (progress && !progress(sent, total)) {
            linked.source.request_stop();
        }
    };
    auto response = transport.send(request, handlers, linked.source.get_token());
    if (!response) {
        return std::unexpected(failed(response.error(), false, linked.source.get_token()));
    }
    Raw out;
    out.status = response->status;
    out.headers = std::move(response->headers);
    out.body = std::move(response->body);
    if (out.body.size() > 64 * 1024) {
        out.body.resize(64 * 1024);
    }
    return out;
}

std::string headerOr(const ai::HttpHeaders& headers, std::string_view name) {
    const auto value = ai::findHeader(headers, name);
    return value ? std::string(*value) : std::string();
}

std::optional<std::string> parseXml(std::string_view text, pugi::xml_document& doc, std::size_t maxBytes) {
    if (text.size() > maxBytes) {
        return "the answer is larger than allowed";
    }
    // No DOCTYPE node is built (parse_doctype off) and pugixml has no external entities: nothing can be fetched or expanded.
    const pugi::xml_parse_result result = doc.load_buffer(text.data(), text.size(), pugi::parse_default);
    if (!result) {
        return std::string("the server's answer is not valid XML");
    }
    return std::nullopt;
}

pugi::xml_node localChild(const pugi::xml_node& node, std::string_view local) {
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        std::string_view name = child.name();
        const std::size_t colon = name.rfind(':');
        if (colon != std::string_view::npos) {
            name.remove_prefix(colon + 1);
        }
        if (name == local) {
            return child;
        }
    }
    return {};
}

std::string localText(const pugi::xml_node& node, std::string_view local) {
    const pugi::xml_node child = localChild(node, local);
    return child ? std::string(child.text().as_string()) : std::string();
}

std::string hostHeader(const ai::UrlParts& parts) {
    std::string host = parts.host.find(':') != std::string::npos ? "[" + parts.host + "]" : parts.host;
    if (parts.port && !((parts.scheme == "https" && *parts.port == 443) || (parts.scheme == "http" && *parts.port == 80))) {
        host += ":" + std::to_string(*parts.port);
    }
    return host;
}

} // namespace wabcut::cloud::detail
