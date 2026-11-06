#include <wabcut/editor/IdleRender.h>

#include <wabcut/export/Encoders.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wabcut::editor {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string_view toString(IdleBlock block) {
    switch (block) {
    case IdleBlock::None:
        return "none";
    case IdleBlock::Disabled:
        return "disabled";
    case IdleBlock::NoSequence:
        return "no-sequence";
    case IdleBlock::Playing:
        return "playing";
    case IdleBlock::Editing:
        return "editing";
    case IdleBlock::Input:
        return "input";
    case IdleBlock::Export:
        return "export";
    case IdleBlock::OtherWork:
        return "other-work";
    case IdleBlock::Held:
        return "held";
    case IdleBlock::Battery:
        return "battery";
    case IdleBlock::BatterySaver:
        return "battery-saver";
    case IdleBlock::CpuBusy:
        return "cpu-busy";
    case IdleBlock::GpuBusy:
        return "gpu-busy";
    }
    return "none";
}

std::string_view toString(IdleRenderStatus::Phase phase) {
    switch (phase) {
    case IdleRenderStatus::Phase::Off:
        return "off";
    case IdleRenderStatus::Phase::Waiting:
        return "waiting";
    case IdleRenderStatus::Phase::Rendering:
        return "rendering";
    case IdleRenderStatus::Phase::Paused:
        return "paused";
    case IdleRenderStatus::Phase::UpToDate:
        return "up-to-date";
    case IdleRenderStatus::Phase::Full:
        return "full";
    }
    return "off";
}

// ---- The rule ---------------------------------------------------------------------------------------------------------

IdleVerdict idleVerdict(const IdleRenderSettings& settings, const IdleInputs& inputs, const SystemLoad& load,
                        bool rendering) {
    const auto no = [](IdleBlock block) { return IdleVerdict{false, block}; };
    if (!settings.enabled) {
        return no(IdleBlock::Disabled);
    }
    if (!inputs.hasSequence) {
        return no(IdleBlock::NoSequence);
    }
    if (inputs.playing) {
        return no(IdleBlock::Playing);
    }
    if (inputs.gesture) {
        return no(IdleBlock::Editing);
    }
    if (inputs.exporting) {
        return no(IdleBlock::Export);
    }
    if (inputs.otherWork) {
        return no(IdleBlock::OtherWork);
    }
    if (inputs.held) {
        return no(IdleBlock::Held);
    }
    if (inputs.secondsSinceInput < settings.idleSeconds) {
        return no(IdleBlock::Input);
    }
    if (settings.pauseOnBattery) {
        if (load.power == SystemLoad::Power::Battery) {
            return no(IdleBlock::Battery);
        }
        if (load.batterySaver) {
            return no(IdleBlock::BatterySaver);
        }
    }
    if (settings.pauseWhenBusy) {
        const double cpuLimit = rendering ? kPauseCpuOthers : kStartCpuOthers;
        if (load.cpuOthers >= 0.0 && load.cpuOthers >= cpuLimit) {
            return no(IdleBlock::CpuBusy);
        }
        // While we render, the GPU's load is mostly our own: only look before a segment starts.
        if (!rendering && load.gpu >= 0.0 && load.gpu >= kStartGpuBusy) {
            return no(IdleBlock::GpuBusy);
        }
    }
    return {true, IdleBlock::None};
}

// ---- The plan ---------------------------------------------------------------------------------------------------------

namespace {

std::string failureKey(const TimeRange& range, const std::string& hash) {
    return std::to_string(range.start.value) + ":" + std::to_string(range.duration.value) + ":" + hash;
}

} // namespace

std::vector<RenderSegment> idleRenderPlan(const ProjectState& state, const PreviewCache& cache, SequenceId sequence,
                                          Ticks playhead, const std::set<std::string>& skip) {
    std::vector<RenderSegment> todo;
    for (RenderSegment& s : cache.renderBar(state, sequence)) {
        if ((s.status != RenderStatus::Red && s.status != RenderStatus::Yellow) || s.range.duration.value <= 0) {
            continue;
        }
        if (!skip.empty() && skip.contains(failureKey(s.range, renderContentHash(state, sequence, s.range)))) {
            continue;
        }
        todo.push_back(std::move(s));
    }
    // Red (cannot play in real time) before yellow; inside a group from the playhead forward, then from the start.
    std::stable_sort(todo.begin(), todo.end(), [&](const RenderSegment& a, const RenderSegment& b) {
        const int ga = a.status == RenderStatus::Red ? 0 : 1;
        const int gb = b.status == RenderStatus::Red ? 0 : 1;
        if (ga != gb) {
            return ga < gb;
        }
        const int fa = a.range.end() > playhead ? 0 : 1;
        const int fb = b.range.end() > playhead ? 0 : 1;
        if (fa != fb) {
            return fa < fb;
        }
        return a.range.start < b.range.start;
    });
    return todo;
}

