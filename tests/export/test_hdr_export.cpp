// HDR and colour-managed exports (I.9, docs/color.md "Colour management"): PQ and HLG files with their colour tags
// (BT.2020 primaries and matrix, ST 2084 / ARIB STD-B67 transfer), the HDR10 mastering display and the content light
// levels measured over the export (MaxCLL / MaxFALL, CTA-861.3), checked with ffprobe in the container (mdcv / clli
// boxes) and in the bitstream (SEI / metadata OBU on the first frame); the SDR version of an HDR project and an ACES
// export equal the program monitor's picture (preview == export).
//
// The HDR content: the motion timeline's V1 clip read as Rec.2100 PQ (its code values as PQ: highlights far above the
// reference white), in an HDR Rec.2100 PQ project with a 1000-nit master.

#include "ExportTestUtil.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/ColorManagementOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>

#include <format>
#include <fstream>
#include <string>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr Rate kRate{30, 1};
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

ExportSettings presetSettings(std::string_view id) {
    auto p = findPreset(id);
    REQUIRE(p.has_value());
    ExportSettings s = p->settings;
    s.range = RangeMode::Custom;
    s.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)};
    s.video.width.reset(); // the sequence size
    s.video.height.reset();
    return s;
}

ExportRequest renderedRequest(const TestProject& project, ExportSettings settings, const std::filesystem::path& output) {
    ExportRequest r = makeRequest(project, std::move(settings), output);
    r.videoSource = {}; // the rendered source
    return r;
}

/// The motion timeline in a project of `mode`; with `hdrContent`, V1 is read as Rec.2100 PQ.
TestProject managedTimeline(ColorManagementMode mode, bool hdrContent) {
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    ColorManagement m;
    m.mode = mode;
    auto set = edit::setColorManagement(project.state, m);
    REQUIRE(set);
    project.state = *set;
    if (hdrContent) {
        const ClipId v1 = project.seq().videoTracks.front()->clips.front().id;
        const ClipId one[] = {v1};
        auto pq = edit::setClipInputColorSpace(project.state, project.sequence, one, InputColorSpace::Rec2100Pq);
        REQUIRE(pq);
        project.state = pq->state;
    }
    return project;
}

bool contains(const std::string& text, std::string_view what) { return text.find(what) != std::string::npos; }

/// The value of `key=` in ffprobe's text after the first occurrence of `after` (empty when absent).
std::string valueAfter(const std::string& text, std::string_view after, std::string_view key) {
    const auto at = text.find(after);
    if (at == std::string::npos) {
        return {};
    }
    const auto k = text.find(std::string(key) + "=", at);
    if (k == std::string::npos) {
        return {};
    }
    const auto start = k + key.size() + 1;
    return text.substr(start, text.find_first_of("\r\n", start) - start);
}

struct HdrCase {
    const char* name;
    const char* preset;
    ColorManagementMode mode;
    const char* file;
    const char* codec;
    const char* pixelFormat;
    const char* transfer;
    bool mastering; ///< PQ: mastering display + content light levels
    bool needsHardware;
};

} // namespace

