// wabcut_playback_bench: real-GPU playback benchmark (contract rev 2, wave-2 perf harness).
//
// Plays N distinct streams stacked on V1..Vn (with transforms and opacity) through the real pipeline -
// TransportClock (free-run, 1x) -> Scheduler (decode sessions on worker threads) -> CompositeMailbox ->
// Compositor on a headless QRhi
// - with this thread acting as the program monitor's render thread at a 60 Hz refresh (or the sequence's rate when
// that is higher: a 144 / 240 Hz gaming monitor). Reports latency
// percentiles against the frame budget:
//   decode           VideoDecoder::frameAt per frame (worker threads; hardware transfer to CPU included)
//   upload+composite import (CPU->GPU upload) + layer pass + output pass, until the GPU finished (render
//   thread) composite only   the same composite re-rendered with its frames resident (no upload) frame age
//   publish -> display (how far ahead of display the scheduler is) display offset   transport time at display
//   - the frame's sequence time (A/V offset; > 1 frame = late)
// plus presented / duplicated / dropped / late / abandoned frames, process CPU and GPU memory (DXGI,
// Windows).
//
//   wabcut_playback_bench --media DIR [--config 3x1080p60|6x1080p60|1x2160p30|3x1080p60pq|1x1440p144|1x1080p240|smoke]
//                         [--seconds 20]
//                         [--software] [--backend pool|sessions] [--zero-copy] [--effects] [--divisor 1]
//                         [--nest | --multicam N] [--color rec709|aces|hdr-pq|hdr-hlg] [--input SPACE] [--pq-output]
//                         [--out report.md]
//
// --nest: the config's layers go into a nested sequence, played through one nested clip (at 90 %) on V1 (C.8).
// --multicam N: the config's first N streams become the angles of a multicam source, played through multicam clips on
// V1 that cut to the next angle every 2 s (C.8; needs N streams in the config, e.g. 6x1080p60 for 4).
// I.9 (gaming): 3x1080p60pq plays three HDR game captures (HEVC Main 10, BT.2020 PQ) tone-mapped into the SDR
// sequence; 1x1440p144 and 1x1080p240 one high-frame-rate capture in a sequence of its rate.
// --color MODE: the project's colour management (I.9): the monitor shows the ACES SDR output or the tone-mapped SDR
// preview of HDR. --input SPACE: every clip's input colour space (e.g. s-log3: the camera-log LUT path; default Auto).
// --pq-output: the monitor stand-in renders the PQ output an HDR display would get (the export role, PQ target).

#include <wabcut/media/Probe.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QGuiApplication>

#include <rhi/qrhi.h>

#if defined(_WIN32)
#include <rhi/qrhi_platform.h>
// clang-format off: windows.h first
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <timeapi.h>
// clang-format on
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace std::chrono_literals;

