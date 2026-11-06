// Background rendering while idle (editor/IdleRender.h): the rule for when it may run, the plan (what, in which order),
// and the worker that renders one segment at a time through a PreviewRenderer, pausing the moment the editor is not
// quiet, giving a segment up when an edit changed it, and never retrying a failure until the project changes.
// The worker is driven with a fake renderer (no GPU, no media) and a fake load probe.

#include "TimelineFixture.h"

#include <wabcut/editor/IdleRender.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/PreviewCache.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
using namespace wabcut::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds limit = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return done();
}

/// Writes a small file per segment after `steps` progress calls of `stepTime` each.
class FakeRenderer final : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange range, const fs::path& file,
                                              const std::function<bool(double)>& progress) override {
        {
            std::lock_guard lock(mutex);
            ranges.push_back(range);
        }
        ++started;
        const int total = steps.load();
        for (int i = 0; i < total; ++i) {
            if (progress && !progress(static_cast<double>(i + 1) / total)) {
                ++cancelled;
                return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
            }
            ++stepsDone;
            if (stepMs.load() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stepMs.load()));
            }
            if (onStep) {
                onStep(i);
            }
        }
        if (fail.load()) {
            return std::unexpected(Error{ErrorCode::EncoderError, "the encoder failed"});
        }
        std::ofstream(file, std::ios::binary) << "preview";
        ++finished;
        return std::int64_t{7};
    }

    std::atomic<int> steps{4};
    std::atomic<int> stepMs{0};
    std::function<void(int)> onStep;
    std::atomic<bool> fail{false};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> stepsDone{0};
    std::mutex mutex;
    std::vector<TimeRange> ranges;

    std::vector<TimeRange> startedRanges() {
        std::lock_guard lock(mutex);
        return ranges;
    }
};

class FakeProbe final : public SystemLoadProbe {
public:
    SystemLoad sample(bool includeGpu) override {
        std::lock_guard lock(mutex);
        SystemLoad out = load;
        if (!includeGpu) {
            out.gpu = -1.0;
        }
        return out;
    }
    void set(const SystemLoad& next) {
        std::lock_guard lock(mutex);
        load = next;
    }
    std::mutex mutex;
    SystemLoad load{.cpuOthers = 5.0, .gpu = 5.0, .power = SystemLoad::Power::Mains, .batterySaver = false};
};

/// What the host would feed: the project, the playhead, the activity.
struct FakeEditor {
    std::mutex mutex;
    IdleRenderFocus focus;
    std::atomic<double> secondsSinceInput{100.0};
    std::atomic<bool> playing{false};
    std::atomic<bool> gesture{false};
    std::atomic<bool> exporting{false};
    std::atomic<bool> otherWork{false};
    std::atomic<bool> held{false};

    void setProject(const ProjectState& state, SequenceId seq, Ticks playhead = {}) {
        std::lock_guard lock(mutex);
        focus.snapshot = std::make_shared<const ProjectState>(state);
        focus.sequence = seq;
        focus.playhead = playhead;
    }

    IdleRenderHost host(std::shared_ptr<SystemLoadProbe> probe) {
        IdleRenderHost h;
        h.focus = [this] {
            std::lock_guard lock(mutex);
            return focus;
        };
        h.inputs = [this] {
            IdleInputs in;
            in.secondsSinceInput = secondsSinceInput.load();
            in.playing = playing.load();
            in.gesture = gesture.load();
            in.exporting = exporting.load();
            in.otherWork = otherWork.load();
            in.held = held.load();
            return in;
        };
        h.probe = std::move(probe);
        return h;
    }
};

