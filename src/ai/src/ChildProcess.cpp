#include <wabcut/ai/ChildProcess.h>

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>

#include <wabcut/platform/HelperProcess.h>
#else
#include <climits>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wabcut::ai {

namespace {

/// Lines read from the child's stdout by a background thread. Bounded (ChildProcess::kMaxLineBytes per line,
/// kMaxQueuedBytes unread): a helper that floods its output cannot grow the editor's memory; past a bound the queue
/// drops everything and reports Overflow, and the reader thread stops reading.
class LineQueue {
public:
    /// False once the queue overflowed (the reader stops).
    bool push(std::string bytes) {
        std::lock_guard lock(m_mutex);
        if (m_overflow) {
            return false;
        }
        m_partial += bytes;
        std::size_t start = 0;
        for (std::size_t nl = m_partial.find('\n'); nl != std::string::npos; nl = m_partial.find('\n', start)) {
            std::string line = m_partial.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!enqueue(std::move(line))) {
                return false;
            }
            start = nl + 1;
        }
        m_partial.erase(0, start);
        if (m_partial.size() > ChildProcess::kMaxLineBytes) {
            overflow();
            return false;
        }
        m_cv.notify_all();
        return true;
    }
    void close() {
        std::lock_guard lock(m_mutex);
        if (!m_partial.empty() && !m_overflow) {
            enqueue(std::move(m_partial)); // a last line without newline
        }
        m_partial.clear();
        m_closed = true;
        m_cv.notify_all();
    }
    ChildProcess::ReadStatus pop(std::string& line, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_lines.empty() || m_closed || m_overflow; })) {
            return ChildProcess::ReadStatus::Timeout;
        }
        if (m_overflow) {
            return ChildProcess::ReadStatus::Overflow;
        }
        if (m_lines.empty()) {
            return ChildProcess::ReadStatus::Closed;
        }
        line = std::move(m_lines.front());
        m_lines.pop_front();
        m_queued -= line.size();
        return ChildProcess::ReadStatus::Line;
    }

private:
    bool enqueue(std::string line) { // m_mutex held
        if (line.size() > ChildProcess::kMaxLineBytes || m_queued + line.size() > ChildProcess::kMaxQueuedBytes) {
            overflow();
            return false;
        }
        m_queued += line.size();
        m_lines.push_back(std::move(line));
        return true;
    }
    void overflow() { // m_mutex held
        m_overflow = true;
        m_lines.clear();
        m_partial.clear();
        m_partial.shrink_to_fit();
        m_queued = 0;
        m_cv.notify_all();
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_lines;
    std::string m_partial;
    std::size_t m_queued = 0;
    bool m_closed = false;
    bool m_overflow = false;
};

std::mutex& policyMutex() {
    static std::mutex mutex;
    return mutex;
}
HelperCpuPolicy& policyStore() {
    static HelperCpuPolicy policy;
    return policy;
}

} // namespace

void setHelperCpuPolicy(HelperCpuPolicy policy) {
    policy.cpuRatePercent = std::clamp(policy.cpuRatePercent, 1, 100);
    std::lock_guard lock(policyMutex());
    policyStore() = policy;
}

HelperCpuPolicy helperCpuPolicy() {
    std::lock_guard lock(policyMutex());
    return policyStore();
}

namespace {
/// The priority and CPU cap a child gets: its own options, or the helper policy for an AI helper.
struct EffectiveLimits {
    bool lowPriority = false;
    int cpuRatePercent = 0; ///< 0 = none
};
EffectiveLimits effectiveLimits(const ChildProcessOptions& o) {
    EffectiveLimits e{o.lowPriority, std::clamp(o.cpuRatePercent, 0, 100)};
    if (o.aiHelper) {
        const HelperCpuPolicy p = helperCpuPolicy();
        e.lowPriority = e.lowPriority || p.lowPriority;
        if (e.cpuRatePercent == 0 && p.cpuRatePercent < 100) {
            e.cpuRatePercent = p.cpuRatePercent;
        }
    }
    if (e.cpuRatePercent >= 100) {
        e.cpuRatePercent = 0;
    }
    return e;
}
} // namespace

