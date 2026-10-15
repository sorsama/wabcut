#include "TestMedia.h"

#include <wabcut/media/ThumbnailService.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <latch>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

void requireMedia(std::string_view name) {
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

bool samePixels(const ThumbnailStrip& a, const ThumbnailStrip& b) {
    if (a.thumbnails.size() != b.thumbnails.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.thumbnails.size(); ++i) {
        const Thumbnail& x = a.thumbnails[i];
        const Thumbnail& y = b.thumbnails[i];
        if (x.time != y.time || x.width != y.width || x.height != y.height || x.rgba != y.rgba) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Thumbnails: evenly spaced, scaled, deterministic", "[media][thumbnails]") {
    requireMedia("h264_1080p60_aac.mp4");
    ThumbnailOptions options;
    options.count = 10;
    options.height = 90;
    auto first = extractThumbnails(mediaPath("h264_1080p60_aac.mp4"), options);
    REQUIRE(first.has_value());
    REQUIRE(first->thumbnails.size() == 10);
    CHECK_FALSE(first->fromCache);
    for (std::size_t i = 0; i < 10; ++i) {
        const Thumbnail& t = first->thumbnails[i];
        CHECK(t.width == 160); // 16:9 at 90 px
        CHECK(t.height == 90);
        CHECK(t.rgba.size() == std::size_t{160} * 90 * 4);
        // Centre of slice i of 10 s = (i + 0.5) s = frame 30 + 60 i.
        CHECK(t.time == framesToTicks(30 + 60 * static_cast<std::int64_t>(i), kRate60));
        CHECK(t.rgba[3] == 255); // opaque
    }
    CHECK(first->thumbnails[0].rgba != first->thumbnails[5].rgba); // the picture moves

    auto second = extractThumbnails(mediaPath("h264_1080p60_aac.mp4"), options);
    REQUIRE(second.has_value());
    CHECK(samePixels(*first, *second));
}

TEST_CASE("Thumbnails: display rotation is applied", "[media][thumbnails]") {
    requireMedia("rotated_90.mp4");
    ThumbnailOptions options;
    options.count = 3;
    options.height = 64;
    auto strip = extractThumbnails(mediaPath("rotated_90.mp4"), options);
    REQUIRE(strip.has_value());
    REQUIRE(strip->thumbnails.size() == 3);
    CHECK(strip->thumbnails[0].height == 64);
    CHECK(strip->thumbnails[0].width == 36); // portrait 360x640 displayed
}

TEST_CASE("Thumbnails: the disk cache is hit on the second run", "[media][thumbnails]") {
    requireMedia("prores_422.mov");
    const auto cacheDir = scratchDir("thumbnail-cache");
    ThumbnailService service(cacheDir);
    ThumbnailOptions options;
    options.count = 6;
    options.height = 72;

    auto first = service.getOrExtract(mediaPath("prores_422.mov"), options);
    REQUIRE(first.has_value());
    CHECK_FALSE(first->fromCache);
    const auto file = service.cacheFileFor(mediaPath("prores_422.mov"), options);
    CHECK(std::filesystem::is_regular_file(file));
    CHECK(file.parent_path() == cacheDir / "thumbnails");

    auto second = service.getOrExtract(mediaPath("prores_422.mov"), options);
    REQUIRE(second.has_value());
    CHECK(second->fromCache);
    CHECK(samePixels(*first, *second));

    SECTION("different options are a different entry") {
        options.height = 48;
        auto other = service.getOrExtract(mediaPath("prores_422.mov"), options);
        REQUIRE(other.has_value());
        CHECK_FALSE(other->fromCache);
        CHECK(service.cacheFileFor(mediaPath("prores_422.mov"), options) != file);
    }
    SECTION("a corrupt cache file is ignored and rewritten") {
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "garbage";
        }
        auto rebuilt = service.getOrExtract(mediaPath("prores_422.mov"), options);
        REQUIRE(rebuilt.has_value());
        CHECK_FALSE(rebuilt->fromCache);
        CHECK(samePixels(*first, *rebuilt));
        auto cached = service.getOrExtract(mediaPath("prores_422.mov"), options);
        REQUIRE(cached.has_value());
        CHECK(cached->fromCache);
    }
    SECTION("the cache follows content: a moved copy hits it, a touched file is extracted again") {
        const auto source = std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / "prores_422.mov";
        const auto copy = scratchDir("thumbnail-copy") / "moved clip.mov";
        std::filesystem::copy_file(source, copy);
        std::filesystem::last_write_time(copy, std::filesystem::last_write_time(source));
        auto a = service.getOrExtract(utf8(copy), options);
        REQUIRE(a.has_value());
        CHECK(a->fromCache); // same fingerprint as the original, extracted above
        CHECK(samePixels(*first, *a));
        std::filesystem::last_write_time(copy,
                                         std::filesystem::last_write_time(copy) + std::chrono::seconds(5));
        auto b = service.getOrExtract(utf8(copy), options);
        REQUIRE(b.has_value());
        CHECK_FALSE(b->fromCache);
        CHECK(service.getOrExtract(utf8(copy), options)->fromCache);
    }
}

TEST_CASE("Thumbnails: background jobs, callbacks and cancellation", "[media][thumbnails]") {
    requireMedia("h264_1080p60_aac.mp4");
    const auto cacheDir = scratchDir("thumbnail-jobs");
    ThumbnailOptions options;
    options.count = 4;
    options.height = 45;

    SECTION("request completes, calls back once, then hits the cache") {
        // Declared before the service: the callback may still run while the service shuts down.
        std::atomic<int> callbacks{0};
        std::latch delivered(1);
        ThumbnailService service(cacheDir, 2);
        auto job = service.request(mediaPath("h264_1080p60_aac.mp4"), options,
                                   [&](const std::expected<ThumbnailStrip, Error>& r) {
                                       if (r) {
                                           ++callbacks;
                                       }
                                       delivered.count_down();
                                   });
        const auto& result = job.wait();
        REQUIRE(result.has_value());
        CHECK(result->thumbnails.size() == 4);
        CHECK_FALSE(result->fromCache);
        CHECK(job.isFinished());
        CHECK(job.progress() == 1.0f);
        auto again = service.request(mediaPath("h264_1080p60_aac.mp4"), options);
        REQUIRE(again.wait().has_value());
        CHECK(again.wait()->fromCache);
        delivered.wait(); // wait() returns when the result is published, possibly before the callback ran
        CHECK(callbacks.load() == 1);
    }
    SECTION("cancel") {
        ThumbnailService service(cacheDir, 1);
        options.count = 400;
        auto job = service.request(mediaPath("h264_1080p60_aac.mp4"), options);
        job.cancel();
        const auto& result = job.wait();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::Cancelled);
    }
    SECTION("destroying the service cancels queued jobs") {
        std::vector<Job<ThumbnailStrip>> jobs;
        {
            ThumbnailService service(cacheDir, 1);
            options.count = 200;
            for (int i = 0; i < 3; ++i) {
                ThumbnailOptions o = options;
                o.height = 40 + i;
                jobs.push_back(service.request(mediaPath("h264_1080p60_aac.mp4"), o));
            }
        }
        for (const auto& job : jobs) {
            REQUIRE(job.isFinished());
        }
        CHECK_FALSE(jobs.back().wait().has_value());
        CHECK(jobs.back().wait().error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("Thumbnails: errors", "[media][thumbnails]") {
    ThumbnailOptions bad;
    bad.count = 0;
    CHECK(extractThumbnails(mediaPath("h264_1080p60_aac.mp4"), bad).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(extractThumbnails(mediaPath("missing.mp4"), {}).error().code == ErrorCode::NotFound);
    ThumbnailService service(scratchDir("thumbnail-errors"));
    CHECK(service.getOrExtract(mediaPath("missing.mp4")).error().code == ErrorCode::NotFound);
    CHECK(service.cacheFileFor(mediaPath("missing.mp4"), {}).empty());
    if (haveMedia("sine_48k.wav")) {
        CHECK(service.getOrExtract(mediaPath("sine_48k.wav")).error().code == ErrorCode::NotFound);
    }
}