/// "V1 |Aaaa..Bbbb..Cccc|": A and C get a blur (yellow), B five (red: 7.5 per frame is over the real-time budget).
ProjectState effectHeavyClips(Timeline& tl) {
    ProjectState state = tl.state;
    const auto add = [&](const char* ref, int times) {
        const std::vector<ClipId> clip{clipAt(state, tl.seq, ref)};
        for (int i = 0; i < times; ++i) {
            state = ok(edit::addEffect(state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
        }
    };
    add("V1@0", 1);
    add("V1@6", 5);
    add("V1@12", 1);
    return state;
}

IdleRenderSettings quick() {
    IdleRenderSettings s;
    s.enabled = true;
    s.idleSeconds = 1.0; // the fake editor says 100 s since the last input unless a test says otherwise
    return s;
}

std::vector<RenderSegment> todoOf(const ProjectState& state, const PreviewCache& cache, SequenceId seq) {
    std::vector<RenderSegment> out;
    for (const RenderSegment& s : cache.renderBar(state, seq)) {
        if (s.status == RenderStatus::Red || s.status == RenderStatus::Yellow) {
            out.push_back(s);
        }
    }
    return out;
}

} // namespace

TEST_CASE("Idle render verdict: every reason, in order, and what does not block", "[editor][idle-render]") {
    IdleRenderSettings settings;
    settings.idleSeconds = 10.0;
    IdleInputs quiet;
    quiet.secondsSinceInput = 30.0;
    SystemLoad calm;
    calm.cpuOthers = 5.0;
    calm.gpu = 10.0;
    calm.power = SystemLoad::Power::Mains;

    CHECK(idleVerdict(settings, quiet, calm, false).run);
    CHECK(idleVerdict(settings, quiet, calm, false).block == IdleBlock::None);

    SECTION("settings off") {
        settings.enabled = false;
        CHECK(idleVerdict(settings, quiet, calm, false).block == IdleBlock::Disabled);
    }
    SECTION("no sequence") {
        quiet.hasSequence = false;
        CHECK(idleVerdict(settings, quiet, calm, false).block == IdleBlock::NoSequence);
    }
    SECTION("playback, a gesture, an export and other work") {
        auto in = quiet;
        in.playing = true;
        CHECK(idleVerdict(settings, in, calm, false).block == IdleBlock::Playing);
        in = quiet;
        in.gesture = true;
        CHECK(idleVerdict(settings, in, calm, false).block == IdleBlock::Editing);
        in = quiet;
        in.exporting = true;
        CHECK(idleVerdict(settings, in, calm, false).block == IdleBlock::Export);
        in = quiet;
        in.otherWork = true;
        CHECK(idleVerdict(settings, in, calm, false).block == IdleBlock::OtherWork);
        in = quiet;
        in.held = true;
        CHECK(idleVerdict(settings, in, calm, false).block == IdleBlock::Held);
    }
    SECTION("input younger than the idle time, exactly at it is quiet") {
        auto in = quiet;
        in.secondsSinceInput = 9.99;
        CHECK(idleVerdict(settings, in, calm, false).block == IdleBlock::Input);
        in.secondsSinceInput = 10.0;
        CHECK(idleVerdict(settings, in, calm, false).run);
    }
    SECTION("battery and energy saver, unless the user allows them") {
        auto load = calm;
        load.power = SystemLoad::Power::Battery;
        CHECK(idleVerdict(settings, quiet, load, false).block == IdleBlock::Battery);
        load = calm;
        load.batterySaver = true;
        CHECK(idleVerdict(settings, quiet, load, false).block == IdleBlock::BatterySaver);
        settings.pauseOnBattery = false;
        load.power = SystemLoad::Power::Battery;
        CHECK(idleVerdict(settings, quiet, load, false).run);
        // Unknown power is not a reason.
        settings.pauseOnBattery = true;
        load = calm;
        load.power = SystemLoad::Power::Unknown;
        CHECK(idleVerdict(settings, quiet, load, false).run);
    }
    SECTION("other programs' load: start and pause thresholds, the GPU only before a segment starts") {
        auto load = calm;
        load.cpuOthers = kStartCpuOthers;
        CHECK(idleVerdict(settings, quiet, load, false).block == IdleBlock::CpuBusy);
        CHECK(idleVerdict(settings, quiet, load, true).run); // a segment in hand pauses only at the higher threshold
        load.cpuOthers = kPauseCpuOthers;
        CHECK(idleVerdict(settings, quiet, load, true).block == IdleBlock::CpuBusy);
        load = calm;
        load.gpu = kStartGpuBusy;
        CHECK(idleVerdict(settings, quiet, load, false).block == IdleBlock::GpuBusy);
        CHECK(idleVerdict(settings, quiet, load, true).run); // while rendering the load is mostly our own
        settings.pauseWhenBusy = false;
        load.cpuOthers = 100.0;
        CHECK(idleVerdict(settings, quiet, load, false).run);
        // Unknown loads (-1) are no reason.
        settings.pauseWhenBusy = true;
        load.cpuOthers = -1.0;
        load.gpu = -1.0;
        CHECK(idleVerdict(settings, quiet, load, false).run);
    }
}

TEST_CASE("Idle render plan: red before yellow, from the playhead forward, rendered segments and failures skipped",
          "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_plan");
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    const ProjectState state = effectHeavyClips(tl);
    PreviewCache cache(dir);

    const auto bar = cache.renderBar(state, tl.seq);
    int red = 0;
    int yellow = 0;
    for (const RenderSegment& s : bar) {
        red += s.status == RenderStatus::Red;
        yellow += s.status == RenderStatus::Yellow;
    }
    REQUIRE(red == 1);
    REQUIRE(yellow == 2);

    // From the start: the red clip B first, then A, then C.
    auto plan = idleRenderPlan(state, cache, tl.seq, Ticks{});
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].status == RenderStatus::Red);
    CHECK(plan[0].range.start == f(6));
    CHECK(plan[1].range.start == f(0));
    CHECK(plan[2].range.start == f(12));

    // The playhead in C: red still first (it cannot play in real time), then C (at the playhead), then A (before it).
    plan = idleRenderPlan(state, cache, tl.seq, f(13));
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].range.start == f(6));
    CHECK(plan[1].range.start == f(12));
    CHECK(plan[2].range.start == f(0));

    // A segment with a valid render file is not in the plan, and a failed one (its hash) is not either.
    const TimeRange aRange = plan[2].range;
    const std::string aHash = renderContentHash(state, tl.seq, aRange);
    std::ofstream(dir / PreviewCache::fileNameFor(tl.seq, aRange, aHash), std::ios::binary) << "preview";
    cache.record(PreviewEntry{tl.seq, aRange, aHash, PreviewCache::fileNameFor(tl.seq, aRange, aHash), 7, 0});
    plan = idleRenderPlan(state, cache, tl.seq, Ticks{});
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].range.start == f(6));
    CHECK(plan[1].range.start == f(12));
    const std::string cHash = renderContentHash(state, tl.seq, plan[1].range);
    const std::string key = std::to_string(plan[1].range.start.value) + ":" + std::to_string(plan[1].range.duration.value) + ":" + cHash;
    plan = idleRenderPlan(state, cache, tl.seq, Ticks{}, {key});
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].range.start == f(6));

    // The estimate is the manual render's (ProRes LT at the sequence's size and rate).
    CHECK(estimatePreviewBytes(state, tl.seq, TimeRange{Ticks{}, Ticks::fromSeconds(10)}) > 0);
    CHECK(estimatePreviewBytes(state, SequenceId{}, TimeRange{Ticks{}, Ticks::fromSeconds(10)}) == 0);
}

