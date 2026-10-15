// C.10 scale test ([perf]: `ctest -L perf` on a quiet machine; left out of the quick run): the synthetic large
// project of ScaleProject.h - 10,000 clips in 50 sequences, 200 media, a chain of 8 nested sequences, 2 multicam
// sources, 5,000 markers, 1,000 review notes, effects, keyframes and transitions - through save and load, 1,000 undo
// steps, diff, a three-way merge and the version history, with the times and the memory printed (and written to
// test-output/scale/scale.md). The limits are generous ceilings; the printed numbers are the result.

#include "ScaleProject.h"
#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/History.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectDiff.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

/// The process's working set and its peak, in MB (0 where the platform does not tell).
std::pair<double, double> memoryMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        return {static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0),
                static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0)};
    }
#endif
    return {0.0, 0.0};
}

struct Report {
    std::vector<std::string> lines;
    void add(const char* format, auto... values) {
        char text[256];
        std::snprintf(text, sizeof text, format, values...);
        std::printf("%s\n", text);
        lines.emplace_back(text);
    }
    void write(const fs::path& file) const {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::trunc);
        out << "# C.10 scale test\n\n";
        for (const std::string& line : lines) {
            out << "- " << line << "\n";
        }
    }
};

/// A tail trim of one frame on a random V1 clip of a random sequence among [first, last).
edit::EditOutcome randomTrim(const ProjectState& state, const std::vector<SequenceId>& sequences, std::size_t first,
                             std::size_t last, std::mt19937_64& rng) {
    const SequenceId seq = sequences[first + rng() % (last - first)];
    const Track& v1 = *state.findSequence(seq)->videoTracks[0];
    const Clip& clip = v1.clips[rng() % v1.clips.size()];
    return edit::trimEdge(state, seq, clip.id, edit::Edge::Tail, -f(1));
}

} // namespace

