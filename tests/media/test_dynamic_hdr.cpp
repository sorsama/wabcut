// Dynamic HDR metadata in media (media/DynamicHdr.h, card "Dolby Vision 8.4 and HDR10+ dynamic metadata"): the PQ conversion
// Dolby Vision's level 1 values use, the scan of a file that carries neither (the generated clips), the errors, and the
// probe leaving ordinary files alone. The files that DO carry HDR10+ or a Dolby Vision configuration are made by the export
// suite (tests/export/test_hdr_dynamic.cpp), which has the muxer.

#include <wabcut/media/DynamicHdr.h>
#include <wabcut/media/Probe.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stop_token>

using namespace wabcut;
using namespace wabcut::media;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path clip(const char* name) {
    return fs::path(WABCUT_TEST_MEDIA_DIR) / name;
}

} // namespace

TEST_CASE("PQ code values convert to nits and back (ST 2084)", "[media][hdr][dynamic]") {
    CHECK(pqToNits(0, 12) == Approx(0.0).margin(1e-6));
    CHECK(pqToNits(4095, 12) == Approx(10000.0).epsilon(1e-6));
    // 100 nits is 0.5081 of the PQ range, 1000 nits 0.7518, 4000 nits 0.9026 (ST 2084 table values).
    CHECK(static_cast<double>(nitsToPq(100.0, 12)) / 4095.0 == Approx(0.5081).margin(0.0006));
    CHECK(static_cast<double>(nitsToPq(1000.0, 12)) / 4095.0 == Approx(0.7518).margin(0.0006));
    CHECK(static_cast<double>(nitsToPq(4000.0, 12)) / 4095.0 == Approx(0.9026).margin(0.0006));
    CHECK(nitsToPq(10000.0, 12) == 4095);
    CHECK(nitsToPq(0.0, 12) == 0);
    CHECK(nitsToPq(-5.0, 12) == 0);
    CHECK(nitsToPq(50000.0, 10) == 1023); // clamped
    for (const double nits : {0.1, 1.0, 100.0, 203.0, 1000.0, 4000.0}) {
        INFO(nits);
        // A round trip within one code step.
        const double back = pqToNits(nitsToPq(nits, 12), 12);
        CHECK(back == Approx(nits).epsilon(0.02).margin(0.05));
    }
    // 10-bit codes (the render path's table) agree with 12-bit ones: code 520 of 1023 is 100 nits to the code step.
    CHECK(pqToNits(520, 10) == Approx(100.0).epsilon(0.03));
}

TEST_CASE("Scanning an ordinary file finds no dynamic metadata and stops on request", "[media][hdr][dynamic]") {
    const fs::path file = clip("h264_1080p60_aac.mp4");
    if (!fs::exists(file)) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto report = scanDynamicHdr(file.string(), -1, 6);
    REQUIRE(report);
    CHECK(report->framesScanned == 6);
    CHECK_FALSE(report->hasHdr10Plus());
    CHECK_FALSE(report->hasDolbyVision());
    CHECK(report->hdr10PlusApplicationVersion == -1);
    CHECK(report->rpuProfile == -1);
    CHECK_FALSE(report->rpuLevel1);
    CHECK_FALSE(report->dolbyVision.has_value());

    const auto config = readDolbyVisionConfig(file.string());
    REQUIRE(config);
    CHECK_FALSE(config->has_value());

    // The probe leaves ordinary files without the new fields (and an H.264 file is never scanned for HDR10+).
    const auto info = probe(file.string());
    REQUIRE(info);
    REQUIRE_FALSE(info->video.empty());
    CHECK_FALSE(info->video.front().dolbyVision.has_value());
    CHECK_FALSE(info->video.front().hdr10Plus);

    // A stop request ends the scan.
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = scanDynamicHdr(file.string(), -1, 6, stop.get_token());
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);

    // Errors: a file that is not there, a stream that is not video.
    const auto missing = scanDynamicHdr((clip("no-such-file.mp4")).string());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    const auto audio = clip("sine_48k.wav");
    if (fs::exists(audio)) {
        const auto noVideo = scanDynamicHdr(audio.string());
        CHECK_FALSE(noVideo.has_value());
    }
}