TEST_CASE("The idle worker renders red first and records files that show the project", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_renders");
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    const ProjectState state = effectHeavyClips(tl);
    FakeEditor editorState;
    editorState.setProject(state, tl.seq);
    auto cache = std::make_shared<PreviewCache>(dir);
    auto renderer = std::make_shared<FakeRenderer>();
    auto probe = std::make_shared<FakeProbe>();

    IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
    worker.setPollInterval(5ms);
    REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::UpToDate; }));

    const IdleRenderStatus status = worker.status();
    CHECK(status.rendered == 3);
    CHECK(status.failed == 0);
    CHECK(status.renderedBytes == 21);
    const auto ranges = renderer->startedRanges();
    REQUIRE(ranges.size() == 3);
    CHECK(ranges[0].start == f(6)); // the red one first
    // Every red and yellow stretch is green now; the files are there and hash to the project.
    CHECK(todoOf(state, *cache, tl.seq).empty());
    REQUIRE(cache->entries().size() == 3);
    for (const PreviewEntry& e : cache->entries()) {
        CHECK(e.hash == renderContentHash(state, tl.seq, e.range));
        CHECK(fs::exists(dir / e.file));
    }
    // Playback finds them: validAt is what the scheduler asks.
    CHECK(cache->validAt(state, tl.seq, f(2)).has_value());
    // Nothing more to do: it does not render again.
    std::this_thread::sleep_for(100ms);
    CHECK(renderer->started == 3);
}

