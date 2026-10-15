// Benchmark playback (P.1, docs/performance.md): how fast the editor answers the transport keys and the playhead,
// through the objects the app plays with. editor::PlaybackSession as PlaybackBridge builds it (the audio engine as the
// master clock, TimelineAudioPlayer + MediaAudioFeed, the PlaybackController's Scheduler decoding on its DecoderPool,
// zero-copy on the program monitor's D3D11 device), the Null audio backend pulled by a device thread in real time, and
// a program monitor stand-in: woken by the CompositeMailbox's publish callback, it acquires the picture for the next
// refresh and composites it on a headless QRhi. Per media kind (H.264 1080p, HEVC 2160p, ProRes 422 1080p, and the
// 2160p clip played from its ProRes Proxy 1080p proxy):
//   * play -> first picture and first sound, cold (a new pipeline) and warm (the same one, back at the start);
//   * J/K/L: L from paused, L L (2x), J (reverse): the first picture showing each;
//   * frame steps (+1) while paused, and one more after the scheduler closed its idle sessions (3 s);
//   * scrubbing: a 60 Hz playhead drag, request -> that frame displayed (p50/p95), and the requests superseded.
// Media: tests/perf/gen_perf_bench_media.py --out <WABCUT_PERF_MEDIA_DIR>. r.details["definition"] says exactly what
// each number includes.

#include "PerfHarness.h"

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/playback/video/TransportClock.h>
#include <wabcut/render/CompositeMailbox.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSize>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// clang-format off: windows.h first
#include <windows.h>
#include <timeapi.h>
// clang-format on
#pragma comment(lib, "winmm.lib")
#endif

#ifndef WABCUT_PERF_MEDIA_DIR
#error "WABCUT_PERF_MEDIA_DIR must name the directory tests/perf/gen_perf_bench_media.py writes"
#endif

