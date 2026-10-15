// I.7: the first-run performance check (wabcut/editor/PerformanceProbe.h): the recommendation table, the report's JSON,
// and a short real run (samples made with the export path, decoded, cleaned up; cancel and an empty budget).

#include <wabcut/editor/PerformanceProbe.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stop_token>
#include <string>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

PerformanceReport measured(double h264Fps, double hevcFps, bool hardwareH264, bool gpuEncoder) {
    PerformanceReport r;
    r.width = 1920;
    r.height = 1080;
    r.frameRate = 30.0;
    DecodeMeasurement h264{"H.264", true, hardwareH264 ? "d3d11va" : "", hardwareH264 ? h264Fps : 0.0,
                           hardwareH264 ? h264Fps / 2 : h264Fps, ""};
    DecodeMeasurement hevc{"HEVC", true, "", 0.0, hevcFps, ""};
    r.decode = {h264, hevc, {"ProRes", true, "", 0.0, 400.0, ""}};
    r.encoders = {{"H.264", gpuEncoder ? "h264_nvenc" : "libx264", gpuEncoder, ""},
                  {"HEVC", gpuEncoder ? "hevc_nvenc" : "libx265", gpuEncoder, ""}};
    return r;
}

fs::path freshDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "wabcut-tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("performance check: recommendations follow how fast this computer decodes", "[editor][performance]") {
    SECTION("4x real time or better: the usual settings, full-quality preview allowed") {
        const auto r = recommend(measured(300.0, 150.0, true, true));
        CHECK(r.proxyPolicy == "auto");
        CHECK(r.previewQuality == "auto");
        CHECK(r.proxyDivisor == 2);
        CHECK(r.proxyFormat == "h264"); // GPU encodes and decodes H.264
        CHECK(r.reasons.size() == 2);
    }
    SECTION("the slower long-GOP codec decides: HEVC at 1.5x real time") {
        const auto r = recommend(measured(300.0, 45.0, true, false));
        CHECK(r.proxyPolicy == "auto");
        CHECK(r.previewQuality == "half");
        CHECK(r.proxyDivisor == 4);
        CHECK(r.proxyFormat == "prores"); // no hardware encoder
    }
    SECTION("slower than real time: proxies for everything, a quarter-size preview") {
        const auto r = recommend(measured(20.0, 12.0, false, false));
        CHECK(r.proxyPolicy == "always");
        CHECK(r.previewQuality == "quarter");
        CHECK(r.proxyDivisor == 4);
        CHECK(r.proxyFormat == "prores");
    }
    SECTION("hardware H.264 encoding without hardware decoding keeps ProRes proxies") {
        CHECK(recommend(measured(300.0, 200.0, false, true)).proxyFormat == "prores");
    }
    SECTION("nothing measured: the defaults, said so") {
        PerformanceReport empty;
        const auto r = recommend(empty);
        CHECK(r.proxyPolicy == "auto");
        CHECK(r.previewQuality == "auto");
        CHECK(r.proxyDivisor == 2);
        REQUIRE_FALSE(r.reasons.empty());
        CHECK(r.reasons.front().find("could not be measured") != std::string::npos);
    }
}

TEST_CASE("performance check: the report survives its JSON", "[editor][performance]") {
    PerformanceReport report = measured(120.0, 70.0, true, true);
    report.gpus = {"NVIDIA", "Intel"};
    report.graphics = "D3D11 \xc2\xb7 RTX 3060";
    report.audioDevice = true;
    report.audioName = "Speakers";
    report.cpuThreads = 16;
    report.seconds = 8.25;
    report.outOfTime = true;
    report.recommendation = recommend(report);
    const auto back = reportFromJson(toJson(report));
    REQUIRE(back);
    CHECK(*back == report);
    CHECK_FALSE(reportFromJson("not json"));
    CHECK_FALSE(reportFromJson("{\"version\": 2}"));
    CHECK_FALSE(reportFromJson("[1, 2]"));
}

TEST_CASE("performance check: a short real run measures, recommends and cleans up", "[editor][performance]") {
    const fs::path dir = freshDir("performance-probe");
    ProbeOptions options;
    options.workDir = dir;
    options.width = 320;
    options.height = 180;
    options.frames = 8;
    options.budgetSeconds = 60.0;
    options.audio = false;
    double lastFraction = -1.0;
    bool monotonic = true;
    const PerformanceReport report = runPerformanceProbe(options, {}, [&](double fraction, std::string_view) {
        monotonic = monotonic && fraction >= lastFraction;
        lastFraction = fraction;
    });
    CHECK(monotonic);
    CHECK(lastFraction == 1.0);
    CHECK_FALSE(report.cancelled);
    CHECK_FALSE(report.outOfTime);
    CHECK(report.width == 320);
    CHECK(report.cpuThreads > 0);
    REQUIRE(report.encoders.size() == 2);
    CHECK_FALSE(report.encoders[0].encoder.empty()); // libx264 at least
    REQUIRE(report.decode.size() == 3);
    for (const DecodeMeasurement& m : report.decode) {
        INFO(m.codec << ": " << m.note);
        CHECK(m.available);
        CHECK(m.softwareFps > 0.0);
    }
    CHECK(report.recommendation == recommend(report));
    CHECK(fs::is_empty(dir)); // the samples are deleted
    fs::remove_all(dir);
}

TEST_CASE("performance check: cancel and an exhausted budget end it early, saying why", "[editor][performance]") {
    const fs::path dir = freshDir("performance-probe-stop");
    ProbeOptions options;
    options.workDir = dir;
    options.width = 320;
    options.height = 180;
    options.audio = false;

    std::stop_source cancelled;
    cancelled.request_stop();
    const PerformanceReport stopped = runPerformanceProbe(options, cancelled.get_token());
    CHECK(stopped.cancelled);
    CHECK(stopped.decode.empty());
    CHECK(stopped.seconds < 5.0);

    options.budgetSeconds = 0.0;
    const PerformanceReport late = runPerformanceProbe(options);
    CHECK(late.outOfTime);
    CHECK_FALSE(late.cancelled);
    REQUIRE(late.decode.size() == 3);
    for (const DecodeMeasurement& m : late.decode) {
        CHECK_FALSE(m.available);
        CHECK(m.note.find("time ran out") != std::string::npos);
    }
    CHECK(late.recommendation.reasons.front().find("could not be measured") != std::string::npos);
    fs::remove_all(dir);
}
