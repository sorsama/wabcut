#include "PerfHarness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace wabcut::perf {

std::string gpuAdapterName(); // PerfGpu.cpp
double gpuProcessMemoryMb();  // PerfGpu.cpp

namespace {

constexpr double kRegression = 1.2; // a gated metric may cost up to 20 % more than its baseline

std::string envString(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

bool envFlag(const char* name) {
    const std::string v = envString(name);
    return !v.empty() && v != "0";
}

Json readJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return Json();
    }
    std::stringstream text;
    text << in.rdbuf();
    Json j = Json::parse(text.str(), nullptr, false);
    return j.is_discarded() ? Json() : j;
}

void writeJson(const std::filesystem::path& file, const Json& j) {
    std::filesystem::create_directories(file.parent_path());
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << '\n';
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::copy_file(tmp, file, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::filesystem::path baselineFile() {
    return std::filesystem::path(WABCUT_PERF_BASELINE_DIR) / (machine().id + ".json");
}

/// "<benchmark>/<metric>" -> {"value", "better"} of this machine's baseline.
const Json& baseline() {
    static const Json j = [] {
        Json b = readJson(baselineFile());
        return b.is_object() && b.contains("metrics") ? b["metrics"] : Json::object();
    }();
    return j;
}

const Json& budgets() {
    static const Json j = [] {
        Json b = readJson(WABCUT_PERF_BUDGETS_FILE);
        return b.is_object() && b.contains("budgets") ? b["budgets"] : Json::object();
    }();
    return j;
}

/// How much more a value costs than `reference` (1.0: the same; 1.3: 30 % worse).
double costRatio(double value, double reference, Better better) {
    if (better == Better::Lower) {
        return reference > 0.0 ? value / reference : (value > 0.0 ? 1e9 : 1.0);
    }
    return value > 0.0 ? reference / value : (reference > 0.0 ? 1e9 : 1.0);
}

double slackOf(const Metric& m) {
    if (m.slack >= 0.0) {
        return m.slack;
    }
    if (m.unit == "ms") {
        return 0.5;
    }
    if (m.unit == "MB") {
        return 5.0;
    }
    return 0.0;
}

/// Whether `m` regressed against `reference`: more than 20 % worse and by more than its slack.
bool regressed(const Metric& m, double reference) {
    const double slack = slackOf(m);
    if (m.better == Better::Lower) {
        return m.value > std::max(reference * kRegression, reference + slack);
    }
    return m.value < std::min(reference / kRegression, reference - slack);
}

std::string formatValue(double v, const std::string& unit) {
    char buf[64];
    if (std::fabs(v) >= 100.0) {
        std::snprintf(buf, sizeof buf, "%.0f %s", v, unit.c_str());
    } else if (std::fabs(v) >= 10.0) {
        std::snprintf(buf, sizeof buf, "%.1f %s", v, unit.c_str());
    } else {
        std::snprintf(buf, sizeof buf, "%.2f %s", v, unit.c_str());
    }
    return buf;
}

std::vector<std::string> regressionsOf(const std::string& benchmark, const PerfResult& r) {
    std::vector<std::string> out;
    for (const Metric& m : r.metrics) {
        const std::string key = benchmark + "/" + m.name;
        if (!m.gated || !baseline().contains(key)) {
            continue;
        }
        const double ref = baseline()[key].value("value", 0.0);
        const double ratio = costRatio(m.value, ref, m.better);
        if (regressed(m, ref)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " (+%.0f %%)", (ratio - 1.0) * 100.0);
            out.push_back(key + ": " + formatValue(m.value, m.unit) + " vs baseline " + formatValue(ref, m.unit) + buf);
        }
    }
    return out;
}

std::vector<std::string> overBudgetOf(const std::string& benchmark, const PerfResult& r) {
    std::vector<std::string> out;
    for (const Metric& m : r.metrics) {
        const std::string key = benchmark + "/" + m.name;
        if (!budgets().contains(key)) {
            continue;
        }
        const Json& b = budgets()[key];
        if (b.contains("max") && m.value > b["max"].get<double>()) {
            out.push_back(key + ": " + formatValue(m.value, m.unit) + " over the " +
                          formatValue(b["max"].get<double>(), m.unit) + " budget");
        }
        if (b.contains("min") && m.value < b["min"].get<double>()) {
            out.push_back(key + ": " + formatValue(m.value, m.unit) + " under the " +
                          formatValue(b["min"].get<double>(), m.unit) + " budget");
        }
    }
    return out;
}

/// Each metric's better value of two attempts (the samples of the attempt it came from).
PerfResult better(const PerfResult& a, const PerfResult& b) {
    PerfResult out = a;
    for (Metric& m : out.metrics) {
        const auto it = std::find_if(b.metrics.begin(), b.metrics.end(), [&](const Metric& o) { return o.name == m.name; });
        if (it != b.metrics.end() && costRatio(it->value, m.value, m.better) < 1.0) {
            m = *it;
        }
    }
    out.details["secondAttempt"] = b.details;
    return out;
}

Json metricJson(const std::string& benchmark, const Metric& m) {
    const std::string key = benchmark + "/" + m.name;
    Json j{{"value", m.value}, {"unit", m.unit}, {"better", m.better == Better::Lower ? "lower" : "higher"},
           {"gated", m.gated}};
    if (m.samples && !m.samples->empty()) {
        j["samples"] = Json{{"n", m.samples->size()},   {"p50", m.samples->p50()}, {"p95", m.samples->p95()},
                            {"min", m.samples->min()},  {"max", m.samples->max()}, {"mean", m.samples->mean()}};
    }
    if (baseline().contains(key)) {
        const double ref = baseline()[key].value("value", 0.0);
        j["baseline"] = ref;
        j["costRatio"] = costRatio(m.value, ref, m.better);
    }
    if (budgets().contains(key)) {
        j["budget"] = budgets()[key];
    }
    return j;
}

std::mutex gFileMutex;

void recordCandidate(const std::string& benchmark, const PerfResult& r) {
    std::lock_guard lock(gFileMutex);
    const MachineInfo& mi = machine();
    const Json machineJson{{"id", mi.id}, {"cpu", mi.cpu}, {"logicalCores", mi.logicalCores},
                           {"memoryGb", mi.memoryGb}, {"gpu", mi.gpu}, {"os", mi.os}};
    const auto merge = [&](const std::filesystem::path& file) {
        Json j = readJson(file);
        if (!j.is_object()) {
            j = Json::object();
        }
        j["machine"] = machineJson;
        j["recorded"] = utcNow();
        if (!j.contains("metrics") || !j["metrics"].is_object()) {
            j["metrics"] = Json::object();
        }
        for (const Metric& m : r.metrics) {
            if (m.gated) {
                j["metrics"][benchmark + "/" + m.name] =
                    Json{{"value", m.value}, {"unit", m.unit}, {"better", m.better == Better::Lower ? "lower" : "higher"}};
            }
        }
        writeJson(file, j);
    };
    merge(std::filesystem::path(outputDir()) / "baseline-candidate.json");
    if (envFlag("WABCUT_PERF_UPDATE_BASELINE")) {
        merge(baselineFile());
    }
}

} // namespace

