#include <wabcut/command/mcp/McpTransport.h>

#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace wabcut::command::mcp {

MemoryTransport::MemoryTransport(std::vector<std::string> input) : m_input(input.begin(), input.end()) {}

std::optional<std::string> MemoryTransport::poll() {
    if (m_input.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(m_input.front());
    m_input.pop_front();
    return line;
}

StdioTransport::StdioTransport(std::size_t maxLineBytes) : m_maxLineBytes(maxLineBytes) {
#ifdef _WIN32
    // Protocol bytes pass unchanged: no CRLF translation, no Ctrl-Z end of file.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    m_reader = std::thread([this] { readerLoop(); });
}

StdioTransport::~StdioTransport() {
    bool ended = false;
    {
        std::lock_guard lock(m_mutex);
        ended = m_ended;
    }
    // The reader only stops at the end of stdin; if the server stops first, it is left to the process exit.
    if (ended) {
        m_reader.join();
    } else {
        m_reader.detach();
    }
}

void StdioTransport::readerLoop() {
    std::string line;
    bool tooLong = false;
    const auto deliver = [&] {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        {
            std::lock_guard lock(m_mutex);
            m_lines.push_back(std::move(line));
        }
        m_ready.notify_one();
        line.clear();
        tooLong = false;
    };
    for (int c = std::getchar(); c != EOF; c = std::getchar()) {
        if (c == '\n') {
            deliver();
        } else if (!tooLong) {
            line.push_back(static_cast<char>(c));
            tooLong = line.size() > m_maxLineBytes;
        }
    }
    if (!line.empty()) {
        deliver();
    }
    {
        std::lock_guard lock(m_mutex);
        m_ended = true;
    }
    m_ready.notify_all();
}

std::optional<std::string> StdioTransport::read() {
    std::unique_lock lock(m_mutex);
    m_ready.wait(lock, [this] { return !m_lines.empty() || m_ended; });
    if (m_lines.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(m_lines.front());
    m_lines.pop_front();
    return line;
}

std::optional<std::string> StdioTransport::poll() {
    std::lock_guard lock(m_mutex);
    if (m_lines.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(m_lines.front());
    m_lines.pop_front();
    return line;
}

void StdioTransport::write(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace wabcut::command::mcp
