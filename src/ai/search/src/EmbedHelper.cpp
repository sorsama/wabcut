#include <wabcut/ai/search/EmbedHelper.h>

#include <wabcut/ai/ChildProcess.h>

#include <cstdio>

namespace wabcut::ai::search {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

ErrorCode codeForHelperError(const std::string& code) {
    if (code == "model") {
        return ErrorCode::NotFound;
    }
    if (code == "bad_request") {
        return ErrorCode::InvalidArgument;
    }
    if (code == "io") {
        return ErrorCode::IoError;
    }
    return ErrorCode::DecoderError;
}

} // namespace

std::filesystem::path defaultEmbedExecutable() {
#ifdef _WIN32
    return currentExecutableDir() / "wabcut-embed.exe";
#else
    return currentExecutableDir() / "wabcut-embed";
#endif
}

EmbedHelper::EmbedHelper(EmbedHelperOptions options) : m_options(std::move(options)) {}

EmbedHelper::~EmbedHelper() { stop(); }

bool EmbedHelper::running() const { return m_child && !m_child->exitCode(); }

std::string EmbedHelper::nextId() { return std::to_string(m_nextId++); }

void EmbedHelper::stop() {
    if (m_child) {
        m_child->writeLine(embed::encode(embed::Request{embed::Shutdown{}}));
        m_child->closeInput();
        if (!m_child->wait(2s)) {
            m_child->kill();
        }
        m_child.reset();
    }
}

Error EmbedHelper::abandon(ErrorCode code, std::string message) {
    if (m_child) {
        m_child->kill();
        m_child.reset();
    }
    return Error(code, std::move(message));
}

Error EmbedHelper::failed(const embed::Failure& failure, const std::string& what) {
    const ErrorCode code = codeForHelperError(failure.code);
    std::string message = "wabcut-embed: " + what + " failed: " + failure.message;
    if (failure.code == "inference" || failure.code == "protocol") {
        return abandon(code, std::move(message)); // the session may be broken: start over next time
    }
    return Error(code, std::move(message));
}

std::expected<embed::Response, Error> EmbedHelper::next(const std::string& what, std::chrono::milliseconds deadline,
                                                        const std::stop_token& stop) {
    const auto until = Clock::now() + deadline;
    std::string line;
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(abandon(ErrorCode::Cancelled, "cancelled while " + what));
        }
        switch (m_child->readLine(line, 50ms)) {
        case ChildProcess::ReadStatus::Line: {
            auto response = embed::decodeResponse(line);
            if (!response) {
                return std::unexpected(abandon(ErrorCode::DecoderError,
                                               "wabcut-embed sent an invalid message while " + what + ": " + response.error().message));
            }
            return response;
        }
        case ChildProcess::ReadStatus::Closed: {
            const auto code = m_child->wait(5s);
            return std::unexpected(abandon(ErrorCode::DecoderError,
                                           "wabcut-embed " + (code ? ChildProcess::describeExit(*code) : std::string("closed its output")) +
                                               " while " + what));
        }
        case ChildProcess::ReadStatus::Overflow:
            return std::unexpected(abandon(ErrorCode::DecoderError, "wabcut-embed sent more output than allowed while " + what));
        case ChildProcess::ReadStatus::Timeout:
            break;
        }
        if (Clock::now() >= until) {
            char waited[32];
            std::snprintf(waited, sizeof waited, "%.1f s", static_cast<double>(deadline.count()) / 1000.0);
            return std::unexpected(abandon(ErrorCode::DecoderError,
                                           "wabcut-embed did not answer within " + std::string(waited) + " while " + what + "; it was stopped"));
        }
    }
}

std::expected<void, Error> EmbedHelper::start(const std::stop_token& stop) {
    if (m_child) {
        if (!m_child->exitCode()) {
            return {};
        }
        m_child.reset(); // it died between calls
    }
    ChildProcessOptions spawn;
    spawn.executable = m_options.executable.empty() ? defaultEmbedExecutable() : m_options.executable;
    spawn.arguments = m_options.arguments;
    spawn.stderrFile = m_options.log;
    spawn.lowPriority = m_options.lowPriority;
    auto spawned = ChildProcess::spawn(spawn);
    if (!spawned) {
        return std::unexpected(Error(spawned.error().code == ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::DecoderError,
                                     "cannot start wabcut-embed (" + spawned.error().message + ")"));
    }
    m_child = std::move(*spawned);
    ++m_session;
    if (!m_child->writeLine(embed::encode(embed::Request{embed::Hello{}}))) {
        return std::unexpected(abandon(ErrorCode::DecoderError, "wabcut-embed exited at start"));
    }
    auto response = next("starting", m_options.startTimeout, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    const auto* hello = std::get_if<embed::HelloReply>(&*response);
    if (!hello) {
        return std::unexpected(abandon(ErrorCode::DecoderError, "wabcut-embed did not say hello while starting"));
    }
    if (hello->protocol != embed::kProtocolVersion) {
        return std::unexpected(abandon(ErrorCode::VersionMismatch, "wabcut-embed speaks protocol " + std::to_string(hello->protocol) +
                                                                      ", not " + std::to_string(embed::kProtocolVersion)));
    }
    return {};
}

std::expected<embed::Response, Error> EmbedHelper::exchange(const embed::Request& request, const std::string& what,
                                                            std::chrono::milliseconds deadline, const std::stop_token& stop) {
    if (auto started = start(stop); !started) {
        return std::unexpected(started.error());
    }
    if (!m_child->writeLine(embed::encode(request))) {
        return std::unexpected(abandon(ErrorCode::DecoderError, "wabcut-embed exited before " + what));
    }
    return next(what, deadline, stop);
}

} // namespace wabcut::ai::search
