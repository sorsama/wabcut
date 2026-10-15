// The portable fuzz driver (tests/fuzz/README.md): a main() for the harnesses when libFuzzer is not used (every build
// without WABCUT_FUZZ). It runs each input given (files, or every file in a folder, in name order), then `-runs`
// deterministic mutations of them, and records what breaks the way libFuzzer does: a crash or a broken invariant
// (abort), an input slower than `-timeout` seconds (a hang) and a working set above `-rss_limit_mb` each write the input
// to <artifact_prefix>crash-|timeout-|oom-<hash> and fail the run. The flags are libFuzzer's, so ctest passes the same
// command line to both drivers. No coverage feedback: the libFuzzer build (win-msvc-asan) does the exploring, this
// keeps the seeds and the found reproducers passing in every build.

#include "FuzzSupport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options {
    std::uint64_t runs = 0;
    std::uint64_t seed = 1;
    std::size_t maxLen = 4096;
    double timeoutSeconds = 10.0;
    std::uint64_t rssLimitMb = 2048;
    double maxTotalSeconds = 0.0; // 0: no limit
    std::string artifactPrefix = "./";
    std::vector<fs::path> inputs;
};

// The input being run, for the crash handlers and the watchdog.
std::vector<std::uint8_t> g_current;
std::atomic<std::int64_t> g_startedMs{-1};
std::string g_artifactPrefix = "./";

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string hashHex(const std::vector<std::uint8_t>& data) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    for (const std::uint8_t b : data) {
        h = (h ^ b) * 1099511628211ull;
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
    return text;
}

void writeArtifact(const char* kind) {
    const std::string path = g_artifactPrefix + kind + "-" + hashHex(g_current);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(g_current.data(), 1, g_current.size(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "==FUZZ== %s: input of %zu bytes written to %s\n", kind, g_current.size(), path.c_str());
    std::fflush(stderr);
}

void onAbort(int) {
    writeArtifact("crash");
    std::_Exit(77);
}

#ifdef _WIN32
LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "==FUZZ== exception 0x%08lx\n", info ? info->ExceptionRecord->ExceptionCode : 0ul);
    writeArtifact("crash");
    std::_Exit(77);
}
#endif

std::uint64_t peakRssMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        return counters.PeakWorkingSetSize / (1024 * 1024);
    }
    return 0;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss) / (1024 * 1024);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024;
#endif
#endif
}

bool parseFlag(const std::string& arg, const char* name, std::string& value) {
    const std::string prefix = std::string("-") + name + "=";
    if (arg.rfind(prefix, 0) != 0) {
        return false;
    }
    value = arg.substr(prefix.size());
    return true;
}

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string v;
        if (parseFlag(arg, "runs", v)) {
            o.runs = std::stoull(v);
        } else if (parseFlag(arg, "seed", v)) {
            o.seed = std::stoull(v);
        } else if (parseFlag(arg, "max_len", v)) {
            o.maxLen = std::stoull(v);
        } else if (parseFlag(arg, "timeout", v)) {
            o.timeoutSeconds = std::stod(v);
        } else if (parseFlag(arg, "rss_limit_mb", v)) {
            o.rssLimitMb = std::stoull(v);
        } else if (parseFlag(arg, "max_total_time", v)) {
            o.maxTotalSeconds = std::stod(v);
        } else if (parseFlag(arg, "artifact_prefix", v)) {
            o.artifactPrefix = v;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "FuzzMain: ignoring %s (a libFuzzer flag this driver does not use)\n", arg.c_str());
        } else {
            o.inputs.emplace_back(arg);
        }
    }
    return o;
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::vector<std::uint8_t>> loadInputs(const std::vector<fs::path>& paths) {
    std::vector<std::vector<std::uint8_t>> out;
    for (const fs::path& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::directory_iterator(p, ec)) {
                if (entry.is_regular_file(ec)) {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) {
                out.push_back(readFile(f));
            }
        } else if (fs::is_regular_file(p, ec)) {
            out.push_back(readFile(p));
        } else {
            std::fprintf(stderr, "FuzzMain: no such input %s\n", p.string().c_str());
        }
    }
    return out;
}

// --- Mutations (deterministic for a seed) ----------------------------------------------------------------------------

constexpr std::array<std::string_view, 34> kTokens{
    "null", "true", "false", "-1", "0", "1e308", "-1e308", "9223372036854775807", "-9223372036854775808",
    "18446744073709551616", "4294967296", "\"\"", "[]", "{}", "[[[[[[[[", "{\"a\":", "\\u0000", "\\ud800", "NaN",
    "<", ">", "</", "/>", "<![CDATA[", "]]>", "&amp;", "&#0;", "&#x110000;", "<?xml version=\"1.0\"?>", "\r\n",
    "\n\n", "data: ", "00:00:00:00", "TITLE"};
constexpr std::array<std::string_view, 12> kNumbers{
    "0", "-1", "1", "2147483647", "-2147483648", "4294967295", "9223372036854775807", "-9223372036854775808",
    "18446744073709551615", "1e999", "0.5", "65536"};

class Mutator {
public:
    explicit Mutator(std::uint64_t seed) : m_rng(seed) {}

    std::vector<std::uint8_t> mutate(const std::vector<std::vector<std::uint8_t>>& corpus, std::size_t maxLen) {
        std::vector<std::uint8_t> out = corpus[pick(corpus.size())];
        const std::size_t limit = std::max(maxLen, out.size());
        const int steps = 1 + static_cast<int>(pick(4));
        for (int s = 0; s < steps; ++s) {
            step(out, corpus);
        }
        if (out.size() > limit) {
            out.resize(limit);
        }
        return out;
    }

private:
    std::size_t pick(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(m_rng() % n); }

