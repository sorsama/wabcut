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