using namespace wabcut;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr Rate kFps = kRate30;                                    // every clip and sequence
constexpr std::int64_t kRefreshNs = 16'666'667;                   // a 60 Hz monitor
constexpr auto kDragInterval = std::chrono::microseconds(16'667); // playhead drag events (60 Hz)
constexpr auto kDevicePeriod = 5ms;                               // the device thread's callback period
constexpr float kAudible = 1e-4f;                                 // -80 dBFS: the first sample above it is "sound"
constexpr int kColdRuns = 5;
constexpr int kWarmRuns = 5;
constexpr int kShuttleRuns = 3;
constexpr int kSteps = 30;
constexpr std::int64_t kStepFrom = 150;            // frame steps start at 5 s
constexpr auto kIdle = 3400ms;                     // > SchedulerOptions::sessionIdleMs (3 s)
constexpr Ticks kShuttleFrom = Ticks::fromSeconds(2);
constexpr playback::TransportSpeed kDouble{2, 1};  // L L
constexpr playback::TransportSpeed kReverse{-1, 1}; // J
const QSize kMonitorSize(1920, 1080);

struct MediaKind {
    const char* name;  ///< the metric suffix
    const char* file;  ///< the media item's file
    const char* proxy; ///< the proxy the preview decodes instead (PreviewSettings::useProxies), or null
    int width;         ///< the sequence
    int height;
};

constexpr MediaKind kKinds[] = {
    {"h264_1080p", "perf_h264_1080p30.mp4", nullptr, 1920, 1080},
    {"hevc_2160p", "perf_hevc_2160p30.mp4", nullptr, 3840, 2160},
    {"prores_1080p", "perf_prores_1080p30.mov", nullptr, 1920, 1080},
    // The 2160p clip edited in a 2160p sequence and previewed from its half-resolution proxy (proxies on).
    {"proxy_1080p", "perf_hevc_2160p30.mp4", "perf_proxy_1080p30.mov", 3840, 2160},
};

fs::path mediaFile(const char* name) {
    return fs::path(WABCUT_PERF_MEDIA_DIR) / name;
}

bool haveMedia(const MediaKind& kind) {
    std::error_code ec;
    return fs::is_regular_file(mediaFile(kind.file), ec) &&
           (kind.proxy == nullptr || fs::is_regular_file(mediaFile(kind.proxy), ec));
}

Ticks frameTime(std::int64_t frame) {
    return framesToTicks(frame, kFps);
}

double msBetween(std::int64_t fromNs, std::int64_t toNs) {
    return static_cast<double>(toNs - fromNs) / 1e6;
}

std::int64_t nsOf(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

/// What a 1 ms sleep takes here: the monitor's and the device thread's waits are built on it (about 1 ms with the
/// TimerResolution below; 15.6 ms where the system grants none).
double sleepGranularityMs() {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(1ms);
    }
    return perf::msSince(start) / 10.0;
}

void addTo(perf::Samples& samples, const std::optional<double>& value, int& missed) {
    if (value) {
        samples.add(*value);
    } else {
        ++missed;
    }
}

int g_argc = 1;
char g_argv0[] = "wabcut_perf_tests";
char* g_argv[] = {g_argv0, nullptr};

/// The QGuiApplication the renderer needs (shader resources, platform state; no window), unless the suite made one.
class GuiApplication {
public:
    GuiApplication() {
        if (QCoreApplication::instance() == nullptr) {
            m_app = std::make_unique<QGuiApplication>(g_argc, g_argv);
        }
    }

private:
    std::unique_ptr<QGuiApplication> m_app;
};

/// 1 ms timer resolution while measuring: the monitor's 1 ms waits and the device thread's 5 ms periods would round up
/// to the 15.6 ms system tick otherwise (Windows grants a process without a visible window no finer one).
class TimerResolution {
public:
    TimerResolution() {
#if defined(_WIN32)
        (void)timeBeginPeriod(1);
#endif
    }
    ~TimerResolution() {
#if defined(_WIN32)
        (void)timeEndPeriod(1);
#endif
    }
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;
};

/// Decoders opened after the renderer is gone get media's own device again.
struct ResetSharedDevice {
    ~ResetSharedDevice() { (void)media::setSharedD3D11Device(nullptr, nullptr); }
};

// ---- The project ---------------------------------------------------------------------------------------------------

/// A project as the editor makes it: a 30 fps sequence, the clip imported and dropped at 0 (V1 + A1, linked).
struct Project {
    std::unique_ptr<EditSession> session;
    SequenceId sequence;
    MediaId item; ///< the clip's media item
};

Project makeProject(const MediaKind& kind) {
    edit::SequenceSettings settings;
    settings.name = kind.name;
    settings.frameRate = kFps;
    settings.width = kind.width;
    settings.height = kind.height;
    auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    Project p;
    p.sequence = created->id;
    p.session = std::make_unique<EditSession>(std::move(created->state));
    const std::vector<fs::path> files{mediaFile(kind.file)};
    const editor::MediaImportResult imported = editor::importMediaFiles(*p.session, files);
    INFO(kind.file << (imported.failed.empty() ? std::string() : ": " + imported.failed.front().error.message));
    REQUIRE(imported.added.size() == 1);
    p.item = imported.added.front();
    REQUIRE(editor::dropMedia(*p.session, {.media = p.item, .sequence = p.sequence, .at = Ticks{}}));
    return p;
}

// ---- The audio device ----------------------------------------------------------------------------------------------

/// The audio device's part on the Null backend: a thread pulls 5 ms blocks at the steady clock's pace, as a device
/// calls back (the engine's clock, which the picture follows, moves only as blocks are pulled), and notes when the
/// first audible sample after arm() comes out. A zero-latency device: a sample is heard at its own time in its block.
class NullDevice {
public:
    explicit NullDevice(audio::AudioEngine& engine)
        : m_engine(engine), m_rate(engine.config().sampleRate), m_channels(engine.config().channels),
          m_thread([this] { run(); }) {}
    ~NullDevice() {
        m_stop.store(true);
        m_thread.join();
    }
    NullDevice(const NullDevice&) = delete;
    NullDevice& operator=(const NullDevice&) = delete;

    /// Listen for the first audible sample of a block pulled from steady time `ns` on.
    void arm(std::int64_t ns) {
        m_heardNs.store(0);
        m_armedNs.store(ns);
    }
    /// Steady time of that sample; 0 while none came.
    std::int64_t heardNs() const { return m_heardNs.load(); }
    std::uint64_t pulls() const { return m_pulls.load(); }
    /// Blocks pulled more than 2.5 ms after their time (a coarse timer makes them come in bursts).
    std::uint64_t latePulls() const { return m_late.load(); }
    std::uint64_t pullErrors() const { return m_errors.load(); }

private:
    void run() {
        const auto channels = static_cast<std::size_t>(m_channels);
        std::vector<float> block(static_cast<std::size_t>(m_rate / 200) * channels); // 5 ms
        auto due = std::chrono::steady_clock::now();
        while (!m_stop.load(std::memory_order_relaxed)) {
            due += kDevicePeriod;
            if (std::chrono::steady_clock::now() - due > 50ms) {
                due = std::chrono::steady_clock::now(); // stalled: carry on from now, as a device does after a glitch
            }
            std::this_thread::sleep_until(due);
            const auto now = std::chrono::steady_clock::now();
            m_pulls.fetch_add(1, std::memory_order_relaxed);
            if (now - due > 2500us) {
                m_late.fetch_add(1, std::memory_order_relaxed);
            }
            if (!m_engine.pull(block, now)) {
                m_errors.fetch_add(1);
                continue;
            }
            const std::int64_t armed = m_armedNs.load();
            const std::int64_t at = nsOf(now);
            if (armed == 0 || at < armed || m_heardNs.load() != 0) {
                continue;
            }
            for (std::size_t i = 0; i < block.size(); ++i) {
                if (std::abs(block[i]) > kAudible) {
                    const auto frame = static_cast<std::int64_t>(i / channels);
                    std::int64_t none = 0;
                    (void)m_heardNs.compare_exchange_strong(none, at + frame * 1'000'000'000LL / m_rate);
                    break;
                }
            }
        }
    }

    audio::AudioEngine& m_engine;
    const int m_rate;
    const int m_channels;
    std::atomic<bool> m_stop{false};
    std::atomic<std::int64_t> m_armedNs{0};
    std::atomic<std::int64_t> m_heardNs{0};
    std::atomic<std::uint64_t> m_pulls{0};
    std::atomic<std::uint64_t> m_late{0};
    std::atomic<std::uint64_t> m_errors{0};
    std::thread m_thread; // last: it starts once everything above exists
};

// ---- The program monitor -------------------------------------------------------------------------------------------

/// A picture the monitor drew.
struct Shown {
    std::int64_t ns = 0;          ///< steady time it was on the canvas (acquired, composited, GPU finished)
    std::int64_t publishedNs = 0; ///< when the scheduler published it
    std::uint64_t epoch = 0;
    Ticks target{};
    int layers = 0;
};

/// What the monitor drew over a media kind's pipelines.
struct Drawn {
    perf::Samples compositeMs; ///< renderTimed() per picture: import or upload + composite, GPU finished
    int errors = 0;
    int layersMissing = 0; ///< pictures drawn with fewer layers than they carried (a frame failed to import)
};

/// The program monitor's part, on this thread. ProgramMonitor wakes on the mailbox's publish callback (and refreshes
/// every frame while the transport moves), acquires the picture for the refresh that becomes visible one interval
/// later and draws it with the Compositor. This one does not wait for a vsync: it refreshes on every publish and at
/// least every millisecond, so its times are the pipeline's, without the 0..16.7 ms a display adds.
class Monitor {
public:
    Monitor(render::OffscreenRenderer& renderer, std::shared_ptr<render::CompositeMailbox> mailbox, Drawn& drawn)
        : m_renderer(renderer), m_mailbox(std::move(mailbox)), m_drawn(drawn) {
        m_mailbox->setPublishCallback([wake = m_wake] {
            {
                std::lock_guard lock(wake->mutex);
                wake->published = true;
            }
            wake->cv.notify_one();
        });
    }
    ~Monitor() { m_mailbox->setPublishCallback({}); }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    /// One refresh: the new picture, if the mailbox had one.
    std::optional<Shown> refresh() {
        {
            std::unique_lock lock(m_wake->mutex);
            m_wake->cv.wait_for(lock, 1ms, [&] { return m_wake->published; });
            m_wake->published = false;
        }
        const render::CompositeMailbox::Selection selection = m_mailbox->acquire(playback::steadyNowNs() + kRefreshNs);
        if (!selection.changed || !selection.composite) {
            return std::nullopt;
        }
        const render::PreparedComposite& c = *selection.composite;
        if (const auto timing = m_renderer.renderTimed(selection.composite, kMonitorSize)) {
            m_drawn.compositeMs.add(timing->cpuMs);
            m_drawn.layersMissing += timing->composite.layersDrawn < static_cast<int>(c.layers.size()) ? 1 : 0;
        } else {
            ++m_drawn.errors;
        }
        m_last = Shown{playback::steadyNowNs(), c.publishedNs, c.seekEpoch, c.targetTime,
                       static_cast<int>(c.layers.size())};
        return m_last;
    }

    /// Refreshes until a picture satisfies `wanted` (returned) or `timeout` passes.
    std::optional<Shown> waitFor(const std::function<bool(const Shown&)>& wanted, std::chrono::milliseconds timeout) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < until) {
            if (const std::optional<Shown> shown = refresh(); shown && wanted(*shown)) {
                return shown;
            }
        }
        return std::nullopt;
    }

    /// Keeps refreshing (and drawing) for `duration`.
    void run(std::chrono::milliseconds duration) {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {
            (void)refresh();
        }
    }

    const Shown& last() const { return m_last; }

private:
    struct Wake {
        std::mutex mutex;
        std::condition_variable cv;
        bool published = false;
    };

    render::OffscreenRenderer& m_renderer;
    std::shared_ptr<render::CompositeMailbox> m_mailbox;
    Drawn& m_drawn;
    std::shared_ptr<Wake> m_wake = std::make_shared<Wake>();
    Shown m_last;
};