// ---- Samples ------------------------------------------------------------------------------------------------------

double Samples::percentile(double q) const {
    if (values.empty()) {
        return 0.0;
    }
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const double rank = std::ceil(std::clamp(q, 0.0, 100.0) / 100.0 * static_cast<double>(sorted.size()));
    const auto index = static_cast<std::size_t>(std::clamp(rank, 1.0, static_cast<double>(sorted.size()))) - 1;
    return sorted[index];
}

double Samples::min() const { return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end()); }
double Samples::max() const { return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end()); }

double Samples::mean() const {
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const double v : values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

// ---- PerfResult ---------------------------------------------------------------------------------------------------

Metric& PerfResult::add(std::string name, std::string unit, double value, Better better) {
    metrics.push_back(Metric{std::move(name), std::move(unit), value, better, true, std::nullopt});
    return metrics.back();
}

Metric& PerfResult::addSamples(std::string name, std::string unit, const Samples& samples, const std::string& stat,
                               Better better) {
    double value = samples.p50();
    if (stat == "p95") {
        value = samples.p95();
    } else if (stat == "max") {
        value = samples.max();
    } else if (stat == "mean") {
        value = samples.mean();
    } else if (stat == "min") {
        value = samples.min();
    }
    metrics.push_back(Metric{std::move(name), std::move(unit), value, better, true, samples});
    return metrics.back();
}

Metric& PerfResult::note(std::string name, std::string unit, double value) {
    metrics.push_back(Metric{std::move(name), std::move(unit), value, Better::Lower, false, std::nullopt});
    return metrics.back();
}

// ---- Outcome ------------------------------------------------------------------------------------------------------

bool BenchmarkOutcome::ok() const {
    return regressions.empty() && (overBudget.empty() || !envFlag("WABCUT_PERF_ENFORCE_BUDGETS"));
}

std::string BenchmarkOutcome::describe() const {
    std::string s;
    for (const std::string& r : regressions) {
        s += "regression: " + r + "\n";
    }
    for (const std::string& b : overBudget) {
        s += "over budget: " + b + "\n";
    }
    if (!baselineFound) {
        s += "no baseline for machine " + machine().id + " (" + machine().cpu + ", " + machine().gpu +
             "): recorded in test-output/perf/baseline-candidate.json\n";
    }
    return s;
}

BenchmarkOutcome runBenchmark(const std::string& benchmark, const std::function<PerfResult()>& measure) {
    BenchmarkOutcome outcome;
    outcome.benchmark = benchmark;
    Json attempts = Json::array();
    const auto attempt = [&]() {
        const Conditions before = sampleConditions(300);
        const auto t0 = std::chrono::steady_clock::now();
        PerfResult r = measure();
        attempts.push_back(Json{{"before", toJson(before)}, {"seconds", msSince(t0) / 1000.0}});
        return r;
    };
    PerfResult result = attempt();
    outcome.regressions = regressionsOf(benchmark, result);
    if (!outcome.regressions.empty()) { // once more: a busy moment is not a regression
        result = better(result, attempt());
        outcome.attempts = 2;
        outcome.regressions = regressionsOf(benchmark, result);
    }
    outcome.overBudget = overBudgetOf(benchmark, result);
    outcome.baselineFound = std::any_of(result.metrics.begin(), result.metrics.end(), [&](const Metric& m) {
        return baseline().contains(benchmark + "/" + m.name);
    });
    outcome.result = result;

    const MachineInfo& mi = machine();
    Json metrics = Json::object();
    for (const Metric& m : result.metrics) {
        metrics[m.name] = metricJson(benchmark, m);
    }
    const Json report{
        {"benchmark", benchmark},
        {"format", "wabcut-perf"},
        {"version", 1},
        {"time", utcNow()},
        {"machine", Json{{"id", mi.id}, {"cpu", mi.cpu}, {"logicalCores", mi.logicalCores}, {"memoryGb", mi.memoryGb},
                         {"gpu", mi.gpu}, {"os", mi.os}}},
#ifdef NDEBUG
        {"build", "release"},
#else
        {"build", "debug"},
#endif
        {"attempts", attempts},
        {"metrics", metrics},
        {"details", result.details},
        {"baselineFound", outcome.baselineFound},
        {"regressions", outcome.regressions},
        {"overBudget", outcome.overBudget},
    };
    {
        std::lock_guard lock(gFileMutex);
        writeJson(std::filesystem::path(outputDir()) / (benchmark + ".json"), report);
    }
    recordCandidate(benchmark, result);

    std::printf("== %s (%d attempt%s, machine %s) ==\n", benchmark.c_str(), outcome.attempts,
                outcome.attempts == 1 ? "" : "s", mi.id.c_str());
    for (const Metric& m : result.metrics) {
        std::string line = "  " + m.name + ": " + formatValue(m.value, m.unit);
        if (m.samples && !m.samples->empty()) {
            line += " (p50 " + formatValue(m.samples->p50(), m.unit) + ", p95 " + formatValue(m.samples->p95(), m.unit) +
                    ", n " + std::to_string(m.samples->size()) + ")";
        }
        const std::string key = benchmark + "/" + m.name;
        if (m.gated && baseline().contains(key)) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "; baseline x%.2f",
                          costRatio(m.value, baseline()[key].value("value", 0.0), m.better));
            line += buf;
        }
        if (!m.gated) {
            line += " (info)";
        }
        std::printf("%s\n", line.c_str());
    }
    for (const std::string& s : outcome.overBudget) {
        std::printf("  over budget: %s\n", s.c_str());
    }
    std::fflush(stdout);
    return outcome;
}