TEST_CASE("The idle worker waits for the editor to be quiet", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_waits");
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::vector<ClipId> clip{clipAt(tl.state, tl.seq, "V1@0")};
    const ProjectState state = ok(edit::addEffect(tl.state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
    FakeEditor editorState;
    editorState.setProject(state, tl.seq);
    auto cache = std::make_shared<PreviewCache>(dir);
    auto renderer = std::make_shared<FakeRenderer>();
    auto probe = std::make_shared<FakeProbe>();

    struct Case {
        const char* name;
        std::function<void()> block;
        std::function<void()> release;
        IdleBlock expected;
    };
    const std::vector<Case> cases{
        {"input", [&] { editorState.secondsSinceInput = 0.0; }, [&] { editorState.secondsSinceInput = 100.0; }, IdleBlock::Input},
        {"playback", [&] { editorState.playing = true; }, [&] { editorState.playing = false; }, IdleBlock::Playing},
        {"gesture", [&] { editorState.gesture = true; }, [&] { editorState.gesture = false; }, IdleBlock::Editing},
        {"export", [&] { editorState.exporting = true; }, [&] { editorState.exporting = false; }, IdleBlock::Export},
        {"other work", [&] { editorState.otherWork = true; }, [&] { editorState.otherWork = false; }, IdleBlock::OtherWork},
        {"held", [&] { editorState.held = true; }, [&] { editorState.held = false; }, IdleBlock::Held},
        {"battery", [&] { probe->set({.cpuOthers = 5, .gpu = 5, .power = SystemLoad::Power::Battery}); },
         [&] { probe->set({.cpuOthers = 5, .gpu = 5, .power = SystemLoad::Power::Mains}); }, IdleBlock::Battery},
        {"busy processor", [&] { probe->set({.cpuOthers = 90, .gpu = 5, .power = SystemLoad::Power::Mains}); },
         [&] { probe->set({.cpuOthers = 5, .gpu = 5, .power = SystemLoad::Power::Mains}); }, IdleBlock::CpuBusy},
        {"busy graphics card", [&] { probe->set({.cpuOthers = 5, .gpu = 95, .power = SystemLoad::Power::Mains}); },
         [&] { probe->set({.cpuOthers = 5, .gpu = 5, .power = SystemLoad::Power::Mains}); }, IdleBlock::GpuBusy},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        cache = std::make_shared<PreviewCache>(dir);
        c.block();
        IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
        worker.setPollInterval(5ms);
        const int before = renderer->started;
        REQUIRE(waitFor([&] {
            const auto s = worker.status();
            return s.phase == IdleRenderStatus::Phase::Waiting && s.block == c.expected;
        }));
        std::this_thread::sleep_for(80ms);
        CHECK(renderer->started == before); // nothing was started while blocked
        c.release();
        worker.wake();
        REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::UpToDate; }));
        CHECK(renderer->started == before + 1);
    }
}

TEST_CASE("Input pauses a segment in hand at once and it goes on when the editor is quiet again", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_pause");
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::vector<ClipId> clip{clipAt(tl.state, tl.seq, "V1@0")};
    const ProjectState state = ok(edit::addEffect(tl.state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
    FakeEditor editorState;
    editorState.setProject(state, tl.seq);
    auto cache = std::make_shared<PreviewCache>(dir);
    auto renderer = std::make_shared<FakeRenderer>();
    renderer->steps = 100;
    renderer->stepMs = 4;
    auto probe = std::make_shared<FakeProbe>();

    IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
    worker.setPollInterval(5ms);
    REQUIRE(waitFor([&] { return renderer->stepsDone >= 6; }));
    editorState.secondsSinceInput = 0.0; // the user touches the mouse
    REQUIRE(waitFor([&] {
        const auto s = worker.status();
        return s.phase == IdleRenderStatus::Phase::Paused && s.block == IdleBlock::Input;
    }));
    const int atPause = renderer->stepsDone;
    CHECK(worker.status().progress > 0.0);
    std::this_thread::sleep_for(150ms);
    CHECK(renderer->stepsDone <= atPause + 1); // at most the frame that was in flight
    CHECK(renderer->cancelled == 0);           // paused, not thrown away
    CHECK(cache->entries().empty());

    editorState.secondsSinceInput = 100.0;
    REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::UpToDate; }));
    CHECK(renderer->started == 1); // the same render went on
    CHECK(renderer->finished == 1);
    CHECK(cache->entries().size() == 1);
}

