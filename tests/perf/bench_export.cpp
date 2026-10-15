// P.1 export throughput (docs/performance.md): a short timeline of the benchmark media exported through the real
// pipeline, the way the app and `wabcut-cli render` run it: ExportJob with the production video source (the snapshot
// evaluated, decoded and composited on the GPU by render::OffscreenRenderer, read back to RGBA8) and the clip's sound
// mixed and encoded to AAC. The matrix is {1080p: perf_h264_1080p30.mp4, 4 s; 2160p: perf_hevc_2160p30.mp4, 2 s} x
// {H.264, HEVC} x {NVENC, x264 / x265}, at the Match Source preset's quality (CRF / CQ 16), into MP4. The software
// encoders run the export's fixed "medium" preset (no setting changes it), so libx265 at 2160p exports 1 s only.
//
// Each combination reports the frames encoded per wall second of the whole job (set-up included: encoder, renderers,
// decoders, muxer) and its realtime factor, with the wall time as a note. An encoder this machine lacks (no NVIDIA GPU,
// not built into FFmpeg, refused at this size) is listed under details.skipped and has no metric.

#include "PerfHarness.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/ProjectState.h>

#include <QCoreApplication>
#include <QGuiApplication>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace wabcut;

namespace {

using Clock = std::chrono::steady_clock;

std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/// The rendered video source composites with QRhi, which needs a QGuiApplication in the process (no window is ever
/// created). The suite may have one already; otherwise this one lives for the test.
std::unique_ptr<QGuiApplication> guiApplication() {
    static int qtArgc = 1;
    static char qtArgv0[] = "wabcut_perf_tests";
    static char* qtArgv[] = {qtArgv0, nullptr};
    if (QCoreApplication::instance() != nullptr) {
        return nullptr;
    }
#if defined(__linux__)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif
    return std::make_unique<QGuiApplication>(qtArgc, qtArgv);
}

/// One clip in a sequence of its size at 30 fps: V1 the clip from 1 s for `length`, A1 its sound.
struct Timeline {
    std::string label; ///< "1080p", "2160p"
    std::filesystem::path file;
    ProjectSnapshot snapshot;
    SequenceId sequence;
    int width = 0;
    int height = 0;
    Ticks length;
};

Timeline timelineOf(std::string label, const std::filesystem::path& file, Ticks length) {
    const auto info = media::probe(utf8(file));
    INFO("probing " << utf8(file) << ": " << (info ? std::string("ok") : info.error().message));
    REQUIRE(info.has_value());
    REQUIRE(info->hasVideo());
    REQUIRE(info->hasAudio());

    ProjectState state;
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = utf8(file.filename());
    item.locator.path = utf8(file);
    item.info = *info;
    const MediaId mediaId = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));

    const auto trackOf = [&](TrackKind kind) {
        Clip clip;
        clip.id = state.ids.allocate<ClipId>();
        clip.media = mediaId;
        clip.stream = {kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio, 0};
        clip.timelineRange = {Ticks{}, length};
        clip.sourceIn = Ticks::fromSeconds(1);
        Track track;
        track.id = state.ids.allocate<TrackId>();
        track.kind = kind;
        track.clips.push_back(std::move(clip));
        return std::make_shared<const Track>(std::move(track));
    };
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Export benchmark " + label;
    seq.width = info->video.front().width;
    seq.height = info->video.front().height;
    seq.frameRate = kRate30;
    seq.duration = length;
    seq.videoTracks.push_back(trackOf(TrackKind::Video));
    seq.audioTracks.push_back(trackOf(TrackKind::Audio));

    Timeline timeline;
    timeline.label = std::move(label);
    timeline.file = file;
    timeline.sequence = seq.id;
    timeline.width = seq.width;
    timeline.height = seq.height;
    timeline.length = length;
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    timeline.snapshot = std::make_shared<const ProjectState>(std::move(state));
    return timeline;
}

struct Combination {
    std::string name;    ///< "h264_1080p_nvenc": the metric names' stem
    std::string encoder; ///< the FFmpeg encoder, used as is (no fallback)
    exporter::VideoCodec codec = exporter::VideoCodec::H264;
    bool hardware = false;
    const Timeline* timeline = nullptr;
    Ticks length; ///< exported from the sequence start
};

exporter::ExportSettings settingsFor(const Combination& c) {
    exporter::ExportSettings s = exporter::findPreset("match-source").value().settings;
    s.container = exporter::Container::Mp4;
    s.video.codec = c.codec;
    s.video.encoder = c.encoder;
    s.range = exporter::RangeMode::Custom;
    s.customRange = TimeRange{Ticks{}, c.length};
    return s;
}

