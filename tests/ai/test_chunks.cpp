// VAD chunk planning and stitching chunk results onto the source timeline.

#include <wabcut/ai/Timestamps.h>
#include <wabcut/ai/VadChunker.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;

namespace {
constexpr std::int64_t kS = 16000; // samples per second
}

TEST_CASE("Chunks cover all speech, skip long silence and respect the size limit", "[ai][chunks]") {
    ChunkOptions o;
    o.maxChunkSamples = 60 * kS;
    o.padSamples = kS / 4;
    o.mergeGapSamples = 2 * kS;
    o.maxGapSamples = 10 * kS;
    const std::vector<SampleRange> speech = {
        {10 * kS, 12 * kS}, {13 * kS, 20 * kS}, // 1 s apart: one stretch
        {25 * kS, 30 * kS},                     // 5 s later: same chunk (gap < maxGap)
        {100 * kS, 101 * kS},                   // 70 s of silence: a new chunk
        {5 * kS, 6 * kS},                       // unsorted input
    };
    const auto chunks = planChunks(speech, 200 * kS, o);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0] == SampleRange{5 * kS - kS / 4, 30 * kS + kS / 4});
    CHECK(chunks[1] == SampleRange{100 * kS - kS / 4, 101 * kS + kS / 4});

    SECTION("a chunk never grows past the limit") {
        std::vector<SampleRange> dense;
        for (std::int64_t s = 0; s < 600; s += 3) {
            dense.push_back({s * kS, (s + 2) * kS}); // 2 s talk, 1 s pause, for 10 minutes
        }
        const auto packed = planChunks(dense, 600 * kS, o);
        for (std::size_t i = 0; i < packed.size(); ++i) {
            CHECK(packed[i].length() <= o.maxChunkSamples);
            CHECK(packed[i].length() > 0);
            if (i > 0) {
                CHECK(packed[i].begin >= packed[i - 1].end);
            }
        }
        CHECK(packed.size() >= 10);
        for (const SampleRange& s : dense) { // every speech sample is inside a chunk
            bool inside = false;
            for (const SampleRange& c : packed) {
                inside = inside || (s.begin >= c.begin && s.end <= c.end);
            }
            CHECK(inside);
        }
    }
    SECTION("one very long stretch is cut into equal parts") {
        const std::vector<SampleRange> monologue = {{0, 150 * kS}};
        const auto parts = planChunks(monologue, 150 * kS, o);
        REQUIRE(parts.size() == 3);
        CHECK(parts[0] == SampleRange{0, 50 * kS});
        CHECK(parts[2] == SampleRange{100 * kS, 150 * kS});
    }
    SECTION("padding is clamped to the audio") {
        const std::vector<SampleRange> edges = {{0, kS}, {199 * kS, 200 * kS}};
        const auto clamped = planChunks(edges, 200 * kS, o);
        CHECK(clamped.front().begin == 0);
        CHECK(clamped.back().end == 200 * kS);
    }
    SECTION("no speech, no chunks; no VAD, fixed chunks") {
        CHECK(planChunks({}, 100 * kS, o).empty());
        const auto fixed = fixedChunks(130 * kS, o);
        REQUIRE(fixed.size() == 3);
        CHECK(fixed.back().end == 130 * kS);
        CHECK(fixedChunks(0, o).empty());
    }
}

TEST_CASE("VAD centiseconds of a block become absolute sample ranges", "[ai][chunks]") {
    const std::vector<std::pair<std::int64_t, std::int64_t>> cs = {{50, 120}, {-5, 10}, {900, 2000}, {30, 30}};
    const auto speech = speechFromCentiseconds(cs, 600 * kS, 15 * kS);
    REQUIRE(speech.size() == 3);
    CHECK(speech[0] == SampleRange{600 * kS + 50 * 160, 600 * kS + 120 * 160});
    CHECK(speech[1] == SampleRange{600 * kS, 600 * kS + 10 * 160});
    CHECK(speech[2] == SampleRange{600 * kS + 900 * 160, 615 * kS}); // clamped to the block
}

TEST_CASE("Stitching maps chunk-relative centiseconds to source time and keeps order", "[ai][chunks]") {
    const Ticks origin = Ticks::fromSeconds(2); // e.g. PCM sample 0 at 2 s of source time
    std::vector<ChunkResult> chunks(2);
    chunks[0].chunk = {16000, 16000 * 11};      // 1 s .. 11 s of PCM
    chunks[0].segments.push_back({50, 250, {{" Hello", 50, 120, 0.9f}, {" there", 130, 250, 0.7f}}, false});
    chunks[1].chunk = {16000 * 30, 16000 * 40}; // 30 s .. 40 s
    chunks[1].segments.push_back({0, 100, {{" again", 0, 100, 1.0f}}, false});
    chunks[1].segments.push_back({900, 2000, {{" late", 990, 1500, 1.0f}}, false}); // past the chunk end
    chunks[1].segments.push_back({100, 100, {{" ", 100, 100, 1.0f}}, false});       // empty: dropped

    Transcript t;
    REQUIRE(stitchChunks(chunks, origin, WordSplit::Whitespace, t));
    REQUIRE(t.validate());
    REQUIRE(t.words.size() == 4);
    REQUIRE(t.segments.size() == 3);
    const auto ms = [](std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; };
    CHECK(t.words[0].text == "Hello");
    CHECK(t.words[0].t0 == ms(2000 + 1000 + 500)); // origin + chunk start + 50 cs
    CHECK(t.words[0].t1 == ms(2000 + 1000 + 1200));
    CHECK(t.words[1].t0 == ms(2000 + 1000 + 1300));
    CHECK(t.words[2].t0 == ms(2000 + 30000));
    CHECK(t.words[3].t0 == ms(2000 + 30000 + 9900));
    CHECK(t.words[3].t1 == ms(2000 + 40000)); // clamped to the end of its chunk
    CHECK(t.segments[0].firstWord == 0);
    CHECK(t.segments[0].wordCount == 2);
    CHECK(t.segments[1].firstWord == 2);
    CHECK(t.segments[2].t0 == ms(2000 + 30000 + 9000));
    CHECK(t.segments[2].t1 == ms(2000 + 40000));

    SECTION("stitching chunk by chunk equals stitching at once") {
        Transcript incremental;
        for (const ChunkResult& c : chunks) {
            for (const RecognizedSegment& s : c.segments) {
                const ChunkResult one{c.chunk, {s}};
                REQUIRE(stitchChunks(std::span(&one, 1), origin, WordSplit::Whitespace, incremental));
            }
        }
        CHECK(incremental == t);
    }
    SECTION("a recogniser going back in time is clamped forward") {
        std::vector<ChunkResult> backwards(1);
        backwards[0].chunk = {0, 16000 * 10};
        backwards[0].segments.push_back({500, 600, {{" b", 500, 600, 1.0f}}, false});
        backwards[0].segments.push_back({100, 200, {{" a", 100, 200, 1.0f}}, false});
        Transcript bt;
        REQUIRE(stitchChunks(backwards, Ticks{}, WordSplit::Whitespace, bt));
        CHECK(bt.validate());
        CHECK(bt.words[1].t0 >= bt.words[0].t0);
    }
}
