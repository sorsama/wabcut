// Project benchmarks (P.1, docs/performance.md): what opening, saving and editing a big project costs below the UI.
//   * The 1-hour, 10,000-clip timeline (PerfProjects.h) and C.10's 10,080-clip project in 52 sequences
//     (tests/model/ScaleProject.h): serialize, parse, save (with the backup rotation of a second save), load.
//   * The editor's document (what the app runs on File > Open / Save and every minute): open (load, media check of
//     200 missing files), save, autosave after an edit.
//   * Version history (C.9): the first checkpoint and the next one after 100 edits, on C.10's project.
//   * Commands (the path MCP, the CLI and the assistant take): move, trim and ripple trim on the 10,000-clip timeline,
//     and undo, each over 30 repetitions.

#include "PerfHarness.h"
#include "PerfProjects.h"
#include "ScaleProject.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/ProxyManager.h>
#include <wabcut/export/Proxy.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/History.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

using namespace wabcut;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

/// Milliseconds of each of `n` runs of `fn`.
perf::Samples timeRuns(int n, const std::function<void()>& fn) {
    perf::Samples s;
    for (int i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        fn();
        s.add(perf::msSince(t0));
    }
    return s;
}

double fileMb(const fs::path& file) {
    std::error_code ec;
    return static_cast<double>(fs::file_size(file, ec)) / (1024.0 * 1024.0);
}

std::int64_t frameTicks(int frames) { return framesToTicks(frames, kRate25).value; }

/// A saved project of `count` camera files on disk, each with its proxy next to the project (where the app puts
/// them): what opening a proxied project hands the proxy manager.
fs::path writeProxiedProject(const fs::path& dir, int count) {
    const fs::path project = dir / "Proxied.wabcut";
    const fs::path originals = dir / "Footage";
    fs::path proxies = dir / "Proxied Proxies";
    fs::create_directories(originals);
    fs::create_directories(proxies);
    ProjectState state;
    for (int i = 0; i < count; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "C%04d.mov", i);
        const fs::path original = originals / name;
        const fs::path proxy = proxies / name;
        std::ofstream(original, std::ios::binary) << std::string(4096, 'o');
        std::ofstream(proxy, std::ios::binary) << std::string(1024, 'p');
        MediaItem item;
        item.name = name;
        item.locator.path = original.generic_string();
        item.info.path = item.locator.path;
        item.info.duration = Ticks::fromSeconds(60);
        VideoStreamInfo v;
        v.index = 0;
        v.codec = "h264";
        v.width = 3840;
        v.height = 2160;
        v.frameRate = kRate25;
        v.duration = item.info.duration;
        item.info.video.push_back(v);
        item.locator.proxy = ProxyMedia{proxy.generic_string(), {std::string(exporter::kProxyGenerator), "prores_ks proxy 1/2", {}, 1}};
        auto added = edit::addMedia(state, item);
        REQUIRE(added);
        state = std::move(added->state);
    }
    REQUIRE(io::saveProject(state, project));
    return project;
}

/// Save, load, serialize and parse `state` through `file`: metrics named "<prefix>_<what>_ms".
void ioMetrics(perf::PerfResult& r, const std::string& prefix, const ProjectState& state, const fs::path& file) {
    std::string json;
    r.addSamples(prefix + "_serialize_ms", "ms", timeRuns(3, [&] {
                     auto text = io::toJson(state);
                     REQUIRE(text);
                     json = std::move(*text);
                 }));
    r.addSamples(prefix + "_parse_ms", "ms", timeRuns(3, [&] { REQUIRE(io::fromJson(json)); }));
    std::error_code ec;
    fs::remove(file, ec);
    REQUIRE(io::saveProject(state, file)); // so every timed save also rotates the backups
    r.addSamples(prefix + "_save_ms", "ms", timeRuns(3, [&] { REQUIRE(io::saveProject(state, file)); }));
    r.addSamples(prefix + "_load_ms", "ms", timeRuns(3, [&] { REQUIRE(io::loadProject(file)); }));
    r.note(prefix + "_file_mb", "MB", fileMb(file));
}

} // namespace

