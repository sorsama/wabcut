// 0.6 (#72) HDR finishing in exports: the output colour space (Rec.2020 SDR, P3-D65 and the Rec.2100 HDR spaces, from the
// sequence or the export) and SDR Conform (an HDR or Wide Gamut sequence delivered as SDR) - how the settings resolve,
// the colour tags written (ffprobe), what an export reports it mapped, and wabcut-cli render's --output-space and
// --sdr-conform options.
//
// The HDR content: the motion timeline's V1 clip read as Rec.2100 PQ (highlights far above the reference white), in an
// HDR Rec.2100 PQ project with a 1000-nit master.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
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
    s.video.width.reset();
    s.video.height.reset();
    return s;
}

/// The motion timeline in an HDR PQ project, V1 read as Rec.2100 PQ, with `conform` as the sequence's SDR Conform.
TestProject hdrTimeline(const SdrConform& conform = {}) {
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    ColorManagement m;
    m.mode = ColorManagementMode::HdrPq;
    m.sdrConform = conform;
    auto set = edit::setColorManagement(project.state, m);
    REQUIRE(set);
    project.state = *set;
    const ClipId v1 = project.seq().videoTracks.front()->clips.front().id;
    const ClipId one[] = {v1};
    auto pq = edit::setClipInputColorSpace(project.state, project.sequence, one, InputColorSpace::Rec2100Pq);
    REQUIRE(pq);
    project.state = pq->state;
    return project;
}

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

ColorManagement setupOf(ColorManagementMode mode) {
    ColorManagement m;
    m.mode = mode;
    return m;
}

struct CliRun {
    int exitCode = -1;
    std::string output;
};

CliRun runCli(const std::string& arguments) {
    const std::string line = "\"" + std::string(WABCUT_CLI_EXECUTABLE) + "\" " + arguments + " 2>&1";
#if defined(_WIN32)
    const std::string command = "\"" + line + "\"";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(line.c_str(), "r");
#endif
    CliRun run;
    if (!pipe) {
        return run;
    }
    char buf[4096];
    while (const std::size_t n = std::fread(buf, 1, sizeof buf, pipe)) {
        run.output.append(buf, n);
    }
#if defined(_WIN32)
    run.exitCode = _pclose(pipe);
#else
    const int status = pclose(pipe);
    run.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return run;
}

std::string quoted(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return "\"" + std::string(reinterpret_cast<const char*>(u.data()), u.size()) + "\"";
}

} // namespace

