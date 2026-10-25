// ExportQueue: one worker, submission order, status/listener reporting, cancel (queued and running).

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <thread>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

ExportSettings wavSettings() {
    auto p = findPreset("audio-wav");
    REQUIRE(p.has_value());
    return p->settings;
}

} // namespace

TEST_CASE("ExportQueue runs jobs in order and reports their status", "[export][media][queue]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("queue");

    std::mutex mutex;
    std::vector<std::pair<ExportJobId, ExportPhase>> events;
    ExportQueue queue([&](const ExportJobStatus& s) {
        std::lock_guard lock(mutex);
        if (events.empty() || events.back() != std::pair{s.id, s.progress.phase}) {
            events.emplace_back(s.id, s.progress.phase);
        }
    });
    const ExportJobId first = queue.enqueue(makeRequest(project, wavSettings(), dir / "first.wav"));
    const ExportJobId second = queue.enqueue(makeRequest(project, wavSettings(), dir / "second.wav"));
    CHECK(first != second);

    const auto s2 = queue.wait(second);
    const auto s1 = queue.status(first);
    REQUIRE(s1.has_value());
    REQUIRE(s2.has_value());
    CHECK(s1->progress.phase == ExportPhase::Done);
    CHECK(s2->progress.phase == ExportPhase::Done);
    REQUIRE(s1->result.has_value());
    CHECK(s1->result->audioSamples == 48'000);
    CHECK(s1->presetId == "audio-wav");
    CHECK(std::filesystem::is_regular_file(dir / "first.wav"));
    CHECK(std::filesystem::is_regular_file(dir / "second.wav"));
    CHECK(queue.jobs().size() == 2);
    CHECK_FALSE(queue.status(12345).has_value());
    CHECK_FALSE(queue.wait(12345).has_value());

    std::lock_guard lock(mutex);
    // The first job finished before the second started.
    const auto firstDone = std::find(events.begin(), events.end(), std::pair{first, ExportPhase::Done});
    const auto secondStart =
        std::find(events.begin(), events.end(), std::pair{second, ExportPhase::Preparing});
    REQUIRE(firstDone != events.end());
    REQUIRE(secondStart != events.end());
    CHECK(firstDone < secondStart);
}

TEST_CASE("ExportQueue cancels running and queued jobs; the destructor cancels the rest",
          "[export][media][queue]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("queue_cancel");

    std::atomic<bool> running{false};
    ExportQueue queue;
    ExportSettings video = findPreset("match-source")->settings;
    ExportRequest slow = makeRequest(project, video, dir / "slow.mp4");
    slow.videoSource = syntheticVideoSourceFactory();
    slow.onProgress = [&](const ExportProgress& p) {
        if (p.framesDone > 0) {
            running = true;
        }
    };
    const ExportJobId a = queue.enqueue(std::move(slow));
    const ExportJobId b = queue.enqueue(makeRequest(project, wavSettings(), dir / "queued.wav"));

    queue.cancel(b); // still queued: finishes at once without running
    const auto sb = queue.status(b);
    REQUIRE(sb.has_value());
    CHECK(sb->progress.phase == ExportPhase::Cancelled);
    REQUIRE(sb->error.has_value());
    CHECK(sb->error->code == ErrorCode::Cancelled);

    while (!running && queue.status(a)->progress.phase != ExportPhase::Done) {
        std::this_thread::yield();
    }
    queue.cancel(a);
    const auto sa = queue.wait(a);
    REQUIRE(sa.has_value());
    // Either the cancel reached it mid-render, or it had already finished (a very fast machine).
    if (sa->progress.phase == ExportPhase::Cancelled) {
        CHECK_FALSE(std::filesystem::exists(dir / "slow.mp4"));
    } else {
        CHECK(sa->progress.phase == ExportPhase::Done);
    }
    CHECK_FALSE(std::filesystem::exists(dir / "queued.wav"));
    CHECK(partialFiles(dir).empty());

    // Destroying a queue with work pending cancels it and joins.
    {
        ExportQueue doomed;
        doomed.enqueue(makeRequest(project, video, dir / "doomed1.mp4"));
        doomed.enqueue(makeRequest(project, video, dir / "doomed2.mp4"));
    }
    CHECK(partialFiles(dir).empty());
}

// 0.4 (export-failed-toast-reason): the listener hears a finished job once, with its outcome - never "Done" without
// the result or "Failed" without the error (the app's failed toast lost its reason, and a done export its warnings,
// that way). The request's own onProgress still sees the terminal phase (watch folders need it).
TEST_CASE("ExportQueue reports a finished job only with its result or error", "[export][queue]") {
    const TestProject project(640, 360, Rate{30, 1}, Ticks::fromSeconds(1)); // an empty sequence: no media needed
    const auto dir = freshOutputDir("queue_outcome");

    std::mutex mutex;
    std::vector<ExportJobStatus> statuses;
    std::vector<ExportPhase> ownPhases;
    const auto request = [&](const std::filesystem::path& output) {
        ExportRequest r = makeRequest(project, wavSettings(), output);
        r.onProgress = [&](const ExportProgress& p) {
            std::lock_guard lock(mutex);
            ownPhases.push_back(p.phase);
        };
        return r;
    };
    {
        ExportQueue queue([&](const ExportJobStatus& s) {
            std::lock_guard lock(mutex);
            statuses.push_back(s);
        });
        const ExportJobId failing = queue.enqueue(request(dir / "no-such-folder" / "fails.wav"));
        const ExportJobId working = queue.enqueue(request(dir / "works.wav"));
        const auto failed = queue.wait(failing);
        const auto done = queue.wait(working);
        REQUIRE(failed.has_value());
        REQUIRE(done.has_value());
        CHECK(failed->progress.phase == ExportPhase::Failed);
        REQUIRE(failed->error.has_value());
        CHECK(failed->error->code == ErrorCode::IoError);
        CHECK(done->progress.phase == ExportPhase::Done);
        CHECK(done->result.has_value());
    } // the worker has joined: every report was delivered

    std::lock_guard lock(mutex);
    int outcomes = 0;
    for (const ExportJobStatus& s : statuses) {
        INFO("job " << s.id << " phase " << static_cast<int>(s.progress.phase));
        if (s.progress.phase == ExportPhase::Failed) {
            CHECK(s.error.has_value());
            ++outcomes;
        } else if (s.progress.phase == ExportPhase::Done) {
            CHECK(s.result.has_value());
            ++outcomes;
        }
        CHECK(s.progress.phase != ExportPhase::Cancelled);
    }
    CHECK(outcomes == 2); // each finished job once
    CHECK(std::count(ownPhases.begin(), ownPhases.end(), ExportPhase::Done) == 1);
    CHECK(std::count(ownPhases.begin(), ownPhases.end(), ExportPhase::Failed) == 1);
    CHECK(std::filesystem::is_regular_file(dir / "works.wav"));
}
