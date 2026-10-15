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
    CHECK(has("cuda"));
    CHECK(has("qsv"));
#elif defined(__APPLE__)
    CHECK(has("videotoolbox"));
#else
    CHECK(has("vaapi"));
#endif
}