TEST_CASE("Benchmark project: open, save, autosave, history and commands on 10,000-clip projects", "[perf][project]") {
    const fs::path dir = fs::path(perf::outputDir()) / "project";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto buildStart = Clock::now();
    const perf::OneHourProject hour = perf::oneHourProject();
    const double hourBuildMs = perf::msSince(buildStart);
    const test::ScaleProject scale = test::makeScaleProject(test::largeScaleSpec());
    REQUIRE(hour.clips == 10'000);

    const auto outcome = perf::runBenchmark("project", [&] {
        perf::PerfResult r;
        r.note("one_hour_build_ms", "ms", hourBuildMs);
        ioMetrics(r, "one_hour", hour.state, dir / "One hour.wabcut");
        ioMetrics(r, "scale", scale.state, dir / "Scale.wabcut");

        // The editor's document: what File > Open, File > Save and the autosave timer run.
        {
            const fs::path file = dir / "Document.wabcut";
            REQUIRE(io::saveProject(hour.state, file));
            perf::Samples open, save, autosave, autosaveGui;
            for (int i = 0; i < 3; ++i) {
                EditSession session;
                editor::ProjectDocument document(session);
                auto t0 = Clock::now();
                REQUIRE(document.open(file));
                open.add(perf::msSince(t0));
                t0 = Clock::now();
                REQUIRE(document.save());
                save.add(perf::msSince(t0));
                // One edit (a marker renamed), so the autosave has something to write.
                REQUIRE(session.apply("rename marker", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
                    auto seq = std::make_shared<Sequence>(*s.findSequence(hour.sequence));
                    seq->markers.front().name = "Edited " + std::to_string(i);
                    return s.withSequence(seq);
                }));
                t0 = Clock::now();
                const auto wrote = document.autosave();
                autosave.add(perf::msSince(t0));
                REQUIRE(wrote);
                // P.1: the app's timer runs the same autosave with its file work on a worker; the GUI thread only
                // prepares the job (the snapshot, rebased and filtered) and records it when written.
                REQUIRE(session.apply("rename marker again", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
                    auto seq = std::make_shared<Sequence>(*s.findSequence(hour.sequence));
                    seq->markers.front().name = "Edited again " + std::to_string(i);
                    return s.withSequence(seq);
                }));
                t0 = Clock::now();
                std::optional<editor::ProjectDocument::AutosaveJob> job = document.prepareAutosave();
                double gui = perf::msSince(t0);
                REQUIRE(job);
                REQUIRE(job->write());
                t0 = Clock::now();
                document.finishAutosave(*job);
                gui += perf::msSince(t0);
                autosaveGui.add(gui);
                CHECK_FALSE(document.prepareAutosave()); // recorded: nothing left to write
            }
            r.addSamples("document_open_ms", "ms", open);
            r.addSamples("document_save_ms", "ms", save);
            r.addSamples("document_autosave_ms", "ms", autosave);
            r.addSamples("document_autosave_gui_ms", "ms", autosaveGui);
        }

        // Opening a project whose 200 camera files have proxies: the proxy manager adopts them (it runs on open and
        // again when the media's status comes in).
        {
            const fs::path file = writeProxiedProject(dir / "proxied", 200);
            perf::Samples attach, refresh;
            for (int i = 0; i < 3; ++i) {
                EditSession session;
                editor::ProjectDocument document(session);
                REQUIRE(document.open(file));
                editor::ProxyManagerOptions options;
                options.untitledProxyRoot = dir / "untitled-proxies";
                options.probeHardware = false;
                auto t0 = Clock::now();
                editor::ProxyManager proxies(document, options);
                attach.add(perf::msSince(t0));
                t0 = Clock::now();
                proxies.refresh(true);
                refresh.add(perf::msSince(t0));
            }
            r.addSamples("proxy_attach_ms", "ms", attach);
            r.addSamples("proxy_refresh_ms", "ms", refresh);
        }

        // Version history on C.10's project: the first checkpoint, then one after 100 edits.
        {
            fs::remove_all(dir / "history", ec);
            auto history = ProjectHistory::open(dir / "history");
            REQUIRE(history);
            auto t0 = Clock::now();
            REQUIRE(history->checkpoint(scale.state, HistoryKind::Save));
            r.add("history_first_checkpoint_ms", "ms", perf::msSince(t0));
            EditSession session(scale.state);
            command::CommandEngine engine(session);
            int edited = 0;
            for (const SequenceId id : scale.sequences) {
                if (edited >= 100) {
                    break;
                }
                const Sequence& s = *scale.state.findSequence(id);
                if (s.videoTracks.empty() || s.videoTracks.front()->clips.empty()) {
                    continue;
                }
                const ClipId clip = s.videoTracks.front()->clips.back().id;
                const auto res = engine.executeJson(command::Json{
                    {"command", "clip.trim"},
                    {"params", {{"sequence", id.value}, {"clip", clip.value}, {"edge", "tail"}, {"delta", -frameTicks(1)}}}});
                edited += res.ok() ? 1 : 0;
            }
            t0 = Clock::now();
            REQUIRE(history->checkpoint(*session.snapshot(), HistoryKind::Auto));
            r.add("history_next_checkpoint_ms", "ms", perf::msSince(t0));
            r.note("history_edits", "edits", edited);
        }

        // Commands on the 10,000-clip timeline: each timed 30 times, undone between runs.
        {
            EditSession session(hour.state);
            command::CommandEngine engine(session);
            const auto timed = [&](const std::string& name, const std::function<command::Json(int)>& request) {
                perf::Samples run, undo;
                for (int i = 0; i < 30; ++i) {
                    const command::Json doc = request(i);
                    auto t0 = Clock::now();
                    const command::CommandResult res = engine.executeJson(doc);
                    run.add(perf::msSince(t0));
                    INFO(name << ": " << doc.dump() << " -> " << (res.ok() ? "ok" : res.error->message));
                    REQUIRE(res.ok());
                    t0 = Clock::now();
                    REQUIRE(session.undo());
                    undo.add(perf::msSince(t0));
                }
                r.addSamples(name + "_p50_ms", "ms", run);
                r.addSamples(name + "_p95_ms", "ms", run, "p95");
                return undo;
            };
            const std::uint64_t seq = hour.sequence.value;
            // Clips in the middle of V1 (index 1,000 + i): far from both ends, so a ripple moves 3,000 later clips
            // on V1 / A1 and the sync-locked V2 / A2 too.
            const auto v1 = [&](int i) { return hour.v1[static_cast<std::size_t>(1000 + i * 7)].value; };
            perf::Samples undo = timed("command_move", [&](int i) {
                return command::Json{{"command", "clip.move"},
                                     {"params", {{"sequence", seq}, {"clips", {v1(i)}}, {"delta", frameTicks(2)},
                                                 {"mode", "overwrite"}}}};
            });
            timed("command_trim", [&](int i) {
                return command::Json{{"command", "clip.trim"},
                                     {"params", {{"sequence", seq}, {"clip", v1(i)}, {"edge", "tail"}, {"delta", -frameTicks(3)}}}};
            });
            perf::Samples rippleUndo = timed("command_ripple_trim", [&](int i) {
                return command::Json{{"command", "clip.rippleTrim"},
                                     {"params", {{"sequence", seq}, {"clip", v1(i)}, {"edge", "tail"}, {"delta", -frameTicks(3)}}}};
            });
            r.addSamples("undo_move_p50_ms", "ms", undo);
            r.addSamples("undo_ripple_p50_ms", "ms", rippleUndo);
        }
        r.details["oneHour"] = command::Json{{"clips", hour.clips}, {"media", 200}, {"tracks", "2 video + 2 audio"},
                                             {"length", "1 hour at 25 fps"}};
        r.details["scale"] = command::Json{{"clips", scale.clips}, {"sequences", scale.state.sequences.size()}};
        return r;
    });
    INFO(outcome.describe());
    if (!outcome.baselineFound) {
        WARN(outcome.describe());
    }
    CHECK(outcome.ok());
}
