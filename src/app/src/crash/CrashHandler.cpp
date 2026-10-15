#include "CrashHandler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#include <crtdbg.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

extern "C" char** environ;
#endif

namespace wabcut::crash {

namespace {

// ---- State set up by install(); the crash path only reads it --------------------------------------------------------

struct State {
    std::filesystem::path folder;
    std::filesystem::path session;
    std::string version;
    std::size_t logLines = 0;
    std::vector<std::array<char, kMaxLogLine + 1>> ring; // redacted lines, NUL-terminated
    std::atomic<std::size_t> next{0};                   // lines ever written
    std::mutex diagnosticsMutex;
    std::map<std::string, std::string> diagnostics;
    std::array<char, 8192> diagnosticsJson{};           // `diagnostics` as a JSON object, for the crash path
    std::vector<std::string> secrets;                   // values of KEY/TOKEN/SECRET/PASSWORD environment variables
    std::array<char, 1024> reportStem{};                // "<folder>/wabcut-" (narrow, for the POSIX path)
    std::vector<char> report;                           // the report's JSON is built here at crash time
#ifdef _WIN32
    std::wstring folderW;
    HANDLE request = nullptr;
    HANDLE done = nullptr;
    EXCEPTION_POINTERS* exception = nullptr;
    DWORD exceptionThread = 0;
    unsigned long code = 0;
    const char* what = nullptr;
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    MiniDumpWriteDumpFn miniDumpWriteDump = nullptr;
#else
    std::vector<char> altStack;
    std::array<void*, 128> frames{};
#endif
};

State* g_state = nullptr;
std::atomic<bool> g_crashing{false};

// ---- Allocation-free formatting for the crash path -----------------------------------------------------------------

struct Out {
    char* data;
    std::size_t size;
    std::size_t used = 0;

    void put(char c) {
        if (used + 1 < size) {
            data[used++] = c;
        }
    }
    void put(const char* s) {
        while (s && *s) {
            put(*s++);
        }
    }
    void number(unsigned long long v) {
        char digits[24];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && n < 24);
        while (n) {
            put(digits[--n]);
        }
    }
    void hex(unsigned long long v) {
        put("0x");
        char digits[20];
        int n = 0;
        do {
            const int d = static_cast<int>(v & 0xF);
            digits[n++] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
            v >>= 4;
        } while (v && n < 20);
        while (n) {
            put(digits[--n]);
        }
    }
    void padded(unsigned v, int width) {
        char digits[8];
        for (int i = width - 1; i >= 0; --i) {
            digits[i] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        for (int i = 0; i < width; ++i) {
            put(digits[i]);
        }
    }
    /// A JSON string literal.
    void string(const char* s) {
        put('"');
        for (; s && *s; ++s) {
            const unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') {
                put('\\');
                put(static_cast<char>(c));
            } else if (c < 0x20) {
                put("\\u00");
                put("0123456789abcdef"[c >> 4]);
                put("0123456789abcdef"[c & 0xF]);
            } else {
                put(static_cast<char>(c));
            }
        }
        put('"');
    }
    void finish() { data[std::min(used, size - 1)] = '\0'; }
};

/// UTC civil time of `t` (seconds since 1970) without the C library (Howard Hinnant's days-to-civil).
void civil(std::int64_t t, unsigned& year, unsigned& month, unsigned& day, unsigned& hour, unsigned& minute, unsigned& second) {
    std::int64_t days = t / 86400;
    std::int64_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    hour = static_cast<unsigned>(rem / 3600);
    minute = static_cast<unsigned>(rem % 3600 / 60);
    second = static_cast<unsigned>(rem % 60);
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<unsigned>(static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0));
}

/// "20261012-102030".
void stamp(Out& o, std::int64_t t, bool iso) {
    unsigned y, mo, d, h, mi, s;
    civil(t, y, mo, d, h, mi, s);
    o.padded(y, 4);
    if (iso) {
        o.put('-');
    }
    o.padded(mo, 2);
    if (iso) {
        o.put('-');
    }
    o.padded(d, 2);
    o.put(iso ? 'T' : '-');
    o.padded(h, 2);
    if (iso) {
        o.put(':');
    }
    o.padded(mi, 2);
    if (iso) {
        o.put(':');
    }
    o.padded(s, 2);
    if (iso) {
        o.put('Z');
    }
}

std::uint32_t processId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

