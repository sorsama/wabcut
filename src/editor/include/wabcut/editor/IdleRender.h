#pragma once

// Background rendering while idle (0.7, card "Background rendering while idle"): the preview render cache (model/PreviewCache.h,
// PreviewRender.h) filled by itself when the editor is quiet, so the red and yellow stretches of the timeline turn green
// without anybody choosing Sequence > Render Effects In to Out.
//
// * When it may run (idleVerdict, pure): the feature is on, nothing is playing, no timeline gesture runs, no export, manual
//   render, proxy job or render-file delete runs, the user has not touched the keyboard, mouse or pen for the configured
//   time (default 10 s), the computer is not on battery power (or in a battery saver), and other programs are not using
//   the CPU (an NVIDIA GPU's load is read before a segment starts) - the load of Wabcut's own render never counts.
// * What it renders (idleRenderPlan, pure): the current sequence's red and yellow render-bar segments that have no valid
//   preview, red first, each nearest after the playhead first. Content hash and file name are the cache's, so a segment
//   rendered here and one rendered by Render In to Out are the same file; an edit that changes a segment's pixels changes
//   its hash, so the file stops being used at once (validAt / renderBar compare hashes) and the next render replaces it.
// * How it runs (IdleRenderer): one low-priority worker thread (the OS background mode on Windows, encoder threads capped
//   to half the cores) renders ONE segment at a time through the same PreviewRenderer as the manual render. It pauses
//   inside the renderer's per-frame progress callback the moment the verdict turns to "no" (input, playback, ...) and goes
//   on when the editor is quiet again, so work is not thrown away by a mouse movement; it gives the segment up when the
//   pause lasts more than kMaxPauseSeconds, when an edit changed the segment's content (the file would be stale), when
//   the feature is turned off, or when a manual render starts. A failed segment is not retried until the project changes.
//   Nothing is rendered when the cache would pass its size budget (no thrash through eviction) or the drive is nearly full.
// * Nothing here touches the GUI: the host (MediaReadinessBridge) feeds the focus (sequence, playhead, snapshot) and the
//   activity flags, and reads status().
//
// Thread safety: every method may be called from any thread; the callbacks run on the worker thread.

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/Time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wabcut::editor {

/// The user's choices (Settings > Media & cache > Background rendering).
struct IdleRenderSettings {
    bool enabled = true;
    /// Seconds without input before it starts (3..600 in the UI; tests use less).
    double idleSeconds = 10.0;
    bool pauseOnBattery = true;
    /// Wait while other programs use the CPU (or an NVIDIA GPU).
    bool pauseWhenBusy = true;

    bool operator==(const IdleRenderSettings&) const = default;
};

inline constexpr double kMinIdleSeconds = 3.0;
inline constexpr double kMaxIdleSeconds = 600.0;
inline constexpr double kDefaultIdleSeconds = 10.0;
/// Other programs' share of the CPU (percent) above which a segment is not started, and above which one that runs pauses.
inline constexpr double kStartCpuOthers = 40.0;
inline constexpr double kPauseCpuOthers = 65.0;
/// GPU utilisation (percent, NVIDIA) above which a segment is not started.
inline constexpr double kStartGpuBusy = 70.0;
/// A pause that lasts longer gives the segment up (its GPU and memory go back); it is started again later.
inline constexpr double kMaxPauseSeconds = 120.0;
/// Free space the preview folder's drive keeps beyond a segment's estimated size.
inline constexpr std::int64_t kIdleRenderDiskMargin = std::int64_t{2} * 1024 * 1024 * 1024;

/// What the computer is doing besides Wabcut's own render. -1 / Unknown = could not be read (then it is not a reason).
struct SystemLoad {
    enum class Power : std::uint8_t { Unknown, Mains, Battery };
    double cpuOthers = -1.0; ///< 0..100, the share of all logical processors used by other processes
    double gpu = -1.0;       ///< 0..100, NVIDIA GPU utilisation (only read when asked: includeGpu)
    Power power = Power::Unknown;
    bool batterySaver = false;
};

/// Reads SystemLoad. Windows: GetSystemTimes minus this process's own times, GetSystemPowerStatus, NVML (nvml.dll from the
/// NVIDIA driver, loaded at run time). Elsewhere everything is unknown (only input and activity gate it there).
class SystemLoadProbe {
public:
    virtual ~SystemLoadProbe() = default;
    /// A sample; the CPU figure is the average since the previous sample (at least ~0.5 s apart: nearer calls return the
    /// previous figure).
    virtual SystemLoad sample(bool includeGpu) = 0;
};
std::unique_ptr<SystemLoadProbe> makeSystemLoadProbe();

/// Why background rendering does not run now (None: it may).
enum class IdleBlock : std::uint8_t {
    None,
    Disabled,
    NoSequence,
    Playing,    ///< a monitor plays
    Editing,    ///< a timeline gesture (drag, trim, slider scrub) runs
    Input,      ///< the user touched the keyboard, mouse or pen less than idleSeconds ago
    Export,     ///< an export runs
    OtherWork,  ///< a manual render, a render-file delete or a proxy job runs
    Held,       ///< the user stopped it or deleted the render files: it resumes after the next edit
    Battery,    ///< on battery power
    BatterySaver,
    CpuBusy,    ///< other programs use the CPU
    GpuBusy,    ///< another program uses the GPU
};
std::string_view toString(IdleBlock block);

/// What the host knows about the editor right now.
struct IdleInputs {
    double secondsSinceInput = 0.0;
    bool playing = false;
    bool gesture = false;
    bool exporting = false;
    bool otherWork = false;
    bool held = false; ///< stopped by the user (or the render files were deleted) until the project changes
    bool hasSequence = true;
};

struct IdleVerdict {
    bool run = false;
    IdleBlock block = IdleBlock::Disabled;
};

/// The pure rule (see the file comment). `rendering`: a segment is being rendered (higher CPU threshold, no GPU check:
/// the GPU load then is mostly our own). The order of the checks is the order of the reasons shown to the user.
IdleVerdict idleVerdict(const IdleRenderSettings& settings, const IdleInputs& inputs, const SystemLoad& load,
                        bool rendering);

/// The segments to render, in order: the current sequence's red and yellow segments without a valid preview, red first,
/// each group from the playhead forward (then wrapping to the start). `skip`: segments not to retry (their hash).
std::vector<RenderSegment> idleRenderPlan(const ProjectState& state, const PreviewCache& cache, SequenceId sequence,
                                          Ticks playhead, const std::set<std::string>& skip = {});

/// Bytes a ProRes LT preview of `range` of `sequence` takes (about 102 Mb/s at 1080p30, scaled by the pixel rate); the
/// same estimate the manual render uses for its free-space check.
std::int64_t estimatePreviewBytes(const ProjectState& state, SequenceId sequence, TimeRange range);

/// Lets the calling thread run in the OS background mode (Windows: THREAD_MODE_BACKGROUND_BEGIN: lowest CPU, I/O and
/// memory priority) until the guard goes out of scope; elsewhere nothing.
class BackgroundThreadScope {
public:
    BackgroundThreadScope();
    ~BackgroundThreadScope();
    BackgroundThreadScope(const BackgroundThreadScope&) = delete;
    BackgroundThreadScope& operator=(const BackgroundThreadScope&) = delete;
    bool active() const { return m_active; }

private:
    bool m_active = false;
};

/// Where the worker looks: the editor's current sequence and playhead and the project as it is now. The host keeps it up to
/// date (cheap to read: a snapshot is a shared pointer).
struct IdleRenderFocus {
    ProjectSnapshot snapshot;
    SequenceId sequence;
    Ticks playhead{};
};

struct IdleRenderHost {
    std::function<IdleRenderFocus()> focus;
    std::function<IdleInputs()> inputs;
    /// Optional: the load (tests give a fake); default makeSystemLoadProbe().
    std::shared_ptr<SystemLoadProbe> probe;
};

/// What the worker is doing, for the UI.
struct IdleRenderStatus {
    enum class Phase : std::uint8_t {
        Off,       ///< turned off in the settings
        Waiting,   ///< something blocks it (block says what) or nothing is quiet yet
        Rendering, ///< a segment is being rendered
        Paused,    ///< a segment is half rendered and waits for the editor to be quiet (block says why)
        UpToDate,  ///< nothing left to render (every red and yellow stretch has a valid preview, or could not be rendered)
        Full,      ///< the preview cache is at its size budget or the drive is nearly full: nothing is rendered
    };
    Phase phase = Phase::Off;
    IdleBlock block = IdleBlock::Disabled;
    TimeRange current{};               ///< the segment being rendered / paused (Rendering, Paused)
    double progress = 0.0;             ///< of that segment, 0..1
    int remaining = 0;                 ///< segments left in the plan, the current one included
    int rendered = 0;                  ///< segments finished since the project was opened / the worker started
    int failed = 0;                    ///< segments that failed (not retried until the project changes)
    std::int64_t renderedBytes = 0;    ///< of the segments finished since the start
    std::string lastError;             ///< the latest failure's message, "" when none
    std::uint64_t generation = 0;      ///< changes with every status change (cheap to compare)
};
std::string_view toString(IdleRenderStatus::Phase phase);

class IdleRenderer {
public:
    /// `cache` and `renderer` are the open project's (the same objects Render In to Out uses). The worker starts at once
    /// (waiting while settings.enabled is false).
    IdleRenderer(std::shared_ptr<PreviewCache> cache, std::shared_ptr<PreviewRenderer> renderer, IdleRenderHost host,
                 IdleRenderSettings settings = {});
    ~IdleRenderer(); ///< cancels the segment in hand and joins the worker

    IdleRenderer(const IdleRenderer&) = delete;
    IdleRenderer& operator=(const IdleRenderer&) = delete;

    void setSettings(const IdleRenderSettings& settings);
    IdleRenderSettings settings() const;
    IdleRenderStatus status() const;
    /// The cache is another project's now: the segment in hand is given up and the failures are forgotten.
    void reset(std::shared_ptr<PreviewCache> cache, std::shared_ptr<PreviewRenderer> renderer);
    /// Gives up the segment in hand at its next frame (a manual render starts, the project closes). The worker looks at
    /// the editor again afterwards: while the host reports other work (IdleInputs::otherWork) it renders nothing.
    void cancelCurrent();
    /// Looks at the editor now instead of at the next 250 ms tick (the host calls it when something changed).
    void wake();
    /// Stops the worker for good (the destructor does it too).
    void stop();

    /// Tests: how often the worker polls (default 250 ms) and how long a paused segment is kept (kMaxPauseSeconds).
    void setPollInterval(std::chrono::milliseconds interval);
    void setMaxPauseSeconds(double seconds);

    struct Shared; ///< the worker's state (IdleRender.cpp)

private:
    void run(std::stop_token stop);
    std::shared_ptr<Shared> m_shared;
    std::jthread m_thread;
};

} // namespace wabcut::editor