// ---- One pipeline --------------------------------------------------------------------------------------------------

editor::PlaybackSessionOptions sessionOptions() {
    editor::PlaybackSessionOptions options;
    options.audio.backend = audio::AudioBackend::Null; // pulled by NullDevice in real time
    options.video.scheduler.zeroCopy = true;           // the app's default (PlaybackBridge, WABCUT_ZERO_COPY unset)
    return options;
}

/// A playback pipeline over `project` as the app builds it, with its device thread and the monitor. Each one opens
/// its own decoders (its scheduler's DecoderPool, its audio feed): a new one is cold.
struct Pipeline {
    editor::PlaybackSession player;
    std::unique_ptr<NullDevice> device;
    std::unique_ptr<Monitor> monitor;

    Pipeline(const Project& project, const MediaKind& kind, render::OffscreenRenderer& renderer, Drawn& drawn)
        : player(*project.session, sessionOptions()) {
        if (kind.proxy != nullptr) {
            // The editor's proxy registry (MediaReadinessBridge::applyPreview): the 2160p item's half-size proxy.
            auto table = std::make_shared<playback::ProxyTable>();
            table->entries[project.item.value] = {pathToUtf8(mediaFile(kind.proxy)), 2};
            player.video().scheduler().setPreview({.useProxies = true, .decodeDivisor = 1, .proxies = table});
        }
        const auto started = player.start();
        INFO((started ? std::string() : started.error().message));
        REQUIRE(started);
        device = std::make_unique<NullDevice>(player.engine());
        monitor = std::make_unique<Monitor>(renderer, player.video().mailbox(), drawn);
    }

