#pragma once

// The performance benchmark harness (P.1, docs/performance.md). A benchmark measures into a PerfResult (metrics, each
// a value with a unit and which way is better, optionally with its samples for p50 / p95); runBenchmark() runs it,
// compares the gated metrics with this machine's stored baseline (tests/perf/baselines/<machine id>.json) and the
// budgets (tests/perf/budgets.json), measures once more when a metric regressed by more than 20 % (a busy moment is
// not a regression), records the conditions of each attempt (CPU load, free memory, compilers running), and writes
// test-output/perf/<benchmark>.json.
//
//   * A machine without a baseline only records one: test-output/perf/baseline-candidate.json collects every
//     benchmark's values, and WABCUT_PERF_UPDATE_BASELINE=1 writes them into tests/perf/baselines/ directly.
//   * Budgets are product targets. They are reported (and listed in the JSON); WABCUT_PERF_ENFORCE_BUDGETS=1 makes an
//     exceeded budget fail too.
//   * Metric keys are "<benchmark>/<metric>", e.g. "project.io/save_ms".

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::perf {

using Json = nlohmann::json;

/// Measured values of one quantity (milliseconds, MB, frames per second, ...).
struct Samples {
    std::vector<double> values;

    void add(double v) { values.push_back(v); }
    bool empty() const { return values.empty(); }
    std::size_t size() const { return values.size(); }
    /// Nearest-rank percentile, q in [0, 100] (0 when empty).
    double percentile(double q) const;
    double p50() const { return percentile(50.0); }
    double p95() const { return percentile(95.0); }
    double min() const;
    double max() const;
    double mean() const;
};

enum class Better { Lower, Higher };

struct Metric {
    std::string name;  ///< "save_ms": snake case, the unit last
    std::string unit;  ///< "ms", "MB", "fps", "x realtime", ...
    double value = 0.0;
    Better better = Better::Lower;
    /// Compared with the baseline (> 20 % worse fails). Informational metrics (a count, a size that is not a cost)
    /// say false.
    bool gated = true;
    std::optional<Samples> samples; ///< for p50 / p95 / max in the JSON
    /// Absolute slack on top of the 20 %: a regression must also exceed the baseline by this much, so a 0.05 ms edit
    /// or a 0 MB leak is not "40 % worse" over noise. Negative: by unit (0.5 for ms, 5 for MB, else 0).
    double slack = -1.0;
};

struct PerfResult {
    std::vector<Metric> metrics;
    Json details = Json::object(); ///< anything else worth keeping (configuration, sizes, notes)

    /// A gated metric.
    Metric& add(std::string name, std::string unit, double value, Better better = Better::Lower);
    /// A gated metric from samples: its value is the p50 (or `stat` = "p95", "max", "mean", "min").
    Metric& addSamples(std::string name, std::string unit, const Samples& samples, const std::string& stat = "p50",
                       Better better = Better::Lower);
    /// Informational: recorded, never compared.
    Metric& note(std::string name, std::string unit, double value);
};

struct BenchmarkOutcome {
    std::string benchmark;
    bool baselineFound = false;
    int attempts = 1;
    std::vector<std::string> regressions; ///< "project.io/save_ms: 412 ms vs baseline 300 ms (+37 %)"
    std::vector<std::string> overBudget;  ///< "playback/first_frame_warm_ms: 180 ms over the 150 ms budget"
    PerfResult result;                    ///< the attempt that was kept
    /// Whether the test should pass: no regression, and no exceeded budget when budgets are enforced.
    bool ok() const;
    /// A one-line summary per problem, for FAIL / WARN messages.
    std::string describe() const;
};

/// Runs `measure` (and once more after a regression, keeping each metric's better attempt), writes the JSON and
/// returns the comparison. `measure` may REQUIRE / CHECK as usual.
BenchmarkOutcome runBenchmark(const std::string& benchmark, const std::function<PerfResult()>& measure);

/// test-output/perf (created).
std::string outputDir();

/// The machine's identity and description (the baseline's key).
struct MachineInfo {
    std::string id;  ///< 16 hex digits: CPU, logical cores, memory size, GPU
    std::string cpu;
    int logicalCores = 0;
    double memoryGb = 0.0;
    std::string gpu;
    std::string os;
};
const MachineInfo& machine();

/// What else ran while measuring: sampled over `ms` milliseconds.
struct Conditions {
    double cpuBusyPercent = 0.0; ///< the whole machine's
    double freeMemoryGb = 0.0;
    int compilers = 0;           ///< cl, link, clang-cl, ninja, cmake, msbuild processes
    int otherTests = 0;          ///< other wabcut_*_tests processes
    bool quiet() const { return cpuBusyPercent < 25.0 && compilers == 0 && otherTests == 0; }
};
Conditions sampleConditions(int ms = 500);
Json toJson(const Conditions& conditions);

// ---- Process measurements ----------------------------------------------------------------------------------------

double workingSetMb();
double privateMb();
double peakWorkingSetMb();
/// This process's GPU memory (DXGI local + non-local, all adapters), MB; -1 when unknown.
double gpuMemoryMb();
/// CPU time this process has used so far (user + kernel, all threads), milliseconds. A difference over an interval
/// divided by its wall seconds is "ms per second": 1000 is one core busy.
double processCpuMs();

/// Milliseconds since `start` (steady clock).
double msSince(std::chrono::steady_clock::time_point start);

} // namespace wabcut::perf