TEST_CASE("Export settings resolve the output colour space and SDR Conform (0.6)", "[export][settings][hdr]") {
    TestProject project(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const Sequence& seq = project.seq();
    // Direct Rec.709: the export's Rec.2020 or P3-D65, or the sequence's; Automatic is Rec.709.
    ExportSettings h264 = presetSettings("youtube-1080p");
    CHECK(resolve(h264, seq)->outputSpace == OutputColorSpace::Rec709);
    h264.video.outputSpace = OutputColorSpace::Rec2020;
    CHECK(resolve(h264, seq)->outputSpace == OutputColorSpace::Rec2020);
    CHECK(resolve(h264, seq)->dynamicRange == DynamicRange::Sdr);
    ColorManagement p3;
    p3.outputSpace = OutputColorSpace::P3D65;
    CHECK(resolve(presetSettings("youtube-1080p"), seq, p3)->outputSpace == OutputColorSpace::P3D65);
    CHECK(resolve(h264, seq, p3)->outputSpace == OutputColorSpace::Rec2020); // the export's own wins
    // Disable Color Management converts nothing.
    ColorManagement off = setupOf(ColorManagementMode::Disabled);
    off.outputSpace = OutputColorSpace::Rec2020;
    CHECK(resolve(presetSettings("youtube-1080p"), seq, off)->outputSpace == OutputColorSpace::Rec709);

    // Rec.2100 PQ / HLG resolve Auto to an HDR master where the format can carry it (a Wide Gamut sequence too), and to
    // the SDR version (Rec.709) where it cannot.
    ColorManagement wide = setupOf(ColorManagementMode::WideGamut);
    wide.outputSpace = OutputColorSpace::Rec2100Pq;
    const auto prores = resolve(presetSettings("prores-422-hq"), seq, wide);
    REQUIRE(prores);
    CHECK(prores->dynamicRange == DynamicRange::Pq);
    CHECK(prores->outputSpace == OutputColorSpace::Rec2100Pq);
    CHECK(resolve(presetSettings("youtube-1080p"), seq, wide)->dynamicRange == DynamicRange::Sdr);
    CHECK(resolve(presetSettings("youtube-1080p"), seq, wide)->outputSpace == OutputColorSpace::Rec709);
    ColorManagement hdr = setupOf(ColorManagementMode::HdrPq);
    hdr.outputSpace = OutputColorSpace::Rec2100Hlg;
    CHECK(resolve(presetSettings("prores-422-hq"), seq, hdr)->dynamicRange == DynamicRange::Hlg);
    // An export that asks for PQ itself in a format that cannot carry it is refused, as is one from an SDR setup.
    ExportSettings h264Pq = presetSettings("youtube-1080p");
    h264Pq.video.outputSpace = OutputColorSpace::Rec2100Pq;
    REQUIRE_FALSE(resolve(h264Pq, seq, wide).has_value());
    ExportSettings proresPq = presetSettings("prores-422-hq");
    proresPq.video.outputSpace = OutputColorSpace::Rec2100Pq;
    REQUIRE_FALSE(resolve(proresPq, seq).has_value());

    // SDR Conform: an SDR export of an HDR or Wide Gamut (No / Minimal) setup with it on; never an HDR master, an ACES
    // setup or an SDR setup; the export's own settings replace the sequence's.
    ColorManagement conformed = setupOf(ColorManagementMode::HdrPq);
    conformed.sdrConform.enabled = true;
    conformed.sdrConform.toneMap = ToneMapOperator::Reinhard;
    const auto sdr = resolve(presetSettings("youtube-1080p"), seq, conformed);
    REQUIRE(sdr);
    REQUIRE(sdr->sdrConform.has_value());
    CHECK(sdr->sdrConform->toneMap == ToneMapOperator::Reinhard);
    CHECK_FALSE(resolve(presetSettings("prores-422-hq"), seq, conformed)->sdrConform.has_value()); // PQ master
    ExportSettings noConform = presetSettings("youtube-1080p");
    noConform.video.sdrConform = SdrConform{};
    CHECK_FALSE(resolve(noConform, seq, conformed)->sdrConform.has_value());
    ExportSettings ownConform = presetSettings("youtube-1080p");
    ownConform.video.sdrConform = SdrConform{true, 203.0, 100.0, ToneMapOperator::Aces, GamutMapping::Clip, true};
    CHECK(resolve(ownConform, seq, conformed)->sdrConform->toneMap == ToneMapOperator::Aces);
    CHECK(resolve(ownConform, seq, setupOf(ColorManagementMode::WideGamutMinimal))->sdrConform.has_value());
    CHECK_FALSE(resolve(ownConform, seq, setupOf(ColorManagementMode::Aces13))->sdrConform.has_value());
    CHECK_FALSE(resolve(ownConform, seq)->sdrConform.has_value());
    ExportSettings badConform = ownConform;
    badConform.video.sdrConform->targetPeakNits = 5000.0;
    REQUIRE_FALSE(resolve(badConform, seq, conformed).has_value());

    // Presets keep both (written only when set) and the overrides take the output colour space.
    Preset preset{"user-conform", "Conform", "", ownConform};
    preset.settings.video.outputSpace = OutputColorSpace::P3D65;
    const auto back = presetFromJson(presetToJson(preset));
    REQUIRE(back);
    CHECK(back->settings.video.outputSpace == OutputColorSpace::P3D65);
    REQUIRE(back->settings.video.sdrConform.has_value());
    CHECK(*back->settings.video.sdrConform == *ownConform.video.sdrConform);
    CHECK(settingsToJson(presetSettings("youtube-1080p")).dump().find("sdrConform") == std::string::npos);
    ExportSettings overridden = presetSettings("youtube-1080p");
    REQUIRE(applyOverrides(overridden, Json{{"outputSpace", "rec2020"}}));
    CHECK(overridden.video.outputSpace == OutputColorSpace::Rec2020);
    CHECK_FALSE(applyOverrides(overridden, Json{{"outputSpace", "rec601"}}).has_value());
}

TEST_CASE("An HDR sequence exported to SDR with SDR Conform writes its colour space and reports what was mapped (0.6)",
          "[export][render][media][gpu][hdr]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    SdrConform conform;
    conform.enabled = true;
    const TestProject project = hdrTimeline(conform);
    struct Case {
        const char* name;
        const char* preset;
        OutputColorSpace space;
        const char* file;
        const char* primaries;
        const char* matrix;
    };
    const Case cases[] = {
        {"prores-2020", "prores-422-hq", OutputColorSpace::Rec2020, "conform_2020.mov", "bt2020", "bt2020nc"},
        {"h264-p3", "youtube-1080p", OutputColorSpace::P3D65, "conform_p3.mp4", "smpte432", "bt709"},
        {"h264-709", "youtube-1080p", OutputColorSpace::Automatic, "conform_709.mp4", "bt709", "bt709"},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            ExportSettings settings = presetSettings(c.preset);
            settings.video.dynamicRange = DynamicRange::Sdr; // the SDR version, also from a 10-bit format
            settings.video.outputSpace = c.space;
            const auto dir = freshOutputDir(std::string("sdr-conform-") + c.name);
            const auto out = dir / c.file;
            ExportRequest request = makeRequest(project, settings, out);
            request.videoSource = {}; // the rendered source
            const auto result = ExportJob(std::move(request)).run();
            INFO((result ? std::string() : result.error().message));
            REQUIRE(result.has_value());
            CHECK(result->dynamicRange == DynamicRange::Sdr);
            CHECK(result->outputSpace == (c.space == OutputColorSpace::Automatic ? OutputColorSpace::Rec709 : c.space));
            REQUIRE(result->sdrConform.has_value());
            const SdrConformReport& report = *result->sdrConform;
            WARN(std::format("{}: {} frames, {:.2f} % tone-mapped (worst {:.1f} %), {:.2f} % gamut-mapped", c.name,
                             report.frames, report.toneMappedShare() * 100.0, report.worstToneMappedFrame * 100.0,
                             report.gamutMappedShare() * 100.0));
            CHECK(report.frames == result->frames);
            CHECK(report.pixels > 0);
            CHECK(report.toneMappedShare() > 0.0); // the PQ-read clip runs far above the reference white
            CHECK(report.toneMappedShare() < 1.0);
            CHECK(report.worstToneMappedFrame >= report.toneMappedShare());

            std::string error;
            const auto streams = ffprobeText(out, "-select_streams v:0 -show_streams", &error);
            INFO(error);
            REQUIRE(streams);
            std::ofstream(dir / (std::string(c.name) + ".ffprobe.txt"), std::ios::binary) << *streams;
            CHECK(valueAfter(*streams, "[STREAM]", "color_primaries") == c.primaries);
            CHECK(valueAfter(*streams, "[STREAM]", "color_space") == c.matrix);
            if (c.space != OutputColorSpace::P3D65) {
                CHECK(valueAfter(*streams, "[STREAM]", "color_transfer") == "bt709");
            }
        }
    }
}