    render::CompositeMailbox& mailbox() { return *player.video().mailbox(); }
};

// ---- Measurements --------------------------------------------------------------------------------------------------

struct StartTimes {
    std::optional<double> frameMs;
    std::optional<double> soundMs;
    Ticks frameTarget{};
};

/// Presses play (from paused) and waits for the first picture of the play request (its seek epoch) and, with
/// `wantSound`, the first audible sample.
StartTimes pressPlay(Pipeline& p, bool wantSound, std::chrono::milliseconds timeout = 5000ms) {
    StartTimes out;
    const std::int64_t t0 = playback::steadyNowNs();
    p.device->arm(t0);
    p.player.play();
    const std::uint64_t epoch = p.mailbox().epoch();
    const auto until = std::chrono::steady_clock::now() + timeout;
    while ((!out.frameMs || (wantSound && !out.soundMs)) && std::chrono::steady_clock::now() < until) {
        if (const std::optional<Shown> shown = p.monitor->refresh();
            shown && !out.frameMs && shown->epoch >= epoch && shown->layers > 0) {
            out.frameMs = msBetween(t0, shown->ns);
            out.frameTarget = shown->target;
        }
        if (const std::int64_t heard = p.device->heardNs(); heard != 0 && !out.soundMs) {
            out.soundMs = msBetween(t0, heard);
        }
    }
    return out;
}

/// Paused at `position` after a jump, its picture on screen, then `settle` (the audio rings refill meanwhile).
bool parkAt(Pipeline& p, Ticks position, std::chrono::milliseconds settle) {
    p.player.pause();
    p.player.seek(position);
    const std::uint64_t epoch = p.mailbox().epoch();
    const auto wanted = [&](const Shown& c) { return c.epoch >= epoch && c.layers > 0 && c.target == position; };
    const bool shown = p.monitor->waitFor(wanted, 5000ms).has_value();
    p.monitor->run(settle);
    return shown;
}

struct ShuttleTimes {
    std::optional<double> play;    ///< L from paused
    std::optional<double> faster;  ///< L L: 1x -> 2x
    std::optional<double> reverse; ///< J: 2x -> -1x
};

ShuttleTimes shuttle(Pipeline& p) {
    ShuttleTimes out;
    if (!parkAt(p, kShuttleFrom, 120ms)) {
        return out;
    }
    // L: the first picture past the paused frame (the motion starts).
    std::int64_t t0 = playback::steadyNowNs();
    p.player.play();
    std::uint64_t epoch = p.mailbox().epoch();
    if (const auto shown = p.monitor->waitFor(
            [&](const Shown& c) { return c.epoch >= epoch && c.layers > 0 && c.target > kShuttleFrom; }, 5000ms)) {
        out.play = msBetween(t0, shown->ns);
    }
    p.monitor->run(400ms);
    // L L: the first picture decoded for 2x (published after the command). Pictures the scheduler had queued at 1x show
    // first, dropped through at the new speed.
    t0 = playback::steadyNowNs();
    const bool faster = p.player.setSpeed(kDouble);
    if (const auto shown =
            p.monitor->waitFor([&](const Shown& c) { return c.publishedNs >= t0 && c.layers > 0; }, 5000ms)) {
        out.faster = msBetween(t0, shown->ns);
    }
    p.monitor->run(400ms);
    // J: the first picture of the reverse request behind the one on screen when J was pressed.
    const Ticks onScreen = p.monitor->last().target;
    t0 = playback::steadyNowNs();
    const bool reversed = p.player.setSpeed(kReverse);
    epoch = p.mailbox().epoch();
    if (const auto shown = p.monitor->waitFor(
            [&](const Shown& c) { return c.epoch >= epoch && c.layers > 0 && c.target < onScreen; }, 5000ms)) {
        out.reverse = msBetween(t0, shown->ns);
    }
    p.monitor->run(200ms);
    p.player.pause();
    CHECK(faster);
    CHECK(reversed);
    return out;
}