std::string outputDir() {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "perf";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---- The machine and its conditions --------------------------------------------------------------------------------

const MachineInfo& machine() {
    static const MachineInfo info = [] {
        MachineInfo m;
        m.logicalCores = static_cast<int>(std::thread::hardware_concurrency());
#ifdef _WIN32
        int regs[4] = {};
        char brand[49] = {};
        __cpuid(regs, 0x80000000);
        if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
            for (int i = 0; i < 3; ++i) {
                __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
                std::memcpy(brand + 16 * i, regs, 16);
            }
        }
        m.cpu = brand;
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof mem;
        if (GlobalMemoryStatusEx(&mem)) {
            m.memoryGb = std::round(static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
        }
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        if (auto* fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
            OSVERSIONINFOW v{};
            v.dwOSVersionInfoSize = sizeof v;
            if (fn(&v) == 0) {
                m.os = "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
                       std::to_string(v.dwBuildNumber);
            }
        }
#else
        m.os = "unix";
#endif
        // Trim the brand's padding.
        while (!m.cpu.empty() && (m.cpu.back() == ' ' || m.cpu.back() == '\0')) {
            m.cpu.pop_back();
        }
        while (!m.cpu.empty() && m.cpu.front() == ' ') {
            m.cpu.erase(m.cpu.begin());
        }
        if (m.cpu.empty()) {
            m.cpu = "unknown CPU";
        }
        m.gpu = gpuAdapterName();
        const std::string key = m.cpu + "|" + std::to_string(m.logicalCores) + "|" + std::to_string(m.memoryGb) + "|" + m.gpu;
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const unsigned char c : key) {
            h = (h ^ c) * 0x100000001b3ull;
        }
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        m.id = hex;
        return m;
    }();
    return info;
}

