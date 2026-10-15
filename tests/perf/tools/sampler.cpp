// wabcut_perf_sampler (P.1): a sampling profiler that needs nothing installed. It launches a program (or attaches to
// a running one), samples its threads at a fixed rate (SuspendThread, GetThreadContext, StackWalk64), and reports the
// hottest functions on the CPU and where threads wait, resolved with DbgHelp from the PDBs next to the binaries. It
// records each module's base, so symbols resolve after the program has exited.
//
//   wabcut_perf_sampler [--hz 1000] [--seconds N] [--threads main|all] [--top 40] [--folded out.txt]
//                       [--report out.txt] [--kill] (--pid <pid> | -- <program> [args...])
//
// A sample is "on CPU" when the thread did real work since the last one (QueryThreadCycleTime moved by more than about
// 50 us of a core), else "waiting" (its stack then shows what it waits in). A sampled thread is suspended only while
// its registers and stack are copied; the unwinding runs on the copy. Folded stacks ("a;b;c count") feed any flame
// graph tool.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <wabcut/platform/DllSearch.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

namespace {

struct Options {
    int hz = 1000;
    double seconds = 0.0; // 0: until the program exits
    bool allThreads = false;
    int top = 40;
    bool kill = false;
    bool catchFaults = false;
    DWORD pid = 0;
    std::wstring commandLine;
    std::string folded;
    std::string report;
};

struct Module {
    std::wstring path;
    DWORD64 base = 0;
    DWORD size = 0;
    std::vector<IMAGE_RUNTIME_FUNCTION_ENTRY> functions; ///< its .pdata, read once (sorted by BeginAddress)
};

/// The modules of the sampled process, for StackWalk64's callbacks: unwinding reads each module's .pdata from our
/// copy, so no symbols are loaded while sampling (DbgHelp would load a whole PDB on the first frame in a module).
std::vector<Module>* gModules = nullptr;

const Module* moduleAt(DWORD64 addr) {
    for (const Module& m : *gModules) {
        if (addr >= m.base && addr < m.base + m.size) {
            return &m;
        }
    }
    return nullptr;
}

PVOID CALLBACK functionTableAccess(HANDLE, DWORD64 addr) {
    const Module* m = moduleAt(addr);
    if (m == nullptr || m->functions.empty()) {
        return nullptr;
    }
    const auto rva = static_cast<DWORD>(addr - m->base);
    const auto it = std::upper_bound(m->functions.begin(), m->functions.end(), rva,
                                     [](DWORD v, const IMAGE_RUNTIME_FUNCTION_ENTRY& f) { return v < f.BeginAddress; });
    if (it == m->functions.begin()) {
        return nullptr;
    }
    const IMAGE_RUNTIME_FUNCTION_ENTRY& f = *(it - 1);
    return rva < f.EndAddress ? const_cast<IMAGE_RUNTIME_FUNCTION_ENTRY*>(&f) : nullptr;
}

DWORD64 CALLBACK moduleBase(HANDLE, DWORD64 addr) {
    const Module* m = moduleAt(addr);
    return m != nullptr ? m->base : 0;
}

/// The sampled thread's stack, copied while it was suspended: unwinding reads it from here once the thread runs again
/// (it is suspended only for the copy). Code and unwind data come from the process (they do not change).
struct StackCopy {
    DWORD64 base = 0; ///< the address of data[0] in the process (the thread's rsp)
    std::vector<unsigned char> data;
    std::size_t size = 0;
};
StackCopy gStack;

/// Reads of module images (unwind data: the same few bytes on every tick, and they never change), cached.
std::unordered_map<DWORD64, std::vector<unsigned char>> gImageReads;

BOOL CALLBACK readMemory(HANDLE process, DWORD64 addr, PVOID buffer, DWORD size, LPDWORD read) {
    if (addr >= gStack.base && addr + size <= gStack.base + gStack.size) {
        std::memcpy(buffer, gStack.data.data() + (addr - gStack.base), size);
        *read = size;
        return TRUE;
    }
    const bool image = size <= 4096 && moduleAt(addr) != nullptr;
    const DWORD64 key = addr ^ (static_cast<DWORD64>(size) << 52);
    if (image) {
        if (const auto it = gImageReads.find(key); it != gImageReads.end()) {
            std::memcpy(buffer, it->second.data(), it->second.size());
            *read = static_cast<DWORD>(it->second.size());
            return TRUE;
        }
    }
    SIZE_T got = 0;
    const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(addr), buffer, size, &got);
    *read = static_cast<DWORD>(got);
    if (ok && image) {
        const auto* bytes = static_cast<const unsigned char*>(buffer);
        gImageReads.emplace(key, std::vector<unsigned char>(bytes, bytes + got));
    }
    return ok;
}