TEST_CASE("HDR exports carry BT.2100 tags, the mastering display and the measured content light levels (ffprobe)",
          "[export][render][media][gpu][hdr]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const HdrCase cases[] = {
        {"prores-pq", "prores-hdr-pq", ColorManagementMode::HdrPq, "prores_pq.mov", "prores", "yuv422p10le", "smpte2084", true, false},
        {"av1-pq", "youtube-hdr-pq-av1", ColorManagementMode::HdrPq, "av1_pq.mp4", "av1", "yuv420p10le", "smpte2084", true, false},
        {"hevc-pq", "youtube-hdr-pq", ColorManagementMode::HdrPq, "hevc_pq.mp4", "hevc", "yuv420p10le", "smpte2084", true, true},
        {"hevc-hlg", "hdr-hlg", ColorManagementMode::HdrHlg, "hevc_hlg.mp4", "hevc", "yuv420p10le", "arib-std-b67", false, true},
    };
    for (const HdrCase& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const ExportSettings settings = presetSettings(c.preset);
            if (c.needsHardware) {
                const auto probe = probeVideoEncoder(settings.video, kWidth, kHeight, kRate);
                if (!probe) {
                    SKIP(std::format("no 10-bit {} encoder here: {}", videoCodecName(settings.video.codec), probe.error().message));
                }
            }
            const TestProject project = managedTimeline(c.mode, true);
            const auto dir = freshOutputDir(std::string("hdr-") + c.name);
            const auto out = dir / c.file;
            const auto result = ExportJob(renderedRequest(project, settings, out)).run();
            INFO((result ? std::string() : result.error().message));
            REQUIRE(result.has_value());
            CHECK(result->dynamicRange == (c.mastering ? DynamicRange::Pq : DynamicRange::Hlg));
            WARN(std::format("{}: encoder {}, MaxCLL {} nits, MaxFALL {} nits", c.name, result->videoEncoder, result->maxCll,
                             result->maxFall));

            std::string error;
            const auto streams = ffprobeText(out, "-select_streams v:0 -show_streams", &error);
            INFO(error);
            REQUIRE(streams);
            // Keep the evidence next to the file.
            std::ofstream(dir / (std::string(c.name) + ".ffprobe.txt"), std::ios::binary) << *streams;
            CHECK(valueAfter(*streams, "[STREAM]", "codec_name") == c.codec);
            CHECK(valueAfter(*streams, "[STREAM]", "pix_fmt") == c.pixelFormat);
            CHECK(valueAfter(*streams, "[STREAM]", "color_space") == "bt2020nc");
            CHECK(valueAfter(*streams, "[STREAM]", "color_primaries") == "bt2020");
            CHECK(valueAfter(*streams, "[STREAM]", "color_transfer") == c.transfer);
            CHECK(valueAfter(*streams, "[STREAM]", "color_range") == "tv");
            if (c.mastering) {
                // The container: mdcv and clli (MP4 / MOV write them with the movie header, after the measurement).
                REQUIRE(contains(*streams, "side_data_type=Mastering display metadata"));
                CHECK(valueAfter(*streams, "side_data_type=Mastering display metadata", "max_luminance") == "10000000/10000");
                CHECK(valueAfter(*streams, "side_data_type=Mastering display metadata", "min_luminance") == "1/10000");
                CHECK(valueAfter(*streams, "side_data_type=Mastering display metadata", "red_x") == "34000/50000");
                REQUIRE(contains(*streams, "side_data_type=Content light level metadata"));
                CHECK(valueAfter(*streams, "side_data_type=Content light level metadata", "max_content") == std::to_string(result->maxCll));
                CHECK(valueAfter(*streams, "side_data_type=Content light level metadata", "max_average") == std::to_string(result->maxFall));
                CHECK(result->maxCll > 203);   // the PQ-read clip is far above the reference white
                CHECK(result->maxCll <= 10000);
                CHECK(result->maxFall > 0);
                CHECK(result->maxFall <= result->maxCll);
                if (std::string_view(c.codec) != "prores") {
                    // The bitstream: the encoder's SEI (HEVC) or metadata OBU (AV1) on the first frame.
                    const auto frames = ffprobeText(out, "-select_streams v:0 -show_frames -read_intervals %+#1", &error);
                    INFO(error);
                    REQUIRE(frames);
                    std::ofstream(dir / (std::string(c.name) + ".frame0.ffprobe.txt"), std::ios::binary) << *frames;
                    CHECK(contains(*frames, "side_data_type=Mastering display metadata"));
                }
            } else {
                CHECK_FALSE(contains(*streams, "Mastering display metadata"));
            }
        }
    }
}