struct StepTimes {
    perf::Samples steps;
    std::optional<double> afterIdle;
    int missed = 0;
    int wrongTarget = 0;
    int sessionsWhenIdle = -1; ///< the scheduler's open sessions just before the step after idling
};

/// stepFrames(+1) while paused -> that frame displayed.
std::optional<double> stepOnce(Pipeline& p, StepTimes& out) {
    const Ticks expected = p.monitor->last().target + frameTime(1);
    const std::int64_t t0 = playback::steadyNowNs();
    p.player.stepFrames(1);
    const std::uint64_t epoch = p.mailbox().epoch();
    const auto shown = p.monitor->waitFor([&](const Shown& c) { return c.epoch >= epoch && c.layers > 0; }, 5000ms);
    if (!shown) {
        ++out.missed;
        return std::nullopt;
    }
    out.wrongTarget += shown->target != expected ? 1 : 0;
    return msBetween(t0, shown->ns);
}

StepTimes frameSteps(Pipeline& p) {
    StepTimes out;
    if (!parkAt(p, frameTime(kStepFrom), 120ms)) {
        ++out.missed;
        return out;
    }
    for (int i = 0; i < kSteps; ++i) {
        if (const std::optional<double> ms = stepOnce(p, out)) {
            out.steps.add(*ms);
        }
    }
    // The scheduler closes sessions unused for 3 s (SchedulerOptions::sessionIdleMs): the next step opens one again.
    p.monitor->run(kIdle);
    out.sessionsWhenIdle = p.player.video().scheduler().stats().activeSessions;
    out.afterIdle = stepOnce(p, out);
    return out;
}

struct DragTimes {
    perf::Samples completed; ///< request -> the frame for that target displayed
    perf::Samples follow;    ///< request -> a frame for it or for a newer request displayed
    int requests = 0;
    int superseded = 0; ///< a newer request began before its frame was displayed (latest seek wins)
    int wrongTarget = 0;
    std::optional<double> finalMs;
};

DragTimes drag(Pipeline& p) {
    DragTimes out;
    if (!parkAt(p, Ticks{}, 120ms)) {
        return out;
    }
    // Forward over 0..8 s (about 5.5 frames per event: into the next GOP every ~5 events), then 2.7 s back.
    std::vector<std::int64_t> frames;
    for (std::int64_t k = 0; k <= 44; ++k) {
        frames.push_back(240 * k / 44);
    }
    for (std::int64_t k = 1; k <= 15; ++k) {
        frames.push_back(240 - 240 * k / 44);
    }
    struct Request {
        Ticks target{};
        std::int64_t ns = 0;
        std::uint64_t epoch = 0;
        std::int64_t shownNs = 0;
    };
    std::vector<Request> requests;
    requests.reserve(frames.size());
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (requests.size() < frames.size() &&
            std::chrono::steady_clock::now() >= start + kDragInterval * static_cast<int>(requests.size())) {
            Request q;
            q.target = frameTime(frames[requests.size()]);
            q.ns = playback::steadyNowNs();
            p.player.scrub(q.target);
            q.epoch = p.mailbox().epoch();
            requests.push_back(q);
        }
        if (requests.size() == frames.size() &&
            (requests.back().shownNs != 0 || playback::steadyNowNs() - requests.back().ns > 3'000'000'000LL)) {
            break;
        }
        const std::optional<Shown> shown = p.monitor->refresh();
        if (!shown || shown->layers == 0) {
            continue;
        }
        // Every scrub is a seek epoch of its own: a picture answers the request of its epoch.
        for (auto it = requests.rbegin(); it != requests.rend(); ++it) {
            if (it->epoch == shown->epoch) {
                if (it->shownNs == 0) {
                    it->shownNs = shown->ns;
                    out.wrongTarget += shown->target != it->target ? 1 : 0;
                }
                break;
            }
        }
    }
    std::int64_t answered = 0; // the earliest picture for this request or a newer one
    for (auto it = requests.rbegin(); it != requests.rend(); ++it) {
        if (it->shownNs != 0 && (answered == 0 || it->shownNs < answered)) {
            answered = it->shownNs;
        }
        if (answered != 0) {
            out.follow.add(msBetween(it->ns, answered));
        }
        if (it->shownNs != 0) {
            out.completed.add(msBetween(it->ns, it->shownNs));
        } else {
            ++out.superseded;
        }
    }
    out.requests = static_cast<int>(requests.size());
    if (!requests.empty() && requests.back().shownNs != 0) {
        out.finalMs = msBetween(requests.back().ns, requests.back().shownNs);
    }
    p.monitor->run(100ms);
    return out;
}

