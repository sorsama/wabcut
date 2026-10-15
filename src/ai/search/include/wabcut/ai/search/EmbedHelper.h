#pragma once

// One wabcut-embed process (EmbedProtocol.h), shared by the clients that talk to it: HelperEmbedder (SigLIP 2) and,
// for AI-9, the subject detector (YuNet + YOLOX). It starts on demand (hello and a protocol check), sends a request
// and waits for its answer within a deadline. A crash, a timeout, output beyond the protocol's bounds, an answer out
// of order or a cancel kills it, and that call fails with a clean Error; the next call starts a new process, whose
// session() differs, so a client knows it must load its models again. Not thread-safe: the clients serialise calls.

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/model/Error.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace wabcut::ai {
class ChildProcess;
}

namespace wabcut::ai::search {

struct EmbedHelperOptions {
    std::filesystem::path executable;   ///< defaultEmbedExecutable() when empty
    std::vector<std::string> arguments; ///< extra helper arguments (tests script a fake helper with them)
    std::filesystem::path log;          ///< the helper's stderr; empty = discarded
    std::chrono::milliseconds startTimeout{std::chrono::seconds(20)};
    bool lowPriority = false;           ///< run below normal priority (A.4: background audio jobs)
};

class EmbedHelper {
public:
    explicit EmbedHelper(EmbedHelperOptions options);
    ~EmbedHelper(); ///< shuts the helper down
    EmbedHelper(const EmbedHelper&) = delete;
    EmbedHelper& operator=(const EmbedHelper&) = delete;

    /// Starts the helper unless it runs. Errors: NotFound (no executable), DecoderError (it failed to start or
    /// answer hello), VersionMismatch, Cancelled.
    std::expected<void, Error> start(const std::stop_token& stop);

    /// Sends `request` (started first) and waits for the answer to `id`: a T, or the helper's Failure as an Error
    /// ("inference" and "protocol" failures also stop the helper). Anything else stops it. `what` names the step in
    /// messages.
    template <class T>
    std::expected<T, Error> call(const embed::Request& request, const std::string& id, const std::string& what,
                                 std::chrono::milliseconds deadline, const std::stop_token& stop) {
        auto response = exchange(request, what, deadline, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (const auto* failure = std::get_if<embed::Failure>(&*response)) {
            return std::unexpected(failed(*failure, what));
        }
        if (auto* value = std::get_if<T>(&*response); value != nullptr && embed::responseId(*response) == id) {
            return std::move(*value);
        }
        return std::unexpected(abandon(ErrorCode::DecoderError, "wabcut-embed answered out of order while " + what));
    }

    /// A fresh request id.
    std::string nextId();
    /// Which process answers: changes every time a new one starts (0 = none yet).
    int session() const { return m_session.load(); }
    /// How many processes were started (safe to read from any thread).
    int starts() const { return m_session.load(); }
    bool running() const;

    /// Shuts the helper down gracefully (it frees its models); the next call starts a new one.
    void stop();
    /// Kills the helper and returns `code`/`message` as an Error (after a failure).
    Error abandon(ErrorCode code, std::string message);

private:
    std::expected<embed::Response, Error> exchange(const embed::Request& request, const std::string& what,
                                                   std::chrono::milliseconds deadline, const std::stop_token& stop);
    std::expected<embed::Response, Error> next(const std::string& what, std::chrono::milliseconds deadline,
                                               const std::stop_token& stop);
    Error failed(const embed::Failure& failure, const std::string& what);

    EmbedHelperOptions m_options;
    std::unique_ptr<ChildProcess> m_child;
    std::atomic<int> m_session{0};
    int m_nextId = 1;
};

/// The wabcut-embed helper next to the running executable.
std::filesystem::path defaultEmbedExecutable();

} // namespace wabcut::ai::search