std::int64_t estimatePreviewBytes(const ProjectState& state, SequenceId sequence, TimeRange range) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return 0;
    }
    const double seconds = std::max(0.0, range.duration.toSeconds());
    const double pixelRate = static_cast<double>(seq->width) * seq->height * seq->frameRate.toDouble();
    return static_cast<std::int64_t>(102e6 * pixelRate / (1920.0 * 1080.0 * 29.97) * seconds / 8.0);
}

// ---- Background mode of the worker thread -------------------------------------------------------------------------------

BackgroundThreadScope::BackgroundThreadScope() {
#ifdef _WIN32
    m_active = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
#endif
}

BackgroundThreadScope::~BackgroundThreadScope() {
#ifdef _WIN32
    if (m_active) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
#endif
}

// ---- System load --------------------------------------------------------------------------------------------------------

namespace {

#ifdef _WIN32

class WindowsLoadProbe final : public SystemLoadProbe {
public:
    ~WindowsLoadProbe() override {
        if (m_nvml) {
            if (m_shutdown) {
                m_shutdown();
            }
            FreeLibrary(m_nvml);
        }
    }

    SystemLoad sample(bool includeGpu) override {
        const std::lock_guard lock(m_mutex);
        const auto now = Clock::now();
        if (!m_haveFirst || now - m_lastCpu >= std::chrono::milliseconds(500)) {
            readCpu();
            m_lastCpu = now;
        }
        SystemLoad out;
        out.cpuOthers = m_cpuOthers;
        SYSTEM_POWER_STATUS status{};
        if (GetSystemPowerStatus(&status)) {
            // 128: no system battery (a desktop); ACLineStatus 0 = running on battery, 1 = mains, 255 = unknown.
            if (status.BatteryFlag == 128 || status.ACLineStatus == 1) {
                out.power = SystemLoad::Power::Mains;
            } else if (status.ACLineStatus == 0) {
                out.power = SystemLoad::Power::Battery;
            }
            out.batterySaver = (status.SystemStatusFlag & 1) != 0;
        }
        if (includeGpu) {
            out.gpu = readGpu();
        }
        return out;
    }

private:
    static std::uint64_t ticks(const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    }

    void readCpu() {
        FILETIME idle{};
        FILETIME kernel{};
        FILETIME user{};
        FILETIME created{};
        FILETIME exited{};
        FILETIME ownKernel{};
        FILETIME ownUser{};
        if (!GetSystemTimes(&idle, &kernel, &user) ||
            !GetProcessTimes(GetCurrentProcess(), &created, &exited, &ownKernel, &ownUser)) {
            return;
        }
        // The kernel time includes the idle time; all of it summed over the logical processors, like the process's.
        const std::uint64_t total = ticks(kernel) + ticks(user);
        const std::uint64_t idleTime = ticks(idle);
        const std::uint64_t own = ticks(ownKernel) + ticks(ownUser);
        if (m_haveFirst && total > m_prevTotal) {
            const double dTotal = static_cast<double>(total - m_prevTotal);
            const double busy = dTotal - static_cast<double>(idleTime - m_prevIdle);
            const double others = std::max(0.0, busy - static_cast<double>(own - m_prevOwn));
            m_cpuOthers = std::clamp(100.0 * others / dTotal, 0.0, 100.0);
        }
        m_haveFirst = true;
        m_prevTotal = total;
        m_prevIdle = idleTime;
        m_prevOwn = own;
    }

    // NVML (the NVIDIA driver's management library, in System32 with every NVIDIA driver; loaded at run time, so a computer
    // without one simply reports the GPU as unknown). https://docs.nvidia.com/deploy/nvml-api/
    struct Utilization {
        unsigned gpu = 0;
        unsigned memory = 0;
    };
    using InitFn = int (*)();
    using CountFn = int (*)(unsigned*);
    using HandleFn = int (*)(unsigned, void**);
    using UtilFn = int (*)(void*, Utilization*);

