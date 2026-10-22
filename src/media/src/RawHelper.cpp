#include <wabcut/media/RawHelper.h>

#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wabcut::media {

namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

std::filesystem::path executableDir() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) {
        return {};
    }
    buffer.resize(n);
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : self.parent_path();
#endif
}

#if !defined(_WIN32)
/// A helper that died leaves its stdin without a reader: writing raises SIGPIPE, whose default action ends the editor.
/// Ignored once per process (as the AI helpers do), the write fails with EPIPE instead.
void ignoreSigpipeOnce() {
    static const bool ignored = [] {
        struct sigaction action {};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        return sigaction(SIGPIPE, &action, nullptr) == 0;
    }();
    (void)ignored;
}
#endif

std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

std::optional<std::filesystem::path> rawHelperFor(const std::filesystem::path& file) {
    // The RAW formats only their vendors' SDKs read (FFmpeg plays the rest itself), and the test helper's.
    static constexpr std::array<std::string_view, 8> kHelperExtensions{".r3d", ".braw", ".ari", ".arx",
                                                                      ".crm", ".cine", ".nev", ".fakeraw"};
    const std::string ext = lowered(pathToUtf8(file.extension()));
    if (std::ranges::find(kHelperExtensions, std::string_view(ext)) == kHelperExtensions.end()) {
        return std::nullopt;
    }
    std::string name = "wabcut-raw-" + ext.substr(1);
#if defined(_WIN32)
    name += ".exe";
#endif
    std::vector<std::filesystem::path> folders;
    if (const char* env = std::getenv("WABCUT_RAW_HELPERS"); env && *env) {
        folders.push_back(pathFromUtf8(env));
    }
    if (const auto dir = executableDir(); !dir.empty()) {
        folders.push_back(dir / "raw-helpers");
    }
    for (const auto& folder : folders) {
        std::error_code ec;
        const auto candidate = folder / pathFromUtf8(name);
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

struct RawHelper::Impl {
    RawHelperHeader header;
    std::size_t frameBytes = 0;
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE toChild = nullptr;
    HANDLE fromChild = nullptr;
#else
    pid_t pid = -1;
    int toChild = -1;
    int fromChild = -1;
#endif

    bool writeAll(const std::string& text) {
#if !defined(_WIN32)
        ignoreSigpipeOnce();
#endif
        std::size_t done = 0;
        while (done < text.size()) {
#if defined(_WIN32)
            DWORD wrote = 0;
            if (!WriteFile(toChild, text.data() + done, static_cast<DWORD>(text.size() - done), &wrote, nullptr) ||
                wrote == 0) {
                return false;
            }
#else
            const ssize_t wrote = ::write(toChild, text.data() + done, text.size() - done);
            if (wrote <= 0) {
                return false;
            }
#endif
            done += static_cast<std::size_t>(wrote);
        }
        return true;
    }
    bool readAll(std::uint8_t* out, std::size_t size) {
        std::size_t done = 0;
        while (done < size) {
#if defined(_WIN32)
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size - done, 1 << 20));
            if (!ReadFile(fromChild, out + done, want, &got, nullptr) || got == 0) {
                return false;
            }
#else
            const ssize_t got = ::read(fromChild, out + done, std::min<std::size_t>(size - done, 1 << 20));
            if (got <= 0) {
                return false;
            }
#endif
            done += static_cast<std::size_t>(got);
        }
        return true;
    }
    std::optional<std::string> readLine() {
        std::string line;
        std::uint8_t c = 0;
        while (line.size() < kMaxHeaderBytes) {
            if (!readAll(&c, 1)) {
                return std::nullopt;
            }
            if (c == '\n') {
                return line;
            }
            line += static_cast<char>(c);
        }
        return std::nullopt;
    }

    ~Impl() {
        writeAll("quit\n");
#if defined(_WIN32)
        if (toChild) {
            CloseHandle(toChild);
        }
        if (process) {
            if (WaitForSingleObject(process, 2000) != WAIT_OBJECT_0) {
                TerminateProcess(process, 1);
            }
            CloseHandle(process);
        }
        if (fromChild) {
            CloseHandle(fromChild);
        }
        if (job) {
            CloseHandle(job);
        }
#else
        if (toChild >= 0) {
            ::close(toChild);
        }
        if (pid > 0) {
            int status = 0;
            for (int i = 0; i < 200 && ::waitpid(pid, &status, WNOHANG) == 0; ++i) {
                ::usleep(10000);
            }
            if (::waitpid(pid, &status, WNOHANG) == 0) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
            }
        }
        if (fromChild >= 0) {
            ::close(fromChild);
        }
#endif
    }
};

