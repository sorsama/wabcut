#include <wabcut/plughost/Ipc.h>
#include <wabcut/plughost/Protocol.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>

#include <wabcut/platform/HelperProcess.h>

#include <array>
#include <cstdio>
#include <mutex>

namespace wabcut::plughost {

namespace {

std::string lastError(std::string_view what) {
    const DWORD code = GetLastError();
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = std::string(what) + " failed (" + std::to_string(code) + ")";
    if (text != nullptr) {
        std::string detail(text);
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r' || detail.back() == '.')) {
            detail.pop_back();
        }
        message += ": " + detail;
        LocalFree(text);
    }
    return message;
}

/// A job object that kills every helper when the editor goes away, however it ends.
HANDLE helperJob() {
    static const HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        }
        return j;
    }();
    return job;
}

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

} // namespace

// ---- Names ------------------------------------------------------------------------------------------------------

InstanceNames instanceNames(std::uint32_t hostPid, std::uint64_t instance) {
    InstanceNames n{};
    std::snprintf(n.memory, sizeof n.memory, "Local\\wabcut-plughost-%u-%llu-memory", hostPid, static_cast<unsigned long long>(instance));
    std::snprintf(n.work, sizeof n.work, "Local\\wabcut-plughost-%u-%llu-work", hostPid, static_cast<unsigned long long>(instance));
    std::snprintf(n.done, sizeof n.done, "Local\\wabcut-plughost-%u-%llu-done", hostPid, static_cast<unsigned long long>(instance));
    return n;
}

// ---- Strings ----------------------------------------------------------------------------------------------------

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::uint32_t currentProcessId() { return GetCurrentProcessId(); }

// ---- Base64 -----------------------------------------------------------------------------------------------------

std::string base64Encode(const void* data, std::size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t a = p[i];
        const std::uint32_t b = i + 1 < size ? p[i + 1] : 0;
        const std::uint32_t c = i + 2 < size ? p[i + 2] : 0;
        const std::uint32_t v = (a << 16) | (b << 8) | c;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kAlphabet[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kAlphabet[v & 63] : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                v[k] = 0;
                ++pad;
                continue;
            }
            if (pad > 0) {
                return std::nullopt; // data after padding
            }
            v[k] = value(c);
            if (v[k] < 0) {
                return std::nullopt;
            }
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                     (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<std::uint8_t>(triple >> 16));
        if (pad < 2) {
            out.push_back(static_cast<std::uint8_t>(triple >> 8));
        }
        if (pad < 1) {
            out.push_back(static_cast<std::uint8_t>(triple));
        }
    }
    return out;
}

// ---- SharedMemory -----------------------------------------------------------------------------------------------

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string& name, std::size_t size, std::string& error) {
    const std::wstring wname = widen(name);
    const auto big = static_cast<std::uint64_t>(size);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(big >> 32),
                                  static_cast<DWORD>(big & 0xffffffffu), wname.c_str());
    if (h == nullptr) {
        error = lastError("CreateFileMapping");
        return nullptr;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        error = "shared memory " + name + " already exists";
        return nullptr;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (view == nullptr) {
        error = lastError("MapViewOfFile");
        CloseHandle(h);
        return nullptr;
    }
    std::unique_ptr<SharedMemory> m(new SharedMemory());
    m->m_handle = h;
    m->m_data = view;
    m->m_size = size;
    return m;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& name, std::size_t size, std::string& error) {
    const std::wstring wname = widen(name);
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
    if (h == nullptr) {
        error = lastError("OpenFileMapping");
        return nullptr;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (view == nullptr) {
        error = lastError("MapViewOfFile");
        CloseHandle(h);
        return nullptr;
    }
    std::unique_ptr<SharedMemory> m(new SharedMemory());
    m->m_handle = h;
    m->m_data = view;
    m->m_size = size;
    return m;
}

SharedMemory::~SharedMemory() {
    if (m_data != nullptr) {
        UnmapViewOfFile(m_data);
    }
    if (m_handle != nullptr) {
        CloseHandle(m_handle);
    }
}

// ---- NamedEvent -------------------------------------------------------------------------------------------------

std::unique_ptr<NamedEvent> NamedEvent::create(const std::string& name, std::string& error) {
    HANDLE h = CreateEventW(nullptr, FALSE, FALSE, widen(name).c_str());
    if (h == nullptr) {
        error = lastError("CreateEvent");
        return nullptr;
    }
    std::unique_ptr<NamedEvent> e(new NamedEvent());
    e->m_handle = h;
    return e;
}

std::unique_ptr<NamedEvent> NamedEvent::open(const std::string& name, std::string& error) {
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, widen(name).c_str());
    if (h == nullptr) {
        error = lastError("OpenEvent");
        return nullptr;
    }
    std::unique_ptr<NamedEvent> e(new NamedEvent());
    e->m_handle = h;
    return e;
}

NamedEvent::~NamedEvent() {
    if (m_handle != nullptr) {
        CloseHandle(m_handle);
    }
}

void NamedEvent::signal() noexcept { SetEvent(m_handle); }

bool NamedEvent::wait(std::chrono::milliseconds timeout) noexcept {
    const auto ms = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(std::min<long long>(timeout.count(), 0x7fffffff));
    return WaitForSingleObject(m_handle, ms) == WAIT_OBJECT_0;
}

// ---- ChildProcess -----------------------------------------------------------------------------------------------