TEST_CASE("A segment paused for too long is given up and a disabled setting stops the render", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_giveup");
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::vector<ClipId> clip{clipAt(tl.state, tl.seq, "V1@0")};
    const ProjectState state = ok(edit::addEffect(tl.state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
    FakeEditor editorState;
    editorState.setProject(state, tl.seq);
    auto probe = std::make_shared<FakeProbe>();

    SECTION("a pause longer than the limit") {
        auto cache = std::make_shared<PreviewCache>(dir);
        auto renderer = std::make_shared<FakeRenderer>();
        renderer->steps = 40;
        renderer->stepMs = 4;
        IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
        worker.setPollInterval(5ms);
        worker.setMaxPauseSeconds(0.1);
        REQUIRE(waitFor([&] { return renderer->stepsDone >= 3; }));
        editorState.secondsSinceInput = 0.0;
        REQUIRE(waitFor([&] { return renderer->cancelled == 1; }));
        CHECK(cache->entries().empty());
        CHECK(worker.status().rendered == 0);
        // It starts the segment again from its first frame once the editor is quiet.
        editorState.secondsSinceInput = 100.0;
        renderer->stepMs = 0;
        worker.wake();
        REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::UpToDate; }));
        CHECK(renderer->started == 2);
        CHECK(cache->entries().size() == 1);
    }
    SECTION("turning it off in the middle") {
        auto cache = std::make_shared<PreviewCache>(dir);
        auto renderer = std::make_shared<FakeRenderer>();
        renderer->steps = 200;
        renderer->stepMs = 4;
        IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
        worker.setPollInterval(5ms);
        REQUIRE(waitFor([&] { return renderer->stepsDone >= 3; }));
        IdleRenderSettings off = quick();
        off.enabled = false;
        worker.setSettings(off);
        REQUIRE(waitFor([&] { return renderer->cancelled == 1; }));
        REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::Off; }));
        CHECK(cache->entries().empty());
        CHECK(worker.status().block == IdleBlock::Disabled);
        std::this_thread::sleep_for(80ms);
        CHECK(renderer->started == 1); // and it stays off
    }
    SECTION("cancelCurrent gives the segment up, then other work keeps it waiting") {
        auto cache = std::make_shared<PreviewCache>(dir);
        auto renderer = std::make_shared<FakeRenderer>();
        renderer->steps = 200;
        renderer->stepMs = 4;
        IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
        worker.setPollInterval(5ms);
        REQUIRE(waitFor([&] { return renderer->stepsDone >= 3; }));
        editorState.otherWork = true; // a manual render starts ...
        worker.cancelCurrent();       // ... and the host tells the worker
        REQUIRE(waitFor([&] { return renderer->cancelled == 1; }));
        REQUIRE(waitFor([&] {
            const auto s = worker.status();
            return s.phase == IdleRenderStatus::Phase::Waiting && s.block == IdleBlock::OtherWork;
        }));
        std::this_thread::sleep_for(80ms);
        CHECK(renderer->started == 1);
    }
}