namespace {

struct Layer {
    std::string file;
    ClipProperties props;
};

struct Config {
    std::string name;
    int width = 1920;
    int height = 1080;
    Rate rate = kRate60;
    std::vector<Layer> layers;
};

ClipProperties motion(double scale, double x, double y, double rotation, double opacity) {
    ClipProperties p;
    p.set(ParamId::Scale, scale);
    p.set(ParamId::PositionX, x);
    p.set(ParamId::PositionY, y);
    p.set(ParamId::Rotation, rotation);
    p.set(ParamId::Opacity, opacity);
    return p;
}

Config makeConfig(const std::string& name) {
    Config c;
    c.name = name;
    if (name == "1x1440p144" || name == "1x1080p240") { // I.9 (gaming): high-frame-rate captures
        const bool p1440 = name == "1x1440p144";
        c.width = p1440 ? 2560 : 1920;
        c.height = p1440 ? 1440 : 1080;
        c.rate = p1440 ? Rate{144, 1} : Rate{240, 1};
        c.layers = {{p1440 ? "perf_1440p144.mp4" : "perf_1080p240.mp4", {}}};
        return c;
    }
    if (name == "1x2160p30") {
        c.width = 3840;
        c.height = 2160;
        c.rate = kRate30;
        c.layers = {{"perf_2160p30.mp4", {}}};
        return c;
    }
    const bool smoke = name == "smoke";
    const char* prefix = smoke ? "perf_smoke_" : name == "3x1080p60pq" ? "perf_1080p60_pq_" : "perf_1080p60_";
    const int count = name == "6x1080p60" ? 6 : 3;
    c.layers.push_back({std::string(prefix) + "a.mp4", {}}); // V1 full frame
    // V2..: picture-in-picture with motion and opacity (all visible: nothing is occlusion-culled).
    const ClipProperties pips[] = {motion(0.5, -480, -270, 0, 0.85), motion(0.45, 480, 270, 12, 0.7),
                                   motion(0.33, 600, -330, -8, 0.9), motion(0.33, -600, 330, 5, 0.6),
                                   motion(0.3, 0, 0, 45, 0.75)};
    for (int i = 1; i < count; ++i) {
        c.layers.push_back({std::string(prefix) + static_cast<char>('a' + i) + ".mp4", pips[i - 1]});
    }
    return c;
}

/// A vsync stand-in: sleep coarsely, then spin the last stretch (Windows sleeps overshoot by a timer period).
void waitUntil(std::chrono::steady_clock::time_point due) {
    const auto coarse = due - std::chrono::microseconds(1500);
    if (std::chrono::steady_clock::now() < coarse) {
        std::this_thread::sleep_until(coarse);
    }
    while (std::chrono::steady_clock::now() < due) {
        std::this_thread::yield();
    }
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(std::lround(q * static_cast<double>(v.size() - 1)));
    return v[std::min(i, v.size() - 1)];
}

double overBudget(const std::vector<double>& v, double budget) {
    if (v.empty()) {
        return 0.0;
    }
    return 100.0 *
           static_cast<double>(std::count_if(v.begin(), v.end(), [&](double x) { return x > budget; })) /
           static_cast<double>(v.size());
}

double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME c{}, e{}, k{}, u{};
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        const auto t = [](FILETIME f) { return (std::uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime; };
        return static_cast<double>(t(k) + t(u)) * 1e-7;
    }
#endif
    return 0.0;
}

/// GPU memory this process uses on the renderer's adapter (DXGI local segment), in MiB; < 0 when unavailable.
double processVramMiB(QRhi* rhi) {
#if defined(_WIN32)
    if (rhi && rhi->backend() == QRhi::D3D11) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        auto* device = handles ? static_cast<ID3D11Device*>(handles->dev) : nullptr;
        IDXGIDevice* dxgiDevice = nullptr;
        double result = -1.0;
        if (device &&
            SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                IDXGIAdapter3* adapter3 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                                      reinterpret_cast<void**>(&adapter3)))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                    if (SUCCEEDED(
                            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                        result = static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
                    }
                    adapter3->Release();
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        return result;
    }
#endif
    (void)rhi;
    return -1.0;
}

std::string row(const char* name, const std::vector<double>& v, double budget) {
    char buf[256];
    if (v.empty()) {
        std::snprintf(buf, sizeof buf, "| %-22s | n/a | n/a | n/a | n/a | n/a |", name);
    } else if (budget > 0) {
        std::snprintf(buf, sizeof buf, "| %-22s | %6.2f | %6.2f | %6.2f | %6.2f | %5.1f %% |", name,
                      percentile(v, 0.5), percentile(v, 0.95), percentile(v, 0.99),
                      *std::max_element(v.begin(), v.end()), overBudget(v, budget));
    } else {
        std::snprintf(buf, sizeof buf, "| %-22s | %6.2f | %6.2f | %6.2f | %6.2f | - |", name,
                      percentile(v, 0.5), percentile(v, 0.95), percentile(v, 0.99),
                      *std::max_element(v.begin(), v.end()));
    }
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv); // shader resources and platform state; no window is created
#if defined(_WIN32)
    timeBeginPeriod(1); // 1 ms timer resolution for the 60 Hz refresh loop (a monitor would be vsync-driven)