    double readGpu() {
        if (!m_nvmlTried) {
            m_nvmlTried = true;
            m_nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m_nvml) {
                auto init = reinterpret_cast<InitFn>(GetProcAddress(m_nvml, "nvmlInit_v2"));
                m_count = reinterpret_cast<CountFn>(GetProcAddress(m_nvml, "nvmlDeviceGetCount_v2"));
                m_handle = reinterpret_cast<HandleFn>(GetProcAddress(m_nvml, "nvmlDeviceGetHandleByIndex_v2"));
                m_util = reinterpret_cast<UtilFn>(GetProcAddress(m_nvml, "nvmlDeviceGetUtilizationRates"));
                m_shutdown = reinterpret_cast<InitFn>(GetProcAddress(m_nvml, "nvmlShutdown"));
                if (!init || !m_count || !m_handle || !m_util || init() != 0) {
                    m_count = nullptr; // unusable
                    m_shutdown = nullptr;
                }
            }
        }
        if (!m_count) {
            return -1.0;
        }
        unsigned count = 0;
        if (m_count(&count) != 0 || count == 0) {
            return -1.0;
        }
        double busiest = -1.0;
        for (unsigned i = 0; i < count && i < 8; ++i) {
            void* device = nullptr;
            Utilization u;
            if (m_handle(i, &device) == 0 && m_util(device, &u) == 0) {
                busiest = std::max(busiest, static_cast<double>(u.gpu));
            }
        }
        return busiest;
    }

    std::mutex m_mutex;
    bool m_haveFirst = false;
    Clock::time_point m_lastCpu{};
    std::uint64_t m_prevTotal = 0;
    std::uint64_t m_prevIdle = 0;
    std::uint64_t m_prevOwn = 0;
    double m_cpuOthers = -1.0;
    bool m_nvmlTried = false;
    HMODULE m_nvml = nullptr;
    CountFn m_count = nullptr;
    HandleFn m_handle = nullptr;
    UtilFn m_util = nullptr;
    InitFn m_shutdown = nullptr;
};

#else

// Linux and macOS: the load is not read (yet): only input, playback, exports and the other jobs gate it there.
class UnknownLoadProbe final : public SystemLoadProbe {
public:
    SystemLoad sample(bool) override { return {}; }
};

#endif

} // namespace

std::unique_ptr<SystemLoadProbe> makeSystemLoadProbe() {
#ifdef _WIN32
    return std::make_unique<WindowsLoadProbe>();
#else
    return std::make_unique<UnknownLoadProbe>();
#endif
}

// ---- The worker ---------------------------------------------------------------------------------------------------------

struct IdleRenderer::Shared {
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::shared_ptr<PreviewCache> cache;
    std::shared_ptr<PreviewRenderer> renderer;
    IdleRenderHost host;
    IdleRenderSettings settings;
    IdleRenderStatus status;
    std::chrono::milliseconds poll{250};
    bool wakeFlag = false;
    std::atomic<bool> abortCurrent{false};
    double maxPause = kMaxPauseSeconds;
    std::set<std::string> failed;     ///< failureKey()s
    // The plan of the last look, reused while nothing it depends on changed.
    const ProjectState* planState = nullptr;
    SequenceId planSequence;
    std::int64_t planBytes = -1;
    std::size_t planEntries = 0;
    std::size_t planFailed = 0;
    Ticks planPlayhead{-1};
    std::vector<RenderSegment> plan;
    bool havePlan = false;

    void setStatus(const IdleRenderStatus& next) {
        // `generation` counts changes of what the UI shows; progress moves it by whole percents.
        std::lock_guard lock(mutex);
        IdleRenderStatus updated = next;
        updated.generation = status.generation;
        const bool same = updated.phase == status.phase && updated.block == status.block &&
                          updated.current == status.current && updated.remaining == status.remaining &&
                          updated.rendered == status.rendered && updated.failed == status.failed &&
                          updated.lastError == status.lastError &&
                          static_cast<int>(updated.progress * 100.0) == static_cast<int>(status.progress * 100.0);
        status = std::move(updated);
        if (!same) {
            ++status.generation;
        }
    }
};

