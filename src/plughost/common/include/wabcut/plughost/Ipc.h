#pragma once

// Process, shared-memory and event primitives for the plugin host (Windows for now; docs/plugins.md). Used by the
// editor side (wabcut_plugins) and the helper (wabcut-plughost).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::plughost {

/// The longest control line either side reads. More (output that never ends a line: not the protocol) ends the
/// channel: the editor treats the helper as crashed.
inline constexpr std::size_t kMaxLineBytes = 16u * 1024u * 1024u;

/// A named shared-memory block, mapped read/write.
class SharedMemory {
public:
    static std::unique_ptr<SharedMemory> create(const std::string& name, std::size_t size, std::string& error);
    static std::unique_ptr<SharedMemory> open(const std::string& name, std::size_t size, std::string& error);
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    void* data() const noexcept { return m_data; }
    std::size_t size() const noexcept { return m_size; }

private:
    SharedMemory() = default;
    void* m_handle = nullptr;
    void* m_data = nullptr;
    std::size_t m_size = 0;
};

/// A named auto-reset event. signal() never blocks (safe on the audio thread).
class NamedEvent {
public:
    static std::unique_ptr<NamedEvent> create(const std::string& name, std::string& error);
    static std::unique_ptr<NamedEvent> open(const std::string& name, std::string& error);
    ~NamedEvent();
    NamedEvent(const NamedEvent&) = delete;
    NamedEvent& operator=(const NamedEvent&) = delete;

    void signal() noexcept;
    /// Waits until signalled or `timeout` passes; true when signalled.
    bool wait(std::chrono::milliseconds timeout) noexcept;

private:
    NamedEvent() = default;
    void* m_handle = nullptr;
};

/// A child process with its stdin and stdout connected to this process (the control channel).
class ChildProcess {
public:
    /// Starts `exe` with `args` (each quoted as needed). stderr goes to `stderrFile` when given, else is discarded.
    static std::unique_ptr<ChildProcess> start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                               const std::filesystem::path& stderrFile, std::string& error);
    ~ChildProcess(); ///< terminates the process if it still runs
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    std::uint32_t pid() const noexcept { return m_pid; }
    /// Writes one line (a '\n' is appended). False when the pipe is broken (the process died).
    bool writeLine(std::string_view line);
    /// Reads one line (without the '\n'); nullopt at the end of the stream (the process died or closed stdout).
    /// Blocks: call it from a reader thread.
    std::optional<std::string> readLine();
    /// Waits for the process to end; its exit code, or nullopt on timeout.
    std::optional<std::uint32_t> wait(std::chrono::milliseconds timeout);
    bool running() const;
    void terminate(std::uint32_t code = 1);
    /// The OS handle of the process (to wait on); null when not started.
    void* handle() const noexcept { return m_process; }

private:
    ChildProcess() = default;
    void* m_process = nullptr;
    void* m_stdinWrite = nullptr;
    void* m_stdoutRead = nullptr;
    std::uint32_t m_pid = 0;
    std::string m_pending; ///< bytes read past the last line
};

/// The helper side of the control channel: stdin lines in, stdout lines out (thread-safe writes).
std::optional<std::string> readStdinLine();
void writeStdoutLine(std::string_view line);
/// The helper, before it loads any plugin: keeps its stdin and stdout for the control channel (duplicated; the two
/// functions above use them) and points the process's standard input at NUL and its standard output - the handle and
/// the C runtime's - where stderr goes (the helper's log), so a plugin that prints never writes into the channel.
bool takeControlChannel(std::string& error);

/// This process's id.
std::uint32_t currentProcessId();

/// UTF-8 <-> wide paths.
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

/// Base64 (RFC 4648, with padding) for plugin state chunks.
std::string base64Encode(const void* data, std::size_t size);
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text);

} // namespace wabcut::plughost
