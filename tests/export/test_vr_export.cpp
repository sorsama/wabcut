// Card 38 (0.7): a VR sequence's export carries the spherical metadata (Google's Spherical Video V2 boxes in MP4, the
// Projection and StereoMode elements in Matroska), and probing the file reads the same format back, so a player or
// YouTube - and Wabcut itself - recognises it as 360 video. A flat sequence writes none.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/Vr.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

namespace fs = std::filesystem;

ExportSettings h264(Container container) {
    const auto p = findPreset("youtube-1080p");
    REQUIRE(p.has_value());
    ExportSettings s = p->settings;
    s.container = container;
    s.video.codec = VideoCodec::H264;
    s.video.encoder = "libx264";
    s.video.width.reset();
    s.video.height.reset();
    s.video.frameRate.reset();
    s.video.rateControl = RateControl::Quality;
    s.video.quality = 30;
    s.audio.enabled = false;
    s.render.smartRender = false;
    s.render.usePreviews = false;
    return s;
}

/// Exports 10 frames of the synthetic pattern of a width x height sequence in `format` and probes the file.
VrFormat exportedFormat(const char* name, int width, int height, VrFormat format, Container container, const char* extension) {
    const fs::path dir = freshOutputDir(name);
    TestProject project(width, height, kRate30, framesToTicks(10, kRate30));
    project.editSequence([&](Sequence& s) { s.vr = format; });
    const fs::path file = dir / (std::string("vr.") + extension);
    const auto result = ExportJob(makeRequest(project, h264(container), file)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 10);
    const auto info = media::probe(file.string());
    REQUIRE(info.has_value());
    REQUIRE(info->hasVideo());
    return info->video.front().vr;
}

} // namespace

TEST_CASE("VR export: an MP4 of a 360 sequence is marked spherical and probes back as 360 (card 38)", "[export][vr]") {
    const VrFormat mono{VrProjection::Equirectangular360, StereoLayout::Mono};
    CHECK(exportedFormat("vr_mp4_mono", 640, 320, mono, Container::Mp4, "mp4") == mono);
}

TEST_CASE("VR export: stereoscopic layouts and VR180 survive the export (card 38)", "[export][vr]") {
    const VrFormat topBottom{VrProjection::Equirectangular360, StereoLayout::TopBottom};
    CHECK(exportedFormat("vr_mp4_tb", 640, 640, topBottom, Container::Mp4, "mp4") == topBottom);
    const VrFormat sideBySide{VrProjection::Equirectangular360, StereoLayout::SideBySide};
    CHECK(exportedFormat("vr_mp4_sbs", 1280, 320, sideBySide, Container::Mp4, "mp4") == sideBySide);
    const VrFormat half{VrProjection::Equirectangular180, StereoLayout::Mono};
    CHECK(exportedFormat("vr_mp4_180", 320, 320, half, Container::Mp4, "mp4") == half);
}

TEST_CASE("VR export: Matroska carries the projection too (card 38)", "[export][vr]") {
    const VrFormat mono{VrProjection::Equirectangular360, StereoLayout::Mono};
    CHECK(exportedFormat("vr_mkv_mono", 640, 320, mono, Container::Mkv, "mkv") == mono);
    const VrFormat topBottom{VrProjection::Equirectangular360, StereoLayout::TopBottom};
    CHECK(exportedFormat("vr_mkv_tb", 640, 640, topBottom, Container::Mkv, "mkv") == topBottom);
}

TEST_CASE("VR export: a flat sequence is not marked and a container without the metadata still exports (card 38)", "[export][vr]") {
    CHECK_FALSE(exportedFormat("vr_flat", 640, 360, VrFormat{}, Container::Mp4, "mp4").isVr());
    // QuickTime has no spherical boxes in the muxer Wabcut uses: the file is made, unmarked.
    const VrFormat mono{VrProjection::Equirectangular360, StereoLayout::Mono};
    CHECK_FALSE(exportedFormat("vr_mov", 640, 320, mono, Container::Mov, "mov").isVr());
}