#endif
    std::string mediaDir;
    std::string configName = "3x1080p60";
    std::string outPath;
    double seconds = 20.0;
    bool software = false;
    bool sessions = false; // --backend sessions: the private-thread decode path instead of the DecoderPool
    bool zeroCopy = false; // --zero-copy: decoders on the renderer's device, frames sampled in place
    bool effects = false;  // --effects: every layer gets a Gaussian blur (radius 12) and a basic correction
    bool timestamps = true; // --no-timestamps: no GPU timestamp queries (they add a coarse wait on D3D11)
    int divisor = 1;
    bool nest = false; // --nest: the layers inside one nested clip
    int multicam = 0;  // --multicam N: N angles of a multicam source, cut every 2 s
    std::string colorMode = "rec709"; // --color: the project's colour management (I.9)
    std::string inputSpace;           // --input: every clip's input colour space
    bool pqOutput = false;            // --pq-output: render the PQ output (export role) instead of the SDR monitor
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--media") {
            mediaDir = next();
        } else if (a == "--config") {
            configName = next();
        } else if (a == "--seconds") {
            seconds = std::stod(next());
        } else if (a == "--software") {
            software = true;
        } else if (a == "--backend") {
            sessions = next() == "sessions";
        } else if (a == "--zero-copy") {
            zeroCopy = true;
        } else if (a == "--effects") {
            effects = true;
        } else if (a == "--no-timestamps") {
            timestamps = false;
        } else if (a == "--divisor") {
            divisor = std::stoi(next());
        } else if (a == "--nest") {
            nest = true;
        } else if (a == "--multicam") {
            multicam = std::stoi(next());
        } else if (a == "--color") {
            colorMode = next();
        } else if (a == "--input") {
            inputSpace = next();
        } else if (a == "--pq-output") {
            pqOutput = true;
        } else if (a == "--out") {
            outPath = next();
        }
    }
    const Config config = makeConfig(configName);
    const std::optional<ColorManagementMode> mode = colorManagementModeFromString(colorMode);
    const std::optional<InputColorSpace> space =
        inputSpace.empty() ? std::nullopt : inputColorSpaceFromString(inputSpace);
    if (!mode || (!inputSpace.empty() && !space)) {
        std::fprintf(stderr, "unknown --color %s or --input %s\n", colorMode.c_str(), inputSpace.c_str());
        return 2;
    }

    // ---- Project: one track per stream ----
    ProjectState state;
    state.colorManagement.mode = *mode;
    Ticks shortest = Ticks::fromSeconds(3600);
    for (const Layer& layer : config.layers) {
        const std::string path = (std::filesystem::path(mediaDir) / layer.file).string();
        auto info = media::probe(path);
        if (!info) {
            std::fprintf(stderr, "cannot probe %s: %s\n", path.c_str(), info.error().message.c_str());
            return 2;
        }
        auto item = std::make_shared<MediaItem>();
        item->id = state.ids.allocate<MediaId>();
        item->locator.path = path;
        shortest = std::min(shortest, info->duration);
        item->info = std::move(*info);
        state.media.push_back(std::move(item));
    }
    const Ticks length = snapToFrame(shortest, config.rate);
    auto seq = std::make_shared<Sequence>();
    seq->id = state.ids.allocate<SequenceId>();
    seq->frameRate = config.rate;
    seq->width = config.width;
    seq->height = config.height;
    for (std::size_t i = 0; i < config.layers.size(); ++i) {
        Track track;
        track.id = state.ids.allocate<TrackId>();
        Clip clip;
        clip.id = state.ids.allocate<ClipId>();
        clip.media = state.media[i]->id;
        clip.timelineRange = {Ticks{}, length};
        clip.setProperties(config.layers[i].props);
        if (space) {
            ClipContent content = clip.content();
            content.inputColorSpace = space;
            clip.setContent(std::move(content));
        }
        if (effects) {
            ClipContent content = clip.content();
            EffectInstance blur;
            blur.id = state.ids.allocate<EffectInstanceId>();
            blur.effect = "wabcut.blur.gaussian";
            blur.set("radius", ParamValue(12.0));
            EffectInstance correction;
            correction.id = state.ids.allocate<EffectInstanceId>();
            correction.effect = "wabcut.color.basic";
            correction.set("exposure", ParamValue(0.3));
            correction.set("contrast", ParamValue(15.0));
            correction.set("saturation", ParamValue(120.0));
            correction.set("temperature", ParamValue(10.0));
            content.effects = {blur, correction};
            clip.setContent(std::move(content));
        }
        track.clips.push_back(clip);
        seq->videoTracks.push_back(std::make_shared<const Track>(std::move(track)));
    }
    seq->duration = length;
    SequenceId sequenceId = seq->id;
    if (multicam > 0) {
        // The streams as angles of a multicam source (all in sync at 0); the program cuts to the next angle every 2 s.
        if (multicam > static_cast<int>(seq->videoTracks.size())) {
            std::fprintf(stderr, "--multicam %d needs as many streams (the config has %zu)\n", multicam, seq->videoTracks.size());
            return 2;
        }
        seq->videoTracks.resize(static_cast<std::size_t>(multicam));
        MulticamInfo info;
        info.sync = MulticamSync::Manual;
        for (int angle = 0; angle < multicam; ++angle) {
            Track full = *seq->videoTracks[static_cast<std::size_t>(angle)];
            std::vector<Clip> clips;
            for (const Clip& c : full.clips) {
                Clip copy = c;
                copy.setProperties({}); // each angle full frame in the source
                clips.push_back(copy);
            }
            full.clips = ClipList(std::move(clips));
            seq->videoTracks[static_cast<std::size_t>(angle)] = std::make_shared<const Track>(full);
            info.angles.push_back({"Angle " + std::to_string(angle + 1), full.id, {}, Ticks{}});
        }
        seq->multicam = std::move(info);
        auto program = std::make_shared<Sequence>();
        program->id = state.ids.allocate<SequenceId>();
        program->frameRate = config.rate;
        program->width = config.width;
        program->height = config.height;
        program->duration = length;
        Track cuts;
        cuts.id = state.ids.allocate<TrackId>();
        const Ticks every = Ticks::fromSeconds(2);
        for (Ticks t{}; t < length; t += every) {
            Clip c;
            c.id = state.ids.allocate<ClipId>();
            c.sequence = seq->id;
            c.angles = AngleSet::of(static_cast<std::size_t>((t.value / every.value) % multicam));
            c.stream = {StreamKind::Video, 0};
            c.timelineRange = TimeRange::fromStartEnd(t, std::min(length, t + every));
            c.sourceIn = t;
            cuts.clips.push_back(c);
        }
        program->videoTracks.push_back(std::make_shared<const Track>(std::move(cuts)));
        sequenceId = program->id;
        state.sequences.push_back(std::move(seq));
        state.sequences.push_back(std::move(program));
    } else if (nest) {
        // The layers in a nested sequence, played through one nested clip at 90 % (not a trivial full-frame copy).
        auto program = std::make_shared<Sequence>();
        program->id = state.ids.allocate<SequenceId>();
        program->frameRate = config.rate;
        program->width = config.width;
        program->height = config.height;
        program->duration = length;
        Track track;
        track.id = state.ids.allocate<TrackId>();
        Clip c;
        c.id = state.ids.allocate<ClipId>();
        c.sequence = seq->id;
        c.stream = {StreamKind::Video, 0};
        c.timelineRange = {Ticks{}, length};
        ClipProperties p;
        p.set(ParamId::Scale, 0.9);
        c.setProperties(p);
        track.clips.push_back(c);
        program->videoTracks.push_back(std::make_shared<const Track>(std::move(track)));
        sequenceId = program->id;
        state.sequences.push_back(std::move(seq));
        state.sequences.push_back(std::move(program));
    } else {
        state.sequences.push_back(std::move(seq));
    }
    EditSession session(state);
    seconds = std::min(seconds, length.toSeconds() - 0.5);

    // ---- Pipeline ----
    // A 60 Hz monitor, or one as fast as the sequence (144 / 240 Hz gaming monitors).
    const std::int64_t kRefreshNs = std::min<std::int64_t>(16'666'667, std::llround(1e9 / config.rate.toDouble()));
    playback::PlaybackOptions options;
    options.scheduler.collectTimings = true;
    options.scheduler.decoder.allowHardware = !software;
    options.scheduler.decodeBackend =
        sessions ? playback::DecodeBackend::Sessions : playback::DecodeBackend::Pool;
    options.scheduler.zeroCopy = zeroCopy;
    options.scheduler.displayLatencyNs = kRefreshNs;
    if (pqOutput) {
        options.scheduler.color.exportTarget = color::OutputTarget::Pq;
    }
    // The renderer first: with --zero-copy it shares its device before any decoder opens.
    render::OffscreenOptions ro;
    ro.role = pqOutput ? render::OutputRole::Export : render::OutputRole::Display;
    ro.tenBit = pqOutput; // an HDR display's 10 bits
    ro.compositor = {render::TexturePool::Preview, divisor, 48};
    ro.shareDeviceWithDecoders = zeroCopy;
    ro.timestamps = timestamps;
    auto renderer = render::OffscreenRenderer::create(ro);
    if (!renderer) {
        std::fprintf(stderr, "renderer: %s\n", renderer.error().message.c_str());
        return 3;
    }
    playback::PlaybackController controller(options);
    controller.setSession(&session);
    controller.setSequence(sequenceId);
    const QSize monitor(1920, 1080);
    render::CompositeMailbox& mailbox = *controller.mailbox();

    // Warm-up: open decoders, first frame at 0.
    controller.seek(Ticks{});
    const auto warmStart = std::chrono::steady_clock::now();
    while (controller.stats().published == 0 && std::chrono::steady_clock::now() - warmStart < 15s) {
        std::this_thread::sleep_for(5ms);
    }
    (void)mailbox.acquire(playback::steadyNowNs());
    (void)controller.scheduler().takeDecodeSamples();
    const render::MailboxStats m0 = mailbox.stats();
    const playback::SchedulerStats s0 = controller.scheduler().stats();
    const double vramBefore = processVramMiB((*renderer)->rhi());

    // ---- Play ----
    std::vector<double> uploadComposite, compositeOnly, gpuTime, frameAge, offset, refreshWork, intervals;
    int effectPasses = 0;
    std::uint64_t bytes = 0;
    double vramPeak = vramBefore;
    controller.play();
    const auto start = std::chrono::steady_clock::now();
    const double cpuStart = processCpuSeconds();
    std::int64_t lastPresentNs = 0;
    int presentedHere = 0;
    for (std::int64_t k = 0;; ++k) {
        const auto due = start + std::chrono::nanoseconds(k * kRefreshNs);
        if (std::chrono::duration<double>(due - start).count() >= seconds) {
            break;
        }
        waitUntil(due);
        const std::int64_t now = playback::steadyNowNs();
        const auto sel = mailbox.acquire(now + kRefreshNs);
        if (sel.changed && sel.composite) {
            const render::PreparedComposite& c = *sel.composite;
            if (c.publishedNs > 0) {
                frameAge.push_back(static_cast<double>(now - c.publishedNs) / 1e6);
            }
            const Ticks shown = controller.transport()->transportTime(now + kRefreshNs);
            offset.push_back((shown - c.targetTime).toSeconds() * 1000.0);
            if (lastPresentNs > 0) {
                intervals.push_back(static_cast<double>(now - lastPresentNs) / 1e6);
            }
            lastPresentNs = now;
            auto timing = (*renderer)->renderTimed(sel.composite, monitor);
            if (timing) {
                uploadComposite.push_back(timing->cpuMs);
                if (timing->gpuMs >= 0.0) {
                    gpuTime.push_back(timing->gpuMs); // GPU execution (timestamps), without waits
                }
                effectPasses = timing->composite.effectPasses;
                bytes += timing->composite.bytesUploaded;
            }
            if (presentedHere % 4 == 0) {
                if (auto again = (*renderer)->renderTimed(sel.composite, monitor)) {
                    compositeOnly.push_back(again->cpuMs);
                }
            }
            ++presentedHere;
            if (presentedHere % 120 == 0) {
                vramPeak = std::max(vramPeak, processVramMiB((*renderer)->rhi()));
            }
        }
        refreshWork.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - due).count());
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double cpu = processCpuSeconds() - cpuStart;
    vramPeak = std::max(vramPeak, processVramMiB((*renderer)->rhi()));
    controller.pause();
    std::vector<double> decode;
    for (std::int64_t ns : controller.scheduler().takeDecodeSamples()) {
        decode.push_back(static_cast<double>(ns) / 1e6);
    }
    const render::MailboxStats m = mailbox.stats();
    const playback::SchedulerStats s = controller.scheduler().stats();
    const playback::PlaybackStats ps = controller.stats();

    // ---- Report ----
    const double budget = 1000.0 / config.rate.toDouble();
    const double expected = wall * config.rate.toDouble();
    std::ostringstream md;
    char line[512];
    md << "### " << config.name << (software ? " (software decode)" : "")
       << (sessions ? " [decode: private sessions]" : " [decode: DecoderPool]") << (zeroCopy ? " [zero-copy]" : "") << (effects ? " [effects: blur 12 + basic correction per layer]" : "")
       << (nest ? " [nested: the layers inside one nested clip at 90 %]" : "")
       << (*mode != ColorManagementMode::DisplayRec709 ? " [colour: " + colorMode + (space ? ", input " + inputSpace : std::string()) +
                                                              (pqOutput ? ", PQ output (10-bit)" : ", SDR monitor") + "]"
                                                       : std::string())
       << (multicam > 0 ? " [multicam: " + std::to_string(multicam) + " angles, a cut every 2 s]" : std::string())
       << ", " << config.layers.size()
       << " stream(s), sequence " << config.width << "x" << config.height << " @ " << config.rate.toDouble()
       << " fps, monitor 1920x1080, divisor " << divisor << "\n\n";
    md << "- Render: " << (*renderer)->backendName() << " on " << (*renderer)->adapterName()
       << "; decode: " << s.decodeBackends << "; transport: " << ps.transport << "\n";
    std::snprintf(
        line, sizeof line,
        "- %.1f s played: presented %llu of %.0f sequence frames (%.1f %%), duplicated refreshes %llu, "
        "dropped %llu, late %llu, abandoned targets %llu, decode errors %llu\n",
        wall, static_cast<unsigned long long>(m.presented - m0.presented), expected,
        100.0 * static_cast<double>(m.presented - m0.presented) / expected,
        static_cast<unsigned long long>(m.repeated - m0.repeated),
        static_cast<unsigned long long>(m.dropped - m0.dropped),
        static_cast<unsigned long long>(m.late - m0.late),
        static_cast<unsigned long long>(s.skippedLate - s0.skippedLate),
        static_cast<unsigned long long>(s.decodeErrors));
    md << line;
    std::snprintf(
        line, sizeof line,
        "- CPU: %.2f cores busy (%.1f %% of %u logical); upload %.0f MB/s; GPU memory (process, DXGI): "
        "%.0f MiB before play, %.0f MiB peak\n\n",
        cpu / wall, 100.0 * cpu / wall / std::max(1u, std::thread::hardware_concurrency()),
        std::thread::hardware_concurrency(), static_cast<double>(bytes) / wall / 1e6, vramBefore, vramPeak);
    md << line;
    std::snprintf(line, sizeof line, "| %-22s | p50 ms | p95 ms | p99 ms | max ms | > %.2f ms |\n", "stage",
                  budget);
    md << line << "|---|---|---|---|---|---|\n";
    md << row("decode (per frame)", decode, budget) << "\n";
    md << row("upload + composite", uploadComposite, budget) << "\n";
    md << row("composite only", compositeOnly, budget) << "\n";
    md << row("GPU time (timestamps)", gpuTime, budget) << "\n";
    md << row("render-thread refresh", refreshWork, budget) << "\n";
    md << row("present interval", intervals, 0) << "\n";
    md << row("frame age (pub->disp)", frameAge, 0) << "\n";
    md << row("display offset", offset, 0) << "\n";
    const render::FrameImporterStats is = (*renderer)->compositor().importer().stats();
    std::snprintf(line, sizeof line, "\nEffect passes per frame: %d\n", effectPasses);
    md << line;
    std::snprintf(line, sizeof line, "\nImporter: %llu imports, %llu uploads, %llu zero-copy, %llu native rejected\n",
                  static_cast<unsigned long long>(is.imports), static_cast<unsigned long long>(is.uploads),
                  static_cast<unsigned long long>(is.nativeImports),
                  static_cast<unsigned long long>(is.nativeRejected));
    md << line;
    const std::string report = md.str();
    std::fputs(report.c_str(), stdout);
    if (!outPath.empty()) {
        std::filesystem::create_directories(std::filesystem::path(outPath).parent_path());
        std::ofstream(outPath) << report;
    }
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
    return m.presented > m0.presented ? 0 : 1;
}
