#include "TestMedia.h"

#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/VideoDecoder.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

void writeBytes(const std::filesystem::path& p, std::size_t size, std::uint8_t seed) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    for (std::size_t i = 0; i < size; ++i) {
        out.put(static_cast<char>((i * 31 + seed) & 0xff));
    }
}

} // namespace

TEST_CASE("Fingerprint: follows content, size and modification time, not the path", "[media][fingerprint]") {
    const auto dir = scratchDir("fingerprint");
    const auto a = dir / "a.bin";
    writeBytes(a, 300'000, 1);
    auto fa = sourceFingerprint(utf8(a));
    REQUIRE(fa.has_value());
    CHECK(*fa != 0);
    CHECK(sourceFingerprint(utf8(a)).value() == *fa); // stable (and memoised)

    SECTION("a copy with the same timestamp has the same fingerprint") {
        const auto b = dir / "renamed copy.bin";
        std::filesystem::copy_file(a, b);
        std::filesystem::last_write_time(b, std::filesystem::last_write_time(a));
        CHECK(sourceFingerprint(utf8(b)).value() == *fa);
    }
    SECTION("touching the file changes it") {
        std::filesystem::last_write_time(a, std::filesystem::last_write_time(a) + std::chrono::seconds(3));
        CHECK(sourceFingerprint(utf8(a)).value() != *fa);
    }
    SECTION("a change in the last 64 KiB with the same size and time changes it") {
        // (Written to a fresh path: results are memoised per path + size + mtime within a process.)
        const auto b = dir / "edited.bin";
        std::filesystem::copy_file(a, b);
        {
            std::fstream f(b, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(299'990);
            f.put('X');
        }
        std::filesystem::last_write_time(b, std::filesystem::last_write_time(a));
        CHECK(sourceFingerprint(utf8(b)).value() != *fa);
    }
    SECTION("small files hash their whole content") {
        const auto small = dir / "small.bin";
        writeBytes(small, 10, 2);
        auto fs = sourceFingerprint(utf8(small));
        REQUIRE(fs.has_value());
        CHECK(*fs != *fa);
    }
    SECTION("missing file") {
        auto missing = sourceFingerprint(utf8(dir / "nope.bin"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);
    }
}

TEST_CASE("Fingerprint: decoders stamp it on every frame", "[media][fingerprint]") {
    if (!haveMedia("vfr.mp4")) {
        SKIP("test clip was not generated");
    }
    DecoderOptions options;
    options.sourceGeneration = 5;
    auto decoder = VideoDecoder::open(mediaPath("vfr.mp4"), -1, options);
    REQUIRE(decoder.has_value());
    const SourceIdentity expected{sourceFingerprint(mediaPath("vfr.mp4")).value(), 5};
    CHECK((*decoder)->source() == expected);
    auto f = (*decoder)->frameAt(Ticks{0});
    REQUIRE(f.has_value());
    CHECK(f->source == expected);
}