/// The report's JSON: what happened, when, the version, the diagnostics and the log ring (already redacted).
std::size_t buildReport(State& s, std::int64_t now, const char* what, unsigned long long code, const char* dumpName) {
    Out o{s.report.data(), s.report.size()};
    o.put("{\n  \"product\": \"Wabcut\",\n  \"version\": ");
    o.string(s.version.c_str());
    o.put(",\n  \"time\": \"");
    stamp(o, now, true);
    o.put("\",\n  \"pid\": ");
    o.number(processId());
    o.put(",\n  \"what\": ");
    o.string(what);
    o.put(",\n  \"code\": \"");
    o.hex(code);
    o.put("\",\n  \"dump\": ");
    if (dumpName) {
        o.string(dumpName);
    } else {
        o.put("null");
    }
    o.put(",\n  \"diagnostics\": ");
    o.put(s.diagnosticsJson.data()[0] ? s.diagnosticsJson.data() : "{}");
    o.put(",\n  \"log\": [");
    const std::size_t written = s.next.load(std::memory_order_acquire);
    const std::size_t count = std::min(written, s.logLines);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& line = s.ring[(written - count + i) % s.logLines];
        o.put(i == 0 ? "\n    " : ",\n    ");
        o.string(line.data());
    }
    o.put(count ? "\n  ]\n}\n" : "]\n}\n");
    o.finish();
    return o.used;
}

/// "C0000005" names for the usual exception codes.
const char* describe(unsigned long code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC00000FD: return "stack overflow";
    case 0xC0000094: return "integer division by zero";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000409: return "stack buffer overrun (fail fast)";
    case 0xC0000374: return "heap corruption";
    case 0xE06D7363: return "unhandled C++ exception (an exception nobody caught)";
    case 0x80000003: return "breakpoint";
    default: return "crash";
    }
}

// ---- Redaction (normal context: log() and tests) -------------------------------------------------------------------

std::string replaceAll(std::string text, const std::regex& pattern, const std::function<std::string(const std::smatch&)>& with) {
    std::string out;
    auto begin = text.cbegin();
    std::smatch m;
    while (std::regex_search(begin, text.cend(), m, pattern)) {
        out.append(begin, m[0].first);
        out += with(m);
        begin = m[0].second;
        if (m[0].length() == 0) {
            if (begin == text.cend()) {
                break;
            }
            out += *begin++;
        }
    }
    out.append(begin, text.cend());
    return out;
}

/// "<path>" keeping the extension of the last segment (".mp4"): useful, and names no person or project.
std::string pathToken(const std::string& path) {
    std::string tail = path;
    while (!tail.empty() && (tail.back() == '\\' || tail.back() == '/' || tail.back() == '.' || tail.back() == ',' ||
                             tail.back() == ')' || tail.back() == ':' || tail.back() == ';')) {
        tail.pop_back();
    }
    const std::size_t slash = tail.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? tail : tail.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos && dot > 0 && name.size() - dot <= 7) { // the dot and up to six characters (".wabcut")
        ext = name.substr(dot);
        if (!std::all_of(ext.begin() + 1, ext.end(), [](unsigned char c) { return std::isalnum(c) != 0; })) {
            ext.clear();
        }
    }
    return "<path>" + ext;
}

bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

/// Where an absolute path starts at `i`: "X:\" or "X:/", "\\server\", "file://", "~/", or "/a/b" after a word break.
bool pathStartsAt(const std::string& s, std::size_t i) {
    const auto at = [&](std::size_t k) { return k < s.size() ? s[k] : '\0'; };
    const bool wordBreak = i == 0 || space(s[i - 1]) || std::string_view("\"'(=[<,:;").find(s[i - 1]) != std::string_view::npos;
    if (std::isalpha(static_cast<unsigned char>(at(i))) && at(i + 1) == ':' && (at(i + 2) == '\\' || at(i + 2) == '/') &&
        (i == 0 || !alnum(s[i - 1]))) {
        return true;
    }
    if (at(i) == '\\' && at(i + 1) == '\\' && (alnum(at(i + 2)) || at(i + 2) == '?')) {
        return wordBreak;
    }
    if (s.size() - i >= 7) {
        std::string scheme = s.substr(i, 7);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (scheme == "file://") {
            return true;
        }
    }
    if (at(i) == '~' && at(i + 1) == '/') {
        return wordBreak;
    }
    if (at(i) == '/' && wordBreak && alnum(at(i + 1))) {
        const std::size_t next = s.find('/', i + 1);
        return next != std::string::npos && next + 1 < s.size() && !space(s[next + 1]) &&
               s.find_first_of(" \t", i + 1) > next; // at least two segments before any space
    }
    return false;
}

