// 0.7 OpenFX video plug-ins in the export: a clip with an OpenFX effect exports the picture the program monitor shows (the
// preview and the export build their composites with the same function), the effect really is in the file, and an export that
// asks for a plug-in this computer does not have says so. Real media (the media suite's h264_1080p60_aac.mp4), the real
// wabcut-ofxhost helper and the test plug-ins of tests/ofxhost.

#include "ExportTestUtil.h"
#include "OfxFixture.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/model/Effects.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr Rate kRate{30, 1};

ExportSettings presetSettings(std::string_view id) {
    auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

/// A 320 x 180 project with the test clip on V1 for `duration`, with the plug-in effect `effectId` on it when it is not empty.
TestProject projectWith(const std::string& effectId, Ticks duration, const wabcut::ofxhost::OfxPluginInfo* plugin = nullptr,
                        double gain = 0.5) {
    TestProject project(320, 180, kRate, duration);
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(media, Ticks{}, duration, Ticks{});
    if (!effectId.empty()) {
        project.editSequence([&](Sequence& s) {
            Track track = *s.videoTracks[0];
            std::vector<Clip> clips;
            for (std::size_t i = 0; i < track.clips.size(); ++i) {
                clips.push_back(track.clips[i]);
            }
            REQUIRE(clips.size() == 1);
            EffectInstance e;
            if (plugin != nullptr) {
                e = wabcut::ofxhost::makeOfxEffect(*plugin, project.state.ids.allocate<EffectInstanceId>());
            } else {
                e.id = project.state.ids.allocate<EffectInstanceId>();
                e.effect = effectId;
                e.setData(EffectData{"plugin.name", std::string("Gone Plug-in (Vendor)")});
            }
            e.set("gain", ParamValue(gain));
            clips[0].setEffects({e});
            track.clips = ClipList(std::move(clips));
            s.videoTracks[0] = std::make_shared<const Track>(std::move(track));
        });
    }
    return project;
}

ExportRequest renderedRequest(const TestProject& project, ExportSettings settings, const std::filesystem::path& output) {
    ExportRequest r = makeRequest(project, std::move(settings), output);
    r.videoSource = {}; // the default: the rendered source (the compositor, as the preview draws)
    return r;
}

double meanLuma(const YuvFrame& f) {
    double sum = 0.0;
    for (const std::uint16_t v : f.planes[0]) {
        sum += v;
    }
    return f.planes[0].empty() ? 0.0 : sum / static_cast<double>(f.planes[0].size());
}

} // namespace

TEST_CASE("OpenFX export: the effect is in the file and the export matches the program monitor", "[export][ofx][gpu]") {
    REQUIRE(haveMedia("h264_1080p60_aac.mp4")); // the media suite's generated clips
    wabcut::test::TestOfx ofx;
    const auto gain = ofx.plugin(wabcut::test::kGain);
    const auto dir = freshOutputDir("ofx-export");
    const Ticks length = ms(500);

    // Without the effect, and with a gain of a half.
    const TestProject plain = projectWith({}, length);
    const TestProject effect = projectWith(wabcut::test::kGain, length, &gain, 0.5);
    ExportJob plainJob(renderedRequest(plain, presetSettings("match-source"), dir / "plain.mp4"));
    const auto plainResult = plainJob.run();
    REQUIRE(plainResult);
    ExportJob effectJob(renderedRequest(effect, presetSettings("match-source"), dir / "effect.mp4"));
    const auto effectResult = effectJob.run();
    REQUIRE(effectResult);
    for (const std::string& w : effectResult->warnings) {
        CHECK(w.find("plug-in") == std::string::npos); // nothing about the plug-in went wrong
    }

    // The effect is in the file: a gain of a half is much darker (video-range luma above black 16).
    std::vector<double> plainLuma;
    std::vector<double> effectLuma;
    std::string error;
    REQUIRE(decodeVideo(dir / "plain.mp4", [&](const YuvFrame& f) { plainLuma.push_back(meanLuma(f)); }, &error));
    REQUIRE(decodeVideo(dir / "effect.mp4", [&](const YuvFrame& f) { effectLuma.push_back(meanLuma(f)); }, &error));
    REQUIRE(plainLuma.size() == effectLuma.size());
    REQUIRE(plainLuma.size() >= 10);
    for (std::size_t i = 0; i < plainLuma.size(); ++i) {
        INFO("frame " << i << ": " << plainLuma[i] << " without, " << effectLuma[i] << " with");
        CHECK((effectLuma[i] - 16.0) < 0.85 * (plainLuma[i] - 16.0));
        CHECK(effectLuma[i] > 16.0); // dimmed, not black
    }

    // The export is the program monitor's picture (same function builds both), within what the encoder loses.
    exporttest::PreviewRenderer preview(effect);
    const Ticks period = framesToTicks(1, kRate);
    int frames = 0;
    double minPsnr = 1e9;
    REQUIRE(decodeVideo(
        dir / "effect.mp4",
        [&](const YuvFrame& f) {
            const auto rgba = preview.render(framesToTicks(frames, kRate), period);
            const double p = psnr(f, yuvFromRgba(rgba, f.width, f.height, f));
            minPsnr = std::min(minPsnr, p);
            ++frames;
        },
        &error));
    metric("ofx export vs preview: " + std::to_string(frames) + " frames, min PSNR " + std::to_string(minPsnr) + " dB");
    CHECK(frames == static_cast<int>(plainLuma.size()));
    CHECK(minPsnr > 28.0);
}

TEST_CASE("OpenFX export: a plug-in this computer does not have is reported before it renders", "[export][ofx][gpu]") {
    REQUIRE(haveMedia("h264_1080p60_aac.mp4"));
    wabcut::test::TestOfx ofx;
    const auto dir = freshOutputDir("ofx-export-missing");
    const TestProject project = projectWith("plugin.ofx.vendor.gone", ms(200));
    ExportJob job(renderedRequest(project, presetSettings("match-source"), dir / "missing.mp4"));
    const auto result = job.run();
    REQUIRE(result); // the export goes on without the effect, and says so
    bool told = false;
    for (const std::string& w : result->warnings) {
        told = told || (w.find("Missing plug-in: Gone Plug-in (Vendor)") != std::string::npos &&
                        w.find("the export does not have this effect") != std::string::npos);
    }
    CHECK(told);
    CHECK(std::filesystem::exists(dir / "missing.mp4"));
}