TEST_CASE("An edit that changes the segment in hand gives its file up; an edit elsewhere keeps it", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_stale");
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const std::vector<ClipId> a{clipAt(tl.state, tl.seq, "V1@0")};
    const std::vector<ClipId> b{clipAt(tl.state, tl.seq, "V1@6")};
    const ProjectState both = ok(edit::addEffect(ok(edit::addEffect(tl.state, tl.seq, std::span(a), "wabcut.blur.gaussian")).state,
                                                 tl.seq, std::span(b), "wabcut.blur.gaussian"))
                                  .state;
    // B is red-free here; give B a second effect later as "the edit" (it changes B's pixels), and an effect on A is the
    // edit elsewhere when B renders first.
    FakeEditor editorState;
    editorState.setProject(both, tl.seq, f(6)); // the playhead in B: B first (both are yellow)
    auto cache = std::make_shared<PreviewCache>(dir);
    auto renderer = std::make_shared<FakeRenderer>();
    renderer->steps = 60;
    renderer->stepMs = 3;
    auto probe = std::make_shared<FakeProbe>();
    IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
    worker.setPollInterval(5ms);

    REQUIRE(waitFor([&] { return renderer->stepsDone >= 4; }));
    REQUIRE(renderer->startedRanges().front().start == f(6));
    // An edit of A (another stretch): the render of B goes on, its file is recorded.
    ProjectState editedA = ok(edit::addEffect(both, tl.seq, std::span(a), "wabcut.sharpen")).state;
    editorState.setProject(editedA, tl.seq, f(6));
    REQUIRE(waitFor([&] { return worker.status().rendered >= 1; }));
    CHECK(renderer->cancelled == 0);
    const auto entries = cache->entries();
    const auto rendered = std::find_if(entries.begin(), entries.end(), [&](const PreviewEntry& e) { return e.range.start == f(6); });
    REQUIRE(rendered != entries.end());
    CHECK(rendered->hash == renderContentHash(editedA, tl.seq, rendered->range));

    // An edit of the segment being rendered (A now): it is given up and rendered again for the new content.
    renderer->stepMs = 6;
    renderer->steps = 80;
    REQUIRE(waitFor([&] { return renderer->started >= 2 && renderer->stepsDone >= 70; }));
    const int cancelledBefore = renderer->cancelled;
    ProjectState editedAgain = ok(edit::addEffect(editedA, tl.seq, std::span(a), "wabcut.vignette")).state;
    editorState.setProject(editedAgain, tl.seq, f(6));
    REQUIRE(waitFor([&] { return renderer->cancelled > cancelledBefore; }));
    renderer->stepMs = 0;
    renderer->steps = 4;
    REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::UpToDate; }));
    for (const PreviewEntry& e : cache->entries()) {
        INFO(e.range.start.value);
        CHECK(e.hash == renderContentHash(editedAgain, tl.seq, e.range));
    }
    CHECK(todoOf(editedAgain, *cache, tl.seq).empty());
}

TEST_CASE("A failed segment is not tried again until the project changes", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_failure");
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::vector<ClipId> clip{clipAt(tl.state, tl.seq, "V1@0")};
    const ProjectState state = ok(edit::addEffect(tl.state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
    FakeEditor editorState;
    editorState.setProject(state, tl.seq);
    auto cache = std::make_shared<PreviewCache>(dir);
    auto renderer = std::make_shared<FakeRenderer>();
    renderer->fail = true;
    auto probe = std::make_shared<FakeProbe>();
    IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
    worker.setPollInterval(5ms);

    REQUIRE(waitFor([&] { return worker.status().failed == 1; }));
    CHECK(worker.status().lastError == "the encoder failed");
    REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::UpToDate; }));
    std::this_thread::sleep_for(150ms);
    CHECK(renderer->started == 1); // no hot loop on a segment that cannot be rendered
    CHECK(cache->entries().empty());

    // The project changes: a different segment content, tried again (and now it works).
    renderer->fail = false;
    ProjectState edited = ok(edit::addEffect(state, tl.seq, std::span(clip), "wabcut.sharpen")).state;
    editorState.setProject(edited, tl.seq);
    REQUIRE(waitFor([&] { return worker.status().rendered == 1; }));
    CHECK(renderer->started == 2);
    CHECK(cache->entries().size() == 1);
}