Conditions sampleConditions(int ms) {
    Conditions c;
#ifdef _WIN32
    FILETIME idle0, kernel0, user0, idle1, kernel1, user1;
    GetSystemTimes(&idle0, &kernel0, &user0);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    GetSystemTimes(&idle1, &kernel1, &user1);
    const auto ticks = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    const double idle = static_cast<double>(ticks(idle1) - ticks(idle0));
    const double total = static_cast<double>((ticks(kernel1) - ticks(kernel0)) + (ticks(user1) - ticks(user0)));
    c.cpuBusyPercent = total > 0.0 ? 100.0 * (1.0 - idle / total) : 0.0;
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) {
        c.freeMemoryGb = static_cast<double>(mem.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        const DWORD self = GetCurrentProcessId();
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            std::wstring name = pe.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            static const wchar_t* const builders[] = {L"cl.exe",    L"link.exe",    L"clang-cl.exe", L"lld-link.exe",
                                                      L"ninja.exe", L"cmake.exe",   L"msbuild.exe",  L"qmlcachegen.exe",
                                                      L"moc.exe",   L"rcc.exe",     L"cmcldeps.exe"};
            for (const wchar_t* b : builders) {
                if (name == b) {
                    ++c.compilers;
                }
            }
            if (pe.th32ProcessID != self && name.rfind(L"wabcut_", 0) == 0 && name.find(L"_tests.exe") != std::wstring::npos) {
                ++c.otherTests;
            }
        }
        CloseHandle(snap);
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
    return c;
}

Json toJson(const Conditions& c) {
    return Json{{"cpuBusyPercent", c.cpuBusyPercent}, {"freeMemoryGb", c.freeMemoryGb}, {"compilers", c.compilers},
                {"otherTests", c.otherTests},         {"quiet", c.quiet()}};
}

// ---- Process measurements ------------------------------------------------------------------------------------------

#ifdef _WIN32
namespace {
PROCESS_MEMORY_COUNTERS_EX processCounters() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return pmc;
}
constexpr double kMb = 1024.0 * 1024.0;
} // namespace

double workingSetMb() { return static_cast<double>(processCounters().WorkingSetSize) / kMb; }
double privateMb() { return static_cast<double>(processCounters().PrivateUsage) / kMb; }
double peakWorkingSetMb() { return static_cast<double>(processCounters().PeakWorkingSetSize) / kMb; }

double processCpuMs() {
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user)) {
        return 0.0;
    }
    const auto ticks = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000.0; // 100 ns units
}
#else
double workingSetMb() {
    std::ifstream statm("/proc/self/statm");
    long pages = 0, resident = 0;
    statm >> pages >> resident;
    return static_cast<double>(resident) * 4096.0 / (1024.0 * 1024.0);
}
double privateMb() { return workingSetMb(); }
double peakWorkingSetMb() { return workingSetMb(); }
double processCpuMs() { return static_cast<double>(std::clock()) * 1000.0 / static_cast<double>(CLOCKS_PER_SEC); }
#endif

double gpuMemoryMb() { return gpuProcessMemoryMb(); }

} // namespace wabcut::perf
