// Benchmark long VODs and high frame rates (A.10, docs/performance.md "Long VODs and high frame rates (A.10)"): what a
// long or fast gaming recording costs the editor, through the objects the app uses. Two recordings, made by
// tests/perf/gen_longvod_media.py (CTest perf.generate_longvod_media; about 33 GB, so no regular perf run makes them):
//   * longvod_h264_1440p60_2h.mkv: 2 hours of OBS-style 2560x1440 60 fps H.264 (NVENC CBR 24 Mb/s, GOP 2 s, 2 B-frames)
//     with three AAC tracks titled Game, Mic and Discord, edited in a 1440p60 sequence;
//   * highfps_hevc_1080p240_30m.mp4: 30 minutes of ShadowPlay-style 1920x1080 240 fps HEVC (CBR 50 Mb/s, GOP 2 s, no
//     B-frames) with two untitled AAC tracks, edited in a 1080p60 sequence (the preview runs at the sequence rate).
// Each case is a benchmark of its own (test-output/perf/longvod.<case>.json) and skips while its recordings are
// missing:
//   1. open: probe, and the app's first import (New Sequence from Clip, the import, the drop on the timeline);
//   2. first frame: a decoder's first picture at the start and in the middle, and the program monitor's first picture
//      of a new pipeline, at the start and after a jump into the middle;
//   3. scrub: 60 random jumps across the whole recording, a 60 Hz playhead drag in the middle, frame steps;
//   4. play: 20 s of real-time playback from the middle: dropped and late frames, frames delivered, CPU time;
//   5. peaks: waveform peaks of every audio track, cold and warm, one stream at a time and as the app requests them
//      (MediaPeakSource, asked by the test thread standing in for the GUI thread, whose 5 ms timer shows whether
//      anything blocks it);
//   6. memory: before opening, opened with its first picture, after 20 s of play, with every track's peaks, closed.
// The pipeline is bench_playback's: editor::PlaybackSession as PlaybackBridge builds it, the Null audio engine pulled
// by a device thread in real time, and a program monitor stand-in compositing every published picture on the GPU.
// Positions are relative (the middle is 1:00:00 of the 2-hour recording, 0:15:00 of the 30-minute one), so shorter
// recordings made with --minutes work too. r.details["definition"] says what each number includes.

#include "PerfHarness.h"

#include "MediaBridge.h" // MediaPeakSource: the timeline's and the source monitor's waveform peaks

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/media/Job.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/media/WaveformPeaks.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/playback/video/TransportClock.h>
#include <wabcut/render/CompositeMailbox.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSize>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
// clang-format off: windows.h first
#include <windows.h>
#include <timeapi.h>
// clang-format on
#include <malloc.h>
#pragma comment(lib, "winmm.lib")
#endif

#ifndef WABCUT_PERF_LONGVOD_DIR
#error "WABCUT_PERF_LONGVOD_DIR must name the directory tests/perf/gen_longvod_media.py writes"
#endif