perf::Json definitions() {
    return perf::Json{
        {"pipeline",
         "editor::PlaybackSession as PlaybackBridge builds it: the audio engine as the master clock (Null backend, "
         "pulled by a device thread in real time), TimelineAudioPlayer + MediaAudioFeed, the PlaybackController's "
         "Scheduler on its own DecoderPool, zero-copy decoding on the renderer's D3D11 device (the app's default). "
         "Every time is steady-clock wall time of the real decode, mix and composite work."},
        {"displayed",
         "acquired from the PlaybackController's CompositeMailbox (acquire(now + 16.7 ms), as "
         "ProgramMonitor::synchronize; the stand-in wakes on the mailbox's publish callback, else every 1 ms) and "
         "composited by the Compositor on a headless QRhi into a 1920x1080 target (OffscreenRenderer::renderTimed: "
         "zero-copy import or upload, composite, QRhi::finish). No vsync wait: a window shows it at its next refresh, "
         "0-16.7 ms later."},
        {"first_sound",
         "play -> the first sample above -80 dBFS the engine renders for the device thread, at its own time within its "
         "5 ms block (a zero-latency device; a real one adds its output latency). first_sound_warm is bounded by the "
         "5 ms period, so it is informational."},
        {"first_frame", "play -> the first picture of the play request (its seek epoch) displayed."},
        {"cold",
         "a new pipeline (decoders closed, nothing cached; its own DecoderPool and audio feed), the sequence set and "
         "play pressed at once, from 0. The process is warm: each kind first plays once in a throwaway pipeline "
         "(codec libraries, the driver's decoder, compositor pipelines), and the OS file cache holds the clip."},
        {"warm",
         "the same pipeline after it played: paused, jumped back to 0, that picture shown, 250 ms settle (the audio "
         "rings refill), play again."},
        {"shuttle_play",
         "L from paused at 2 s -> the first picture past the paused frame displayed (the motion starts)."},
        {"shuttle_2x",
         "L L after 0.4 s at 1x: speed 2x -> the first picture published after the command displayed (decoded at "
         "the 2x cadence; pictures already queued at 1x show first)."},
        {"shuttle_reverse",
         "J after 0.4 s at 2x: speed -1x -> the first picture of the reverse request behind the one on screen."},
        {"frame_step",
         "paused, stepFrames(+1) -> that frame displayed; 30 steps from 5 s. frame_step_after_idle: one more step "
         "after 3.4 s idle (the scheduler closes sessions unused for 3 s)."},
        {"scrub",
         "paused, a playhead drag: 60 scrub() calls 16.7 ms apart (45 forward over 0-8 s, 15 back), each its own seek "
         "epoch. scrub_p50/p95: request -> the frame for that target displayed, over the completed requests; "
         "superseded: a newer request began first (latest seek wins), never displayed. scrub_follow_p50/p95: request "
         "-> the first picture for it or any newer request displayed (how far the picture trails the hand)."},
    };
}