IdleRenderer::IdleRenderer(std::shared_ptr<PreviewCache> cache, std::shared_ptr<PreviewRenderer> renderer,
                           IdleRenderHost host, IdleRenderSettings settings)
    : m_shared(std::make_shared<Shared>()) {
    if (!host.probe) {
        host.probe = std::shared_ptr<SystemLoadProbe>(makeSystemLoadProbe());
    }
    m_shared->cache = std::move(cache);
    m_shared->renderer = std::move(renderer);
    m_shared->host = std::move(host);
    m_shared->settings = settings;
    m_shared->status.phase = settings.enabled ? IdleRenderStatus::Phase::Waiting : IdleRenderStatus::Phase::Off;
    m_shared->status.block = settings.enabled ? IdleBlock::Input : IdleBlock::Disabled;
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

IdleRenderer::~IdleRenderer() {
    stop();
}

void IdleRenderer::stop() {
    if (m_thread.joinable()) {
        m_shared->abortCurrent = true;
        m_thread.request_stop();
        m_shared->cv.notify_all();
        m_thread.join();
    }
}

void IdleRenderer::setSettings(const IdleRenderSettings& settings) {
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->settings = settings;
        m_shared->wakeFlag = true;
    }
    if (!settings.enabled) {
        m_shared->abortCurrent = true; // a segment in hand is given up at its next frame
    }
    m_shared->cv.notify_all();
}

IdleRenderSettings IdleRenderer::settings() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->settings;
}

IdleRenderStatus IdleRenderer::status() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->status;
}

void IdleRenderer::reset(std::shared_ptr<PreviewCache> cache, std::shared_ptr<PreviewRenderer> renderer) {
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->cache = std::move(cache);
        m_shared->renderer = std::move(renderer);
        m_shared->failed.clear();
        m_shared->havePlan = false;
        m_shared->plan.clear();
        const std::uint64_t generation = m_shared->status.generation + 1;
        m_shared->status = {};
        m_shared->status.generation = generation;
        m_shared->status.phase = m_shared->settings.enabled ? IdleRenderStatus::Phase::Waiting : IdleRenderStatus::Phase::Off;
        m_shared->status.block = m_shared->settings.enabled ? IdleBlock::Input : IdleBlock::Disabled;
        m_shared->wakeFlag = true;
    }
    m_shared->abortCurrent = true;
    m_shared->cv.notify_all();
}

void IdleRenderer::cancelCurrent() {
    m_shared->abortCurrent = true;
    m_shared->cv.notify_all();
}

void IdleRenderer::wake() {
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->wakeFlag = true;
    }
    m_shared->cv.notify_all();
}

void IdleRenderer::setPollInterval(std::chrono::milliseconds interval) {
    std::lock_guard lock(m_shared->mutex);
    m_shared->poll = std::max(std::chrono::milliseconds(1), interval);
}

void IdleRenderer::setMaxPauseSeconds(double seconds) {
    std::lock_guard lock(m_shared->mutex);
    m_shared->maxPause = std::max(0.0, seconds);
}