/// The end of the path that starts at `start`: names may contain spaces ("Wedding Day.mp4"), so the path runs to a quote,
/// bracket or the line's end, and then ends after the extension of its last segment (".mp4" followed by a break), or,
/// without one, at the first space after its last separator.
std::size_t pathEnd(const std::string& s, std::size_t start) {
    std::size_t limit = s.find_first_of("\"'<>|\r\n", start);
    if (limit == std::string::npos) {
        limit = s.size();
    }
    std::size_t lastSep = start;
    for (std::size_t k = start; k < limit; ++k) {
        if (s[k] == '\\' || s[k] == '/') {
            lastSep = k;
        }
    }
    for (std::size_t k = lastSep + 1; k < limit; ++k) {
        if (s[k] != '.' || k + 1 >= limit) {
            continue;
        }
        std::size_t e = k + 1;
        bool letter = false;
        while (e < limit && alnum(s[e]) && e - k <= 6) {
            letter = letter || std::isalpha(static_cast<unsigned char>(s[e])) != 0;
            ++e;
        }
        if (e > k + 1 && e - k <= 7 && letter && (e == limit || space(s[e]) || std::string_view(",;:)]").find(s[e]) != std::string_view::npos)) {
            return e;
        }
    }
    std::size_t end = lastSep + 1;
    while (end < limit && !space(s[end])) {
        ++end;
    }
    while (end > start + 1 && std::string_view(".,;:)]").find(s[end - 1]) != std::string_view::npos) {
        --end;
    }
    return end;
}

std::string redactPaths(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t start = i;
        while (start < s.size() && !pathStartsAt(s, start)) {
            ++start;
        }
        out.append(s, i, start - i);
        if (start >= s.size()) {
            break;
        }
        const std::size_t end = pathEnd(s, start);
        out += pathToken(s.substr(start, end - start));
        i = std::max(end, start + 1);
    }
    return out;
}

} // namespace

namespace {

std::mutex g_secretRedactionMutex;
std::function<std::string(std::string_view)> g_secretRedaction;

} // namespace

void setSecretRedaction(std::function<std::string(std::string_view)> redactor) {
    std::lock_guard lock(g_secretRedactionMutex);
    g_secretRedaction = std::move(redactor);
}

std::string redact(std::string_view input) {
    // Known secrets first, on the whole line, so that no cut leaves a part of one (review wave 5, S5-P2-10).
    std::string whole(input);
    std::function<std::string(std::string_view)> secrets;
    {
        std::lock_guard lock(g_secretRedactionMutex);
        secrets = g_secretRedaction;
    }
    if (secrets) {
        whole = secrets(whole);
    }
    if (g_state) {
        for (const std::string& secret : g_state->secrets) {
            for (std::size_t at = whole.find(secret); at != std::string::npos; at = whole.find(secret, at)) {
                whole.replace(at, secret.size(), "<redacted>");
            }
        }
    }
    std::string text = redactPaths(whole.substr(0, kMaxLogLine * 4));
    using std::regex;
    static const regex bearer(R"((Bearer\s+)[^\s"',;]+)", regex::icase);
    static const regex assignment(
        R"(((?:x-api-key|api[_-]?key|access[_-]?token|key|token|secret|password|passwd|authorization)["']?\s*[=:]\s*["']?)[^\s"',;&]+)",
        regex::icase);
    static const regex knownKey(R"((?:sk|pk|rk)-[A-Za-z0-9_\-]{10,}|AIza[0-9A-Za-z_\-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|xox[abpr]-[A-Za-z0-9\-]{10,})");
    static const regex longHex(R"(\b[0-9A-Fa-f]{32,}\b)");
    static const regex longToken(R"(\b[A-Za-z0-9+/_\-]{40,}={0,2})");
    static const regex email(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
    text = replaceAll(std::move(text), bearer, [](const std::smatch& m) { return m.str(1) + "<redacted>"; });
    text = replaceAll(std::move(text), assignment, [](const std::smatch& m) { return m.str(1) + "<redacted>"; });
    text = replaceAll(std::move(text), knownKey, [](const std::smatch&) { return std::string("<redacted>"); });
    text = replaceAll(std::move(text), longHex, [](const std::smatch&) { return std::string("<redacted>"); });
    text = replaceAll(std::move(text), longToken, [](const std::smatch&) { return std::string("<redacted>"); });
    text = replaceAll(std::move(text), email, [](const std::smatch&) { return std::string("<email>"); });
    if (text.size() > kMaxLogLine) {
        text.resize(kMaxLogLine);
    }
    return text;
}

namespace {

std::vector<std::string> secretEnvironment() {
    std::vector<std::string> out;
#ifdef _WIN32
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const std::size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) {
                continue;
            }
            std::wstring name = entry.substr(0, eq);
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
            if (name.find(L"KEY") == std::wstring::npos && name.find(L"TOKEN") == std::wstring::npos &&
                name.find(L"SECRET") == std::wstring::npos && name.find(L"PASSWORD") == std::wstring::npos) {
                continue;
            }
            const std::wstring value = entry.substr(eq + 1);
            if (value.size() < 8) {
                continue;
            }
            const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), n, nullptr, nullptr);
            out.push_back(std::move(utf8));
        }
        FreeEnvironmentStringsW(block);
    }