void measureKind(const MediaKind& kind, render::OffscreenRenderer& renderer, perf::PerfResult& r) {
    const auto began = std::chrono::steady_clock::now();
    const std::string suffix = std::string("_") + kind.name;
    const Project project = makeProject(kind);
    Drawn drawn;
    perf::Json info{{"media", kind.file},
                    {"proxy", kind.proxy != nullptr ? perf::Json(kind.proxy) : perf::Json()},
                    {"sequence", std::to_string(kind.width) + "x" + std::to_string(kind.height) + " 30 fps"}};

    // Warm-up: the codec's libraries, the driver's decoder and the compositor's pipelines are set up once per process,
    // so "cold" below is a new pipeline (decoders closed, nothing cached), not a new process.
    bool wantSound = true;
    {
        Pipeline warmUp(project, kind, renderer, drawn);
        warmUp.player.setSequence(project.sequence);
        const bool shown = warmUp.monitor->waitFor([](const Shown& c) { return c.layers > 0; }, 10000ms).has_value();
        INFO(kind.name << ": no picture; decode " << warmUp.player.stats().video.decodeBackends);
        CHECK(shown);
        if (!shown) {
            info["error"] = "no picture within 10 s";
            r.details["kinds"][kind.name] = info;
            return;
        }
        const StartTimes t = pressPlay(warmUp, true);
        warmUp.player.pause();
        wantSound = t.soundMs.has_value();
        CHECK(wantSound);
    }

    // 1. Play -> first picture and first sound: cold (a new pipeline each run), then warm (the last one, back at 0).
    perf::Samples frameCold;
    perf::Samples soundCold;
    perf::Samples frameWarm;
    perf::Samples soundWarm;
    perf::Json coldTargets = perf::Json::array();
    int noFrame = 0;
    int noSound = 0;
    std::unique_ptr<Pipeline> p;
    for (int run = 0; run < kColdRuns; ++run) {
        p.reset(); // the previous pipeline is closed before the next one opens anything
        p = std::make_unique<Pipeline>(project, kind, renderer, drawn);
        p->player.setSequence(project.sequence);
        const StartTimes t = pressPlay(*p, wantSound);
        p->player.pause();
        addTo(frameCold, t.frameMs, noFrame);
        if (t.frameMs) {
            coldTargets.emplace_back(t.frameTarget.toSeconds());
        }
        if (wantSound) {
            addTo(soundCold, t.soundMs, noSound);
        }
    }
    if (frameCold.empty()) {
        FAIL_CHECK(kind.name << ": no first picture within 5 s in " << kColdRuns << " cold runs");
        info["error"] = "no first picture within 5 s";
        r.details["kinds"][kind.name] = info;
        return;
    }
    for (int run = 0; run < kWarmRuns; ++run) {
        if (!parkAt(*p, Ticks{}, 250ms)) {
            ++noFrame;
            continue;
        }
        const StartTimes t = pressPlay(*p, wantSound);
        p->player.pause();
        addTo(frameWarm, t.frameMs, noFrame);
        if (wantSound) {
            addTo(soundWarm, t.soundMs, noSound);
        }
    }

    // 2. J/K/L, 3. scrubbing, 4. frame steps (the idle step last), on the warm pipeline.
    perf::Samples shuttlePlay;
    perf::Samples shuttleFaster;
    perf::Samples shuttleReverse;
    int shuttleMissed = 0;
    for (int run = 0; run < kShuttleRuns; ++run) {
        const ShuttleTimes t = shuttle(*p);
        addTo(shuttlePlay, t.play, shuttleMissed);
        addTo(shuttleFaster, t.faster, shuttleMissed);
        addTo(shuttleReverse, t.reverse, shuttleMissed);
    }
    const DragTimes scrubbed = drag(*p);
    // P.1 P-2: the decoder sessions open right after the scrub (each jump used to open one while under the cap).
    const int scrubSessions = p->player.video().scheduler().stats().activeSessions;
    const StepTimes stepped = frameSteps(*p);

    const editor::PlaybackSessionStats stats = p->player.stats();
    info["decodeBackends"] = stats.video.decodeBackends; // "d3d11va x1"; "software x1" without hardware decoding
    info["transport"] = stats.video.transport;
    info["layers"] = {{"proxy", stats.video.proxyLayers},
                      {"original", stats.video.originalLayers},
                      {"decodeDivisor", stats.video.decodeDivisor}};
    info["video"] = {{"published", stats.video.published},
                     {"skippedLate", stats.video.skippedLate},
                     {"decodeErrors", stats.video.decodeErrors}};
    info["audio"] = {{"backend", stats.audioBackend},
                     {"framesFed", stats.audio.framesFed},
                     {"underrunFrames", stats.audio.underrunFrames},
                     {"feedErrors", stats.audio.feedErrors},
                     {"devicePulls", p->device->pulls()},
                     {"deviceLatePulls", p->device->latePulls()},
                     {"devicePullErrors", p->device->pullErrors()}};
    if (kind.proxy != nullptr) {
        INFO(kind.name << ": decode " << stats.video.decodeBackends);
        CHECK(stats.video.proxyLayers == 1); // the proxy is what played
    }
    p.reset();

    const auto gated = [&](const std::string& what, const perf::Samples& samples, const char* stat) {
        if (!samples.empty()) {
            r.addSamples(what + suffix, "ms", samples, stat);
        }
    };
    gated("first_frame_cold_p50_ms", frameCold, "p50");
    gated("first_frame_warm_p50_ms", frameWarm, "p50");
    gated("first_sound_cold_p50_ms", soundCold, "p50");
    if (!soundWarm.empty()) {
        r.note("first_sound_warm_p50_ms" + suffix, "ms", soundWarm.p50());
    }
    gated("shuttle_play_p50_ms", shuttlePlay, "p50");
    gated("shuttle_2x_p50_ms", shuttleFaster, "p50");
    gated("shuttle_reverse_p50_ms", shuttleReverse, "p50");
    gated("frame_step_p50_ms", stepped.steps, "p50");
    gated("frame_step_p95_ms", stepped.steps, "p95");
    if (stepped.afterIdle) {
        r.note("frame_step_after_idle_ms" + suffix, "ms", *stepped.afterIdle);
    }
    gated("scrub_p50_ms", scrubbed.completed, "p50");
    gated("scrub_p95_ms", scrubbed.completed, "p95");
    gated("scrub_follow_p50_ms", scrubbed.follow, "p50");
    gated("scrub_follow_p95_ms", scrubbed.follow, "p95");
    r.note("scrub_completed" + suffix, "requests", static_cast<double>(scrubbed.completed.size()));
    r.note("scrub_superseded" + suffix, "requests", static_cast<double>(scrubbed.superseded));
    r.note("scrub_sessions" + suffix, "sessions", static_cast<double>(scrubSessions));
    if (scrubbed.finalMs) {
        r.note("scrub_final_ms" + suffix, "ms", *scrubbed.finalMs);
    }
    if (!drawn.compositeMs.empty()) {
        r.note("composite_p50_ms" + suffix, "ms", drawn.compositeMs.p50());
    }

    info["start"] = {{"coldRuns", kColdRuns},
                     {"warmRuns", kWarmRuns},
                     {"coldFirstFrameTargetsS", coldTargets},
                     {"noFrame", noFrame},
                     {"noSound", noSound},
                     {"sound", wantSound}};
    info["shuttle"] = {{"runs", kShuttleRuns}, {"fromS", kShuttleFrom.toSeconds()}, {"missed", shuttleMissed}};
    info["scrub"] = {{"requests", scrubbed.requests},
                     {"completed", scrubbed.completed.size()},
                     {"superseded", scrubbed.superseded},
                     {"wrongTarget", scrubbed.wrongTarget},
                     {"finalMs", scrubbed.finalMs ? perf::Json(*scrubbed.finalMs) : perf::Json()}};
    info["frameStep"] = {{"steps", kSteps},
                         {"fromFrame", kStepFrom},
                         {"missed", stepped.missed},
                         {"wrongTarget", stepped.wrongTarget},
                         {"idleMs", kIdle.count()},
                         {"sessionsWhenIdle", stepped.sessionsWhenIdle},
                         {"afterIdleMs", stepped.afterIdle ? perf::Json(*stepped.afterIdle) : perf::Json()}};
    info["monitor"] = {{"pictures", drawn.compositeMs.size()},
                       {"compositeP50Ms", drawn.compositeMs.p50()},
                       {"compositeP95Ms", drawn.compositeMs.p95()},
                       {"errors", drawn.errors},
                       {"layersMissing", drawn.layersMissing}};
    info["seconds"] = perf::msSince(began) / 1000.0;
    r.details["kinds"][kind.name] = info;

    INFO(kind.name << ": " << info.dump());
    CHECK(noFrame == 0);
    CHECK(noSound == 0);
    CHECK(shuttleMissed == 0);
    CHECK(stepped.missed == 0);
    CHECK(scrubbed.finalMs.has_value());
    CHECK(drawn.errors == 0);
}

