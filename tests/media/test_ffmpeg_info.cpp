#include <wabcut/media/FfmpegInfo.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("FFmpeg links and reports its version", "[media][toolchain]") {
    const auto v = wabcut::media::ffmpegVersionString();
    CHECK(v.find("libavcodec") != std::string::npos);
}

TEST_CASE("Platform hardware decode backends are compiled in", "[media][toolchain]") {
    const auto types = wabcut::media::compiledHwDeviceTypes();
    const auto has = [&](const char* t) { return std::find(types.begin(), types.end(), t) != types.end(); };
#if defined(_WIN32)
    CHECK(has("d3d11va"));
#if defined(_M_ARM64) || defined(_M_ARM64EC)
    // Windows on ARM64: no NVIDIA (CUDA) or Intel Quick Sync builds of FFmpeg's hardware layers (vcpkg.json, x64 only).
    CHECK_FALSE(has("cuda"));
    CHECK_FALSE(has("qsv"));
#else
    CHECK(has("cuda"));
    CHECK(has("qsv"));
#endif
#elif defined(__APPLE__)
    CHECK(has("videotoolbox"));
#else
    CHECK(has("vaapi"));
#endif
}