#else
    for (char** e = environ; e && *e; ++e) {
        const std::string entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = entry.substr(0, eq);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (name.find("KEY") == std::string::npos && name.find("TOKEN") == std::string::npos &&
            name.find("SECRET") == std::string::npos && name.find("PASSWORD") == std::string::npos) {
            continue;
        }
        if (entry.size() - eq - 1 >= 8) {
            out.push_back(entry.substr(eq + 1));
        }
    }
#endif
    return out;
}

/// Rewrites the session file and the diagnostics JSON the crash path copies (diagnosticsMutex held).
void writeSession(State& s) {
    std::string json = "{";
    bool first = true;
    for (const auto& [key, value] : s.diagnostics) {
        std::array<char, 2048> buffer{};
        Out o{buffer.data(), buffer.size()};
        o.put(first ? "" : ", ");
        o.string(key.c_str());
        o.put(": ");
        o.string(value.c_str());
        o.finish();
        json += buffer.data();
        first = false;
    }
    json += "}";
    std::fill(s.diagnosticsJson.begin(), s.diagnosticsJson.end(), '\0');
    std::memcpy(s.diagnosticsJson.data(), json.data(), std::min(json.size(), s.diagnosticsJson.size() - 1));

    std::array<char, 512> head{};
    Out o{head.data(), head.size()};
    o.put("{\n  \"product\": \"Wabcut\",\n  \"version\": ");
    o.string(s.version.c_str());
    o.put(",\n  \"pid\": ");
    o.number(processId());
    o.put(",\n  \"started\": \"");
    stamp(o, static_cast<std::int64_t>(std::time(nullptr)), true);
    o.put("\",\n  \"diagnostics\": ");
    o.finish();
    std::ofstream out(s.session, std::ios::binary | std::ios::trunc);
    out << head.data() << json << "\n}\n";
}

// ---- Writing a report ----------------------------------------------------------------------------------------------

#ifdef _WIN32

/// The report and dump file names of this crash: "wabcut-<stamp>-<pid>" + extension, wide, under the folder.
std::wstring crashPath(State& s, std::int64_t now, const wchar_t* extension, char* narrowName, std::size_t narrowSize) {
    Out n{narrowName, narrowSize};
    n.put("wabcut-");
    stamp(n, now, false);
    n.put('-');
    n.number(processId());
    const std::size_t stem = n.used;
    for (const wchar_t* e = extension; *e; ++e) {
        n.put(static_cast<char>(*e));
    }
    n.finish();
    std::wstring path = s.folderW;
    path += L'\\';
    for (std::size_t i = 0; i < stem + wcslen(extension); ++i) {
        path += static_cast<wchar_t>(static_cast<unsigned char>(narrowName[i]));
    }
    return path;
}

void writeAll(HANDLE file, const char* data, std::size_t size) {
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, data, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &written, nullptr) || written == 0) {
            return;
        }
        data += written;
        size -= written;
    }
}