/// Why `c` cannot run on this machine; nullopt when it can. The encoder is opened (and a hardware one test-encodes a
/// frame) exactly as the export decides, outside the measurement.
std::optional<std::string> unavailable(const Combination& c, const std::vector<std::string>& vendors) {
    if (c.hardware && std::find(vendors.begin(), vendors.end(), "NVIDIA") == vendors.end()) {
        return std::string("no NVIDIA GPU");
    }
    const exporter::ExportSettings s = settingsFor(c);
    const auto probe = exporter::probeVideoEncoder(s.video, c.timeline->width, c.timeline->height, kRate30);
    if (!probe) {
        return probe.error().message;
    }
    return std::nullopt;
}

struct Measured {
    exporter::ExportResult result;
    double setupSeconds = 0.0;     ///< job start to the first frame (encoder, video source, audio mix, muxer)
    double renderingSeconds = 0.0; ///< first frame to the trailer
    std::uintmax_t bytes = 0;
};

/// One export of `c` to `output` (deleted afterwards). The request has no video source factory: the production one.
std::expected<Measured, Error> exportOnce(const Combination& c, const std::filesystem::path& output) {
    exporter::ExportRequest request;
    request.snapshot = c.timeline->snapshot;
    request.sequence = c.timeline->sequence;
    request.settings = settingsFor(c);
    request.output = output;
    std::optional<Clock::time_point> rendering;
    std::optional<Clock::time_point> finalizing;
    request.onProgress = [&rendering, &finalizing](const exporter::ExportProgress& p) {
        const Clock::time_point now = Clock::now();
        if (p.phase == exporter::ExportPhase::Rendering && !rendering) {
            rendering = now;
        } else if (p.phase == exporter::ExportPhase::Finalizing && !finalizing) {
            finalizing = now;
        }
    };
    const Clock::time_point started = Clock::now();
    auto result = exporter::ExportJob(std::move(request)).run();
    if (!result) {
        return std::unexpected(result.error());
    }
    Measured m;
    m.result = std::move(*result);
    const auto seconds = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double>(to - from).count();
    };
    if (rendering) {
        m.setupSeconds = seconds(started, *rendering);
        if (finalizing) {
            m.renderingSeconds = seconds(*rendering, *finalizing);
        }
    }
    std::error_code ec;
    m.bytes = std::filesystem::file_size(m.result.output, ec);
    if (ec) {
        m.bytes = 0;
    }
    std::filesystem::remove(m.result.output, ec);
    return m;
}

} // namespace