perf::PerfResult measurePlayback(const std::vector<const MediaKind*>& kinds) {
    perf::PerfResult r;
    ResetSharedDevice resetDevice; // declared first: runs after the renderer is gone
    render::OffscreenOptions options;
    options.role = render::OutputRole::Display;
    options.compositor = {render::TexturePool::Preview, 1, 48};
    // ProgramMonitor shares its device with the decoders (zero-copy): decoders opened from now on decode on it.
    options.shareDeviceWithDecoders = true;
    auto renderer = render::OffscreenRenderer::create(options);
    INFO((renderer ? std::string() : renderer.error().message));
    REQUIRE(renderer);
    r.details["definition"] = definitions();
    r.details["renderer"] = {{"backend", (*renderer)->backendName()},
                             {"adapter", (*renderer)->adapterName()},
                             {"zeroCopyDevice", !media::sharedD3D11Device().isCpu()},
                             {"monitor", "1920x1080"}};
    r.details["audio"] = {{"backend", "Null, pulled by a device thread at the steady clock's pace"},
                          {"periodMs", 5},
                          {"sampleRate", audio::AudioDeviceConfig{}.sampleRate},
                          {"audibleThreshold", kAudible}};
    r.details["sleep1msActualMs"] = sleepGranularityMs();
    r.details["kinds"] = perf::Json::object();
    for (const MediaKind* kind : kinds) {
        measureKind(*kind, **renderer, r);
    }
    const render::FrameImporterStats imports = (*renderer)->compositor().importer().stats();
    r.details["importer"] = {{"imports", imports.imports},
                             {"uploads", imports.uploads},
                             {"zeroCopyImports", imports.nativeImports},
                             {"nativeRejected", imports.nativeRejected}};
    return r;
}

} // namespace

TEST_CASE("Benchmark playback: transport response and scrub latency", "[perf][gpu][playback]") {
    std::vector<const MediaKind*> kinds;
    perf::Json missing = perf::Json::array();
    for (const MediaKind& kind : kKinds) {
        if (haveMedia(kind)) {
            kinds.push_back(&kind);
        } else {
            missing.emplace_back(kind.name);
        }
    }
    if (kinds.empty()) {
        SKIP("benchmark media missing in " WABCUT_PERF_MEDIA_DIR " (tests/perf/gen_perf_bench_media.py --out <dir>)");
    }
    if (!missing.empty()) {
        WARN("benchmark media missing, not measured: " << missing.dump());
    }
    GuiApplication app;
    TimerResolution timer;
    const perf::BenchmarkOutcome outcome = perf::runBenchmark("playback", [&] {
        perf::PerfResult r = measurePlayback(kinds);
        r.details["missingMedia"] = missing;
        return r;
    });
    if (!outcome.baselineFound) {
        WARN(outcome.describe());
    }
    INFO(outcome.describe());
    CHECK(outcome.ok());
}
