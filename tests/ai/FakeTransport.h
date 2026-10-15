#pragma once

// A scripted IHttpTransport for the AI platform tests (owner: AI platform engineer). Each send() pops the next
// scripted response and replays its body through the real StreamDecoder in small chunks, the way a network
// transport would deliver it, so SSE/NDJSON parsing is exercised exactly as in production. Every request is
// recorded for snapshot checks. No test ever touches the network.

#include <wabcut/ai/Http.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace wabcut::ai::test {

struct ScriptedResponse {
    int status = 200;
    HttpHeaders headers;
    std::string body;
    std::optional<AiError> error;  ///< fail instead of answering (e.g. Network)
    bool failAfterHead = false;    ///< deliver the head and half the body, then fail with `error`
    std::size_t chunkSize = 7;     ///< bytes per feed() call: small, to split lines and UTF-8 sequences
    /// Runs while this response is being served (the provider is "working"): tests change state mid-call with it,
    /// e.g. turn the no-cloud lock on between two requests of one operation.
    std::function<void()> before;
};

/// A path in the temp directory that is unique to this process: ctest runs test cases in parallel processes, so
/// fixed names would let one case delete another's file.
inline std::filesystem::path uniqueTempPath(const std::string& stem, const std::string& extension = {}) {
    static const std::string tag = std::to_string(std::random_device{}()) + "-" +
                                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / (stem + "-" + tag + extension);
}

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

/// A 200 text/event-stream response with this body.
inline ScriptedResponse sseBody(std::string body) {
    ScriptedResponse r;
    r.headers = {{"content-type", "text/event-stream"}};
    r.body = std::move(body);
    return r;
}

class FakeTransport final : public IHttpTransport {
public:
    void push(ScriptedResponse r) {
        std::lock_guard lock(m_mutex);
        m_script.push_back(std::move(r));
    }
    void pushJson(int status, std::string body, HttpHeaders headers = {}) {
        ScriptedResponse r;
        r.status = status;
        r.body = std::move(body);
        r.headers = std::move(headers);
        r.headers.push_back({"content-type", "application/json"});
        push(std::move(r));
    }
    void pushError(AiErrorCode code, std::string message = "scripted failure") {
        ScriptedResponse r;
        r.error = AiError(code, std::move(message));
        push(std::move(r));
    }

    std::vector<HttpRequest> requests() const {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }
    std::size_t remaining() const {
        std::lock_guard lock(m_mutex);
        return m_script.size();
    }

    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override {
        ScriptedResponse r;
        {
            std::lock_guard lock(m_mutex);
            m_requests.push_back(request);
            if (m_script.empty()) {
                return std::unexpected(AiError(AiErrorCode::Network, "FakeTransport: no scripted response"));
            }
            r = std::move(m_script.front());
            m_script.pop_front();
        }
        if (r.before) {
            r.before();
        }
        if (stop.stop_requested()) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
        }
        if (r.error && !r.failAfterHead) {
            return std::unexpected(*r.error);
        }
        HttpResponse response;
        response.status = r.status;
        response.headers = r.headers;
        const bool ok = r.status >= 200 && r.status < 300;
        if (!ok || request.responseMode == BodyMode::Buffered) {
            if (ok && handlers.onHead && !handlers.onHead({r.status, r.headers})) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "aborted by onHead"));
            }
            response.body = r.body;
            return response;
        }
        if (handlers.onHead && !handlers.onHead({r.status, r.headers})) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "aborted by onHead"));
        }
        StreamDecoder decoder(request.responseMode, handlers);
        const std::size_t limit = r.failAfterHead ? r.body.size() / 2 : r.body.size();
        for (std::size_t pos = 0; pos < limit; pos += r.chunkSize) {
            if (stop.stop_requested()) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
            }
            if (!decoder.feed(std::string_view(r.body).substr(pos, std::min(r.chunkSize, limit - pos)))) {
                return std::unexpected(decoder.error() ? *decoder.error() : AiError(AiErrorCode::Cancelled, "aborted by a handler"));
            }
        }
        if (r.failAfterHead) {
            return std::unexpected(*r.error);
        }
        if (!decoder.finish()) {
            return std::unexpected(decoder.error() ? *decoder.error() : AiError(AiErrorCode::Cancelled, "aborted by a handler"));
        }
        return response;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<ScriptedResponse> m_script;
    std::vector<HttpRequest> m_requests;
};

} // namespace wabcut::ai::test