TEST_CASE("Benchmark export throughput (1080p/4K H.264/HEVC on NVENC and x264/x265)", "[perf][gpu][export]") {
    const std::filesystem::path mediaDir(WABCUT_PERF_MEDIA_DIR);
    const std::filesystem::path hdFile = mediaDir / "perf_h264_1080p30.mp4";
    const std::filesystem::path uhdFile = mediaDir / "perf_hevc_2160p30.mp4";
    if (!std::filesystem::is_regular_file(hdFile) || !std::filesystem::is_regular_file(uhdFile)) {
        SKIP("the benchmark media were not generated (tests/perf/gen_perf_bench_media.py --out " WABCUT_PERF_MEDIA_DIR
             ")");
    }
    const std::unique_ptr<QGuiApplication> app = guiApplication();
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        SKIP("the process has a QCoreApplication that is not a QGuiApplication: the rendered video source needs one");
    }

    const Timeline hd = timelineOf("1080p", hdFile, Ticks::fromSeconds(4));
    const Timeline uhd = timelineOf("2160p", uhdFile, Ticks::fromSeconds(2));
    struct EncoderCase {
        const char* encoder;
        const char* label;
        exporter::VideoCodec codec;
        bool hardware;
    };
    const EncoderCase encoders[] = {
        {"h264_nvenc", "nvenc", exporter::VideoCodec::H264, true},
        {"hevc_nvenc", "nvenc", exporter::VideoCodec::Hevc, true},
        {"libx264", "x264", exporter::VideoCodec::H264, false},
        {"libx265", "x265", exporter::VideoCodec::Hevc, false},
    };
    std::vector<Combination> combinations;
    for (const Timeline* timeline : {&hd, &uhd}) {
        for (const EncoderCase& e : encoders) {
            Combination c;
            c.name = std::string(exporter::videoCodecName(e.codec)) + "_" + timeline->label + "_" + e.label;
            c.encoder = e.encoder;
            c.codec = e.codec;
            c.hardware = e.hardware;
            c.timeline = timeline;
            // libx265 "medium" encodes 2160p at a few frames per second: one second of it bounds the run time.
            const bool slow = timeline == &uhd && !e.hardware && e.codec == exporter::VideoCodec::Hevc;
            c.length = slow ? Ticks::fromSeconds(1) : timeline->length;
            combinations.push_back(std::move(c));
        }
    }

    const std::vector<std::string> vendors = media::gpuVendors();
    std::vector<Combination> runnable;
    perf::Json skipped = perf::Json::object();
    for (const Combination& c : combinations) {
        if (const auto why = unavailable(c, vendors)) {
            skipped[c.name] = *why;
        } else {
            runnable.push_back(c);
        }
    }
    INFO("skipped: " << skipped.dump());
    REQUIRE_FALSE(runnable.empty());

    const std::filesystem::path dir = std::filesystem::path(perf::outputDir()) / "export-bench";
    std::filesystem::create_directories(dir);
    {
        // Not measured: the process's first rendered export also loads the GPU driver's shader and pipeline caches
        // and the decoders.
        const auto x264 = std::find_if(runnable.begin(), runnable.end(),
                                       [](const Combination& c) { return c.name == "h264_1080p_x264"; });
        Combination warmUp = x264 != runnable.end() ? *x264 : runnable.front();
        warmUp.length = Ticks{kTicksPerSecond / 2};
        const auto warmed = exportOnce(warmUp, dir / "warm-up.mp4");
        INFO("warm-up " << warmUp.name << ": " << (warmed ? std::string("ok") : warmed.error().message));
        REQUIRE(warmed.has_value());
    }

    const exporter::RenderedSourceOptions source{}; // what defaultVideoSourceFactory() renders with
    const perf::BenchmarkOutcome outcome = perf::runBenchmark("export", [&] {
        perf::PerfResult r;
        r.details["media"] = perf::Json{{hd.label, utf8(hd.file)}, {uhd.label, utf8(uhd.file)}};
        r.details["timeline"] = "V1 the clip from 1 s, A1 its AAC sound, sequence at the clip's size and 30 fps";
        r.details["settings"] = perf::Json{
            {"preset", "match-source"},
            {"container", "mp4"},
            {"quality", settingsFor(runnable.front()).video.quality},
            {"audio", "AAC 320 kb/s stereo"},
            {"softwarePreset", "medium (fixed by the export)"},
            {"nvencPreset", "p4, tune hq, vbr + cq"},
            {"lengthSeconds", perf::Json{{hd.label, hd.length.toSeconds()},
                                         {uhd.label, uhd.length.toSeconds()},
                                         {"hevc_" + uhd.label + "_x265", 1.0}}}};
        r.details["videoSource"] = perf::Json{{"factory", "defaultVideoSourceFactory (production, as the app exports)"},
                                              {"hardwareDecode", source.hardwareDecode},
                                              {"renderThreads", source.renderThreads},
                                              {"maxInFlight", source.maxInFlight}};
        r.details["gpuVendors"] = vendors;
        r.details["skipped"] = skipped;
        r.details["runs"] = perf::Json::object();
        for (const Combination& c : runnable) {
            const auto m = exportOnce(c, dir / (c.name + ".mp4"));
            if (!m) {
                r.details["failed"][c.name] = m.error().message;
                FAIL_CHECK(c.name << " (" << c.encoder << ") failed: " << m.error().message);
                continue;
            }
            const exporter::ExportResult& e = m->result;
            const double frames = static_cast<double>(e.frames);
            r.add(c.name + "_fps", "fps", e.seconds > 0.0 ? frames / e.seconds : 0.0, perf::Better::Higher);
            r.add(c.name + "_x_realtime", "x realtime", e.realtimeFactor, perf::Better::Higher);
            r.note(c.name + "_wall_s", "s", e.seconds);
            r.details["runs"][c.name] = perf::Json{
                {"encoder", e.videoEncoder},
                {"audioEncoder", e.audioEncoder},
                {"frames", e.frames},
                {"width", c.timeline->width},
                {"height", c.timeline->height},
                {"durationSeconds", e.duration.toSeconds()},
                {"wallSeconds", e.seconds},
                {"setupSeconds", m->setupSeconds},
                {"renderingSeconds", m->renderingSeconds},
                {"renderingFps", m->renderingSeconds > 0.0 ? frames / m->renderingSeconds : 0.0},
                {"bytes", m->bytes},
                {"videoSource", e.videoSource},
                {"fallbacks", e.encoderFallbacks}};
        }
        return r;
    });
    INFO(outcome.describe());
    CHECK(outcome.ok());
}