TEST_CASE("The idle worker does not render past the cache budget", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_full");
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::vector<ClipId> clip{clipAt(tl.state, tl.seq, "V1@0")};
    const ProjectState state = ok(edit::addEffect(tl.state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
    FakeEditor editorState;
    editorState.setProject(state, tl.seq);
    auto cache = std::make_shared<PreviewCache>(dir, 1); // a one-byte budget: nothing fits
    auto renderer = std::make_shared<FakeRenderer>();
    auto probe = std::make_shared<FakeProbe>();
    IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
    worker.setPollInterval(5ms);
    REQUIRE(waitFor([&] { return worker.status().phase == IdleRenderStatus::Phase::Full; }));
    std::this_thread::sleep_for(80ms);
    CHECK(renderer->started == 0);
    CHECK(worker.status().remaining == 1);
}

TEST_CASE("The idle worker looks at the sequence it is given and resets with a new cache", "[editor][idle-render]") {
    const fs::path dir = freshDir("idle_reset");
    const fs::path dirTwo = freshDir("idle_reset_two");
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const std::vector<ClipId> clip{clipAt(tl.state, tl.seq, "V1@0")};
    const ProjectState state = ok(edit::addEffect(tl.state, tl.seq, std::span(clip), "wabcut.blur.gaussian")).state;
    FakeEditor editorState;
    editorState.setProject(state, SequenceId{}); // no sequence open
    auto cache = std::make_shared<PreviewCache>(dir);
    auto renderer = std::make_shared<FakeRenderer>();
    auto probe = std::make_shared<FakeProbe>();
    IdleRenderer worker(cache, renderer, editorState.host(probe), quick());
    worker.setPollInterval(5ms);
    REQUIRE(waitFor([&] {
        const auto s = worker.status();
        return s.phase == IdleRenderStatus::Phase::Waiting && s.block == IdleBlock::NoSequence;
    }));
    CHECK(renderer->started == 0);

    editorState.setProject(state, tl.seq);
    REQUIRE(waitFor([&] { return worker.status().rendered == 1; }));
    CHECK(cache->entries().size() == 1);

    // Another project's cache: the counts start again and the segment is rendered into the new folder.
    auto other = std::make_shared<PreviewCache>(dirTwo);
    worker.reset(other, renderer);
    CHECK(worker.status().rendered == 0);
    REQUIRE(waitFor([&] { return worker.status().rendered == 1; }));
    CHECK(other->entries().size() == 1);
    CHECK(renderer->started == 2);
}

TEST_CASE("The background mode of a thread is scoped and the load probe answers", "[editor][idle-render]") {
    {
        BackgroundThreadScope scope;
#ifdef _WIN32
        CHECK(scope.active());
#endif
    }
    auto probe = makeSystemLoadProbe();
    REQUIRE(probe);
    const SystemLoad first = probe->sample(true);
    std::this_thread::sleep_for(600ms);
    const SystemLoad second = probe->sample(false);
    CHECK(second.gpu == -1.0); // not asked for
    if (second.cpuOthers >= 0.0) {
        CHECK(second.cpuOthers <= 100.0);
    }
    (void)first;
}

TEST_CASE("The export thread limit of a scope applies to its thread only", "[editor][idle-render][export]") {
    exporter::setExportThreadLimit(0);
    CHECK(exporter::exportThreadLimit() == 0);
    {
        exporter::ScopedExportThreadLimit scope(3);
        CHECK(exporter::exportThreadLimit() == 3);
        int seenElsewhere = -1;
        std::thread other([&] { seenElsewhere = exporter::exportThreadLimit(); });
        other.join();
        CHECK(seenElsewhere == 0); // a real export started on another thread keeps the user's own limit
        exporter::setExportThreadLimit(2); // the user's own, smaller limit wins
        CHECK(exporter::exportThreadLimit() == 2);
        exporter::setExportThreadLimit(8);
        CHECK(exporter::exportThreadLimit() == 3);
        {
            exporter::ScopedExportThreadLimit inner(1);
            CHECK(exporter::exportThreadLimit() == 1);
        }
        CHECK(exporter::exportThreadLimit() == 3);
    }
    CHECK(exporter::exportThreadLimit() == 8);
    exporter::setExportThreadLimit(0);
}