using namespace wabcut;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::int64_t kRefreshNs = 16'666'667;                   // a 60 Hz monitor
constexpr auto kDragInterval = std::chrono::microseconds(16'667); // playhead drag events (60 Hz)
constexpr auto kDevicePeriod = 5ms;                               // the device thread's callback period
constexpr auto kPictureTimeout = 10000ms;                         // a picture that takes longer counts as missed
constexpr int kOpenRuns = 5;                                      // probes and imports per recording
constexpr int kColdRuns = 3;                                      // new decoders / pipelines per first picture
constexpr int kSeeks = 60;                                        // random jumps across the recording
constexpr std::uint64_t kSeekSeed = 0xA10;                        // the same jumps every run
constexpr int kSteps = 30;                                        // frame steps from the middle
constexpr auto kPlayFor = 20000ms;                                // real-time playback from the middle
constexpr auto kGuiTick = 5ms;                                    // the stand-in GUI thread's timer
constexpr std::size_t kTimelineColumns = 1600;                    // a zoomed-out timeline's waveform columns
constexpr auto kPeaksTimeout = std::chrono::minutes(30);          // every track's peaks, as the app requests them
constexpr MediaId kPeaksMedia{1};                                 // the media id MediaPeakSource knows the file by
const QSize kMonitorSize(1920, 1080);

struct Vod {
    const char* name; ///< the metric suffix
    const char* file; ///< in WABCUT_PERF_LONGVOD_DIR
    const char* what;
    int width;        ///< the sequence the scrub, play and memory cases edit it in
    int height;
    Rate rate;
};

constexpr Vod kVods[] = {
    {"h264_1440p60", "longvod_h264_1440p60_2h.mkv",
     "2 hours, OBS-style MKV: 2560x1440 60 fps H.264 (NVENC CBR 24 Mb/s, GOP 2 s, 2 B-frames), three AAC tracks titled "
     "Game, Mic and Discord; edited in a 2560x1440 60 fps sequence",
     2560, 1440, kRate60},
    {"hevc_1080p240", "highfps_hevc_1080p240_30m.mp4",
     "30 minutes, ShadowPlay-style MP4: 1920x1080 240 fps HEVC (CBR 50 Mb/s, GOP 2 s, no B-frames), two untitled AAC "
     "tracks; edited in a 1920x1080 60 fps sequence",
     1920, 1080, kRate60},
};

fs::path vodFile(const Vod& vod) {
    return fs::path(WABCUT_PERF_LONGVOD_DIR) / vod.file;
}

/// The recordings this run measures: skips the test case when there are none, warns about the missing ones.
std::vector<const Vod*> vodsOrSkip(perf::Json& missing) {
    std::vector<const Vod*> vods;
    for (const Vod& vod : kVods) {
        std::error_code ec;
        if (fs::is_regular_file(vodFile(vod), ec)) {
            vods.push_back(&vod);
        } else {
            missing.emplace_back(vod.file);
        }
    }
    if (vods.empty()) {
        SKIP("long-VOD recordings missing in " WABCUT_PERF_LONGVOD_DIR
             " (about 33 GB: `ctest --preset win-msvc -L longvod`, or tests/perf/gen_longvod_media.py --out <dir>)");
    }
    if (!missing.empty()) {
        WARN("long-VOD recordings missing, not measured: " << missing.dump());
    }
    return vods;
}

double msBetween(std::int64_t fromNs, std::int64_t toNs) {
    return static_cast<double>(toNs - fromNs) / 1e6;
}

std::int64_t nsOf(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

double mbOf(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void addTo(perf::Samples& samples, const std::optional<double>& value, int& missed) {
    if (value) {
        samples.add(*value);
    } else {
        ++missed;
    }
}

/// Whether this tree's AudioStreamInfo carries the stream's title (A.10 multi-track ingest).
template <class Stream>
concept TitledStream = requires(const Stream& stream) { stream.title; };

/// The stream's title (the MKV track name or MP4 handler name) where AudioStreamInfo carries one; empty otherwise.
template <class Stream>
std::string streamTitle([[maybe_unused]] const Stream& stream) {
    if constexpr (TitledStream<Stream>) {
        return std::string(stream.title);
    } else {
        return {};
    }
}

MediaInfo probeVod(const Vod& vod) {
    auto info = media::probe(pathToUtf8(vodFile(vod)));
    INFO(vod.file << ": " << (info ? std::string() : info.error().message));
    REQUIRE(info);
    REQUIRE(info->hasVideo());
    return std::move(*info);
}

perf::Json probeJson(const MediaInfo& m) {
    const VideoStreamInfo& v = m.video.front();
    perf::Json streams = perf::Json::array();
    for (const AudioStreamInfo& a : m.audio) {
        streams.push_back(perf::Json{{"index", a.index},
                                     {"codec", a.codec},
                                     {"sampleRate", a.sampleRate.num},
                                     {"channels", a.channels},
                                     {"title", streamTitle(a)}});
    }
    return perf::Json{{"container", m.container},
                      {"durationS", m.duration.toSeconds()},
                      {"fileGb", static_cast<double>(m.fileSize) / 1e9},
                      {"video",
                       {{"codec", v.codec},
                        {"profile", v.profile},
                        {"width", v.width},
                        {"height", v.height},
                        {"fps", v.frameRate.toDouble()},
                        {"variableFrameRate", v.variableFrameRate},
                        {"frameCount", v.frameCount},
                        {"pixelFormat", v.pixelFormat}}},
                      {"audio", streams}};
}

int g_argc = 1;
char g_argv0[] = "wabcut_perf_tests";
char* g_argv[] = {g_argv0, nullptr};

/// The Qt application object, unless the suite made one: a QGuiApplication for the renderer (shader resources,
/// platform state; no window), a QCoreApplication where only MediaPeakSource's queued signal needs one.
class Application {
public:
    explicit Application(bool gui) {
        if (QCoreApplication::instance() != nullptr) {
            return;
        }
        if (gui) {
            m_app = std::make_unique<QGuiApplication>(g_argc, g_argv);
        } else {
            m_app = std::make_unique<QCoreApplication>(g_argc, g_argv);
        }
    }

private:
    std::unique_ptr<QCoreApplication> m_app;
};

/// 1 ms timer resolution while measuring: the monitor's 1 ms waits, the device thread's 5 ms periods and the stand-in
/// GUI timer would round up to the 15.6 ms system tick otherwise (Windows grants a process without a visible window no
/// finer one).
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

/// The freed heap goes back to the system before a memory baseline (as bench_timeline's memory benchmark does).
void releaseFreedHeap() {
#if defined(_WIN32)
    (void)_heapmin();
#endif
}

// ---- The project ---------------------------------------------------------------------------------------------------

/// A project as the editor makes it: the recording imported and dropped at 0 into the Vod's sequence (V1 and one audio
/// track per stream, linked).
struct Project {
    std::unique_ptr<EditSession> session;
    SequenceId sequence;
    MediaId item;
    Ticks length{}; ///< the sequence's content: the whole recording
};

Project makeProject(const Vod& vod) {
    edit::SequenceSettings settings;
    settings.name = vod.name;
    settings.frameRate = vod.rate;
    settings.width = vod.width;
    settings.height = vod.height;
    auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    Project p;
    p.sequence = created->id;
    p.session = std::make_unique<EditSession>(std::move(created->state));
    const std::vector<fs::path> files{vodFile(vod)};
    const editor::MediaImportResult imported = editor::importMediaFiles(*p.session, files);
    INFO(vod.file << (imported.failed.empty() ? std::string() : ": " + imported.failed.front().error.message));
    REQUIRE(imported.added.size() == 1);
    p.item = imported.added.front();
    REQUIRE(editor::dropMedia(*p.session, {.media = p.item, .sequence = p.sequence, .at = Ticks{}}));
    const Sequence* sequence = p.session->snapshot()->findSequence(p.sequence);
    REQUIRE(sequence != nullptr);
    p.length = sequence->contentEnd();
    REQUIRE(p.length > Ticks{});
    return p;
}

/// The middle of the recording on the sequence's frame grid.
Ticks middleOf(const Project& p, Rate rate) {
    return snapToFrame(Ticks{p.length.value / 2}, rate);
}

// ---- The audio device ----------------------------------------------------------------------------------------------

/// The audio device's part on the Null backend: a thread pulls 5 ms blocks at the steady clock's pace, as a device
/// calls back (the engine's clock, which the picture follows, moves only as blocks are pulled).
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

    std::uint64_t pulls() const { return m_pulls.load(); }
    /// Blocks pulled more than 2.5 ms after their time.
    std::uint64_t latePulls() const { return m_late.load(); }
    std::uint64_t pullErrors() const { return m_errors.load(); }

private:
    void run() {
        std::vector<float> block(static_cast<std::size_t>(m_rate / 200) * static_cast<std::size_t>(m_channels)); // 5 ms
        auto due = Clock::now();
        while (!m_stop.load(std::memory_order_relaxed)) {
            due += kDevicePeriod;
            if (Clock::now() - due > 50ms) {
                due = Clock::now(); // stalled: carry on from now, as a device does after a glitch
            }
            std::this_thread::sleep_until(due);
            const auto now = Clock::now();
            m_pulls.fetch_add(1, std::memory_order_relaxed);
            if (now - due > 2500us) {
                m_late.fetch_add(1, std::memory_order_relaxed);
            }
            if (!m_engine.pull(block, now)) {
                m_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    audio::AudioEngine& m_engine;
    const int m_rate;
    const int m_channels;
    std::atomic<bool> m_stop{false};
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

/// What the monitor drew over a recording's pipelines.
struct Drawn {
    perf::Samples compositeMs; ///< renderTimed() per picture: import or upload + composite, GPU finished
    int errors = 0;
};

/// bench_playback's program monitor stand-in, on this thread: woken by the mailbox's publish callback (else every
/// millisecond), it acquires the picture for the refresh that becomes visible one interval later and draws it with the
/// Compositor. No vsync wait: its times are the pipeline's, without the 0..16.7 ms a display adds.
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
        } else {
            ++m_drawn.errors;
        }
        m_last = Shown{playback::steadyNowNs(), c.publishedNs, c.seekEpoch, c.targetTime,
                       static_cast<int>(c.layers.size())};
        return m_last;
    }

    /// Refreshes until a picture satisfies `wanted` (returned) or `timeout` passes.
    std::optional<Shown> waitFor(const std::function<bool(const Shown&)>& wanted, std::chrono::milliseconds timeout) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            if (const std::optional<Shown> shown = refresh(); shown && wanted(*shown)) {
                return shown;
            }
        }
        return std::nullopt;
    }

    /// Keeps refreshing (and drawing) for `duration`.
    void run(std::chrono::milliseconds duration) {
        const auto until = Clock::now() + duration;
        while (Clock::now() < until) {
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

/// A playback pipeline over `project` as the app builds it, with its device thread and the monitor. Each one opens its
/// own decoders (its scheduler's DecoderPool, its audio feed): a new one is cold.
struct Pipeline {
    editor::PlaybackSession player;
    std::unique_ptr<NullDevice> device;
    std::unique_ptr<Monitor> monitor;

    Pipeline(const Project& project, render::OffscreenRenderer& renderer, Drawn& drawn)
        : player(*project.session, sessionOptions()) {
        const auto started = player.start();
        INFO((started ? std::string() : started.error().message));
        REQUIRE(started);
        device = std::make_unique<NullDevice>(player.engine());
        monitor = std::make_unique<Monitor>(renderer, player.video().mailbox(), drawn);
    }

    render::CompositeMailbox& mailbox() { return *player.video().mailbox(); }
    playback::SchedulerStats schedulerStats() { return player.video().scheduler().stats(); }
};

/// Steady time `t0Ns` -> the first picture satisfying `wanted` displayed, ms; nothing within the timeout.
std::optional<double> pictureAfter(Pipeline& p, std::int64_t t0Ns, const std::function<bool(const Shown&)>& wanted,
                                   std::chrono::milliseconds timeout = kPictureTimeout) {
    if (const std::optional<Shown> shown = p.monitor->waitFor(wanted, timeout)) {
        return msBetween(t0Ns, shown->ns);
    }
    return std::nullopt;
}

/// setSequence on the pipeline -> its first picture (paused at the start), ms.
std::optional<double> openSequence(Pipeline& p, SequenceId sequence) {
    const std::int64_t t0 = playback::steadyNowNs();
    p.player.setSequence(sequence);
    const std::uint64_t epoch = p.mailbox().epoch();
    return pictureAfter(p, t0, [&](const Shown& c) { return c.epoch >= epoch && c.layers > 0; });
}

/// Paused at `position` after a jump, its picture on screen, then `settle` (the audio rings refill meanwhile).
bool parkAt(Pipeline& p, Ticks position, std::chrono::milliseconds settle) {
    p.player.pause();
    p.player.seek(position);
    const std::uint64_t epoch = p.mailbox().epoch();
    const auto wanted = [&](const Shown& c) { return c.epoch >= epoch && c.layers > 0 && c.target == position; };
    const bool shown = p.monitor->waitFor(wanted, kPictureTimeout).has_value();
    p.monitor->run(settle);
    return shown;
}

/// The codec's libraries, the driver's decoder and the compositor's pipelines are set up once per process: one
/// throwaway pipeline per recording first, so "cold" below is a new pipeline (decoders closed), not a new process.
void warmUp(const Project& project, render::OffscreenRenderer& renderer) {
    Drawn drawn;
    Pipeline p(project, renderer, drawn);
    const std::optional<double> first = openSequence(p, project.sequence);
    INFO("no first picture within 10 s; decode " << p.player.stats().video.decodeBackends);
    CHECK(first.has_value());
}

/// The program monitor's renderer: a headless QRhi sharing its D3D11 device with the decoders (zero-copy), as
/// ProgramMonitor does. Declare a ResetSharedDevice before it.
std::unique_ptr<render::OffscreenRenderer> makeRenderer(perf::PerfResult& r) {
    render::OffscreenOptions options;
    options.role = render::OutputRole::Display;
    options.compositor = {render::TexturePool::Preview, 1, 48};
    options.shareDeviceWithDecoders = true;
    auto renderer = render::OffscreenRenderer::create(options);
    INFO((renderer ? std::string() : renderer.error().message));
    REQUIRE(renderer);
    r.details["renderer"] = {{"backend", (*renderer)->backendName()},
                             {"adapter", (*renderer)->adapterName()},
                             {"zeroCopyDevice", !media::sharedD3D11Device().isCpu()},
                             {"monitor", "1920x1080"}};
    return std::move(*renderer);
}

perf::Json pipelineDefinition() {
    return perf::Json(
        "editor::PlaybackSession as PlaybackBridge builds it: the audio engine as the master clock (Null backend, "
        "pulled by a device thread in real time), TimelineAudioPlayer + MediaAudioFeed (every audio track of the "
        "recording), the PlaybackController's Scheduler on its own DecoderPool, zero-copy decoding on the renderer's "
        "D3D11 device. A picture counts when the monitor stand-in acquired it from the CompositeMailbox (acquire(now + "
        "16.7 ms), woken by the publish callback, else every 1 ms) and composited it into a 1920x1080 target on a "
        "headless QRhi (GPU finished); a window shows it at its next refresh, 0-16.7 ms later.");
}

// ---- 1. Open -------------------------------------------------------------------------------------------------------

struct OpenRun {
    double describeMs = 0.0; ///< editor::describeMediaFile: probe + fingerprint (ProjectLifecycle's settingsFor)
    double sequenceMs = 0.0; ///< the new project with its sequence from the clip
    double importMs = 0.0;   ///< editor::importMediaFiles: probe + fingerprint again, one undo step
    double dropMs = 0.0;     ///< editor::dropMedia at the start of the sequence: the insert edit
    double totalMs = 0.0;
    std::size_t placed = 0;  ///< clips the drop placed
    perf::Json tracks = perf::Json::array();
    perf::Json sequence;
};

/// The app's first import of a recording into a new project (ProjectLifecycle::importFiles on a fresh project):
/// New Sequence from Clip (the first video's size and rate), the import, then the drop on the timeline at 0.
OpenRun openLikeTheApp(const Vod& vod) {
    OpenRun run;
    const fs::path file = vodFile(vod);
    const auto started = Clock::now();
    auto t0 = Clock::now();
    const auto described = editor::describeMediaFile(file);
    run.describeMs = perf::msSince(t0);
    INFO(vod.file << ": " << (described ? std::string() : described.error().message));
    REQUIRE(described);
    REQUIRE(described->info.hasVideo());
    t0 = Clock::now();
    edit::SequenceSettings settings;
    const VideoStreamInfo& v = described->info.video.front();
    const bool sideways = v.rotationDegrees == 90 || v.rotationDegrees == 270;
    settings.width = sideways ? v.height : v.width;
    settings.height = sideways ? v.width : v.height;
    if (v.frameRate.isValid() && !v.variableFrameRate) {
        settings.frameRate = v.frameRate;
    }
    auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    const SequenceId sequenceId = created->id;
    EditSession session(std::move(created->state));
    run.sequenceMs = perf::msSince(t0);
    t0 = Clock::now();
    const std::vector<fs::path> files{file};
    const editor::MediaImportResult imported = editor::importMediaFiles(session, files);
    run.importMs = perf::msSince(t0);
    INFO((imported.failed.empty() ? std::string() : imported.failed.front().error.message));
    REQUIRE(imported.added.size() == 1);
    t0 = Clock::now();
    const auto dropped =
        editor::dropMedia(session, {.media = imported.added.front(), .sequence = sequenceId, .at = Ticks{}});
    run.dropMs = perf::msSince(t0);
    INFO((dropped ? std::string() : dropped.error().message));
    REQUIRE(dropped);
    run.totalMs = perf::msSince(started);
    run.placed = dropped->placed.size();
    const Sequence* sequence = session.snapshot()->findSequence(sequenceId);
    REQUIRE(sequence != nullptr);
    for (const TrackPtr& track : sequence->audioTracks) {
        run.tracks.push_back(perf::Json{{"name", track->name}, {"clips", track->clips.size()}});
    }
    run.sequence = perf::Json{{"width", sequence->width},
                              {"height", sequence->height},
                              {"fps", sequence->frameRate.toDouble()},
                              {"videoTracks", sequence->videoTracks.size()},
                              {"audioTracks", sequence->audioTracks.size()}};
    return run;
}

perf::PerfResult measureOpen(const std::vector<const Vod*>& vods) {
    perf::PerfResult r;
    r.details["definition"] = {
        {"probe", "media::probe of the recording (container and stream headers; the MP4's sample index), 5 runs"},
        {"open",
         "the app's first import into a new project, 5 runs, each a new project: editor::describeMediaFile (probe + "
         "fingerprint: New Sequence from Clip takes the first video's size and rate), edit::addSequence, "
         "editor::importMediaFiles (probe + fingerprint again, one undo step), editor::dropMedia at 0 (the insert "
         "edit: V1 and one audio track per stream). open = the four together. The OS file cache holds the headers "
         "after the first run (probe_first / open_first)."},
    };
    for (const Vod* vod : vods) {
        const std::string suffix = std::string("_") + vod->name;
        const std::string path = pathToUtf8(vodFile(*vod));
        perf::Samples probes;
        std::optional<MediaInfo> info;
        for (int i = 0; i < kOpenRuns; ++i) {
            const auto t0 = Clock::now();
            auto probed = media::probe(path);
            probes.add(perf::msSince(t0));
            INFO(vod->file << ": " << (probed ? std::string() : probed.error().message));
            REQUIRE(probed);
            info = std::move(*probed);
        }
        REQUIRE(info->hasVideo());
        perf::Samples describes;
        perf::Samples sequences;
        perf::Samples imports;
        perf::Samples drops;
        perf::Samples totals;
        OpenRun last;
        for (int i = 0; i < kOpenRuns; ++i) {
            last = openLikeTheApp(*vod);
            describes.add(last.describeMs);
            sequences.add(last.sequenceMs);
            imports.add(last.importMs);
            drops.add(last.dropMs);
            totals.add(last.totalMs);
        }
        r.addSamples("probe_p50_ms" + suffix, "ms", probes);
        r.note("probe_first_ms" + suffix, "ms", probes.values.front());
        r.addSamples("describe_p50_ms" + suffix, "ms", describes);
        r.addSamples("import_p50_ms" + suffix, "ms", imports);
        r.addSamples("drop_p50_ms" + suffix, "ms", drops);
        r.addSamples("open_p50_ms" + suffix, "ms", totals);
        r.note("open_first_ms" + suffix, "ms", totals.values.front());
        r.note("audio_streams" + suffix, "streams", static_cast<double>(info->audio.size()));
        r.note("duration_min" + suffix, "min", info->duration.toSeconds() / 60.0);
        r.note("file_gb" + suffix, "GB", static_cast<double>(info->fileSize) / 1e9);
        r.details["recordings"][vod->name] = {{"file", vod->file},
                                              {"what", vod->what},
                                              {"probe", probeJson(*info)},
                                              {"sequenceFromClip", last.sequence},
                                              {"clipsPlaced", last.placed},
                                              {"audioTracksAfterDrop", last.tracks},
                                              {"sequenceMsP50", sequences.p50()}};
        // Every stream ends up on the timeline: V1 and one audio track per stream.
        CHECK(last.placed == 1 + info->audio.size());
    }
    return r;
}

// ---- 2. First frame ------------------------------------------------------------------------------------------------

perf::PerfResult measureFirstFrame(const std::vector<const Vod*>& vods) {
    perf::PerfResult r;
    ResetSharedDevice resetDevice; // declared first: runs after the renderer is gone
    const std::unique_ptr<render::OffscreenRenderer> renderer = makeRenderer(r);
    r.details["definition"] = {
        {"pipeline", pipelineDefinition()},
        {"decoder_first_frame",
         "media::VideoDecoder::open with the default options (hardware backends first, CPU frames) + frameAt(the first "
         "frame), a new decoder each of 3 runs: what the media layer costs before anything is shown"},
        {"decoder_jump_frame",
         "a new decoder + frameAt(the middle): the seek to the keyframe before it and the decode forward from there; "
         "decoder_jump_decoded_frames is how many frames the codec decoded to reach it"},
        {"first_frame",
         "a new pipeline (its own DecoderPool and audio feed), setSequence -> the first picture displayed (paused at "
         "the start): what a user waits for after opening the project, 3 runs"},
        {"jump_frame",
         "a new pipeline, setSequence and at once seek(the middle: 1:00:00 of the 2-hour recording, 0:15:00 of the "
         "30-minute one) -> that frame displayed: a jump into the middle of a long VOD right after opening it, 3 runs"},
    };
    for (const Vod* vod : vods) {
        const std::string suffix = std::string("_") + vod->name;
        const std::string path = pathToUtf8(vodFile(*vod));
        const MediaInfo info = probeVod(*vod);
        const VideoStreamInfo& v = info.video.front();
        const Ticks halfway{info.duration.value / 2};
        const Ticks sourceMiddle = v.frameRate.isValid() ? snapToFrame(halfway, v.frameRate) : halfway;

        // The decoder alone.
        perf::Samples decoderFirst;
        perf::Samples decoderJump;
        std::string backend;
        std::uint64_t jumpDecoded = 0;
        for (int i = 0; i < kColdRuns; ++i) {
            {
                const auto t0 = Clock::now();
                auto decoder = media::VideoDecoder::open(path);
                INFO(vod->file << ": " << (decoder ? std::string() : decoder.error().message));
                REQUIRE(decoder);
                const auto frame = (*decoder)->frameAt((*decoder)->firstFrameTime());
                INFO((frame ? std::string() : frame.error().message));
                REQUIRE(frame);
                decoderFirst.add(perf::msSince(t0));
                backend = std::string((*decoder)->backendName());
            }
            {
                const auto t0 = Clock::now();
                auto decoder = media::VideoDecoder::open(path);
                REQUIRE(decoder);
                const auto frame = (*decoder)->frameAt(sourceMiddle);
                INFO((frame ? std::string() : frame.error().message));
                REQUIRE(frame);
                decoderJump.add(perf::msSince(t0));
                jumpDecoded = (*decoder)->stats().framesDecoded;
            }
        }

        // The pipeline: a new one per run.
        const Project project = makeProject(*vod);
        warmUp(project, *renderer);
        const Ticks middle = middleOf(project, vod->rate);
        perf::Samples pipelineFirst;
        perf::Samples pipelineJump;
        int missed = 0;
        std::string decodeBackends;
        Drawn drawn;
        for (int i = 0; i < kColdRuns; ++i) {
            {
                Pipeline p(project, *renderer, drawn);
                addTo(pipelineFirst, openSequence(p, project.sequence), missed);
            }
            {
                Pipeline p(project, *renderer, drawn);
                const std::int64_t t0 = playback::steadyNowNs();
                p.player.setSequence(project.sequence);
                p.player.seek(middle);
                const std::uint64_t epoch = p.mailbox().epoch();
                addTo(pipelineJump, pictureAfter(p, t0, [&](const Shown& c) {
                          return c.epoch >= epoch && c.layers > 0 && c.target == middle;
                      }),
                      missed);
                decodeBackends = p.player.stats().video.decodeBackends;
            }
        }

        r.addSamples("decoder_first_frame_p50_ms" + suffix, "ms", decoderFirst);
        r.addSamples("decoder_jump_frame_p50_ms" + suffix, "ms", decoderJump);
        r.note("decoder_jump_decoded_frames" + suffix, "frames", static_cast<double>(jumpDecoded));
        if (!pipelineFirst.empty()) {
            r.addSamples("first_frame_p50_ms" + suffix, "ms", pipelineFirst);
        }
        if (!pipelineJump.empty()) {
            r.addSamples("jump_frame_p50_ms" + suffix, "ms", pipelineJump);
        }
        r.details["recordings"][vod->name] = {{"file", vod->file},
                                              {"what", vod->what},
                                              {"decoderBackend", backend},
                                              {"pipelineDecodeBackends", decodeBackends},
                                              {"middleS", middle.toSeconds()},
                                              {"sourceMiddleS", sourceMiddle.toSeconds()},
                                              {"missed", missed},
                                              {"compositeP50Ms", drawn.compositeMs.p50()},
                                              {"monitorErrors", drawn.errors}};
        INFO(vod->name << ": " << r.details["recordings"][vod->name].dump());
        CHECK(missed == 0);
        CHECK(drawn.errors == 0);
    }
    return r;
}

// ---- 3. Scrub ------------------------------------------------------------------------------------------------------

/// Whether this tree's SchedulerStats counts the decoder pool's codec frames (A.10 WP4b).
template <class Stats>
concept CountsPoolFrames = requires(const Stats& stats) {
    stats.poolFramesDecoded;
    stats.poolFramesDelivered;
};

/// The decoder pool's codec frames decoded and frames delivered, where SchedulerStats counts them: more decoded than
/// delivered = frames decoded only to step over them (a seek's walk from the keyframe, a 240 fps source read at a
/// 60 fps sequence's times).
template <class Stats>
std::optional<std::pair<std::uint64_t, std::uint64_t>> poolFrames([[maybe_unused]] const Stats& stats) {
    if constexpr (CountsPoolFrames<Stats>) {
        return std::make_pair(static_cast<std::uint64_t>(stats.poolFramesDecoded),
                              static_cast<std::uint64_t>(stats.poolFramesDelivered));
    } else {
        return std::nullopt;
    }
}

/// Decoded and delivered frames between two stats readings; nullopt where they are not counted.
std::optional<std::pair<std::uint64_t, std::uint64_t>> poolFramesBetween(const playback::SchedulerStats& before,
                                                                          const playback::SchedulerStats& after) {
    const auto a = poolFrames(before);
    const auto b = poolFrames(after);
    if (!a || !b) {
        return std::nullopt;
    }
    return std::make_pair(b->first - a->first, b->second - a->second);
}

struct SeekTimes {
    perf::Samples ms; ///< scrub(target) -> the picture of that request displayed
    int missed = 0;
    int wrongTarget = 0;
};

/// 60 jumps to random frames of the whole recording (a fixed seed), one at a time: scrub() (the previous picture
/// holds, as on a click in the ruler) -> the picture of that request displayed, then 50 ms on it.
SeekTimes randomSeeks(Pipeline& p, Ticks length, Rate rate) {
    SeekTimes out;
    std::mt19937_64 rng(kSeekSeed);
    const auto frames = static_cast<std::uint64_t>(std::max<std::int64_t>(1, ticksToFrames(length, rate) - 1));
    for (int i = 0; i < kSeeks; ++i) {
        const Ticks target = framesToTicks(static_cast<std::int64_t>(rng() % frames), rate);
        const std::int64_t t0 = playback::steadyNowNs();
        p.player.scrub(target);
        const std::uint64_t epoch = p.mailbox().epoch();
        const auto shown = p.monitor->waitFor([&](const Shown& c) { return c.epoch >= epoch && c.layers > 0; },
                                              kPictureTimeout);
        if (!shown) {
            ++out.missed;
            continue;
        }
        out.ms.add(msBetween(t0, shown->ns));
        out.wrongTarget += shown->target != target ? 1 : 0;
        p.monitor->run(50ms);
    }
    return out;
}

struct DragTimes {
    perf::Samples completed; ///< request -> the frame for that target displayed
    perf::Samples follow;    ///< request -> a frame for it or for a newer request displayed
    int requests = 0;
    int superseded = 0;      ///< a newer request began before its frame was displayed (latest seek wins)
};

/// bench_playback's playhead drag, from `from`: 60 scrub() calls 16.7 ms apart, 45 forward over 8 s, then 15 back
/// over 2.7 s, each its own seek epoch.
DragTimes drag(Pipeline& p, Ticks from, Rate rate) {
    DragTimes out;
    if (!parkAt(p, from, 120ms)) {
        return out;
    }
    const std::int64_t start = ticksToFrames(from, rate);
    const std::int64_t span = ticksToFrames(Ticks::fromSeconds(8), rate);
    std::vector<std::int64_t> frames;
    for (std::int64_t k = 0; k <= 44; ++k) {
        frames.push_back(start + span * k / 44);
    }
    for (std::int64_t k = 1; k <= 15; ++k) {
        frames.push_back(start + span - span * k / 44);
    }
    struct Request {
        Ticks target{};
        std::int64_t ns = 0;
        std::uint64_t epoch = 0;
        std::int64_t shownNs = 0;
    };
    std::vector<Request> requests;
    requests.reserve(frames.size());
    const auto began = Clock::now();
    for (;;) {
        if (requests.size() < frames.size() &&
            Clock::now() >= began + kDragInterval * static_cast<int>(requests.size())) {
            Request q;
            q.target = framesToTicks(frames[requests.size()], rate);
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
        for (auto it = requests.rbegin(); it != requests.rend(); ++it) {
            if (it->epoch == shown->epoch) {
                if (it->shownNs == 0) {
                    it->shownNs = shown->ns;
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
    p.monitor->run(100ms);
    return out;
}

struct StepTimes {
    perf::Samples steps;
    int missed = 0;
    int wrongTarget = 0;
};

/// Paused at `from`, stepFrames(+1) -> that frame displayed, 30 times. A sequence frame of the 240 fps recording in its
/// 60 fps sequence is 4 source frames.
StepTimes frameSteps(Pipeline& p, Ticks from, Rate rate) {
    StepTimes out;
    if (!parkAt(p, from, 120ms)) {
        ++out.missed;
        return out;
    }
    for (int i = 0; i < kSteps; ++i) {
        const Ticks expected = p.monitor->last().target + framesToTicks(1, rate);
        const std::int64_t t0 = playback::steadyNowNs();
        p.player.stepFrames(1);
        const std::uint64_t epoch = p.mailbox().epoch();
        const auto shown = p.monitor->waitFor([&](const Shown& c) { return c.epoch >= epoch && c.layers > 0; },
                                              kPictureTimeout);
        if (!shown) {
            ++out.missed;
            continue;
        }
        out.steps.add(msBetween(t0, shown->ns));
        out.wrongTarget += shown->target != expected ? 1 : 0;
    }
    return out;
}

perf::PerfResult measureScrub(const std::vector<const Vod*>& vods) {
    perf::PerfResult r;
    ResetSharedDevice resetDevice; // declared first: runs after the renderer is gone
    const std::unique_ptr<render::OffscreenRenderer> renderer = makeRenderer(r);
    r.details["definition"] = {
        {"pipeline", pipelineDefinition()},
        {"seek",
         "paused, 60 jumps to random frames of the whole recording (a fixed seed, the same every run), one at a time: "
         "scrub(target) -> the picture of that request displayed, then 50 ms on it. Through the same path as "
         "bench_playback's scrub (PoolFrameSource on the scheduler's DecoderPool): a jump seeks the layer's own idle "
         "decoder to the keyframe before the target and decodes forward."},
        {"scrub",
         "paused at the middle, a playhead drag: 60 scrub() calls 16.7 ms apart (45 forward over 8 s, 15 back), each "
         "its own seek epoch. scrub_p95: request -> the frame for that target displayed, over the completed requests; "
         "scrub_follow_p50/p95: request -> the first picture for it or any newer request displayed."},
        {"frame_step", "paused at the middle, stepFrames(+1) -> that frame displayed, 30 steps"},
    };
    for (const Vod* vod : vods) {
        const std::string suffix = std::string("_") + vod->name;
        const auto began = Clock::now();
        const Project project = makeProject(*vod);
        warmUp(project, *renderer);
        Drawn drawn;
        Pipeline p(project, *renderer, drawn);
        REQUIRE(openSequence(p, project.sequence).has_value());
        const Ticks middle = middleOf(project, vod->rate);

        const playback::SchedulerStats beforeSeeks = p.schedulerStats();
        const SeekTimes seeks = randomSeeks(p, project.length, vod->rate);
        const auto seekFrames = poolFramesBetween(beforeSeeks, p.schedulerStats());
        const DragTimes dragged = drag(p, middle, vod->rate);
        const StepTimes stepped = frameSteps(p, middle, vod->rate);
        const playback::SchedulerStats stats = p.schedulerStats();

        if (!seeks.ms.empty()) {
            r.addSamples("seek_p50_ms" + suffix, "ms", seeks.ms, "p50");
            r.addSamples("seek_p95_ms" + suffix, "ms", seeks.ms, "p95");
            r.note("seek_max_ms" + suffix, "ms", seeks.ms.max());
        }
        r.note("seek_missed" + suffix, "requests", static_cast<double>(seeks.missed));
        if (seekFrames && !seeks.ms.empty()) {
            // What a jump costs the codec: the frames from the keyframe before the target to the target.
            r.note("seek_decoded_frames_per_jump" + suffix, "frames",
                   static_cast<double>(seekFrames->first) / static_cast<double>(seeks.ms.size()));
        }
        if (!dragged.follow.empty()) {
            r.addSamples("scrub_follow_p50_ms" + suffix, "ms", dragged.follow, "p50");
            r.addSamples("scrub_follow_p95_ms" + suffix, "ms", dragged.follow, "p95");
        }
        if (!dragged.completed.empty()) {
            r.addSamples("scrub_p95_ms" + suffix, "ms", dragged.completed, "p95");
        }
        r.note("scrub_superseded" + suffix, "requests", static_cast<double>(dragged.superseded));
        if (!stepped.steps.empty()) {
            r.addSamples("frame_step_p50_ms" + suffix, "ms", stepped.steps, "p50");
            r.addSamples("frame_step_p95_ms" + suffix, "ms", stepped.steps, "p95");
        }
        r.note("sessions_after" + suffix, "sessions", static_cast<double>(stats.activeSessions));
        r.details["recordings"][vod->name] = {
            {"file", vod->file},
            {"what", vod->what},
            {"lengthS", project.length.toSeconds()},
            {"middleS", middle.toSeconds()},
            {"seek", {{"requests", kSeeks}, {"seed", kSeekSeed}, {"missed", seeks.missed},
                      {"wrongTarget", seeks.wrongTarget}}},
            {"scrub", {{"requests", dragged.requests}, {"completed", dragged.completed.size()},
                       {"superseded", dragged.superseded}}},
            {"frameStep", {{"steps", kSteps}, {"missed", stepped.missed}, {"wrongTarget", stepped.wrongTarget}}},
            {"seekPoolFrames", seekFrames
                                   ? perf::Json{{"decoded", seekFrames->first}, {"delivered", seekFrames->second}}
                                   : perf::Json("not counted in this build")},
            {"decodeBackends", stats.decodeBackends},
            {"decodeErrors", stats.decodeErrors},
            {"compositeP50Ms", drawn.compositeMs.p50()},
            {"monitorErrors", drawn.errors},
            {"seconds", perf::msSince(began) / 1000.0}};
        INFO(vod->name << ": " << r.details["recordings"][vod->name].dump());
        CHECK(seeks.missed == 0);
        CHECK(seeks.wrongTarget == 0);
        CHECK(stepped.missed == 0);
        CHECK(drawn.errors == 0);
    }
    return r;
}

// ---- 4. Play -------------------------------------------------------------------------------------------------------

struct PlayRun {
    double seconds = 0.0;               ///< wall time from play to pause
    std::optional<double> firstFrameMs; ///< play -> the first picture past the paused frame
    std::uint64_t expected = 0;         ///< sequence frames in that time
    std::uint64_t presented = 0;        ///< pictures the monitor showed
    std::uint64_t published = 0;
    std::uint64_t skippedLate = 0;      ///< targets the scheduler abandoned (their layers were not ready in time)
    std::uint64_t mailboxDropped = 0;   ///< published but superseded before they were shown
    std::uint64_t late = 0;             ///< shown after the end of their own frame interval
    std::uint64_t delivered = 0;        ///< frames the decoder pool delivered to the scheduler
    /// The pool's codec frames decoded and frames delivered, where SchedulerStats counts them (A.10 WP4b).
    std::optional<std::pair<std::uint64_t, std::uint64_t>> poolCounts;
    std::uint64_t decodeErrors = 0;
    std::uint64_t underrunFrames = 0;   ///< audio track frames the callback found missing
    double cpuMs = 0.0;                 ///< this process's CPU time over the play
    perf::Json lateByMedia;
    std::string decodeBackends;
};

/// 20 s of playback from the middle (less on a recording too short for it), measured from the paused picture.
PlayRun playFromMiddle(Pipeline& p, const Project& project, Rate rate) {
    PlayRun out;
    const Ticks middle = middleOf(project, rate);
    const double room = (project.length - middle).toSeconds() - 1.0;
    const auto playFor = std::chrono::milliseconds(
        static_cast<std::int64_t>(std::clamp(room, 1.0, static_cast<double>(kPlayFor.count()) / 1000.0) * 1000.0));
    REQUIRE(parkAt(p, middle, 500ms));
    const playback::SchedulerStats s0 = p.schedulerStats();
    const render::MailboxStats m0 = p.mailbox().stats();
    const editor::PlaybackSessionStats a0 = p.player.stats();
    const double cpu0 = perf::processCpuMs();
    const auto wall0 = Clock::now();
    const std::int64_t t0 = playback::steadyNowNs();
    p.player.play();
    const std::uint64_t epoch = p.mailbox().epoch();
    out.firstFrameMs =
        pictureAfter(p, t0, [&](const Shown& c) { return c.epoch >= epoch && c.layers > 0 && c.target > middle; },
                     5000ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - wall0);
    if (elapsed < playFor) {
        p.monitor->run(playFor - elapsed);
    }
    const double cpu1 = perf::processCpuMs();
    out.seconds = perf::msSince(wall0) / 1000.0;
    const playback::SchedulerStats s1 = p.schedulerStats();
    const render::MailboxStats m1 = p.mailbox().stats();
    const editor::PlaybackSessionStats a1 = p.player.stats();
    p.player.pause();
    p.monitor->run(200ms);

    out.expected = static_cast<std::uint64_t>(out.seconds * rate.toDouble() + 0.5);
    out.presented = m1.presented - m0.presented;
    out.published = s1.published - s0.published;
    out.skippedLate = s1.skippedLate - s0.skippedLate;
    out.mailboxDropped = m1.dropped - m0.dropped;
    out.late = m1.late - m0.late;
    out.delivered = s1.decodedFrames - s0.decodedFrames;
    out.poolCounts = poolFramesBetween(s0, s1);
    out.decodeErrors = s1.decodeErrors - s0.decodeErrors;
    out.underrunFrames = a1.audio.underrunFrames - a0.audio.underrunFrames;
    out.cpuMs = cpu1 - cpu0;
    out.lateByMedia = s1.lateFramesByMedia;
    out.decodeBackends = s1.decodeBackends;
    return out;
}

perf::PerfResult measurePlay(const std::vector<const Vod*>& vods) {
    perf::PerfResult r;
    ResetSharedDevice resetDevice; // declared first: runs after the renderer is gone
    const std::unique_ptr<render::OffscreenRenderer> renderer = makeRenderer(r);
    r.details["definition"] = {
        {"pipeline", pipelineDefinition()},
        {"play",
         "paused at the middle (its picture shown, 500 ms settle), play at 1x for 20 s of wall time. The 240 fps "
         "recording plays in a 60 fps sequence: the preview runs at the sequence rate."},
        {"dropped_frames",
         "sequence frames never shown: targets the scheduler abandoned because their layers were not decoded in time "
         "(SchedulerStats::skippedLate) + pictures published but superseded before the monitor showed them "
         "(MailboxStats::dropped). 0 is smooth playback."},
        {"late_frames", "pictures shown after the end of their own frame interval (MailboxStats::late)"},
        {"delivered_fps",
         "frames the decoder pool delivered to the scheduler per second (SchedulerStats::decodedFrames): one per "
         "sequence frame. codec_decoded_fps / decoded_per_delivered (where SchedulerStats counts the pool's codec "
         "frames): every source frame is decoded, 4 per sequence frame at 240 fps, but only delivered ones are read "
         "back, converted and uploaded"},
        {"cpu_ms_per_s",
         "this process's CPU time (user + kernel, every thread: decoding, the audio feed and mix, the scheduler, the "
         "monitor's composite submission) per second of play; 1000 = one core busy"},
    };
    for (const Vod* vod : vods) {
        const std::string suffix = std::string("_") + vod->name;
        const Project project = makeProject(*vod);
        warmUp(project, *renderer);
        Drawn drawn;
        Pipeline p(project, *renderer, drawn);
        REQUIRE(openSequence(p, project.sequence).has_value());
        const PlayRun run = playFromMiddle(p, project, vod->rate);
        const double seconds = std::max(run.seconds, 1e-3);

        // A frame or two can drop to a scheduling hiccup of this process: the gate needs more than 3 to call it a
        // regression (and CPU time 50 ms/s more, as well as 20 %).
        const double dropped = static_cast<double>(run.skippedLate + run.mailboxDropped);
        r.add("dropped_frames" + suffix, "frames", dropped).slack = 3.0;
        r.add("late_frames" + suffix, "frames", static_cast<double>(run.late)).slack = 3.0;
        r.add("cpu_ms_per_s" + suffix, "ms/s", run.cpuMs / seconds).slack = 50.0;
        r.note("presented_fps" + suffix, "fps", static_cast<double>(run.presented) / seconds);
        r.note("delivered_fps" + suffix, "fps", static_cast<double>(run.delivered) / seconds);
        // The codec decodes every source frame, 4 per sequence frame of the 240 fps recording: where the pool counts
        // them (SchedulerStats::poolFramesDecoded, A.10 WP4b), decoded per delivered shows the preview reading the
        // source at the sequence rate (only delivered frames are read back, converted, cached and uploaded).
        if (run.poolCounts) {
            const double decodedFrames = static_cast<double>(run.poolCounts->first);
            const double deliveredFrames = static_cast<double>(run.poolCounts->second);
            r.note("codec_decoded_fps" + suffix, "fps", decodedFrames / seconds);
            r.note("decoded_per_delivered" + suffix, "x",
                   deliveredFrames > 0.0 ? decodedFrames / deliveredFrames : 0.0);
        }
        if (run.firstFrameMs) {
            r.note("play_first_frame_ms" + suffix, "ms", *run.firstFrameMs);
        }
        r.note("audio_underrun_frames" + suffix, "frames", static_cast<double>(run.underrunFrames));
        r.details["recordings"][vod->name] = {{"file", vod->file},
                                              {"what", vod->what},
                                              {"fromS", middleOf(project, vod->rate).toSeconds()},
                                              {"seconds", run.seconds},
                                              {"expectedFrames", run.expected},
                                              {"presented", run.presented},
                                              {"published", run.published},
                                              {"skippedLate", run.skippedLate},
                                              {"mailboxDropped", run.mailboxDropped},
                                              {"late", run.late},
                                              {"delivered", run.delivered},
                                              {"poolFrames", run.poolCounts
                                                                 ? perf::Json{{"decoded", run.poolCounts->first},
                                                                              {"delivered", run.poolCounts->second}}
                                                                 : perf::Json("not counted in this build")},
                                              {"decodeErrors", run.decodeErrors},
                                              {"lateFramesByMedia", run.lateByMedia},
                                              {"decodeBackends", run.decodeBackends},
                                              {"cpuMs", run.cpuMs},
                                              {"audioUnderrunFrames", run.underrunFrames},
                                              {"devicePulls", p.device->pulls()},
                                              {"deviceLatePulls", p.device->latePulls()},
                                              {"devicePullErrors", p.device->pullErrors()},
                                              {"compositeP50Ms", drawn.compositeMs.p50()},
                                              {"compositeP95Ms", drawn.compositeMs.p95()},
                                              {"monitorErrors", drawn.errors}};
        INFO(vod->name << ": " << r.details["recordings"][vod->name].dump());
        CHECK(run.firstFrameMs.has_value());
        CHECK(run.decodeErrors == 0);
        CHECK(drawn.errors == 0);
    }
    return r;
}

// ---- 5. Waveform peaks ---------------------------------------------------------------------------------------------

/// WaveformPeaksService::request's partial-result argument (A.10 WP4b: snapshots while a long stream decodes), when
/// this tree has one: its 4th parameter. Deduced instead of named, so the benchmark builds with and without it.
template <class R, class C, class A0, class A1, class A2, class A3>
A3 fourthParameter(R (C::*)(A0, A1, A2, A3));

template <class Service>
concept PartialPeaksApi = requires { fourthParameter(&Service::request); };

template <class Service>
using PartialsOf = std::remove_cvref_t<decltype(fourthParameter(&Service::request))>;

/// Whether `Service::request` takes partial-result callbacks this benchmark can use (an `onPartial` member taking a
/// snapshot).
template <class Service>
concept UsablePartialPeaks =
    PartialPeaksApi<Service> &&
    requires(PartialsOf<Service> partials, std::function<void(std::shared_ptr<const media::WaveformPeaks>)> callback) {
        partials.onPartial = callback;
    };

/// service.request(path, options), with `onPartial` called on each partial snapshot where the API has them.
template <class Service>
media::Job<media::WaveformPeaks> requestPeaks(Service& service, const std::string& path,
                                              const media::WaveformOptions& options,
                                              [[maybe_unused]] std::function<void()> onPartial) {
    if constexpr (UsablePartialPeaks<Service>) {
        PartialsOf<Service> partials{};
        partials.onPartial = [callback = std::move(onPartial)](std::shared_ptr<const media::WaveformPeaks>) {
            callback();
        };
        return service.request(path, options, {}, std::move(partials));
    } else {
        return service.request(path, options);
    }
}

constexpr bool kPartialPeaks = UsablePartialPeaks<media::WaveformPeaksService>;

struct StreamPeaks {
    int stream = -1;                ///< the container's stream index
    bool ok = false;
    std::string error;
    double totalMs = 0.0;           ///< request -> the result
    double firstProgressMs = -1.0;  ///< request -> Job::progress() above 0 (the first decoded block)
    double firstPartialMs = -1.0;   ///< request -> the first partial snapshot (where the API has them)
    double mb = 0.0;                ///< the result's peak levels
    std::int64_t samples = 0;
    bool fromCache = false;
};

/// One stream's peaks on `service`, alone: request -> first progress, first partial, result.
StreamPeaks streamPeaks(media::WaveformPeaksService& service, const std::string& path, int stream) {
    StreamPeaks out;
    out.stream = stream;
    media::WaveformOptions options;
    options.stream = stream;
    auto firstPartialNs = std::make_shared<std::atomic<std::int64_t>>(0);
    const auto t0 = Clock::now();
    const media::Job<media::WaveformPeaks> job = requestPeaks(service, path, options, [firstPartialNs] {
        std::int64_t none = 0;
        (void)firstPartialNs->compare_exchange_strong(none, nsOf(Clock::now()));
    });
    while (!job.waitFor(1ms)) {
        if (out.firstProgressMs < 0.0 && job.progress() > 0.0f) {
            out.firstProgressMs = perf::msSince(t0);
        }
    }
    out.totalMs = perf::msSince(t0);
    if (out.firstProgressMs < 0.0) {
        out.firstProgressMs = out.totalMs; // finished before a poll saw progress (a cache hit)
    }
    if (const std::int64_t at = firstPartialNs->load(); at != 0) {
        out.firstPartialMs = static_cast<double>(at - nsOf(t0)) / 1e6;
    }
    const auto& result = job.wait();
    if (!result) {
        out.error = result.error().message;
        return out;
    }
    out.ok = true;
    std::uint64_t bytes = 0;
    for (const media::PeakLevel& level : result->levels) {
        bytes += static_cast<std::uint64_t>(level.peaks.size() * sizeof(std::int16_t));
    }
    out.mb = mbOf(bytes);
    out.samples = result->sampleCount;
    out.fromCache = result->fromCache;
    return out;
}

perf::Json streamJson(const StreamPeaks& s) {
    return perf::Json{{"stream", s.stream},
                      {"ok", s.ok},
                      {"error", s.error},
                      {"totalMs", s.totalMs},
                      {"firstProgressMs", s.firstProgressMs},
                      {"firstPartialMs", s.firstPartialMs},
                      {"mb", s.mb},
                      {"samples", s.samples},
                      {"fromCache", s.fromCache}};
}

/// Whether MediaPeakSource reports how far a stream's partial peaks reach (A.10 WP4b: buildingUntil()).
template <class Source>
concept ReportsPartialPeaks = requires(const Source& source, MediaId item, StreamRef stream) {
    source.buildingUntil(item, stream);
};

/// While `stream`'s peaks are still decoding: how far the decoded part reaches (Ticks{} before the first partial
/// result); nullopt once complete, and always where the source does not report it.
template <class Source>
std::optional<Ticks> partialPeaksUntil([[maybe_unused]] const Source& source, [[maybe_unused]] MediaId item,
                                       [[maybe_unused]] StreamRef stream) {
    if constexpr (ReportsPartialPeaks<Source>) {
        return source.buildingUntil(item, stream);
    } else {
        return std::nullopt;
    }
}

struct AppPeaks {
    std::vector<double> visibleMs;  ///< per stream: request -> MediaPeakSource::peaks() first true; -1: never
    std::vector<double> completeMs; ///< per stream: request -> drawn and no longer building; -1: never
    double allDoneMs = -1.0;        ///< request -> MediaPeakSource::idle() (every stream finished), to the 5 ms tick
    perf::Samples lateness;         ///< the GUI timer's lateness per tick, ms
    double callMaxMs = 0.0;         ///< the longest peaks() call on the GUI thread
    int ticks = 0;
    bool timedOut = false;
};

/// Every stream's peaks as the app requests them: MediaPeakSource (the timeline's and the source monitor's peak source,
/// with the app's worker count), asked by this thread standing in for the GUI thread (it owns the Qt application and
/// the source, as the GUI thread does). A 5 ms timer delivers the queued signals (as the event loop would) and asks for
/// every stream's waveform over the whole recording (kTimelineColumns columns: a zoomed-out timeline repainting) until
/// each one is complete; it records how late each tick fired, so a GUI thread that waits for peaks anywhere shows as
/// lateness. The results come from the service's worker threads, never from this one. A stream is visible when
/// peaks() first returns true (partial peaks: their decoded start, the rest flat), complete when it is drawn and the
/// source no longer reports it building.
AppPeaks appPeaks(const fs::path& cacheDir, const std::string& path, const std::vector<int>& streams, Ticks length) {
    AppPeaks out;
    out.visibleMs.assign(streams.size(), -1.0);
    out.completeMs.assign(streams.size(), -1.0);
    app::MediaPeakSource source(cacheDir);
    std::unordered_map<std::uint64_t, app::MediaPeakSource::MediaFile> files;
    files.emplace(kPeaksMedia.value, app::MediaPeakSource::MediaFile{path, streams});
    source.setMediaFiles(std::move(files));

    std::vector<float> columns(kTimelineColumns);
    const auto start = Clock::now();
    auto due = start;
    std::optional<Clock::time_point> finishedAt;
    for (int tick = 0;; ++tick) {
        if (tick > 0) {
            due += kGuiTick;
            std::this_thread::sleep_until(due);
            out.lateness.add(std::chrono::duration<double, std::milli>(Clock::now() - due).count());
        }
        ++out.ticks;
        QCoreApplication::processEvents(); // peaksReady and whatever else the source queued for the GUI thread
        bool all = true;
        for (std::size_t s = 0; s < streams.size(); ++s) {
            if (out.completeMs[s] >= 0.0) {
                continue;
            }
            const StreamRef stream{StreamKind::Audio, static_cast<std::int32_t>(s)};
            const auto c0 = Clock::now();
            const bool drawn = source.peaks(kPeaksMedia, stream, Ticks{}, length, columns);
            const bool building = partialPeaksUntil(source, kPeaksMedia, stream).has_value();
            out.callMaxMs = std::max(out.callMaxMs, perf::msSince(c0));
            if (drawn && out.visibleMs[s] < 0.0) {
                out.visibleMs[s] = perf::msSince(start);
            }
            if (drawn && !building) {
                out.completeMs[s] = perf::msSince(start);
            } else {
                all = false;
            }
        }
        // The first tick queued every stream, so idle() means they all finished.
        const bool finished = source.idle();
        if (finished && all) {
            break;
        }
        // A stream that failed is never drawn: half a second after every stream finished ends the wait.
        if (!finished) {
            finishedAt.reset();
        } else if (!finishedAt) {
            finishedAt = Clock::now();
        } else if (Clock::now() - *finishedAt > 500ms) {
            break;
        }
        if (Clock::now() - start > kPeaksTimeout) {
            out.timedOut = true;
            break;
        }
    }
    out.allDoneMs = perf::msSince(start);
    return out;
}

perf::Json appJson(const AppPeaks& a) {
    return perf::Json{{"visibleMs", a.visibleMs},
                      {"completeMs", a.completeMs},
                      {"allDoneMs", a.allDoneMs},
                      {"guiTicks", a.ticks},
                      {"guiLateP50Ms", a.lateness.p50()},
                      {"guiLateP99Ms", a.lateness.percentile(99.0)},
                      {"guiLateMaxMs", a.lateness.max()},
                      {"guiPeaksCallMaxMs", a.callMaxMs},
                      {"timedOut", a.timedOut}};
}

perf::PerfResult measurePeaks(const std::vector<const Vod*>& vods) {
    perf::PerfResult r;
    r.details["definition"] = {
        {"stream",
         "one audio stream at a time on a media::WaveformPeaksService with one worker (default levels 256 / 2048 / "
         "16384 samples per bin): request -> result. cold: a fresh cache directory (the OS file cache may still hold "
         "parts of the recording); warm: the same requests again, served from the disk cache. first_progress: request "
         "-> Job::progress() above 0, i.e. the first decoded block (65,536 samples)."},
        {"app",
         "every stream at once as the app requests them: app::MediaPeakSource (TimelineView's and SourceWaveform's "
         "peak source, with the app's own worker count) asked by the test thread standing in for the GUI thread. Its "
         "5 ms timer delivers queued signals and asks for each stream's waveform over the whole recording until it is "
         "complete. first_visible / last_visible: request -> MediaPeakSource::peaks() first returns true for the "
         "first / the last stream (partial peaks count: their decoded start is drawn); last_complete: request -> the "
         "last stream drawn and no longer building (buildingUntil(), where the source reports it); app_all: request "
         "-> every stream finished (idle()). cold: a fresh cache directory; warm: a new MediaPeakSource on the same "
         "one."},
        {"gui_timer_late_max",
         "the stand-in GUI thread's 5 ms timer: the most a tick fired late while the peaks built. Anything that blocks "
         "the GUI thread on peaks (a decode, a cache read, a lock held by a worker) shows here."},
        {"partial", kPartialPeaks
                        ? "WaveformPeaksService::request takes partial-result callbacks: first_partial is request -> "
                          "the first snapshot"
                        : "WaveformPeaksService has no partial-result API in this build: no first_partial metric"},
    };
    r.details["partialPeaksApi"] = kPartialPeaks;
    r.details["partialPeaksShown"] = ReportsPartialPeaks<app::MediaPeakSource>;
    const fs::path root = fs::path(perf::outputDir()) / "longvod";
    for (const Vod* vod : vods) {
        const std::string suffix = std::string("_") + vod->name;
        const std::string path = pathToUtf8(vodFile(*vod));
        const MediaInfo info = probeVod(*vod);
        std::vector<int> streams;
        for (const AudioStreamInfo& a : info.audio) {
            streams.push_back(a.index);
        }
        REQUIRE_FALSE(streams.empty());
        perf::Json details{{"file", vod->file}, {"what", vod->what}, {"streams", streams}};

        // One stream at a time, cold, then warm.
        const fs::path streamCache = root / (std::string("peaks-streams-") + vod->name);
        std::error_code ec;
        fs::remove_all(streamCache, ec);
        perf::Samples coldMs;
        perf::Samples warmMs;
        perf::Samples firstProgressMs;
        perf::Samples firstPartialMs;
        double serialMs = 0.0;
        double resultMb = 0.0;
        perf::Json cold = perf::Json::array();
        perf::Json warm = perf::Json::array();
        {
            media::WaveformPeaksService service(streamCache, 1);
            for (const int stream : streams) {
                const StreamPeaks s = streamPeaks(service, path, stream);
                INFO(vod->file << " stream " << stream << ": " << s.error);
                CHECK(s.ok);
                CHECK_FALSE(s.fromCache);
                coldMs.add(s.totalMs);
                serialMs += s.totalMs;
                resultMb += s.mb;
                firstProgressMs.add(s.firstProgressMs);
                if (s.firstPartialMs >= 0.0) {
                    firstPartialMs.add(s.firstPartialMs);
                }
                cold.push_back(streamJson(s));
            }
            for (const int stream : streams) {
                const StreamPeaks s = streamPeaks(service, path, stream);
                CHECK(s.ok);
                CHECK(s.fromCache);
                warmMs.add(s.totalMs);
                warm.push_back(streamJson(s));
            }
        }
        r.addSamples("peaks_cold_stream_max_ms" + suffix, "ms", coldMs, "max");
        r.note("peaks_cold_serial_ms" + suffix, "ms", serialMs);
        r.addSamples("peaks_first_progress_max_ms" + suffix, "ms", firstProgressMs, "max");
        if (!firstPartialMs.empty()) {
            r.addSamples("peaks_first_partial_max_ms" + suffix, "ms", firstPartialMs, "max");
        }
        r.addSamples("peaks_warm_stream_max_ms" + suffix, "ms", warmMs, "max");
        r.note("peaks_mb" + suffix, "MB", resultMb);
        details["streamCold"] = cold;
        details["streamWarm"] = warm;

        // As the app requests them, cold, then warm.
        const fs::path appCache = root / (std::string("peaks-app-") + vod->name);
        fs::remove_all(appCache, ec);
        const AppPeaks appCold = appPeaks(appCache, path, streams, info.duration);
        const AppPeaks appWarm = appPeaks(appCache, path, streams, info.duration);
        const auto never = [](double ms) { return ms < 0.0; };
        const bool allDrawn = std::ranges::none_of(appCold.completeMs, never) &&
                              std::ranges::none_of(appWarm.completeMs, never);
        if (allDrawn) {
            r.add("peaks_first_visible_ms" + suffix, "ms", std::ranges::min(appCold.visibleMs));
            r.add("peaks_last_visible_ms" + suffix, "ms", std::ranges::max(appCold.visibleMs));
            r.note("peaks_last_complete_ms" + suffix, "ms", std::ranges::max(appCold.completeMs));
            r.add("peaks_app_cold_all_ms" + suffix, "ms", appCold.allDoneMs);
            r.add("peaks_app_warm_all_ms" + suffix, "ms", appWarm.allDoneMs);
        }
        // Timer jitter is a millisecond or two; a blocked GUI thread is tens of milliseconds late.
        const double lateMax = std::max(appCold.lateness.max(), appWarm.lateness.max());
        r.add("gui_timer_late_max_ms" + suffix, "ms", lateMax).slack = 5.0;
        r.note("gui_peaks_call_max_ms" + suffix, "ms", std::max(appCold.callMaxMs, appWarm.callMaxMs));
        details["appCold"] = appJson(appCold);
        details["appWarm"] = appJson(appWarm);
        r.details["recordings"][vod->name] = details;
        INFO(vod->name << ": " << details.dump());
        CHECK(allDrawn);
        CHECK_FALSE(appCold.timedOut);
        CHECK_FALSE(appWarm.timedOut);
    }
    return r;
}

// ---- 6. Memory -----------------------------------------------------------------------------------------------------

struct MemoryPoint {
    double workingSet = 0.0;
    double privateBytes = 0.0;
    double gpu = -1.0;
};

MemoryPoint memoryPoint() {
    return MemoryPoint{perf::workingSetMb(), perf::privateMb(), perf::gpuMemoryMb()};
}

perf::Json pointJson(const MemoryPoint& m) {
    return perf::Json{{"workingSetMb", m.workingSet}, {"privateMb", m.privateBytes}, {"gpuMb", m.gpu}};
}

perf::PerfResult measureMemory(const std::vector<const Vod*>& vods) {
    perf::PerfResult r;
    ResetSharedDevice resetDevice; // declared first: runs after the renderer is gone
    const std::unique_ptr<render::OffscreenRenderer> renderer = makeRenderer(r);
    r.details["definition"] = {
        {"pipeline", pipelineDefinition()},
        {"points",
         "private bytes (commit charge), working set and this process's GPU memory. before: after a warm-up pipeline "
         "on the recording was opened and closed (codec libraries, the driver's decoder, compositor pipelines) and the "
         "freed heap released. opened: the recording imported and dropped into its sequence, a pipeline showing its "
         "first picture, 500 ms later. played: after 20 s of play from the middle and 500 ms paused. peaks: with every "
         "audio stream's peaks held (as MediaPeakSource holds them; a persistent cache directory, so only the first "
         "run decodes). closed: the peaks, their service, the pipeline and the project gone, 500 ms later."},
        {"retained_after_close", "closed - before: what opening, playing and drawing a long recording leaves behind"},
    };
    const fs::path peaksCache = fs::path(perf::outputDir()) / "longvod" / "peaks-memory";
    for (const Vod* vod : vods) {
        const std::string suffix = std::string("_") + vod->name;
        const std::string path = pathToUtf8(vodFile(*vod));
        {
            const Project warm = makeProject(*vod);
            warmUp(warm, *renderer);
        }
        releaseFreedHeap();
        std::this_thread::sleep_for(500ms);
        const MemoryPoint before = memoryPoint();

        auto project = std::make_unique<Project>(makeProject(*vod));
        Drawn drawn;
        auto p = std::make_unique<Pipeline>(*project, *renderer, drawn);
        CHECK(openSequence(*p, project->sequence).has_value());
        p->monitor->run(500ms);
        const MemoryPoint opened = memoryPoint();

        const PlayRun run = playFromMiddle(*p, *project, vod->rate);
        p->monitor->run(500ms);
        const MemoryPoint played = memoryPoint();

        // Every stream's peaks at once (a worker each), held as MediaPeakSource holds them.
        const MediaItem* item = project->session->snapshot()->findMedia(project->item);
        REQUIRE(item != nullptr);
        const std::vector<AudioStreamInfo> audioStreams = item->info.audio;
        REQUIRE_FALSE(audioStreams.empty());
        std::vector<std::shared_ptr<const media::WaveformPeaks>> held;
        std::uint64_t peakBytes = 0;
        bool peaksFromCache = true;
        auto service =
            std::make_unique<media::WaveformPeaksService>(peaksCache, static_cast<int>(audioStreams.size()));
        {
            std::vector<media::Job<media::WaveformPeaks>> jobs;
            for (const AudioStreamInfo& a : audioStreams) {
                media::WaveformOptions options;
                options.stream = a.index;
                jobs.push_back(service->request(path, options));
            }
            for (const media::Job<media::WaveformPeaks>& job : jobs) {
                const auto& result = job.wait();
                INFO((result ? std::string() : result.error().message));
                REQUIRE(result);
                for (const media::PeakLevel& level : result->levels) {
                    peakBytes += static_cast<std::uint64_t>(level.peaks.size() * sizeof(std::int16_t));
                }
                peaksFromCache = peaksFromCache && result->fromCache;
                held.push_back(std::make_shared<const media::WaveformPeaks>(*result));
            }
        }
        std::this_thread::sleep_for(500ms);
        const MemoryPoint peaks = memoryPoint();
        const double peakWorkingSet = perf::peakWorkingSetMb();

        held.clear();
        service.reset();
        p.reset();
        project.reset();
        std::this_thread::sleep_for(500ms);
        const MemoryPoint closed = memoryPoint();

        r.note("before_private_mb" + suffix, "MB", before.privateBytes);
        r.add("open_cost_mb" + suffix, "MB", opened.privateBytes - before.privateBytes);
        r.add("play_growth_mb" + suffix, "MB", played.privateBytes - opened.privateBytes);
        r.add("peaks_cost_mb" + suffix, "MB", peaks.privateBytes - played.privateBytes);
        r.note("peaks_result_mb" + suffix, "MB", mbOf(peakBytes));
        r.add("retained_after_close_mb" + suffix, "MB", closed.privateBytes - before.privateBytes);
        r.note("open_cost_ws_mb" + suffix, "MB", opened.workingSet - before.workingSet);
        r.note("played_ws_mb" + suffix, "MB", played.workingSet);
        if (opened.gpu >= 0.0 && before.gpu >= 0.0) {
            r.note("open_cost_gpu_mb" + suffix, "MB", opened.gpu - before.gpu);
        }
        r.note("peak_ws_mb" + suffix, "MB", peakWorkingSet);
        r.details["recordings"][vod->name] = {{"file", vod->file},
                                              {"what", vod->what},
                                              {"before", pointJson(before)},
                                              {"opened", pointJson(opened)},
                                              {"played", pointJson(played)},
                                              {"peaks", pointJson(peaks)},
                                              {"closed", pointJson(closed)},
                                              {"playSeconds", run.seconds},
                                              {"peaksFromCache", peaksFromCache},
                                              {"peaksResultMb", mbOf(peakBytes)},
                                              {"monitorErrors", drawn.errors}};
        INFO(vod->name << ": " << r.details["recordings"][vod->name].dump());
        CHECK(drawn.errors == 0);
    }
    return r;
}

/// Runs `measure` as benchmark `name` on the recordings present (skips without any) and checks the outcome.
void benchmark(const std::string& name, bool gui,
               const std::function<perf::PerfResult(const std::vector<const Vod*>&)>& measure) {
    perf::Json missing = perf::Json::array();
    const std::vector<const Vod*> vods = vodsOrSkip(missing);
    Application application(gui);
    TimerResolution timer;
    const perf::BenchmarkOutcome outcome = perf::runBenchmark(name, [&] {
        perf::PerfResult r = measure(vods);
        r.details["missingRecordings"] = missing;
        r.details["recordingsDir"] = WABCUT_PERF_LONGVOD_DIR;
        return r;
    });
    if (!outcome.baselineFound) {
        WARN(outcome.describe());
    }
    INFO(outcome.describe());
    CHECK(outcome.ok());
}

} // namespace

TEST_CASE("Benchmark longvod: open and import a long recording", "[perf][longvod]") {
    benchmark("longvod.open", false, measureOpen);
}

TEST_CASE("Benchmark longvod: first frame after opening and after a jump", "[perf][gpu][longvod]") {
    benchmark("longvod.first_frame", true, measureFirstFrame);
}

TEST_CASE("Benchmark longvod: scrub and frame step across the whole recording", "[perf][gpu][longvod]") {
    benchmark("longvod.scrub", true, measureScrub);
}

TEST_CASE("Benchmark longvod: real-time playback from the middle", "[perf][gpu][longvod]") {
    benchmark("longvod.play", true, measurePlay);
}

TEST_CASE("Benchmark longvod: waveform peaks of every audio track", "[perf][longvod]") {
    // The cold builds decode each track twice (alone, then as the app requests them): for the 2-hour recording that is
    // six passes over 22 GB, minutes rather than seconds.
    benchmark("longvod.peaks", false, measurePeaks);
}

TEST_CASE("Benchmark longvod: memory from open to close", "[perf][gpu][longvod]") {
    benchmark("longvod.memory", true, measureMemory);
}