#ifdef _WIN32

std::filesystem::path currentExecutableDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return std::filesystem::current_path();
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// Quotes one argument for CommandLineToArgvW / the MSVC CRT.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += arg[i];
    }
    cmd += L'"';
}

std::string lastError(const char* what) {
    return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

} // namespace

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    std::thread reader;
    LineQueue lines;
    std::mutex writeMutex;

    ~Impl() {
        if (process != nullptr) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
        }
        if (stdinWrite != nullptr) {
            CloseHandle(stdinWrite);
        }
        if (reader.joinable()) {
            reader.join(); // the child is gone, so its stdout end is closed and ReadFile returns
        }
        if (stdoutRead != nullptr) {
            CloseHandle(stdoutRead);
        }
        if (process != nullptr) {
            CloseHandle(process);
        }
        if (job != nullptr) {
            CloseHandle(job);
        }
    }
};

std::expected<std::unique_ptr<ChildProcess>, Error> ChildProcess::spawn(const ChildProcessOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(options.executable, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "helper not found: " + pathToUtf8(options.executable)});
    }
    auto impl = std::make_unique<Impl>();
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdinRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdinRead, &impl->stdinWrite, &inherit, 0) ||
        !CreatePipe(&impl->stdoutRead, &stdoutWrite, &inherit, 0)) {
        return std::unexpected(Error{ErrorCode::IoError, lastError("cannot create pipes")});
    }
    // Only the child's ends are inherited.
    SetHandleInformation(impl->stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl->stdoutRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE errorOut = INVALID_HANDLE_VALUE;
    const std::wstring errorPath = options.stderrFile.empty() ? std::wstring(L"NUL") : options.stderrFile.wstring();
    errorOut = CreateFileW(errorPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    auto closeChildEnds = [&] {
        CloseHandle(stdinRead);
        CloseHandle(stdoutWrite);
        if (errorOut != INVALID_HANDLE_VALUE) {
            CloseHandle(errorOut);
        }
    };

    // Inherit exactly these handles, nothing else the editor has open.
    HANDLE inherited[3] = {stdinRead, stdoutWrite, errorOut};
    const DWORD inheritedCount = errorOut != INVALID_HANDLE_VALUE ? 3 : 2;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
        closeChildEnds();
        return std::unexpected(Error{ErrorCode::IoError, lastError("cannot set up the helper's handles")});
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdinRead;
    startup.StartupInfo.hStdOutput = stdoutWrite;
    startup.StartupInfo.hStdError = errorOut != INVALID_HANDLE_VALUE ? errorOut : nullptr;
    startup.lpAttributeList = attributes;

    std::wstring commandLine;
    appendQuoted(commandLine, options.executable.wstring());
    for (const std::string& arg : options.arguments) {
        commandLine += L' ';
        appendQuoted(commandLine, widen(arg));
    }

    // Kill-on-close job: if the editor dies (even by TerminateProcess), Windows ends the helper too.
    impl->job = CreateJobObjectW(nullptr, nullptr);
    const bool sandboxed = options.memoryLimitBytes > 0 || options.noChildProcesses || options.restrictUi;
    bool limited = impl->job != nullptr;
    if (impl->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (options.memoryLimitBytes > 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = options.memoryLimitBytes;
        }
        if (options.noChildProcesses) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            limits.BasicLimitInformation.ActiveProcessLimit = 1;
        }
        limited = SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0;
        if (limited && options.restrictUi) {
            JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
            ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                                     JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                     JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                     JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
            limited = SetInformationJobObject(impl->job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) != 0;
        }
    }
    if (sandboxed && !limited) {
        closeChildEnds();
        return std::unexpected(Error{ErrorCode::IoError, lastError("cannot set up the helper's sandbox limits")});
    }
    // 0.2: an AI helper's (or an explicit) CPU rate cap, a hard one: the job's processes together get at most that share
    // of every processor's time, so playback keeps its headroom. Best effort: a helper still runs without it.
    const EffectiveLimits effective = effectiveLimits(options);
    if (impl->job != nullptr && effective.cpuRatePercent > 0) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        rate.CpuRate = static_cast<DWORD>(effective.cpuRatePercent) * 100; // in 1/100 of a percent
        (void)SetInformationJobObject(impl->job, JobObjectCpuRateControlInformation, &rate, sizeof(rate));
    }

    PROCESS_INFORMATION info{};
    // An explicit, minimal environment (no provider keys) and, unless the caller names one, the helper's own folder as
    // its working directory: never the editor's whole environment or its current directory (S5-P2-5).
    const std::filesystem::path executable = std::filesystem::absolute(options.executable, ec);
    std::wstring environment = platform::helperEnvironmentFor(executable);
    const std::wstring workingDirectory =
        (options.workingDirectory.empty() ? executable.parent_path() : options.workingDirectory).wstring();
    const DWORD priority = effective.lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0;
    const BOOL created = CreateProcessW(options.executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                                            CREATE_UNICODE_ENVIRONMENT | priority,
                                        environment.data(), workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    closeChildEnds();
    if (!created) {
        return std::unexpected(Error{ErrorCode::IoError, lastError("cannot start the helper")});
    }
    // Into the job before it runs a single instruction; a sandboxed child that cannot join it never runs.
    if (impl->job != nullptr && !AssignProcessToJobObject(impl->job, info.hProcess) && sandboxed) {
        const std::string why = lastError("cannot put the helper into its sandbox");
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return std::unexpected(Error{ErrorCode::IoError, why});
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    impl->process = info.hProcess;

    Impl* raw = impl.get();
    impl->reader = std::thread([raw] {
        char buffer[16384];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(raw->stdoutRead, buffer, sizeof buffer, &got, nullptr) || got == 0) {
                break;
            }
            if (!raw->lines.push(std::string(buffer, got))) {
                break; // overflow: stop reading
            }
        }
        raw->lines.close();
    });
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