    void insert(std::vector<std::uint8_t>& out, std::string_view bytes) {
        const std::size_t at = pick(out.size() + 1);
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin(), bytes.end());
    }

    void step(std::vector<std::uint8_t>& out, const std::vector<std::vector<std::uint8_t>>& corpus) {
        switch (pick(9)) {
        case 0: // flip a bit
            if (!out.empty()) {
                out[pick(out.size())] ^= static_cast<std::uint8_t>(1u << pick(8));
            }
            break;
        case 1: { // an interesting byte
            static constexpr std::array<std::uint8_t, 16> kBytes{0, 0xFF, 0x7F, 0x80, '"', '<', '>', '{', '}', '[', ']',
                                                                 ',', ':', '\\', '\n', '-'};
            if (!out.empty()) {
                out[pick(out.size())] = kBytes[pick(kBytes.size())];
            }
            break;
        }
        case 2: { // random bytes
            std::string bytes(1 + pick(8), '\0');
            for (char& c : bytes) {
                c = static_cast<char>(m_rng() & 0xFF);
            }
            insert(out, bytes);
            break;
        }
        case 3: // delete a range
            if (!out.empty()) {
                const std::size_t at = pick(out.size());
                const std::size_t n = std::min(out.size() - at, 1 + pick(16));
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            break;
        case 4: // duplicate a range
            if (!out.empty()) {
                const std::size_t at = pick(out.size());
                const std::size_t n = std::min(out.size() - at, 1 + pick(64));
                const std::string copy(out.begin() + static_cast<std::ptrdiff_t>(at),
                                       out.begin() + static_cast<std::ptrdiff_t>(at + n));
                insert(out, copy);
            }
            break;
        case 5: { // splice a range of another input
            const std::vector<std::uint8_t>& other = corpus[pick(corpus.size())];
            if (!other.empty()) {
                const std::size_t at = pick(other.size());
                const std::size_t n = std::min(other.size() - at, 1 + pick(128));
                insert(out, std::string(other.begin() + static_cast<std::ptrdiff_t>(at),
                                        other.begin() + static_cast<std::ptrdiff_t>(at + n)));
            }
            break;
        }
        case 6: // a token
            insert(out, kTokens[pick(kTokens.size())]);
            break;
        case 7: { // replace a number with an extreme one
            std::vector<std::size_t> digits;
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (out[i] >= '0' && out[i] <= '9' && (i == 0 || out[i - 1] < '0' || out[i - 1] > '9')) {
                    digits.push_back(i);
                }
            }
            if (!digits.empty()) {
                const std::size_t at = digits[pick(digits.size())];
                std::size_t end = at;
                while (end < out.size() && out[end] >= '0' && out[end] <= '9') {
                    ++end;
                }
                const std::string_view n = kNumbers[pick(kNumbers.size())];
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(end));
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), n.begin(), n.end());
            }
            break;
        }
        default: // truncate
            if (!out.empty()) {
                out.resize(pick(out.size()));
            }
            break;
        }
    }

    std::mt19937_64 m_rng;
};

void runOne(const std::vector<std::uint8_t>& input, const Options& options) {
    g_current = input;
    g_startedMs.store(nowMs());
    LLVMFuzzerTestOneInput(input.data(), input.size());
    g_startedMs.store(-1);
    if (peakRssMb() > options.rssLimitMb) {
        std::fprintf(stderr, "==FUZZ== working set %llu MB is over -rss_limit_mb=%llu\n",
                     static_cast<unsigned long long>(peakRssMb()), static_cast<unsigned long long>(options.rssLimitMb));
        writeArtifact("oom");
        std::_Exit(78);
    }
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    g_artifactPrefix = options.artifactPrefix;
    std::signal(SIGABRT, onAbort);
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        const auto limitMs = static_cast<std::int64_t>(options.timeoutSeconds * 1000.0);
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::int64_t started = g_startedMs.load();
            if (started >= 0 && nowMs() - started > limitMs) {
                std::fprintf(stderr, "==FUZZ== an input ran longer than -timeout=%g s\n", options.timeoutSeconds);
                writeArtifact("timeout");
                std::_Exit(70);
            }
        }
    });

    const auto start = Clock::now();
    std::vector<std::vector<std::uint8_t>> corpus = loadInputs(options.inputs);
    for (const auto& input : corpus) {
        runOne(input, options);
    }
    const std::size_t seeds = corpus.size();
    if (corpus.empty()) {
        corpus.emplace_back(); // mutate from nothing
    }
    Mutator mutator(options.seed);
    std::uint64_t mutations = 0;
    for (; mutations < options.runs; ++mutations) {
        if (options.maxTotalSeconds > 0 &&
            std::chrono::duration<double>(Clock::now() - start).count() > options.maxTotalSeconds) {
            break;
        }
        runOne(mutator.mutate(corpus, options.maxLen), options);
    }
    done.store(true);
    watchdog.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("#%llu DONE inputs: %zu mutations: %llu time: %.1f s exec/s: %.0f peak_rss: %llu MB\n",
                static_cast<unsigned long long>(seeds + mutations), seeds, static_cast<unsigned long long>(mutations),
                seconds, seconds > 0 ? static_cast<double>(seeds + mutations) / seconds : 0.0,
                static_cast<unsigned long long>(peakRssMb()));
    return 0;
}