namespace {

/// The editor as the worker sees it right now.
struct Look {
    IdleRenderFocus focus;
    IdleInputs inputs;
};

Look lookAtEditor(IdleRenderer::Shared& s) {
    Look look;
    if (s.host.focus) {
        look.focus = s.host.focus();
    }
    if (s.host.inputs) {
        look.inputs = s.host.inputs();
    }
    look.inputs.hasSequence = look.focus.snapshot && look.focus.sequence.isValid() &&
                              look.focus.snapshot->findSequence(look.focus.sequence) != nullptr;
    return look;
}

/// Renders `segment` of `focus` and puts it in the cache. Returns with the status updated.
void renderSegment(IdleRenderer::Shared& s, const std::stop_token& stop, const std::shared_ptr<PreviewCache>& cache,
                   const std::shared_ptr<PreviewRenderer>& renderer, const IdleRenderFocus& focus,
                   const RenderSegment& segment, IdleRenderStatus st) {
    const SequenceId sequence = focus.sequence;
    const ProjectSnapshot snapshot = focus.snapshot;
    const std::string hash = renderContentHash(*snapshot, sequence, segment.range);
    if (hash.empty()) {
        return;
    }
    const std::string file = PreviewCache::fileNameFor(sequence, segment.range, hash);
    const fs::path path = cache->folder() / file;
    std::error_code ec;
    fs::create_directories(cache->folder(), ec);

    s.abortCurrent = false;
    st.phase = IdleRenderStatus::Phase::Rendering;
    st.block = IdleBlock::None;
    st.current = segment.range;
    st.progress = 0.0;
    s.setStatus(st);

    enum class Abort { None, Stopped, Disabled, Cancelled, Stale, PausedTooLong };
    Abort abort = Abort::None;
    std::optional<Clock::time_point> pausedSince;
    const ProjectState* checked = snapshot.get(); // the newest project state known to still show this segment as rendered

    const auto progress = [&](double fraction) -> bool {
        for (;;) {
            if (stop.stop_requested()) {
                abort = Abort::Stopped;
                return false;
            }
            if (s.abortCurrent.load()) {
                abort = Abort::Cancelled;
                return false;
            }
            IdleRenderSettings settings;
            double maxPause = kMaxPauseSeconds;
            {
                std::lock_guard lock(s.mutex);
                settings = s.settings;
                maxPause = s.maxPause;
            }
            if (!settings.enabled) {
                abort = Abort::Disabled;
                return false;
            }
            const Look look = lookAtEditor(s);
            const SystemLoad load = s.host.probe->sample(false);
            const IdleVerdict verdict = idleVerdict(settings, look.inputs, load, true);
            if (verdict.run) {
                // An edit may have changed what this segment shows: its file would be stale. Looked at once per new
                // project state, and only here (while quiet), so a burst of edits costs one hash.
                if (look.focus.snapshot && look.focus.snapshot.get() != checked) {
                    if (renderContentHash(*look.focus.snapshot, sequence, segment.range) != hash) {
                        abort = Abort::Stale;
                        return false;
                    }
                    checked = look.focus.snapshot.get();
                }
                break;
            }
            // Something needs the computer: wait here, with the encoder and its frames as they are, until it is quiet again.
            const auto now = Clock::now();
            if (!pausedSince) {
                pausedSince = now;
            }
            if (std::chrono::duration<double>(now - *pausedSince).count() > maxPause) {
                abort = Abort::PausedTooLong;
                return false;
            }
            st.phase = IdleRenderStatus::Phase::Paused;
            st.block = verdict.block;
            st.progress = fraction;
            s.setStatus(st);
            std::unique_lock lock(s.mutex);
            s.cv.wait_for(lock, stop, std::chrono::milliseconds(50), [&] { return s.abortCurrent.load(); });
        }
        pausedSince.reset();
        st.phase = IdleRenderStatus::Phase::Rendering;
        st.block = IdleBlock::None;
        st.progress = std::clamp(fraction, 0.0, 1.0);
        s.setStatus(st);
        return true;
    };

    auto result = renderer->render(*snapshot, sequence, segment.range, path, progress);
    st.progress = 0.0;
    if (result) {
        // An edit can land between the last frame and here: a file that no longer shows the project is not recorded.
        const Look look = lookAtEditor(s);
        const bool fresh = !look.focus.snapshot || look.focus.snapshot.get() == snapshot.get() ||
                           renderContentHash(*look.focus.snapshot, sequence, segment.range) == hash;
        if (fresh) {
            cache->record(PreviewEntry{sequence, segment.range, hash, file, *result, 0});
            (void)cache->save();
            ++st.rendered;
            st.renderedBytes += *result;
        } else {
            fs::remove(path, ec);
        }
    } else if (abort != Abort::None || result.error().code == ErrorCode::Cancelled) {
        fs::remove(path, ec); // the renderer leaves none; defensive
    } else {
        std::lock_guard lock(s.mutex);
        s.failed.insert(failureKey(segment.range, hash));
        ++st.failed;
        st.lastError = result.error().message;
        fs::remove(path, ec);
    }
    if (abort == Abort::Stopped) {
        return;
    }
    st.phase = IdleRenderStatus::Phase::Waiting;
    st.block = IdleBlock::Input;
    s.setStatus(st);
    {
        std::lock_guard lock(s.mutex);
        s.havePlan = false;
        s.wakeFlag = true; // on to the next segment without waiting for the tick
    }
}

/// One look at the editor and, when it may, one segment rendered.
void step(IdleRenderer::Shared& s, const std::stop_token& stop) {
    IdleRenderSettings settings;
    std::shared_ptr<PreviewCache> cache;
    std::shared_ptr<PreviewRenderer> renderer;
    IdleRenderStatus st;
    {
        std::lock_guard lock(s.mutex);
        settings = s.settings;
        cache = s.cache;
        renderer = s.renderer;
        st = s.status;
    }
    if (!settings.enabled) {
        st.phase = IdleRenderStatus::Phase::Off;
        st.block = IdleBlock::Disabled;
        s.setStatus(st);
        return;
    }
    const Look look = lookAtEditor(s);
    if (!cache || !renderer) {
        st.phase = IdleRenderStatus::Phase::Waiting;
        st.block = IdleBlock::NoSequence;
        s.setStatus(st);
        return;
    }
    const SystemLoad load = s.host.probe->sample(true);
    const IdleVerdict verdict = idleVerdict(settings, look.inputs, load, false);
    if (!verdict.run) {
        st.phase = IdleRenderStatus::Phase::Waiting;
        st.block = verdict.block;
        st.progress = 0.0;
        s.setStatus(st);
        return;
    }
    // What is left to render: the plan is kept while the project, the cache and the failures are as they were.
    std::vector<RenderSegment> plan;
    {
        const std::int64_t bytes = cache->totalBytes();
        const std::size_t entries = cache->entries().size();
        std::set<std::string> failed;
        bool same = false;
        {
            std::lock_guard lock(s.mutex);
            failed = s.failed;
            same = s.havePlan && s.planState == look.focus.snapshot.get() && s.planSequence == look.focus.sequence &&
                   s.planBytes == bytes && s.planEntries == entries && s.planFailed == failed.size() &&
                   s.planPlayhead == look.focus.playhead;
            if (same) {
                plan = s.plan;
            }
        }
        if (!same) {
            // Off the lock: hashing a long sequence's segments takes a while and the GUI reads status() meanwhile.
            plan = idleRenderPlan(*look.focus.snapshot, *cache, look.focus.sequence, look.focus.playhead, failed);
            std::lock_guard lock(s.mutex);
            s.plan = plan;
            s.havePlan = true;
            s.planState = look.focus.snapshot.get();
            s.planSequence = look.focus.sequence;
            s.planBytes = bytes;
            s.planEntries = entries;
            s.planFailed = failed.size();
            s.planPlayhead = look.focus.playhead;
        }
    }
    st.remaining = static_cast<int>(plan.size());
    st.progress = 0.0;
    if (plan.empty()) {
        st.phase = IdleRenderStatus::Phase::UpToDate;
        st.block = IdleBlock::None;
        s.setStatus(st);
        return;
    }
    // Room: not past the cache's size budget (rendering more would evict what was rendered before) and not on a full drive.
    const RenderSegment& next = plan.front();
    const std::int64_t estimate = estimatePreviewBytes(*look.focus.snapshot, look.focus.sequence, next.range);
    bool full = cache->totalBytes() + estimate > cache->budget();
    if (!full) {
        fs::path probe = cache->folder();
        std::error_code ec;
        while (!probe.empty() && !fs::exists(probe, ec) && probe.has_parent_path() && probe.parent_path() != probe) {
            probe = probe.parent_path();
        }
        const fs::space_info space = fs::space(probe, ec);
        full = !ec && static_cast<std::int64_t>(space.available) < estimate + kIdleRenderDiskMargin;
    }
    if (full) {
        st.phase = IdleRenderStatus::Phase::Full;
        st.block = IdleBlock::None;
        s.setStatus(st);
        return;
    }
    renderSegment(s, stop, cache, renderer, look.focus, next, st);
}

} // namespace

void IdleRenderer::run(std::stop_token stop) {
    // The whole worker runs in the OS background mode, and the encoders / converters / render threads of the preview
    // export it starts use half the cores at most (or the user's own, smaller export limit).
    BackgroundThreadScope background;
    const int half = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
    exporter::ScopedExportThreadLimit threads(half);
    exporter::ScopedExportBackgroundMode helpers; // the render, decode and convert threads of the export it starts too
    Shared& s = *m_shared;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(s.mutex);
            const std::chrono::milliseconds poll = s.poll;
            s.cv.wait_for(lock, stop, poll, [&] { return s.wakeFlag; });
            s.wakeFlag = false;
        }
        if (stop.stop_requested()) {
            break;
        }
        step(s, stop);
    }
}

} // namespace wabcut::editor