bool ChildProcess::writeLine(std::string_view line) {
    std::lock_guard lock(m_impl->writeMutex);
    if (m_impl->stdinWrite == nullptr) {
        return false;
    }
    std::string data(line);
    data += '\n';
    std::size_t written = 0;
    while (written < data.size()) {
        DWORD n = 0;
        if (!WriteFile(m_impl->stdinWrite, data.data() + written, static_cast<DWORD>(data.size() - written), &n, nullptr)) {
            return false;
        }
        written += n;
    }
    return true;
}

void ChildProcess::closeInput() {
    std::lock_guard lock(m_impl->writeMutex);
    if (m_impl->stdinWrite != nullptr) {
        CloseHandle(m_impl->stdinWrite);
        m_impl->stdinWrite = nullptr;
    }
}

void ChildProcess::kill() {
    if (m_impl->process != nullptr && WaitForSingleObject(m_impl->process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(m_impl->process, 0xDEAD);
        WaitForSingleObject(m_impl->process, 5000);
    }
}

std::optional<int> ChildProcess::wait(std::chrono::milliseconds timeout) {
    if (WaitForSingleObject(m_impl->process, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    return exitCode();
}

std::uint64_t ChildProcess::processId() const { return m_impl->process != nullptr ? GetProcessId(m_impl->process) : 0; }

std::optional<std::size_t> ChildProcess::peakMemoryBytes() const {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    if (m_impl->job == nullptr ||
        !QueryInformationJobObject(m_impl->job, JobObjectExtendedLimitInformation, &info, sizeof(info), nullptr)) {
        return std::nullopt;
    }
    return info.PeakProcessMemoryUsed;
}

std::optional<int> ChildProcess::cpuRateLimitPercent() const {
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
    if (m_impl->job == nullptr ||
        !QueryInformationJobObject(m_impl->job, JobObjectCpuRateControlInformation, &rate, sizeof(rate), nullptr) ||
        (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) == 0 || (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) == 0) {
        return std::nullopt;
    }
    return static_cast<int>(rate.CpuRate / 100);
}

bool ChildProcess::belowNormalPriority() const {
    return m_impl->process != nullptr && GetPriorityClass(m_impl->process) == BELOW_NORMAL_PRIORITY_CLASS;
}

std::optional<int> ChildProcess::exitCode() {
    DWORD code = 0;
    if (!GetExitCodeProcess(m_impl->process, &code) || code == STILL_ACTIVE) {
        return std::nullopt;
    }
    return static_cast<int>(code);
}

std::string ChildProcess::describeExit(int code) {
    const auto u = static_cast<unsigned>(code);
    if (u >= 0xC0000000u) {
        char buf[64];
        const char* what = u == 0xC0000005u   ? " access violation"
                           : u == 0xC00000FDu ? " stack overflow"
                           : u == 0xC0000409u ? " fast fail / abort"
                           : u == 0xC0000017u ? " out of memory"
                           : u == 0xC000001Du ? " illegal instruction"
                                              : "";
        std::snprintf(buf, sizeof buf, "crashed (0x%08X%s)", u, what);
        return buf;
    }
    if (u == 0xDEAD) {
        return "killed";
    }
    if (u == 3) {
        return "aborted (exit code 3)";
    }
    return "exit code " + std::to_string(code);
}

#else // POSIX

#ifdef __APPLE__
extern "C" int _NSGetExecutablePath(char* buf, std::uint32_t* bufsize);
#endif

std::filesystem::path currentExecutableDir() {
    std::error_code ec;
#ifdef __APPLE__
    char buffer[PATH_MAX];
    std::uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return std::filesystem::weakly_canonical(buffer, ec).parent_path();
    }
    return std::filesystem::current_path();
#else
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : exe.parent_path();
#endif
}

struct ChildProcess::Impl {
    pid_t pid = -1;
    bool lowPriority = false; ///< niced (nice +10)
    int stdinWrite = -1;
    int stdoutRead = -1;
    std::optional<int> status;
    std::thread reader;
    LineQueue lines;
    std::mutex writeMutex;

    std::optional<int> reap(bool block) {
        if (status || pid < 0) {
            return status;
        }
        int raw = 0;
        const pid_t r = waitpid(pid, &raw, block ? 0 : WNOHANG);
        if (r == pid) {
            status = WIFEXITED(raw) ? WEXITSTATUS(raw) : (WIFSIGNALED(raw) ? 128 + WTERMSIG(raw) : 255);
        }
        return status;
    }

    ~Impl() {
        if (pid > 0 && !reap(false)) {
            ::kill(pid, SIGKILL);
            reap(true);
        }
        if (stdinWrite >= 0) {
            ::close(stdinWrite);
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (stdoutRead >= 0) {
            ::close(stdoutRead);
        }
    }
};

std::expected<std::unique_ptr<ChildProcess>, Error> ChildProcess::spawn(const ChildProcessOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(options.executable, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "helper not found: " + pathToUtf8(options.executable)});
    }
    int in[2];
    int out[2];
    if (pipe(in) != 0 || pipe(out) != 0) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot create pipes"});
    }
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    const std::string errorPath = options.stderrFile.empty() ? std::string("/dev/null") : options.stderrFile.string();

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, errorPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_addclose(&actions, in[0]);
    posix_spawn_file_actions_addclose(&actions, out[1]);

    const std::string exe = options.executable.string();
    // posix_spawn has no portable working-directory action (addchdir_np is an extension), so a working directory or
    // low priority runs through a tiny /bin/sh wrapper that execs the helper; without either it is spawned directly.
    std::vector<std::string> storage{exe};
    storage.insert(storage.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    for (std::string& s : storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> shellStorage;
    std::vector<char*> shellArgv;
    std::string program = exe;
    const bool lowPriority = effectiveLimits(options).lowPriority;
    if (!options.workingDirectory.empty() || lowPriority) {
        // sh -c 'cd "$0" && exec nice -n 10 "$@"' <dir> <exe> <args...>: arguments are passed, never interpolated.
        shellStorage = {"/bin/sh", "-c",
                        std::string(options.workingDirectory.empty() ? "" : "cd \"$0\" && ") + "exec " +
                            (lowPriority ? "nice -n 10 " : "") + "\"$@\"",
                        options.workingDirectory.empty() ? std::string(".") : options.workingDirectory.string()};
        shellStorage.insert(shellStorage.end(), storage.begin(), storage.end());
        for (std::string& s : shellStorage) {
            shellArgv.push_back(s.data());
        }
        shellArgv.push_back(nullptr);
        program = "/bin/sh";
    }

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, nullptr,
                               shellArgv.empty() ? argv.data() : shellArgv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) {
        ::close(in[1]);
        ::close(out[0]);
        return std::unexpected(Error{ErrorCode::IoError, "cannot start the helper (errno " + std::to_string(rc) + ")"});
    }
    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->lowPriority = lowPriority;
    impl->stdinWrite = in[1];
    impl->stdoutRead = out[0];
    Impl* raw = impl.get();
    impl->reader = std::thread([raw] {
        char buffer[16384];
        for (;;) {
            const ssize_t got = ::read(raw->stdoutRead, buffer, sizeof buffer);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            if (!raw->lines.push(std::string(buffer, static_cast<std::size_t>(got)))) {
                break; // overflow: stop reading
            }
        }
        raw->lines.close();
    });
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

namespace {

/// A helper that has died leaves its stdin pipe without a reader: writing to it raises SIGPIPE, whose default action
/// ends the whole editor (I.7 portability audit). Ignored once per process, the write fails with EPIPE instead, which
/// writeLine() reports as a failed write like any other.
void ignoreSigpipeOnce() {
    static const bool ignored = [] {
        struct sigaction action {};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        return sigaction(SIGPIPE, &action, nullptr) == 0;
    }();
    (void)ignored;
}

} // namespace

bool ChildProcess::writeLine(std::string_view line) {
    ignoreSigpipeOnce();
    std::lock_guard lock(m_impl->writeMutex);
    if (m_impl->stdinWrite < 0) {
        return false;
    }
    std::string data(line);
    data += '\n';
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(m_impl->stdinWrite, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

void ChildProcess::closeInput() {
    std::lock_guard lock(m_impl->writeMutex);
    if (m_impl->stdinWrite >= 0) {
        ::close(m_impl->stdinWrite);
        m_impl->stdinWrite = -1;
    }
}

void ChildProcess::kill() {
    if (m_impl->pid > 0 && !m_impl->reap(false)) {
        ::kill(m_impl->pid, SIGKILL);
        m_impl->reap(true);
    }
}

std::optional<int> ChildProcess::wait(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!m_impl->reap(false)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return m_impl->status;
}

std::uint64_t ChildProcess::processId() const { return m_impl->pid > 0 ? static_cast<std::uint64_t>(m_impl->pid) : 0; }

std::optional<std::size_t> ChildProcess::peakMemoryBytes() const { return std::nullopt; }

std::optional<int> ChildProcess::cpuRateLimitPercent() const { return std::nullopt; }

bool ChildProcess::belowNormalPriority() const { return m_impl->lowPriority; }

std::optional<int> ChildProcess::exitCode() { return m_impl->reap(false); }

std::string ChildProcess::describeExit(int code) {
    if (code > 128 && code < 160) {
        return "killed by signal " + std::to_string(code - 128);
    }
    return "exit code " + std::to_string(code);
}

#endif

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

ChildProcess::~ChildProcess() = default;

ChildProcess::ReadStatus ChildProcess::readLine(std::string& line, std::chrono::milliseconds timeout) {
    return m_impl->lines.pop(line, timeout);
}

} // namespace wabcut::ai