/// Whether a leaf frame is a system call that waits (a thread suspended there is blocked, whatever its cycle count
/// says: one that woke briefly since the last tick has used cycles).
bool waitsIn(const std::string& leaf) {
    if (leaf.find("[ntdll.dll]") == std::string::npos && leaf.find("[win32u.dll]") == std::string::npos) {
        return false;
    }
    for (const char* w : {"Wait", "DelayExecution", "RemoveIoCompletion", "ReplyWaitReceivePort"}) {
        if (leaf.find(w) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<IMAGE_RUNTIME_FUNCTION_ENTRY> readPdata(HANDLE process, DWORD64 base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base), &dos, sizeof dos, &got) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + dos.e_lfanew), &nt, sizeof nt, &got) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return {};
    }
    const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    std::vector<IMAGE_RUNTIME_FUNCTION_ENTRY> functions(dir.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY));
    if (functions.empty() ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + dir.VirtualAddress), functions.data(),
                           functions.size() * sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY), &got)) {
        return {};
    }
    std::sort(functions.begin(), functions.end(),
              [](const auto& a, const auto& b) { return a.BeginAddress < b.BeginAddress; });
    return functions;
}

struct ThreadState {
    HANDLE handle = nullptr;
    DWORD id = 0;
    ULONG64 cycles = 0;
    bool primed = false;
    DWORD64 stackTop = 0; ///< the end of the stack's region (found once)
};

struct StackKey {
    std::vector<DWORD64> frames; // leaf first
    bool onCpu = false;
    DWORD thread = 0;
    bool operator==(const StackKey& o) const { return onCpu == o.onCpu && thread == o.thread && frames == o.frames; }
};

struct StackHash {
    std::size_t operator()(const StackKey& k) const noexcept {
        std::size_t h = k.onCpu ? 0x9e3779b97f4a7c15ull : 0;
        h ^= k.thread * 0x100000001b3ull;
        for (const DWORD64 f : k.frames) {
            h = (h ^ static_cast<std::size_t>(f)) * 0x100000001b3ull;
        }
        return h;
    }
};

std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n - 1, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n - 1, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) {
        return a;
    }
    std::wstring q = L"\"";
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++slashes;
            ++i;
        }
        if (i == a.size()) {
            q.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            q.append(slashes * 2 + 1, L'\\');
        } else {
            q.append(slashes, L'\\');
        }
        q.push_back(a[i]);
    }
    q.push_back(L'"');
    return q;
}

int usage() {
    std::fprintf(stderr,
                 "usage: wabcut_perf_sampler [--hz 1000] [--seconds N] [--threads main|all] [--top 40]\n"
                 "                           [--folded out.txt] [--report out.txt] [--kill]\n"
                 "       wabcut_perf_sampler --catch [--top N] -- <program> [args...]   (stacks of faults)\n"
                 "                           (--pid <pid> | -- <program> [args...])\n");
    return 2;
}