RawHelper::RawHelper(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
RawHelper::~RawHelper() = default;

const RawHelperHeader& RawHelper::header() const { return m_impl->header; }
void RawHelper::setFrameBytes(std::size_t bytes) { m_impl->frameBytes = bytes; }
std::size_t RawHelper::frameBytes() const { return m_impl->frameBytes; }

std::expected<void, Error> RawHelper::readFrame(std::int64_t index, std::uint8_t* out) {
    if (index < 0 || index >= m_impl->header.frames || m_impl->frameBytes == 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No such frame"});
    }
    if (!m_impl->writeAll(std::to_string(index) + "\n") || !m_impl->readAll(out, m_impl->frameBytes)) {
        return std::unexpected(Error{ErrorCode::IoError, "The RAW helper stopped"});
    }
    return {};
}

std::expected<std::unique_ptr<RawHelper>, Error> RawHelper::start(const std::filesystem::path& helper,
                                                                 const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(helper, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "RAW helper not found: " + pathToUtf8(helper)});
    }
    auto impl = std::make_unique<Impl>();
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childIn = nullptr;
    HANDLE childOut = nullptr;
    if (!CreatePipe(&childIn, &impl->toChild, &inherit, 0) || !CreatePipe(&impl->fromChild, &childOut, &inherit, 0)) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create the RAW helper's pipes"});
    }
    SetHandleInformation(impl->toChild, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl->fromChild, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childIn;
    startup.hStdOutput = childOut;
    startup.hStdError = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    std::wstring commandLine = L"\"" + helper.wstring() + L"\" \"" + file.wstring() + L"\"";
    impl->job = CreateJobObjectW(nullptr, nullptr);
    if (impl->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // it dies with the editor
        SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(helper.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (!created) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot start the RAW helper " + pathToUtf8(helper)});
    }
    if (impl->job) {
        AssignProcessToJobObject(impl->job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    impl->process = info.hProcess;
#else
    int in[2];
    int out[2];
    if (pipe(in) != 0 || pipe(out) != 0) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create the RAW helper's pipes"});
    }
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, in[0]);
    posix_spawn_file_actions_addclose(&actions, out[1]);
    std::string exe = helper.string();
    std::string arg = file.string();
    char* argv[] = {exe.data(), arg.data(), nullptr};
    const int rc = posix_spawn(&impl->pid, exe.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(in[0]);
    ::close(out[1]);
    impl->toChild = in[1];
    impl->fromChild = out[0];
    if (rc != 0) {
        impl->pid = -1;
        return std::unexpected(Error{ErrorCode::IoError, "Cannot start the RAW helper " + pathToUtf8(helper)});
    }
#endif
    const auto line = impl->readLine();
    if (!line) {
        return std::unexpected(Error{ErrorCode::IoError, "The RAW helper sent no header"});
    }
    const nlohmann::json j = nlohmann::json::parse(*line, nullptr, false);
    if (!j.is_object()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The RAW helper's header is not JSON"});
    }
    if (j.contains("error") && j["error"].is_string()) {
        return std::unexpected(Error{ErrorCode::Unsupported, j["error"].get<std::string>().substr(0, 512)});
    }
    RawHelperHeader& h = impl->header;
    const auto integer = [&](const char* key, std::int64_t lo, std::int64_t hi) -> std::optional<std::int64_t> {
        if (!j.contains(key) || !j[key].is_number_integer()) {
            return std::nullopt;
        }
        const auto v = j[key].get<std::int64_t>();
        return v >= lo && v <= hi ? std::optional(v) : std::nullopt;
    };
    const auto width = integer("width", 1, 32768);
    const auto height = integer("height", 1, 32768);
    const auto rateNum = integer("rateNum", 1, 1'000'000);
    const auto rateDen = integer("rateDen", 1, 1'000'000);
    const auto frames = integer("frames", 1, 100'000'000);
    if (!width || !height || !rateNum || !rateDen || !frames || !j.contains("pixelFormat") || !j["pixelFormat"].is_string()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The RAW helper's header is malformed"});
    }
    h.width = static_cast<int>(*width);
    h.height = static_cast<int>(*height);
    h.rateNum = static_cast<std::int32_t>(*rateNum);
    h.rateDen = static_cast<std::int32_t>(*rateDen);
    h.frames = *frames;
    h.pixelFormat = j["pixelFormat"].get<std::string>().substr(0, 32);
    h.colorTransfer = j.contains("colorTransfer") && j["colorTransfer"].is_string()
                          ? j["colorTransfer"].get<std::string>().substr(0, 16)
                          : std::string("bt709");
    return std::unique_ptr<RawHelper>(new RawHelper(std::move(impl)));
}

} // namespace wabcut::media
