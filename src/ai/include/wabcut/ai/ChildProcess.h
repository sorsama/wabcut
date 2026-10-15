#pragma once

// A child process with line-based pipes, for out-of-process helpers such as wabcut-infer. Pure C++ on the OS
// APIs (Win32 / POSIX), no Qt.
//
// The child's stdin and stdout are pipes; its stderr goes to a file (or nowhere), so a chatty child can never
// block on a full pipe nobody reads. On Windows the child runs in a job object that kills it when the parent
// dies, and in every case it dies with this object (kill() in the destructor), so a helper can't outlive the
// editor or a cancelled job.

#include <wabcut/model/Error.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

struct ChildProcessOptions {
    std::filesystem::path executable;
    std::vector<std::string> arguments; ///< UTF-8, without the program name
    /// Where the child's stderr goes; empty = discarded.
    std::filesystem::path stderrFile;
    /// The child's working directory; empty = the editor's.
    std::filesystem::path workingDirectory;
    /// Run below normal priority (Windows BELOW_NORMAL_PRIORITY_CLASS, POSIX nice +10): background inference never
    /// competes with playback for the CPU.
    bool lowPriority = false;
    /// Sandbox limits for helpers that run untrusted code (the extension host, C.12), enforced by the job object on
    /// Windows; spawn() fails rather than start the child without them. POSIX: not enforced yet (rlimit / cgroups
    /// are a follow-up, docs/extensions.md).
    ///   memoryLimitBytes  the child's committed memory cap (0 = none): an allocation past it fails in the child
    ///   noChildProcesses  the child cannot start processes of its own
    ///   restrictUi        no clipboard, desktop switching, global atoms, system settings or other processes' windows
    std::size_t memoryLimitBytes = 0;
    bool noChildProcesses = false;
    bool restrictUi = false;
};

/// Directory of the running executable (helpers ship next to it).
std::filesystem::path currentExecutableDir();

class ChildProcess {
public:
    /// Overflow: the child broke the line protocol's bounds (a line longer than kMaxLineBytes, or more than
    /// kMaxQueuedBytes written and not yet read); its output is dropped from then on and the caller should kill() it.
    enum class ReadStatus { Line, Timeout, Closed, Overflow };
    static constexpr std::size_t kMaxLineBytes = std::size_t{16} * 1024 * 1024;
    static constexpr std::size_t kMaxQueuedBytes = std::size_t{64} * 1024 * 1024;

    /// Errors: NotFound (no such executable), IoError (pipes or process creation failed).
    static std::expected<std::unique_ptr<ChildProcess>, Error> spawn(const ChildProcessOptions& options);

    ~ChildProcess(); ///< kill()s a child that still runs.
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Writes `line` and a newline to the child's stdin. False if the pipe is closed (the child exited).
    bool writeLine(std::string_view line);
    /// Closes the child's stdin (it sees end of input).
    void closeInput();

    /// The next line from the child's stdout (without the line ending), waiting at most `timeout`. Closed: the
    /// child closed stdout (normally: it exited) and every line has been read.
    ReadStatus readLine(std::string& line, std::chrono::milliseconds timeout);

    /// Terminates the child at once (no-op once it has exited).
    void kill();
    /// Waits up to `timeout` for the child to exit; its exit code if it did.
    std::optional<int> wait(std::chrono::milliseconds timeout);
    /// The exit code if the child has exited.
    std::optional<int> exitCode();
    /// The operating system's id of the child process.
    std::uint64_t processId() const;
    /// The most memory the child has committed so far (Windows job accounting; also after it exited); nullopt where
    /// the platform does not report it.
    std::optional<std::size_t> peakMemoryBytes() const;

    /// Human-readable description of an exit code ("exit code 3", "crashed (0xC0000005 access violation)",
    /// "killed by signal 9").
    static std::string describeExit(int code);

private:
    struct Impl;
    explicit ChildProcess(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::ai