class Sampler {
public:
    Sampler(HANDLE process, DWORD mainThread, const Options& options)
        : m_process(process), m_mainThread(mainThread), m_options(options) {
        gModules = &m_modules;
        gStack.data.resize(512 * 1024);
    }
    ~Sampler() {
        for (auto& [id, t] : m_threads) {
            CloseHandle(t.handle);
        }
        if (m_symbols) {
            SymCleanup(m_process);
        }
        gModules = nullptr;
    }

    void refreshModules() {
        HMODULE handles[1024];
        DWORD needed = 0;
        if (!EnumProcessModulesEx(m_process, handles, sizeof handles, &needed, LIST_MODULES_ALL)) {
            return;
        }
        const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < count; ++i) {
            const auto base = reinterpret_cast<DWORD64>(handles[i]);
            if (m_moduleBases.count(base) != 0) {
                continue;
            }
            MODULEINFO info{};
            wchar_t path[MAX_PATH * 2] = {};
            if (!GetModuleInformation(m_process, handles[i], &info, sizeof info) ||
                GetModuleFileNameExW(m_process, handles[i], path, MAX_PATH * 2) == 0) {
                continue;
            }
            m_moduleBases[base] = m_modules.size();
            m_modules.push_back({path, base, info.SizeOfImage, readPdata(m_process, base)});
        }
    }

    void refreshThreads(DWORD pid) {
        if (!m_options.allThreads) { // the main thread only: open it once (a snapshot lists every thread of the system)
            if (m_threads.empty()) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                      m_mainThread);
                if (h != nullptr) {
                    m_threads[m_mainThread] = ThreadState{h, m_mainThread, 0, false};
                }
            }
            return;
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        std::map<DWORD, bool> alive;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) {
                continue;
            }
            if (!m_options.allThreads && te.th32ThreadID != m_mainThread) {
                continue;
            }
            alive[te.th32ThreadID] = true;
            if (m_threads.count(te.th32ThreadID) == 0) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                      te.th32ThreadID);
                if (h != nullptr) {
                    m_threads[te.th32ThreadID] = ThreadState{h, te.th32ThreadID, 0, false};
                }
            }
        }
        CloseHandle(snap);
        for (auto it = m_threads.begin(); it != m_threads.end();) {
            if (!alive.count(it->first)) {
                CloseHandle(it->second.handle);
                it = m_threads.erase(it);
            } else {
                ++it;
            }
        }
    }

    void sampleOnce() {
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& [id, t] : m_threads) {
            ULONG64 cycles = 0;
            QueryThreadCycleTime(t.handle, &cycles);
            // Suspending a thread runs a kernel APC on it, which counts a few thousand cycles even when it is blocked:
            // only real work (about 50 us of a core since the last tick) counts as on CPU.
            constexpr ULONG64 kBusyCycles = 200'000;
            const bool onCpu = t.primed && cycles - t.cycles > kBusyCycles;
            t.cycles = cycles;
            if (!t.primed) {
                t.primed = true;
                continue;
            }
            if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            StackKey key;
            key.onCpu = onCpu;
            key.thread = m_options.allThreads ? id : 0;
            const bool haveContext = GetThreadContext(t.handle, &ctx) != FALSE;
            gStack.size = 0;
            if (haveContext) {
                if (t.stackTop == 0) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQueryEx(m_process, reinterpret_cast<LPCVOID>(ctx.Rsp), &mbi, sizeof mbi) != 0) {
                        t.stackTop = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
                    }
                }
                constexpr DWORD64 kMaxCopy = 512 * 1024;
                const DWORD64 want = t.stackTop > ctx.Rsp ? std::min(t.stackTop - ctx.Rsp, kMaxCopy) : 0;
                SIZE_T got = 0;
                if (want > 0 && ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(ctx.Rsp), gStack.data.data(),
                                                  static_cast<SIZE_T>(want), &got)) {
                    gStack.base = ctx.Rsp;
                    gStack.size = got;
                }
            }
            ResumeThread(t.handle);
            if (haveContext) {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = ctx.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                for (int depth = 0; depth < 160; ++depth) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, m_process, t.handle, &frame, &ctx, readMemory,
                                     functionTableAccess, moduleBase, nullptr) ||
                        frame.AddrPC.Offset == 0) {
                        break;
                    }
                    key.frames.push_back(frame.AddrPC.Offset);
                }
            }
            if (!key.frames.empty()) {
                ++m_stacks[std::move(key)];
                ++(onCpu ? m_onCpu : m_waiting);
            }
        }
        ++m_ticks;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        m_sampleUs += us;
        m_maxSampleUs = std::max(m_maxSampleUs, us);
    }

    void writeReports(double seconds) {
        // Symbols now (deferred: a PDB loads on the first address in its module), from the files on disk, so this
        // works after the program has exited.
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
        m_symbols = SymInitializeW(m_process, nullptr, FALSE) != FALSE;
        for (const Module& m : m_modules) {
            SymLoadModuleExW(m_process, nullptr, m.path.c_str(), nullptr, m.base, m.size, nullptr, 0);
        }
        // Resolve every address once.
        std::unordered_map<DWORD64, std::string> names;
        const auto nameOf = [&](DWORD64 addr) -> const std::string& {
            auto it = names.find(addr);
            if (it != names.end()) {
                return it->second;
            }
            std::string name;
            alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
            auto* sym = reinterpret_cast<SYMBOL_INFOW*>(buffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
            sym->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddrW(m_process, addr, &displacement, sym)) {
                name = narrow(sym->Name);
            } else {
                char hex[32];
                std::snprintf(hex, sizeof hex, "0x%llx", static_cast<unsigned long long>(addr));
                name = hex;
            }
            const std::string module = moduleOf(addr);
            if (!module.empty()) {
                name += " [" + module + "]";
            }
            return names.emplace(addr, std::move(name)).first->second;
        };
        std::map<std::string, std::uint64_t> exclusiveCpu, inclusiveCpu, waitingLeaf, waitingCaller;
        std::map<std::string, std::uint64_t> folded;
        m_onCpu = m_waiting = 0;
        for (const auto& [key, count] : m_stacks) {
            std::vector<std::string> frames;
            frames.reserve(key.frames.size());
            for (const DWORD64 f : key.frames) {
                frames.push_back(nameOf(f));
            }
            const bool onCpu = key.onCpu && !waitsIn(frames.front());
            (onCpu ? m_onCpu : m_waiting) += count;
            if (onCpu) {
                exclusiveCpu[frames.front()] += count;
                std::vector<std::string> seen;
                for (const std::string& f : frames) {
                    if (std::find(seen.begin(), seen.end(), f) == seen.end()) {
                        inclusiveCpu[f] += count;
                        seen.push_back(f);
                    }
                }
            } else {
                waitingLeaf[frames.front()] += count;
                // The first frame of our own code (not the OS or Qt): what the thread waits for.
                for (const std::string& f : frames) {
                    if (f.find("[wabcut") != std::string::npos || f.find("[Qt6") != std::string::npos) {
                        waitingCaller[f] += count;
                        break;
                    }
                }
            }
            std::string line = onCpu ? "cpu" : "wait";
            if (key.thread != 0) {
                line += ";thread " + std::to_string(key.thread);
            }
            for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
                line += ";" + *it;
            }
            folded[line] += count;
        }
        std::string out;
        char header[320];
        std::snprintf(header, sizeof header,
                      "samples: %llu on CPU, %llu waiting, %llu ticks in %.2f s (%d Hz asked); a tick took %.0f us on "
                      "average, %.0f us at most\n",
                      static_cast<unsigned long long>(m_onCpu), static_cast<unsigned long long>(m_waiting),
                      static_cast<unsigned long long>(m_ticks), seconds, m_options.hz,
                      m_ticks ? m_sampleUs / static_cast<double>(m_ticks) : 0.0, m_maxSampleUs);
        out += header;
        const auto section = [&](const char* title, const std::map<std::string, std::uint64_t>& table, std::uint64_t total) {
            std::vector<std::pair<std::string, std::uint64_t>> rows(table.begin(), table.end());
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            out += std::string("\n== ") + title + " ==\n";
            for (std::size_t i = 0; i < rows.size() && i < static_cast<std::size_t>(m_options.top); ++i) {
                char line[64];
                std::snprintf(line, sizeof line, "%8llu %5.1f%%  ", static_cast<unsigned long long>(rows[i].second),
                              total ? 100.0 * static_cast<double>(rows[i].second) / static_cast<double>(total) : 0.0);
                out += line + rows[i].first + "\n";
            }
        };
        section("on CPU, exclusive (leaf)", exclusiveCpu, m_onCpu);
        section("on CPU, inclusive", inclusiveCpu, m_onCpu);
        section("waiting, in (leaf)", waitingLeaf, m_waiting);
        section("waiting, first wabcut/Qt frame", waitingCaller, m_waiting);
        std::fputs(out.c_str(), stdout);
        if (!m_options.report.empty()) {
            std::ofstream(m_options.report, std::ios::binary) << out;
        }
        if (!m_options.folded.empty()) {
            std::ofstream f(m_options.folded, std::ios::binary);
            for (const auto& [line, count] : folded) {
                f << line << ' ' << count << '\n';
            }
        }
    }

