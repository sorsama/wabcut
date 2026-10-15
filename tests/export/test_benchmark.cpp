// Export speed benchmark (hidden: run explicitly with `wabcut_export_tests "[.benchmark]"`): a 10 s 1080p60
// 3-layer timeline (synthetic video source) with two AAC audio tracks, H.264 through NVENC (auto) and
// libx264. Results go to stdout and build/<preset>/test-output/export/benchmark/results.txt.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <format>
#include <fstream>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

TEST_CASE("Benchmark: 1080p60 3-layer export, NVENC vs x264", "[.benchmark][export]") {
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("sine_48k.wav")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const Ticks length = Ticks::fromSeconds(10);
    TestProject project(1920, 1080, kRate60, length);
    const MediaId h264 = project.addMedia("h264_1080p60_aac.mp4");
    const MediaId sine = project.addMedia("sine_48k.wav");
    for (int layer = 0; layer < 3; ++layer) {
        project.addVideoTrack(h264, Ticks{}, length, Ticks{});
    }
    project.addAudioTrack(h264, Ticks{}, length, Ticks{});
    project.addAudioTrack(sine, Ticks{}, Ticks::fromSeconds(5), Ticks{}, -6.0);
    const auto dir = freshOutputDir("benchmark");

    struct Run {
        std::string label;
        std::string encoder;
    };
    std::vector<Run> runs;
    const auto vendors = media::gpuVendors();
    if (std::find(vendors.begin(), vendors.end(), "NVIDIA") != vendors.end()) {
        runs.push_back({"NVENC (auto)", "auto"});
    }
    runs.push_back({"libx264 (medium, CRF 16)", "libx264"});

    std::string report =
        "1080p60, 10 s (600 frames), synthetic 3-layer source, 2 audio tracks -> AAC 320k, MP4\n";
    for (const Run& run : runs) {
        ExportSettings settings = findPreset("match-source")->settings;
        settings.video.encoder = run.encoder;
        double best = 0.0;
        ExportResult bestResult;
        for (int attempt = 0; attempt < 3; ++attempt) { // the machine is shared: keep the best of three
            const auto r =
                ExportJob(makeRequest(project, settings, dir / std::format("{}.mp4", run.encoder))).run();
            INFO((r ? std::string() : r.error().message));
            REQUIRE(r.has_value());
            if (r->realtimeFactor > best) {
                best = r->realtimeFactor;
                bestResult = *r;
            }
        }
        const std::string line =
            std::format("{:<28} encoder {:<12} {:6.2f}x realtime  {:6.1f} fps  {:5.2f} s  (best of 3)\n",
                        run.label, bestResult.videoEncoder, bestResult.realtimeFactor,
                        static_cast<double>(bestResult.frames) / bestResult.seconds, bestResult.seconds);
        report += line;
    }
    std::fputs(report.c_str(), stdout);
    std::ofstream(dir / "results.txt") << report;
}

TEST_CASE("Benchmark: rendered 1080p60 3-layer timeline, NVENC vs x264", "[.benchmark][export][render]") {
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("sine_48k.wav")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const Ticks length = Ticks::fromSeconds(10);
    TestProject project(1920, 1080, kRate60, length);
    const MediaId h264 = project.addMedia("h264_1080p60_aac.mp4");
    const MediaId sine = project.addMedia("sine_48k.wav");
    project.addTrack(TrackKind::Video, {project.clip(h264, StreamKind::Video, Ticks{}, length, Ticks{})});
    Clip pip = project.clip(h264, StreamKind::Video, Ticks{}, Ticks::fromSeconds(8), Ticks::fromSeconds(2));
    ClipProperties pipProps;
    pipProps.set(ParamId::Scale, 0.5);
    pipProps.set(ParamId::PositionX, 400.0);
    pipProps.set(ParamId::PositionY, -200.0);
    pipProps.set(ParamId::Opacity, 0.8);
    pip.setProperties(pipProps);
    project.addTrack(TrackKind::Video, {pip});
    Clip corner = project.clip(h264, StreamKind::Video, Ticks{}, length, Ticks{});
    ClipProperties cornerProps;
    cornerProps.set(ParamId::Scale, 0.35);
    cornerProps.set(ParamId::PositionX, -500.0);
    cornerProps.set(ParamId::PositionY, 250.0);
    cornerProps.set(ParamId::Rotation, 15.0);
    cornerProps.set(ParamId::Opacity, 0.9);
    corner.setProperties(cornerProps);
    project.addTrack(TrackKind::Video, {corner});
    project.addAudioTrack(h264, Ticks{}, length, Ticks{});
    project.addAudioTrack(sine, Ticks{}, Ticks::fromSeconds(5), Ticks{}, -6.0);
    const auto dir = freshOutputDir("benchmark_rendered");

    struct Run {
        std::string label;
        std::string encoder;
        bool hardwareDecode = false;
    };
    std::vector<Run> runs;
    const auto vendors = media::gpuVendors();
    if (std::find(vendors.begin(), vendors.end(), "NVIDIA") != vendors.end()) {
        runs.push_back({"NVENC, software decode", "auto", false});
        runs.push_back({"NVENC, hardware decode", "auto", true});
    }
    runs.push_back({"libx264 medium, sw decode", "libx264", false});

    std::string report =
        "Rendered 1080p60 timeline, 10 s (600 frames): 3 layers of 1080p60 H.264 (full frame, "
        "PiP 50 % + opacity, rotated 35 %), 2 audio tracks -> AAC, MP4\n";
    for (const Run& run : runs) {
        ExportSettings settings = findPreset("match-source")->settings;
        settings.video.encoder = run.encoder;
        RenderedSourceOptions source;
        source.hardwareDecode = run.hardwareDecode;
        ExportResult best;
        for (int attempt = 0; attempt < 3; ++attempt) { // the machine is shared: keep the best of three
            ExportRequest request = makeRequest(project, settings, dir / std::format("{}.mp4", attempt));
            request.videoSource = renderedVideoSourceFactory(source);
            const auto r = ExportJob(std::move(request)).run();
            INFO((r ? std::string() : r.error().message));
            REQUIRE(r.has_value());
            if (r->realtimeFactor > best.realtimeFactor) {
                best = *r;
            }
        }
        report +=
            std::format("{:<28} encoder {:<11} {:6.2f}x realtime  {:6.1f} fps  {:5.2f} s  (best of 3; {})\n",
                        run.label, best.videoEncoder, best.realtimeFactor,
                        static_cast<double>(best.frames) / best.seconds, best.seconds, best.videoSource);
    }
    std::fputs(report.c_str(), stdout);
    std::ofstream(dir / "results.txt") << report;
}