TEST_CASE("Scale: a 10,000-clip project saves, loads, undoes 1,000 steps, diffs, merges and checkpoints",
          "[model][perf][scale]") {
    Report report;
    const auto memoryAtStart = memoryMb();
    auto start = Clock::now();
    const ScaleProject big = makeScaleProject(largeScaleSpec());
    const double buildMs = msSince(start);
    const ProjectState& initial = big.state;
    std::size_t markers = 0;
    for (const SequencePtr& s : initial.sequences) {
        markers += s->markers.size();
    }
    REQUIRE(big.clips >= 10'000);
    REQUIRE(big.sequences.size() == 50);
    REQUIRE(initial.media.size() == 200);
    REQUIRE(markers == 5'000);
    REQUIRE(initial.reviewNotes.size() == 1'000);
    report.add("project: %zu clips, %zu sequences (%zu multicam sources), %zu media, %zu markers, %zu review notes, "
               "nesting depth 8; built in %.0f ms",
               big.clips, initial.sequences.size(), big.multicams.size(), initial.media.size(), markers,
               initial.reviewNotes.size(), buildMs);
    report.add("memory after building: %.0f MB working set (+%.0f MB)", memoryMb().first,
               memoryMb().first - memoryAtStart.first);

    // Save and load, in memory and through a file.
    start = Clock::now();
    const auto json = io::toJson(initial);
    const double toJsonMs = msSince(start);
    REQUIRE(json);
    start = Clock::now();
    const auto loaded = io::fromJson(*json);
    const double fromJsonMs = msSince(start);
    REQUIRE(loaded);
    CHECK(*loaded == initial);
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "scale";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "large.wabcut";
    start = Clock::now();
    REQUIRE(io::saveProject(initial, file, io::SaveOptions{0}));
    const double saveMs = msSince(start);
    start = Clock::now();
    const auto reloaded = io::loadProject(file);
    const double loadMs = msSince(start);
    REQUIRE(reloaded);
    CHECK(*reloaded == initial);
    report.add("save: %.0f ms to JSON (%.1f MB), %.0f ms to a file; load: %.0f ms from JSON, %.0f ms from the file",
               toJsonMs, static_cast<double>(json->size()) / (1024.0 * 1024.0), saveMs, fromJsonMs, loadMs);
    CHECK(saveMs < 5000.0);
    CHECK(loadMs < 5000.0);

    // 1,000 undo steps, all the way back and forward again.
    std::mt19937_64 rng(0xC10);
    EditSessionOptions options;
    options.undoLimit = 1'000;
    options.undoMemoryBudget = std::size_t{2} * 1024 * 1024 * 1024;
    options.validateEdits = false;
    EditSession session(initial, options);
    const auto memoryBeforeEdits = memoryMb();
    start = Clock::now();
    int edits = 0;
    while (edits < 1'000) {
        if (session.apply("Trim", [&](const ProjectState& s) { return randomTrim(s, big.sequences, 0, 50, rng); })) {
            ++edits;
        }
    }
    const double editMs = msSince(start);
    const ProjectSnapshot edited = session.snapshot();
    REQUIRE(session.undoCount() == 1'000);
    const double undoKeptMb = static_cast<double>(session.undoMemoryBytes()) / (1024.0 * 1024.0);
    const double editMemoryMb = memoryMb().first - memoryBeforeEdits.first;
    start = Clock::now();
    while (session.canUndo()) {
        session.undo();
    }
    const double undoMs = msSince(start);
    CHECK(*session.snapshot() == initial);
    start = Clock::now();
    while (session.canRedo()) {
        session.redo();
    }
    const double redoMs = msSince(start);
    CHECK(*session.snapshot() == *edited);
    CHECK(validate(*edited, Validation::Strict).has_value());
    report.add("1,000 edits: %.0f ms (%.2f ms each), undo history %.1f MB by the session's estimate, +%.0f MB working set; "
               "undo all %.0f ms, redo all %.0f ms; undo-all gives the initial project exactly",
               editMs, editMs / 1000.0, undoKeptMb, editMemoryMb, undoMs, redoMs);
    CHECK(undoMs < 5000.0);

    // Diff of the two versions.
    start = Clock::now();
    const ProjectDiff diff = diffProjects(initial, *edited);
    const double diffMs = msSince(start);
    std::size_t changedClips = 0;
    for (const SequenceDiff& s : diff.sequences) {
        changedClips += s.clips.size();
    }
    CHECK(changedClips > 0);
    report.add("diff after 1,000 edits: %.0f ms, %zu clips changed in %zu sequences", diffMs, changedClips,
               diff.sequences.size());
    CHECK(diffMs < 2000.0);

    // Three-way merge: 500 edits in the first 25 sequences against 500 in the last 25.
    ProjectState mine = initial;
    ProjectState theirs = initial;
    for (int i = 0; i < 500; ++i) {
        if (auto r = randomTrim(mine, big.sequences, 0, 25, rng)) {
            mine = std::move(r->state);
        }
        if (auto r = randomTrim(theirs, big.sequences, 25, 50, rng)) {
            theirs = std::move(r->state);
        }
    }
    start = Clock::now();
    const auto merged = mergeProjects(initial, mine, theirs);
    const double mergeMs = msSince(start);
    REQUIRE(merged);
    CHECK(merged->conflicts.empty());
    CHECK(validate(merged->merged, Validation::Strict).has_value());
    report.add("three-way merge, 500 + 500 edits in disjoint sequences: %.0f ms, %zu conflicts", mergeMs,
               merged->conflicts.size());
    CHECK(mergeMs < 2000.0);

    // Version history.
    auto history = ProjectHistory::open(dir / "large.wabcut.history");
    REQUIRE(history);
    start = Clock::now();
    const auto first = history->checkpoint(initial, HistoryKind::Save);
    const double firstMs = msSince(start);
    REQUIRE(first);
    start = Clock::now();
    const auto second = history->checkpoint(*edited, HistoryKind::Auto);
    const double secondMs = msSince(start);
    REQUIRE(second);
    start = Clock::now();
    const auto back = history->load(first->id);
    const double historyLoadMs = msSince(start);
    REQUIRE(back);
    CHECK(*back == initial);
    report.add("history: first checkpoint %.0f ms (%llu bytes), next after 1,000 edits %.0f ms (%llu bytes), load %.0f ms, "
               "folder %llu bytes",
               firstMs, static_cast<unsigned long long>(first->addedBytes), secondMs,
               static_cast<unsigned long long>(second->addedBytes), historyLoadMs,
               static_cast<unsigned long long>(history->diskBytes()));
    CHECK(firstMs < 5000.0);
    report.add("peak working set: %.0f MB", memoryMb().second);
    report.write(dir / "scale.md");
}