TEST_CASE("PQ and HLG need a 10-bit HEVC, ProRes or AV1 export of an HDR or ACES project", "[export][settings][hdr]") {
    TestProject sdr(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    ExportSettings pq = presetSettings("youtube-hdr-pq");
    CHECK(validate(pq).has_value());
    const auto refused = resolve(pq, sdr.seq());
    REQUIRE_FALSE(refused.has_value());
    CHECK(contains(refused.error().message, "HDR or Wide Gamut"));
    ColorManagement hdr;
    hdr.mode = ColorManagementMode::HdrPq;
    const auto resolved = resolve(pq, sdr.seq(), hdr);
    REQUIRE(resolved);
    CHECK(resolved->dynamicRange == DynamicRange::Pq);
    CHECK(resolved->tenBitRender);
    // Auto: an HDR project's own range when the format has 10 bits, its SDR version otherwise.
    ExportSettings h264 = presetSettings("youtube-1080p");
    CHECK(resolve(h264, sdr.seq(), hdr)->dynamicRange == DynamicRange::Sdr);
    CHECK_FALSE(resolve(h264, sdr.seq(), hdr)->tenBitRender);
    ExportSettings proresAuto = presetSettings("prores-422-hq");
    CHECK(resolve(proresAuto, sdr.seq(), hdr)->dynamicRange == DynamicRange::Pq);
    // A display-referred project keeps the 8-bit path whatever the format (byte-identical exports).
    CHECK_FALSE(resolve(proresAuto, sdr.seq())->tenBitRender);
    // Wrong formats.
    ExportSettings eightBit = pq;
    eightBit.video.pixelFormat = OutputPixelFormat::Yuv420p;
    CHECK_FALSE(validate(eightBit).has_value());
    ExportSettings h264Pq = presetSettings("youtube-1080p");
    h264Pq.video.dynamicRange = DynamicRange::Pq;
    h264Pq.video.pixelFormat = OutputPixelFormat::Yuv420p10;
    CHECK_FALSE(validate(h264Pq).has_value());
    ExportSettings av1Mov = presetSettings("youtube-hdr-pq-av1");
    av1Mov.container = Container::Mov;
    CHECK_FALSE(validate(av1Mov).has_value());
    CHECK(softwareEncoder(VideoCodec::Av1) == "libsvtav1");
    CHECK(videoCodecName(VideoCodec::Av1) == "av1");
}

TEST_CASE("HDR masters equal the GPU's export picture: PQ and HLG (ProRes 422 HQ 10-bit, preview == export)",
          "[export][render][media][gpu][hdr]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    struct Case {
        const char* name;
        ColorManagementMode mode;
        DynamicRange range;
    };
    const Case cases[] = {{"pq", ColorManagementMode::HdrPq, DynamicRange::Pq}, {"hlg", ColorManagementMode::HdrHlg, DynamicRange::Hlg}};
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const TestProject project = managedTimeline(c.mode, true);
            ExportSettings settings = presetSettings("prores-hdr-pq");
            settings.video.dynamicRange = c.range;
            const auto dir = freshOutputDir(std::string("hdr-master-") + c.name);
            const auto out = dir / (std::string("master_") + c.name + ".mov");
            const auto result = ExportJob(renderedRequest(project, settings, out)).run();
            INFO((result ? std::string() : result.error().message));
            REQUIRE(result.has_value());
            CHECK(result->dynamicRange == c.range);
            // The GPU's picture for the export (export role, 10 bits, the PQ / HLG output) against the decoded file.
            exporttest::PreviewRenderer gpu(project, PreviewOutput::HdrExport);
            int frames = 0;
            double minPsnr = 1e9;
            const Ticks period = framesToTicks(1, kRate);
            std::string error;
            REQUIRE(decodeVideo(
                out,
                [&](const YuvFrame& f) {
                    const auto words = gpu.render(framesToTicks(frames, kRate), period);
                    minPsnr = std::min(minPsnr, psnr(f, yuvFromRgb10(words, f.width, f.height, f)));
                    ++frames;
                },
                &error));
            INFO(error);
            WARN(std::format("{} master: preview==export PSNR min {:.2f} dB over {} frames (10-bit)", c.name, minPsnr, frames));
            CHECK(frames == 30);
            CHECK(minPsnr >= 48.0); // ProRes 422 HQ at 10 bits (measured: PQ 56.7 dB, HLG 50.5 dB)
        }
    }
}

TEST_CASE("Colour-managed exports equal the program monitor: an HDR project's SDR version and an ACES master",
          "[export][render][media][gpu][hdr][aces]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    struct Case {
        const char* name;
        ColorManagementMode mode;
        const char* preset;
        double minPsnr;
    };
    const Case cases[] = {
        {"hdr-pq-sdr-version", ColorManagementMode::HdrPq, "youtube-1080p", 40.0}, // 8-bit H.264, tone-mapped like the monitor
        {"aces-prores", ColorManagementMode::Aces13, "prores-422-hq", 45.0},       // 10-bit render of the ACES SDR output
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const TestProject project = managedTimeline(c.mode, c.mode != ColorManagementMode::Aces13);
            ExportSettings settings = presetSettings(c.preset);
            const auto dir = freshOutputDir(std::string("managed-") + c.name);
            const auto out = dir / (std::string(c.name) + (settings.container == Container::Mov ? ".mov" : ".mp4"));
            const auto result = ExportJob(renderedRequest(project, settings, out)).run();
            INFO((result ? std::string() : result.error().message));
            REQUIRE(result.has_value());
            CHECK(result->dynamicRange == DynamicRange::Sdr);
            const auto streams = ffprobeText(out, "-select_streams v:0 -show_streams");
            REQUIRE(streams);
            CHECK(valueAfter(*streams, "[STREAM]", "color_transfer") == "bt709");
            CHECK(valueAfter(*streams, "[STREAM]", "color_primaries") == "bt709");
            CHECK_FALSE(contains(*streams, "Mastering display metadata"));
            // Preview == export.
            exporttest::PreviewRenderer preview(project);
            int frames = 0;
            double minPsnr = 1e9;
            const Ticks period = framesToTicks(1, kRate);
            std::string error;
            REQUIRE(decodeVideo(
                out,
                [&](const YuvFrame& f) {
                    const auto rgba = preview.render(framesToTicks(frames, kRate), period);
                    minPsnr = std::min(minPsnr, psnr(f, yuvFromRgba(rgba, f.width, f.height, f)));
                    if (frames == 15) { // the program monitor's picture (the report's screenshot)
                        const QImage shot(rgba.data(), preview.width(), preview.height(), preview.width() * 4,
                                          QImage::Format_RGBA8888);
                        CHECK(shot.save(QString::fromStdString((dir / (std::string(c.name) + "_monitor.png")).string())));
                    }
                    ++frames;
                },
                &error));
            WARN(std::format("{}: preview==export PSNR min {:.2f} dB over {} frames", c.name, minPsnr, frames));
            CHECK(frames == 30);
            CHECK(minPsnr >= c.minPsnr);
        }
    }
}