/// Writes the dump (when a context is known) and the report. Runs on the writer thread (or, for tests, any thread).
void writeCrash(State& s, EXCEPTION_POINTERS* exception, DWORD thread, unsigned long code, const char* what) {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    char dumpName[256] = {};
    char reportName[256] = {};
    bool dumped = false;
    if (s.miniDumpWriteDump) {
        const std::wstring dumpPath = crashPath(s, now, L".dmp", dumpName, sizeof dumpName);
        const HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{};
            info.ThreadId = thread;
            info.ExceptionPointers = exception;
            info.ClientPointers = FALSE;
            // Stacks, threads and modules; stack memory filtered down to what unwinding needs (no data from the stack,
            // so no fragments of keys or paths), module paths cut to their file names. No heap.
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpFilterMemory | MiniDumpFilterModulePaths |
                                                         MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            dumped = s.miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception ? &info : nullptr,
                                         nullptr, nullptr) != FALSE;
            CloseHandle(file);
            if (!dumped) {
                DeleteFileW(dumpPath.c_str());
            }
        }
    }
    const std::wstring reportPath = crashPath(s, now, L".json", reportName, sizeof reportName);
    const std::size_t size = buildReport(s, now, what ? what : describe(code), code, dumped ? dumpName : nullptr);
    const HANDLE file = CreateFileW(reportPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        writeAll(file, s.report.data(), size);
        CloseHandle(file);
    }
}

DWORD WINAPI writerMain(LPVOID) {
    State& s = *g_state;
    for (;;) {
        if (WaitForSingleObject(s.request, INFINITE) != WAIT_OBJECT_0) {
            return 0;
        }
        writeCrash(s, s.exception, s.exceptionThread, s.code, s.what);
        SetEvent(s.done);
    }
}

/// Hands the crash to the writer thread (a whole stack, even after a stack overflow) and ends the process. Windows
/// Error Reporting is not involved: nothing leaves this computer.
[[noreturn]] void crashed(EXCEPTION_POINTERS* exception, unsigned long code, const char* what) {
    State* s = g_state;
    if (s && !g_crashing.exchange(true)) {
        s->exception = exception;
        s->exceptionThread = GetCurrentThreadId();
        s->code = code;
        s->what = what;
        SetEvent(s->request);
        WaitForSingleObject(s->done, 60'000);
    }
    TerminateProcess(GetCurrentProcess(), code ? static_cast<UINT>(code) : 3u);
    for (;;) {
    }
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* exception) {
    crashed(exception, exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0, nullptr);
}

/// A crash without an exception (abort, terminate, a CRT check): capture this thread's context for the dump.
[[noreturn]] void crashedHere(unsigned long code, const char* what) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
    EXCEPTION_POINTERS pointers{&record, &context};
    crashed(&pointers, code, what);
}

void onAbort(int) { crashedHere(0xE0000001, "abort() (an assertion or a fatal error)"); }
void onTerminate() { crashedHere(0xE0000002, "std::terminate (an exception nobody caught)"); }
void onPureCall() { crashedHere(0xE0000003, "pure virtual function call"); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    crashedHere(0xE0000004, "invalid parameter passed to a C runtime function");
}

bool installPlatform(State& s) {
    s.folderW = s.folder.wstring();
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        s.miniDumpWriteDump = reinterpret_cast<State::MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    }
    s.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.request || !s.done || !CreateThread(nullptr, 256 * 1024, writerMain, nullptr, 0, nullptr)) {
        return false;
    }
    // Room for the filter on a thread whose stack overflowed.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(onUnhandledException);
    std::signal(SIGABRT, onAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no "abort() was called" box, no WER report
    std::set_terminate(onTerminate);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
    // No Windows Error Reporting dialog (and no upload) if anything slips past the filter.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    return true;
}

#else // POSIX

void writeFd(int fd, const char* data, std::size_t size) {
    while (size) {
        const ssize_t n = ::write(fd, data, size);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "segmentation fault (SIGSEGV)";
    case SIGBUS: return "bus error (SIGBUS)";
    case SIGILL: return "illegal instruction (SIGILL)";
    case SIGFPE: return "arithmetic error (SIGFPE)";
    case SIGABRT: return "abort() (SIGABRT)";
    default: return "fatal signal";
    }
}

