// AI video effects (0.7): the per-clip cache of processed pictures. A stored frame survives encoding and decoding
// byte for byte, a damaged file is refused (and forgotten), the lookup finds the nearest start within a tolerance, the
// cache survives a restart, writes are atomic, the decoded copies obey a memory budget, and the provenance and measured
// speeds are kept.

#include "VideoTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;

namespace {

StoredFrame yuvFrame(int w, int h, std::int64_t pts, int seed = 0) {
    StoredFrame f;
    f.width = w;
    f.height = h;
    f.pts = pts;
    f.duration = frameTicks(1).value;
    f.streamPts = pts / 7;
    f.timeBaseNum = 1;
    f.timeBaseDen = 90000;
    f.sampleNum = 4;
    f.sampleDen = 3;
    f.format = 7; // YUV444P10
    f.matrix = 2;
    f.range = 1;
    f.primaries = 1;
    f.transfer = 1;
    f.chroma = 1;
    f.alpha = 0;
    for (int p = 0; p < 3; ++p) {
        StoredPlane plane;
        plane.width = w;
        plane.height = h;
        plane.bytesPerTexel = 2;
        plane.data.resize(static_cast<std::size_t>(w) * h * 2);
        for (std::size_t i = 0; i < plane.data.size(); i += 2) {
            const int v = (static_cast<int>(i / 2) * 3 + p * 100 + seed) % 1024;
            plane.data[i] = static_cast<std::uint8_t>(v & 0xFF);
            plane.data[i + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        f.planes.push_back(std::move(plane));
    }
    return f;
}

bool same(const StoredFrame& a, const StoredFrame& b) {
    if (a.width != b.width || a.height != b.height || a.pts != b.pts || a.duration != b.duration || a.streamPts != b.streamPts ||
        a.timeBaseNum != b.timeBaseNum || a.timeBaseDen != b.timeBaseDen || a.sampleNum != b.sampleNum || a.sampleDen != b.sampleDen ||
        a.format != b.format || a.matrix != b.matrix || a.range != b.range || a.primaries != b.primaries || a.transfer != b.transfer ||
        a.chroma != b.chroma || a.alpha != b.alpha || a.planes.size() != b.planes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.planes.size(); ++i) {
        if (a.planes[i].width != b.planes[i].width || a.planes[i].height != b.planes[i].height ||
            a.planes[i].bytesPerTexel != b.planes[i].bytesPerTexel || a.planes[i].data != b.planes[i].data) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("A stored frame survives encoding and decoding exactly, and is smaller than raw", "[ai][video][cache]") {
    const StoredFrame f = yuvFrame(64, 36, 4'000'000);
    REQUIRE(f.valid());
    auto bytes = encodeFrame(f);
    REQUIRE(bytes);
    auto back = decodeFrame(*bytes);
    REQUIRE(back);
    CHECK(same(f, *back));
    CHECK(bytes->size() < f.bytes());
    // Alpha and an 8-bit RGBA plane too.
    const StoredFrame rgba = packRgba8(pattern(33, 21, 4, true));
    auto rb = encodeFrame(rgba);
    REQUIRE(rb);
    auto rback = decodeFrame(*rb);
    REQUIRE(rback);
    CHECK(same(rgba, *rback));
}

TEST_CASE("A damaged, truncated or hostile cache file is refused", "[ai][video][cache]") {
    auto bytes = encodeFrame(yuvFrame(16, 8, 0));
    REQUIRE(bytes);
    CHECK_FALSE(decodeFrame({}));
    CHECK_FALSE(decodeFrame(std::vector<std::uint8_t>(bytes->begin(), bytes->begin() + 10)));
    CHECK_FALSE(decodeFrame(std::vector<std::uint8_t>(bytes->begin(), bytes->end() - 5))); // truncated
    auto magic = *bytes;
    magic[0] = 'X';
    CHECK_FALSE(decodeFrame(magic));
    auto corrupt = *bytes;
    corrupt[corrupt.size() - 3] ^= 0xFF; // inside the deflate stream
    CHECK_FALSE(decodeFrame(corrupt));
    auto huge = *bytes;
    huge[4] = huge[5] = huge[6] = huge[7] = 0xFF; // a width of 4 billion
    CHECK_FALSE(decodeFrame(huge));
    StoredFrame bad = yuvFrame(8, 8, 0);
    bad.planes[1].data.pop_back();
    CHECK_FALSE(bad.valid());
    CHECK_FALSE(encodeFrame(bad));
}

TEST_CASE("The cache finds the nearest picture within a tolerance", "[ai][video][cache]") {
    ProcessedCache cache(freshDir("cache-lookup"));
    const CacheIdentity id = identity();
    const std::int64_t step = frameTicks(1).value;
    for (int n : {0, 1, 2, 5}) {
        REQUIRE(cache.put(id, yuvFrame(8, 4, n * step, n)));
    }
    const Ticks half{step / 2};
    CHECK(cache.contains(id, Ticks{2 * step}, half));
    CHECK(cache.contains(id, Ticks{2 * step + step / 3}, half)); // inside the frame
    CHECK_FALSE(cache.contains(id, Ticks{3 * step}, half));      // no picture for frame 3 or 4
    CHECK_FALSE(cache.contains(id, Ticks{4 * step}, half));
    auto five = cache.get(id, Ticks{5 * step}, half);
    REQUIRE(five);
    CHECK(five->pts == 5 * step);
    auto near = cache.get(id, Ticks{5 * step - step / 4}, half);
    REQUIRE(near);
    CHECK(near->pts == 5 * step);
    CHECK_FALSE(cache.get(id, Ticks{3 * step}, half));
    CHECK_FALSE(cache.get(id, Ticks{9 * step}, half));
    CHECK(cache.get(id, Ticks{3 * step}, Ticks{step})); // a wider tolerance reaches frame 2 or 5
    const auto frames = cache.frames(id);
    REQUIRE(frames.size() == 4);
    CHECK(frames[0].value == 0);
    CHECK(frames[3].value == 5 * step);
    const std::vector<Ticks> wanted{Ticks{0}, Ticks{step}, Ticks{2 * step}, Ticks{3 * step}, Ticks{4 * step}, Ticks{5 * step}};
    CHECK(cache.countCovered(id, wanted, half) == 4);
    // Another chain, another stream, another model version: separate caches.
    CacheIdentity other = id;
    other.chain = 8;
    CHECK_FALSE(cache.contains(other, Ticks{0}, half));
    other = id;
    other.stream = 1;
    CHECK_FALSE(cache.contains(other, Ticks{0}, half));
    other = id;
    other.modelVersion = "fake-model-p2";
    CHECK_FALSE(cache.contains(other, Ticks{0}, half));
    CHECK_FALSE(cache.contains(CacheIdentity{}, Ticks{0}, half)); // no identity
}

TEST_CASE("The cache survives a restart, replaces a picture, and writes atomically", "[ai][video][cache]") {
    const auto root = freshDir("cache-restart");
    const CacheIdentity id = identity();
    {
        ProcessedCache cache(root);
        REQUIRE(cache.put(id, yuvFrame(8, 4, 0, 1)));
        REQUIRE(cache.put(id, yuvFrame(8, 4, 1000, 2)));
        CHECK(cache.diskBytes(id) > 0);
    }
    ProcessedCache again(root);
    CHECK(again.frames(id).size() == 2);
    auto f = again.get(id, Ticks{1000}, Ticks{1});
    REQUIRE(f);
    CHECK(same(*f, yuvFrame(8, 4, 1000, 2)));
    // Replacing a picture: the new one is read, the old decoded copy is gone.
    REQUIRE(again.put(id, yuvFrame(8, 4, 1000, 9)));
    auto g = again.get(id, Ticks{1000}, Ticks{1});
    REQUIRE(g);
    CHECK(same(*g, yuvFrame(8, 4, 1000, 9)));
    CHECK(again.frames(id).size() == 2);
    // No temporary file is left behind.
    int temporary = 0;
    for (const auto& entry : std::filesystem::directory_iterator(again.directoryOf(id))) {
        temporary += entry.path().extension() == ".tmp" ? 1 : 0;
    }
    CHECK(temporary == 0);
    CHECK(again.totalDiskBytes() >= again.diskBytes(id));
}

TEST_CASE("A damaged picture on disk is forgotten and made again", "[ai][video][cache]") {
    ProcessedCache cache(freshDir("cache-damaged"));
    const CacheIdentity id = identity();
    REQUIRE(cache.put(id, yuvFrame(8, 4, 0)));
    const auto file = cache.directoryOf(id) / "0.wpf";
    REQUIRE(std::filesystem::exists(file));
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "not a picture";
    }
    CacheIdentity fresh = id; // a second cache object reads the disk: the first one's decoded copy is not involved
    ProcessedCache second(cache.root());
    CHECK_FALSE(second.get(fresh, Ticks{0}, Ticks{1}));
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK_FALSE(second.contains(fresh, Ticks{0}, Ticks{1}));
    CHECK(second.put(fresh, yuvFrame(8, 4, 0))); // made again
    CHECK(second.get(fresh, Ticks{0}, Ticks{1}));
}

TEST_CASE("Remove and clear delete the pictures; revision tells pollers", "[ai][video][cache]") {
    ProcessedCache cache(freshDir("cache-remove"));
    const CacheIdentity a = identity(1);
    const CacheIdentity b = identity(2);
    const std::uint64_t r0 = cache.revision();
    REQUIRE(cache.put(a, yuvFrame(8, 4, 0)));
    REQUIRE(cache.put(b, yuvFrame(8, 4, 0)));
    CHECK(cache.revision() > r0);
    const std::uint64_t r1 = cache.revision();
    cache.remove(a);
    CHECK(cache.revision() > r1);
    CHECK_FALSE(cache.contains(a, Ticks{0}, Ticks{1}));
    CHECK_FALSE(std::filesystem::exists(cache.directoryOf(a)));
    CHECK(cache.contains(b, Ticks{0}, Ticks{1}));
    cache.clear();
    CHECK_FALSE(cache.contains(b, Ticks{0}, Ticks{1}));
    CHECK(cache.totalDiskBytes() == 0);
}

TEST_CASE("Decoded pictures in memory obey the budget", "[ai][video][cache]") {
    const StoredFrame sample = yuvFrame(32, 32, 0);
    ProcessedCache cache(freshDir("cache-budget"), sample.bytes() * 2 + 100); // room for two
    const CacheIdentity id = identity();
    for (int n = 0; n < 4; ++n) {
        REQUIRE(cache.put(id, yuvFrame(32, 32, n * 1000, n)));
    }
    std::shared_ptr<const StoredFrame> first = cache.get(id, Ticks{0}, Ticks{1});
    REQUIRE(first);
    for (int n = 1; n < 4; ++n) {
        REQUIRE(cache.get(id, Ticks{n * 1000}, Ticks{1}));
    }
    // The first was pushed out of memory but a lookup reads it from disk again, equal.
    auto again = cache.get(id, Ticks{0}, Ticks{1});
    REQUIRE(again);
    CHECK(again.get() != first.get());
    CHECK(same(*again, *first));
    // A hit returns the very same object (so the compositor's upload is skipped).
    CHECK(cache.get(id, Ticks{0}, Ticks{1}).get() == again.get());
}

TEST_CASE("Provenance and measured speeds are kept beside the pictures", "[ai][video][cache]") {
    const auto root = freshDir("cache-provenance");
    ProcessedCache cache(root);
    const CacheIdentity id = identity();
    CHECK_FALSE(cache.provenance(id));
    Provenance p;
    p.chain = "Restore 2x";
    p.settings = "scale 2x, deblur 50";
    p.modelVersion = id.modelVersion;
    p.models = "Real-ESRGAN x2plus (BSD-3-Clause)";
    p.created = "2026-11-05T01:00:00Z";
    p.app = "Wabcut 0.7.0";
    REQUIRE(cache.writeProvenance(id, p));
    auto back = ProcessedCache(root).provenance(id);
    REQUIRE(back);
    CHECK(back->chain == "Restore 2x");
    CHECK(back->models == "Real-ESRGAN x2plus (BSD-3-Clause)");
    CHECK(back->app == "Wabcut 0.7.0");

    CHECK(cache.speed("drunet-color", "DirectML") == 0.0);
    cache.recordSpeed("drunet-color", "DirectML", 1000.0);
    CHECK(cache.speed("drunet-color", "DirectML") == 1000.0);
    cache.recordSpeed("drunet-color", "DirectML", 2000.0);
    CHECK(cache.speed("drunet-color", "DirectML") > 1000.0); // a running mean
    CHECK(cache.speed("drunet-color", "DirectML") < 2000.0);
    CHECK(cache.speed("drunet-color", "CPU") == 0.0);
    cache.recordSpeed("", "CPU", 5.0);
    cache.recordSpeed("x", "CPU", -1.0);
    CHECK(cache.speed("x", "CPU") == 0.0);
    CHECK(ProcessedCache(root).speed("drunet-color", "DirectML") > 1000.0); // on disk
}