TEST_CASE("wabcut-cli render takes the output colour space and SDR Conform and reports what it mapped (0.6)",
          "[export][media][cli][gpu][hdr]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("cli_sdr_conform");
    const TestProject project = hdrTimeline();
    const auto projectFile = dir / "project.wabcut";
    REQUIRE(io::saveProject(project.state, projectFile, io::SaveOptions{0}).has_value());
    const CliRun run = runCli("render " + quoted(projectFile) + " " + quoted(dir / "conformed.mp4") +
                              " --preset youtube-1080p --output-space rec2020 --sdr-conform --sdr-conform-operator reinhard"
                              " --sdr-conform-gamut saturation --sdr-conform-peak 203");
    INFO(run.output);
    REQUIRE(run.exitCode == 0);
    CHECK(run.output.find("colour: Rec.2020 (SDR)") != std::string::npos);
    CHECK(run.output.find("sdr conform:") != std::string::npos);
    std::string error;
    const auto streams = ffprobeText(dir / "conformed.mp4", "-select_streams v:0 -show_streams", &error);
    INFO(error);
    REQUIRE(streams);
    CHECK(valueAfter(*streams, "[STREAM]", "color_primaries") == "bt2020");
    // Bad values are refused before anything renders.
    CHECK(runCli("render " + quoted(projectFile) + " " + quoted(dir / "x.mp4") + " --output-space rec601").exitCode == 2);
    CHECK(runCli("render " + quoted(projectFile) + " " + quoted(dir / "x.mp4") + " --sdr-conform-operator hable").exitCode == 2);
}