private:
    std::string moduleOf(DWORD64 addr) const {
        for (const Module& m : m_modules) {
            if (addr >= m.base && addr < m.base + m.size) {
                const std::size_t slash = m.path.find_last_of(L"\\/");
                return narrow(slash == std::wstring::npos ? m.path : m.path.substr(slash + 1));
            }
        }
        return {};
    }

    HANDLE m_process;
    DWORD m_mainThread;
    const Options& m_options;
    std::vector<Module> m_modules;
    std::map<DWORD64, std::size_t> m_moduleBases;
    std::map<DWORD, ThreadState> m_threads;
    std::unordered_map<StackKey, std::uint64_t, StackHash> m_stacks;
    std::uint64_t m_onCpu = 0;
    std::uint64_t m_waiting = 0;
    std::uint64_t m_ticks = 0;
    double m_sampleUs = 0.0;
    double m_maxSampleUs = 0.0;
    bool m_symbols = false;
};

DWORD oldestThread(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD best = 0;
    ULONGLONG bestTime = ~0ull;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        FILETIME created{}, exited{}, kernel{}, user{};
        if (h != nullptr && GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            const ULONGLONG t = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (t < bestTime) {
                bestTime = t;
                best = te.th32ThreadID;
            }
        }
        if (h != nullptr) {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return best;
}

/// --catch: runs the program under the debugger API and prints the stack of every thread that faults (first chance,
/// before the program's own handlers see it: an access violation, heap corruption, a stack buffer overrun), then lets
/// the program handle it. For crashes that only happen deep into a long run, where no debugger is installed.
int catchCrashes(const std::wstring& commandLine, int maxReports) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "cannot start %s (error %lu)\n", narrow(commandLine).c_str(), GetLastError());
        return 1;
    }
    std::vector<Module> modules;
    gModules = &modules;
    std::map<DWORD, HANDLE> threads{{pi.dwThreadId, pi.hThread}};
    HANDLE process = pi.hProcess;
    bool symbols = false;
    int reports = 0;
    DWORD exitCode = 0;
    const auto refresh = [&] {
        HMODULE handles[1024];
        DWORD needed = 0;
        if (!EnumProcessModulesEx(process, handles, sizeof handles, &needed, LIST_MODULES_ALL)) {
            return;
        }
        for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
            const auto base = reinterpret_cast<DWORD64>(handles[i]);
            if (std::any_of(modules.begin(), modules.end(), [&](const Module& m) { return m.base == base; })) {
                continue;
            }
            MODULEINFO info{};
            wchar_t path[MAX_PATH * 2] = {};
            if (GetModuleInformation(process, handles[i], &info, sizeof info) &&
                GetModuleFileNameExW(process, handles[i], path, MAX_PATH * 2) != 0) {
                modules.push_back({path, base, info.SizeOfImage, readPdata(process, base)});
                if (symbols) {
                    SymLoadModuleExW(process, nullptr, path, nullptr, base, info.SizeOfImage, nullptr, 0);
                }
            }
        }
    };
    const auto nameOf = [&](DWORD64 addr) {
        alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string name;
        if (SymFromAddrW(process, addr, &displacement, sym)) {
            name = narrow(sym->Name);
            IMAGEHLP_LINEW64 line{};
            line.SizeOfStruct = sizeof line;
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddrW64(process, addr, &lineDisplacement, &line)) {
                name += "  (" + narrow(line.FileName) + ":" + std::to_string(line.LineNumber) + ")";
            }
        } else {
            char hex[32];
            std::snprintf(hex, sizeof hex, "0x%llx", static_cast<unsigned long long>(addr));
            name = hex;
        }
        if (const Module* m = moduleAt(addr)) {
            const std::size_t slash = m->path.find_last_of(L"\\/");
            name += " [" + narrow(slash == std::wstring::npos ? m->path : m->path.substr(slash + 1)) + "]";
        }
        return name;
    };
    DEBUG_EVENT ev{};
    while (WaitForDebugEvent(&ev, INFINITE)) {
        DWORD status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hFile) {
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) {
                CloseHandle(ev.u.LoadDll.hFile);
            }
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            threads.erase(ev.dwThreadId);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD& er = ev.u.Exception.ExceptionRecord;
            const DWORD code = er.ExceptionCode;
            if (code == EXCEPTION_BREAKPOINT || code == 0x4000001F /* WOW64 breakpoint */) {
                break; // the loader's initial breakpoint
            }
            status = DBG_EXCEPTION_NOT_HANDLED;
            const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000374 /* heap corruption */ ||
                               code == 0xC0000409 /* stack buffer overrun */ || code == EXCEPTION_ILLEGAL_INSTRUCTION;
            if (!fatal || reports >= maxReports) {
                break;
            }
            ++reports;
            refresh();
            if (!symbols) {
                SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
                symbols = SymInitializeW(process, nullptr, FALSE) != FALSE;
                for (const Module& m : modules) {
                    SymLoadModuleExW(process, nullptr, m.path.c_str(), nullptr, m.base, m.size, nullptr, 0);
                }
            }
            std::printf("\n=== %s 0x%08lx at %s (thread %lu)", ev.u.Exception.dwFirstChance ? "first chance" : "SECOND CHANCE",
                        code, nameOf(reinterpret_cast<DWORD64>(er.ExceptionAddress)).c_str(), ev.dwThreadId);
            if (code == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
                std::printf(": %s 0x%llx", er.ExceptionInformation[0] == 0 ? "read" : er.ExceptionInformation[0] == 1 ? "write" : "execute",
                            static_cast<unsigned long long>(er.ExceptionInformation[1]));
            }
            std::printf("\n");
            const auto it = threads.find(ev.dwThreadId);
            if (it != threads.end()) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(it->second, &ctx)) {
                    gStack.size = 0; // the thread is stopped: read its stack live
                    STACKFRAME64 frame{};
                    frame.AddrPC.Offset = ctx.Rip;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = ctx.Rbp;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = ctx.Rsp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    for (int depth = 0; depth < 64; ++depth) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, it->second, &frame, &ctx, readMemory,
                                         functionTableAccess, moduleBase, nullptr) ||
                            frame.AddrPC.Offset == 0) {
                            break;
                        }
                        std::printf("  %2d %s\n", depth, nameOf(frame.AddrPC.Offset).c_str());
                    }
                }
            }
            std::fflush(stdout);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exitCode = ev.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            if (symbols) {
                SymCleanup(process);
            }
            CloseHandle(pi.hThread);
            CloseHandle(process);
            gModules = nullptr;
            std::printf("\n=== exited with 0x%08lx after %d fault report(s)\n", exitCode, reports);
            return static_cast<int>(exitCode);
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    Options o;
    int i = 1;
    for (; i < argc; ++i) {
        const std::wstring a = argv[i];
        const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--hz") {
            o.hz = std::max(1, _wtoi(next().c_str()));
        } else if (a == L"--seconds") {
            o.seconds = _wtof(next().c_str());
        } else if (a == L"--threads") {
            o.allThreads = next() == L"all";
        } else if (a == L"--top") {
            o.top = std::max(1, _wtoi(next().c_str()));
        } else if (a == L"--folded") {
            o.folded = narrow(next());
        } else if (a == L"--report") {
            o.report = narrow(next());
        } else if (a == L"--kill") {
            o.kill = true;
        } else if (a == L"--catch") {
            o.catchFaults = true;
        } else if (a == L"--pid") {
            o.pid = static_cast<DWORD>(_wtoi(next().c_str()));
        } else if (a == L"--") {
            ++i;
            break;
        } else {
            return usage();
        }
    }
    for (int k = i; k < argc; ++k) {
        if (!o.commandLine.empty()) {
            o.commandLine += L' ';
        }
        o.commandLine += quoteArg(argv[k]);
    }
    if ((o.pid == 0) == o.commandLine.empty()) {
        return usage();
    }
    if (o.catchFaults) {
        return catchCrashes(o.commandLine, o.top);
    }
    HANDLE process = nullptr;
    HANDLE mainThreadHandle = nullptr;
    DWORD pid = o.pid;
    DWORD mainThread = 0;
    if (o.pid != 0) {
        process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) {
            std::fprintf(stderr, "cannot open process %lu (error %lu)\n", pid, GetLastError());
            return 1;
        }
        mainThread = oldestThread(pid);
    } else {
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = o.commandLine;
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "cannot start %s (error %lu)\n", narrow(o.commandLine).c_str(), GetLastError());
            return 1;
        }
        process = pi.hProcess;
        mainThreadHandle = pi.hThread;
        pid = pi.dwProcessId;
        mainThread = pi.dwThreadId;
    }
    timeBeginPeriod(1);
    int exitCode = 0;
    {
        Sampler sampler(process, mainThread, o);
        if (mainThreadHandle != nullptr) {
            ResumeThread(mainThreadHandle);
        }
        const auto start = std::chrono::steady_clock::now();
        const auto period = std::chrono::microseconds(1'000'000 / o.hz);
        auto nextTick = start;
        auto nextRefresh = start;
        while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            const auto now = std::chrono::steady_clock::now();
            if (o.seconds > 0.0 && std::chrono::duration<double>(now - start).count() >= o.seconds) {
                break;
            }
            if (now >= nextRefresh) { // new modules and threads, 4 times a second (a thread snapshot is slow)
                sampler.refreshModules();
                sampler.refreshThreads(pid);
                nextRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            }
            sampler.sampleOnce();
            nextTick += period;
            const auto wait = nextTick - std::chrono::steady_clock::now();
            if (wait > std::chrono::microseconds(0)) {
                Sleep(static_cast<DWORD>(std::max<long long>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(wait).count())));
            } else {
                nextTick = std::chrono::steady_clock::now();
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        sampler.refreshModules();
        if (o.kill && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 1);
        }
        sampler.writeReports(seconds);
        DWORD code = 0;
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code)) {
            exitCode = static_cast<int>(code);
        }
    }
    timeEndPeriod(1);
    if (mainThreadHandle != nullptr) {
        CloseHandle(mainThreadHandle);
    }
    CloseHandle(process);
    return exitCode;
}