std::unique_ptr<ChildProcess> ChildProcess::start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                                  const std::filesystem::path& stderrFile, std::string& error) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inRead = nullptr;
    HANDLE inWrite = nullptr;
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0) || !CreatePipe(&outRead, &outWrite, &inherit, 0)) {
        error = lastError("CreatePipe");
        return nullptr;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = nullptr;
    if (!stderrFile.empty()) {
        err = CreateFileW(stderrFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (err == nullptr || err == INVALID_HANDLE_VALUE) {
        err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    }
    // Only these three handles reach the child, whatever else this process marked inheritable.
    std::array<HANDLE, 3> handles{inRead, outWrite, err};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<std::uint8_t> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof(HANDLE) * handles.size(), nullptr,
                              nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inRead;
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attrs;
    std::wstring command = quoteArg(exe.wstring());
    for (const std::string& a : args) {
        command += L" " + quoteArg(widen(a));
    }
    PROCESS_INFORMATION pi{};
    // An explicit, minimal environment (no provider keys) and the helper's own folder as its working directory, never
    // the editor's (S5-P2-5).
    std::wstring environment = platform::helperEnvironmentFor(exe);
    const std::wstring folder = exe.parent_path().wstring();
    const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                   environment.data(), folder.empty() ? nullptr : folder.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        CloseHandle(err);
    }
    if (!ok) {
        error = lastError("CreateProcess " + narrow(exe.wstring()));
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return nullptr;
    }
    if (HANDLE job = helperJob(); job != nullptr) {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::unique_ptr<ChildProcess> p(new ChildProcess());
    p->m_process = pi.hProcess;
    p->m_stdinWrite = inWrite;
    p->m_stdoutRead = outRead;
    p->m_pid = pi.dwProcessId;
    return p;
}

ChildProcess::~ChildProcess() {
    if (m_stdinWrite != nullptr) {
        CloseHandle(m_stdinWrite);
    }
    if (m_process != nullptr) {
        if (running()) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 2000);
        }
        CloseHandle(m_process);
    }
    if (m_stdoutRead != nullptr) {
        CloseHandle(m_stdoutRead);
    }
}

bool ChildProcess::writeLine(std::string_view line) {
    std::string data(line);
    data.push_back('\n');
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        DWORD wrote = 0;
        if (!WriteFile(m_stdinWrite, p, static_cast<DWORD>(left), &wrote, nullptr) || wrote == 0) {
            return false;
        }
        p += wrote;
        left -= wrote;
    }
    return true;
}

std::optional<std::string> ChildProcess::readLine() {
    std::size_t searched = 0; // bytes of m_pending known to hold no line end
    for (;;) {
        if (const auto nl = m_pending.find('\n', searched); nl != std::string::npos) {
            std::string line = m_pending.substr(0, nl);
            m_pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        if (m_pending.size() > kMaxLineBytes) {
            // A line that never ends (output that is not the protocol's): the channel is lost, the helper treated as
            // crashed.
            m_pending.clear();
            terminate();
            return std::nullopt;
        }
        searched = m_pending.size();
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(m_stdoutRead, buffer, sizeof buffer, &got, nullptr) || got == 0) {
            return std::nullopt;
        }
        m_pending.append(buffer, got);
    }
}

std::optional<std::uint32_t> ChildProcess::wait(std::chrono::milliseconds timeout) {
    if (WaitForSingleObject(m_process, static_cast<DWORD>(std::max<long long>(timeout.count(), 0))) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(m_process, &code);
    return code;
}

bool ChildProcess::running() const { return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT; }

void ChildProcess::terminate(std::uint32_t code) {
    if (running()) {
        TerminateProcess(m_process, code);
    }
}

// ---- Helper side ------------------------------------------------------------------------------------------------

namespace {
/// The control channel's ends once takeControlChannel() moved them away from the standard handles.
HANDLE gControlIn = nullptr;
HANDLE gControlOut = nullptr;
} // namespace

bool takeControlChannel(std::string& error) {
    HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &gControlIn, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(self, GetStdHandle(STD_OUTPUT_HANDLE), self, &gControlOut, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = lastError("DuplicateHandle");
        gControlIn = nullptr;
        gControlOut = nullptr;
        return false;
    }
    // Output: where stderr goes (the helper's log, or NUL). Input: NUL. Then the C runtime's stdout and stdin.
    std::fflush(stdout);
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    _dup2(_fileno(stderr), _fileno(stdout));
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (nul != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_INPUT_HANDLE, nul);
    }
    FILE* in = nullptr;
    freopen_s(&in, "NUL", "r", stdin);
    return true;
}

std::optional<std::string> readStdinLine() {
    static std::string pending;
    HANDLE in = gControlIn != nullptr ? gControlIn : GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        if (const auto nl = pending.find('\n'); nl != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        if (pending.size() > kMaxLineBytes) {
            return std::nullopt; // not the editor's protocol: stop
        }
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(in, buffer, sizeof buffer, &got, nullptr) || got == 0) {
            return std::nullopt;
        }
        pending.append(buffer, got);
    }
}

void writeStdoutLine(std::string_view line) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    HANDLE out = gControlOut != nullptr ? gControlOut : GetStdHandle(STD_OUTPUT_HANDLE);
    std::string data(line);
    data.push_back('\n');
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        DWORD wrote = 0;
        if (!WriteFile(out, p, static_cast<DWORD>(left), &wrote, nullptr) || wrote == 0) {
            return;
        }
        p += wrote;
        left -= wrote;
    }
}

} // namespace wabcut::plughost
