#pragma once

// Line transports for the MCP server. One JSON-RPC message per line, which is the MCP stdio binding:
// newline-delimited, no embedded newlines, UTF-8.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wabcut::command::mcp {

class McpTransport {
public:
    virtual ~McpTransport() = default;
    /// The next incoming line, waiting until one arrives; nullopt once the input has ended.
    virtual std::optional<std::string> read() = 0;
    /// The next incoming line if one has already arrived; never waits. The server polls while a long call
    /// runs, so a cancellation can reach it.
    virtual std::optional<std::string> poll() = 0;
    /// Sends one message (the transport adds the newline).
    virtual void write(std::string_view line) = 0;
};

/// In-memory transport for tests: input lines are queued up front, output lines are collected.
class MemoryTransport : public McpTransport {
public:
    MemoryTransport() = default;
    explicit MemoryTransport(std::vector<std::string> input);

    void push(std::string line) { m_input.push_back(std::move(line)); }
    std::optional<std::string> read() override { return poll(); }
    std::optional<std::string> poll() override;
    void write(std::string_view line) override { output.emplace_back(line); }

    std::vector<std::string> output;

private:
    std::deque<std::string> m_input;
};

/// stdin/stdout in binary mode. A reader thread splits stdin into lines so poll() never waits. A line
/// longer than `maxLineBytes` is cut to maxLineBytes + 1 bytes (the rest is dropped), so the server sees
/// it is too large without the transport holding it.
class StdioTransport : public McpTransport {
public:
    explicit StdioTransport(std::size_t maxLineBytes);
    ~StdioTransport() override;
    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    std::optional<std::string> read() override;
    std::optional<std::string> poll() override;
    void write(std::string_view line) override;

private:
    void readerLoop();

    std::size_t m_maxLineBytes;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<std::string> m_lines;
    bool m_ended = false;
    std::thread m_reader;
};

} // namespace wabcut::command::mcp