/// The report (.json) and a backtrace (.txt) beside it: only async-signal-safe calls (open, write, close; backtrace was
/// warmed up at install so it does not allocate here).
void writeCrashPosix(State& s, int sig, const char* what) {
    const auto now = static_cast<std::int64_t>(::time(nullptr));
    char path[1400];
    char name[256];
    Out n{name, sizeof name};
    n.put("wabcut-");
    stamp(n, now, false);
    n.put('-');
    n.number(processId());
    n.put(".txt");
    n.finish();
    Out p{path, sizeof path};
    p.put(s.reportStem.data());
    p.put(name);
    p.finish();
    const int frames = backtrace(s.frames.data(), static_cast<int>(s.frames.size()));
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        backtrace_symbols_fd(s.frames.data(), frames, fd);
        ::close(fd);
    }
    const std::size_t size = buildReport(s, now, what, static_cast<unsigned long long>(sig), name);
    // name ends in ".txt": the same stem with ".json".
    p.used -= 4;
    p.put(".json");
    p.finish();
    fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        writeFd(fd, s.report.data(), size);
        ::close(fd);
    }
}

volatile std::sig_atomic_t g_terminating = 0; // the SIGABRT comes from std::terminate

void onSignal(int sig, siginfo_t*, void*) {
    if (g_state && !g_crashing.exchange(true)) {
        writeCrashPosix(*g_state, sig,
                        sig == SIGABRT && g_terminating ? "std::terminate (an exception nobody caught)" : signalName(sig));
    }
    // SA_RESETHAND restored the default action: re-raise for the usual exit status (and a core file if enabled).
    ::raise(sig);
}

void onTerminate() {
    g_terminating = 1;
    std::abort(); // SIGABRT writes the report
}

bool installPlatform(State& s) {
    const std::string stem = s.folder.string() + "/";
    std::memcpy(s.reportStem.data(), stem.data(), std::min(stem.size(), s.reportStem.size() - 1));
    // Warm up backtrace(): its first call may load libgcc and allocate, which a signal handler must not do.
    void* warm[4];
    (void)backtrace(warm, 4);
    s.altStack.resize(std::max<std::size_t>(SIGSTKSZ, 64 * 1024));
    stack_t alt{};
    alt.ss_sp = s.altStack.data();
    alt.ss_size = s.altStack.size();
    if (sigaltstack(&alt, nullptr) != 0) {
        return false;
    }
    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        sigaction(sig, &action, nullptr);
    }
    std::set_terminate(onTerminate);
    return true;
}

#endif

} // namespace

bool install(const CrashOptions& options) {
    if (g_state) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.folder / "sessions", ec);
    if (ec) {
        return false;
    }
    auto state = std::make_unique<State>();
    state->folder = options.folder;
    state->session = options.folder / "sessions" / (std::to_string(processId()) + ".json");
    state->version = options.version;
    state->logLines = std::max<std::size_t>(1, options.logLines);
    state->ring.resize(state->logLines);
    state->report.resize(state->logLines * (kMaxLogLine * 2 + 16) + 32 * 1024);
    state->secrets = secretEnvironment();
    g_state = state.get();
    {
        std::lock_guard lock(state->diagnosticsMutex);
        writeSession(*state);
    }
    if (!installPlatform(*state)) {
        g_state = nullptr;
        return false;
    }
    state.release(); // lives as long as the process
    return true;
}

bool installed() { return g_state != nullptr; }

std::filesystem::path folder() { return g_state ? g_state->folder : std::filesystem::path(); }

void log(std::string_view line) {
    State* s = g_state;
    if (!s) {
        return;
    }
    const std::string clean = redact(line);
    const std::size_t index = s->next.fetch_add(1, std::memory_order_acq_rel) % s->logLines;
    auto& slot = s->ring[index];
    const std::size_t n = std::min(clean.size(), kMaxLogLine);
    std::memcpy(slot.data(), clean.data(), n);
    slot[n] = '\0';
}

void setDiagnostic(std::string_view key, std::string_view value) {
    State* s = g_state;
    if (!s) {
        return;
    }
    std::lock_guard lock(s->diagnosticsMutex);
    s->diagnostics[std::string(key)] = redact(value);
    writeSession(*s);
}

void markCleanExit() {
    if (State* s = g_state) {
        std::error_code ec;
        std::filesystem::remove(s->session, ec);
    }
}

namespace detail {

bool writeReportForTesting(unsigned long code, const char* what) {
    State* s = g_state;
    if (!s) {
        return false;
    }
#ifdef _WIN32
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    EXCEPTION_POINTERS pointers{&record, &context};
    writeCrash(*s, &pointers, GetCurrentThreadId(), code, what);
#else
    writeCrashPosix(*s, static_cast<int>(code), what);
#endif
    return true;
}

} // namespace detail

} // namespace wabcut::crash
