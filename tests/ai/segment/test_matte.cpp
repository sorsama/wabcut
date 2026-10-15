// AI-14 mattes: the run-length codec (round trips, every malformed input refused), base64, boxes, IoU, resampling, the
// signed-distance field the renderer samples, tracks as cache JSON (bounded), their identity (the cache key), and the
// store over the analysis cache.

#include "SegmentTestUtil.h"

#include <wabcut/ai/segment/Matte.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace wabcut;
using namespace wabcut::ai::segment;
using namespace wabcut::ai::segment::test;
using Catch::Approx;

namespace {

MatteImage ellipseMatte(int w, int h, double cx, double cy, double rx, double ry, bool soft = true) {
    MatteImage m;
    m.width = w;
    m.height = h;
    m.alpha.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double dx = ((x + 0.5) / w - cx) / rx, dy = ((y + 0.5) / h - cy) / ry;
            const double d = std::sqrt(dx * dx + dy * dy);
            const double a = soft ? std::clamp((1.0 - d) * 40.0 + 0.5, 0.0, 1.0) : (d <= 1.0 ? 1.0 : 0.0);
            m.alpha[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::lround(a * 255.0));
        }
    }
    return m;
}

MatteTrack sampleTrack() {
    MatteTrack t;
    t.fingerprint = 0x0123456789ABCDEFull;
    t.stream = 1;
    t.model = "sam2.1-tiny-video-aa7a8542-0461896d-v1";
    t.params = "00ff00ff00ff00ff";
    t.width = 64;
    t.height = 36;
    t.aspect = 16.0 / 9.0;
    t.frameDuration = frameTicks(1);
    t.prompts = {Prompt{frameTicks(3), {PromptPoint{0.25, 0.5, true}, PromptPoint{0.7, 0.1, false}}, MatteBox{0.1, 0.2, 0.3, 0.4}}};
    for (int i = 0; i < 5; ++i) {
        MatteFrame f;
        f.time = frameTicks(i);
        const MatteImage m = ellipseMatte(64, 36, 0.3 + 0.05 * i, 0.5, 0.15, 0.25);
        f.rle = encodeRle(m);
        f.box = boxOf(m);
        f.present = i != 2;
        f.confidence = 0.5f + 0.1f * i;
        t.frames.push_back(std::move(f));
    }
    t.cuts = {frameTicks(5)};
    return t;
}

} // namespace

TEST_CASE("Matte RLE: round trips, compact for mattes, refuses truncated or mis-sized runs", "[ai][segment][matte]") {
    SECTION("an object's matte is a few kilobytes and decodes exactly") {
        const MatteImage m = ellipseMatte(512, 288, 0.4, 0.5, 0.2, 0.35);
        const std::vector<std::uint8_t> rle = encodeRle(m);
        CHECK(rle.size() < 16 * 1024);
        CHECK(rle.size() * 20 < m.alpha.size());
        auto back = decodeRle(rle, 512, 288);
        REQUIRE(back);
        CHECK(back->alpha == m.alpha);
    }
    SECTION("noise (no runs) and long runs (multi-byte lengths) round trip") {
        std::mt19937 rng(7);
        MatteImage m;
        m.width = 300;
        m.height = 200;
        m.alpha.resize(60000);
        for (std::size_t i = 0; i < m.alpha.size(); ++i) {
            m.alpha[i] = i < 40000 ? static_cast<std::uint8_t>(rng() & 0xff) : 255;
        }
        auto back = decodeRle(encodeRle(m), 300, 200);
        REQUIRE(back);
        CHECK(back->alpha == m.alpha);
        MatteImage empty;
        empty.width = kMaxMatteSide;
        empty.height = kMaxMatteSide;
        empty.alpha.assign(static_cast<std::size_t>(kMaxMatteSide) * kMaxMatteSide, 0);
        CHECK(encodeRle(empty).size() == 5); // one run: the value and a 4-byte length
        CHECK(decodeRle(encodeRle(empty), kMaxMatteSide, kMaxMatteSide).has_value());
    }
    SECTION("malformed input is a ParseError, never a crash or a short matte") {
        const MatteImage m = ellipseMatte(40, 30, 0.5, 0.5, 0.3, 0.3);
        std::vector<std::uint8_t> rle = encodeRle(m);
        CHECK_FALSE(decodeRle(std::span(rle).first(rle.size() - 1), 40, 30));       // truncated run length
        CHECK_FALSE(decodeRle(std::span(rle).first(rle.size() - 2), 40, 30));       // too short
        CHECK_FALSE(decodeRle(rle, 40, 29));                                        // too long for the size
        CHECK_FALSE(decodeRle(rle, 0, 30));
        CHECK_FALSE(decodeRle(rle, kMaxMatteSide + 1, 30));
        const std::vector<std::uint8_t> zeroRun{255, 0};
        CHECK_FALSE(decodeRle(zeroRun, 1, 1));
        const std::vector<std::uint8_t> endless{255, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
        CHECK_FALSE(decodeRle(endless, 4, 4));
        const auto failed = decodeRle(std::span(rle).first(3), 40, 30);
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == ErrorCode::ParseError);
    }
}

TEST_CASE("Base64, boxes, IoU, resampling and matte sizes", "[ai][segment][matte]") {
    for (std::size_t n : {0u, 1u, 2u, 3u, 4u, 100u}) {
        std::vector<std::uint8_t> bytes(n);
        for (std::size_t i = 0; i < n; ++i) {
            bytes[i] = static_cast<std::uint8_t>(i * 37 + 11);
        }
        const auto back = fromBase64(toBase64(bytes));
        REQUIRE(back);
        CHECK(*back == bytes);
    }
    CHECK(toBase64(std::vector<std::uint8_t>{'M', 'a'}) == "TWE=");
    CHECK_FALSE(fromBase64("TWE"));
    CHECK_FALSE(fromBase64("T=WE"));
    CHECK_FALSE(fromBase64("TW!="));

    const MatteImage m = ellipseMatte(100, 50, 0.5, 0.5, 0.2, 0.3, false);
    const auto box = boxOf(m);
    REQUIRE(box);
    CHECK(box->x == Approx(0.3).margin(0.011));
    CHECK(box->width == Approx(0.4).margin(0.021));
    CHECK(box->y == Approx(0.2).margin(0.021));
    CHECK(box->height == Approx(0.6).margin(0.041));
    MatteImage none = m;
    std::fill(none.alpha.begin(), none.alpha.end(), 0);
    CHECK_FALSE(boxOf(none));

    CHECK(iouOf(m, m) == Approx(1.0));
    CHECK(iouOf(none, none) == Approx(1.0));
    CHECK(iouOf(m, none) == Approx(0.0));
    const MatteImage shifted = ellipseMatte(100, 50, 0.55, 0.5, 0.2, 0.3, false);
    CHECK(iouOf(m, shifted) > 0.6);
    CHECK(iouOf(m, shifted) < 0.95);

    const MatteImage up = resized(m, 400, 200);
    REQUIRE(up.valid());
    CHECK(iouOf(resized(up, 100, 50), m) > 0.97);

    CHECK(matteSize(16.0 / 9.0) == std::pair{512, 288});
    CHECK(matteSize(9.0 / 16.0) == std::pair{288, 512});
    CHECK(matteSize(4.0 / 3.0, 640) == std::pair{640, 480});
    CHECK(matteSize(0.0) == std::pair{512, 288}); // a bad aspect: 16:9
}

TEST_CASE("The signed distance field: positive inside, zero at the edge, in frame heights", "[ai][segment][matte]") {
    // A disc of radius 0.25 frame heights in a 16:9 frame, centred.
    const double aspect = 16.0 / 9.0;
    const MatteImage m = ellipseMatte(512, 288, 0.5, 0.5, 0.25 / aspect, 0.25, false);
    const std::vector<float> d = signedDistance(m, aspect);
    REQUIRE(d.size() == m.alpha.size());
    const auto at = [&](double u, double v) { return d[static_cast<std::size_t>(v * 288) * 512 + static_cast<std::size_t>(u * 512)]; };
    CHECK(at(0.5, 0.5) == Approx(0.25).margin(0.01));        // the centre: the radius
    CHECK(at(0.5, 0.05) == Approx(-0.2).margin(0.01));       // above it: outside
    CHECK(std::abs(at(0.5 + 0.25 / aspect, 0.5)) < 0.006);  // the edge
    CHECK(at(0.95, 0.5) < -0.5);
    MatteImage empty = m;
    std::fill(empty.alpha.begin(), empty.alpha.end(), 0);
    const std::vector<float> far = signedDistance(empty, aspect);
    CHECK(std::ranges::all_of(far, [](float v) { return v < -100.0f; })); // nothing there: far outside everywhere
}

TEST_CASE("Matte tracks: JSON round trip, bounds, identity and the cache key", "[ai][segment][matte]") {
    const MatteTrack t = sampleTrack();
    auto back = trackFromJson(trackToJson(t));
    REQUIRE(back);
    CHECK(back->identity() == t.identity());
    CHECK(back->prompts == t.prompts);
    REQUIRE(back->frames.size() == t.frames.size());
    for (std::size_t i = 0; i < t.frames.size(); ++i) {
        CHECK(back->frames[i].rle == t.frames[i].rle);
        CHECK(back->frames[i].present == t.frames[i].present);
        CHECK(back->frames[i].box == t.frames[i].box);
    }
    CHECK(back->cuts == t.cuts);
    auto image = back->imageOf(back->frames[1]);
    REQUIRE(image);
    CHECK(image->width == 64);

    SECTION("identity: the fingerprint, the stream, the model, the run's hash") {
        CHECK(t.identity() == "0123456789abcdef/1/sam2.1-tiny-video-aa7a8542-0461896d-v1/00ff00ff00ff00ff");
        auto id = parseTrackId(t.identity());
        REQUIRE(id);
        CHECK(id->fingerprint == t.fingerprint);
        CHECK(id->stream == 1);
        CHECK(id->model == t.model);
        CHECK_FALSE(parseTrackId("0123/1/m/p"));
        CHECK_FALSE(parseTrackId("0123456789abcdef/1/m/../../x"));
        CHECK_FALSE(parseTrackId("0123456789abcdef/1/m"));
        CHECK_FALSE(parseTrackId("0123456789abcdef/1/m/p q"));
    }
    SECTION("the cache key changes with the prompts, the span, the size and the run") {
        const std::string base = promptsHash(t.prompts, TimeRange{Ticks{}, frameTicks(100)}, 512, 288, "both");
        CHECK(base.size() == 16);
        CHECK(base == promptsHash(t.prompts, TimeRange{Ticks{}, frameTicks(100)}, 512, 288, "both"));
        std::vector<Prompt> other = t.prompts;
        other[0].points[0].x += 0.01;
        CHECK(base != promptsHash(other, TimeRange{Ticks{}, frameTicks(100)}, 512, 288, "both"));
        other = t.prompts;
        other[0].points[1].positive = true;
        CHECK(base != promptsHash(other, TimeRange{Ticks{}, frameTicks(100)}, 512, 288, "both"));
        CHECK(base != promptsHash(t.prompts, TimeRange{Ticks{}, frameTicks(101)}, 512, 288, "both"));
        CHECK(base != promptsHash(t.prompts, TimeRange{Ticks{}, frameTicks(100)}, 640, 360, "both"));
        CHECK(base != promptsHash(t.prompts, TimeRange{Ticks{}, frameTicks(100)}, 512, 288, "forward"));
    }
    SECTION("malformed or oversized cache files are refused") {
        nlohmann::json j = trackToJson(t);
        j["format"] = "something else";
        CHECK_FALSE(trackFromJson(j));
        j = trackToJson(t);
        j["width"] = kMaxMatteSide + 1;
        CHECK_FALSE(trackFromJson(j));
        j = trackToJson(t);
        j["frames"][1]["m"] = "not base64!";
        CHECK_FALSE(trackFromJson(j));
        j = trackToJson(t);
        std::swap(j["frames"][0], j["frames"][1]);
        CHECK_FALSE(trackFromJson(j)); // out of order
        j = trackToJson(t);
        j["prompts"][0]["points"] = nlohmann::json::array();
        for (std::size_t i = 0; i <= kMaxPromptPoints; ++i) {
            j["prompts"][0]["points"].push_back(nlohmann::json::array({0.5, 0.5, 1}));
        }
        CHECK_FALSE(trackFromJson(j));
        j = trackToJson(t);
        j["frames"][0].erase("t");
        CHECK_FALSE(trackFromJson(j));
        CHECK_FALSE(trackFromJson(nlohmann::json::array()));
    }
    SECTION("merge replaces frames at the same times and adds the others") {
        MatteTrack a = t;
        MatteTrack b = t;
        b.frames.erase(b.frames.begin(), b.frames.begin() + 3); // frames 3, 4
        b.frames[0].confidence = 0.01f;
        MatteFrame extra = b.frames.back();
        extra.time = frameTicks(9);
        b.frames.push_back(extra);
        a.merge(b);
        REQUIRE(a.frames.size() == 6);
        CHECK(a.frames[3].confidence == 0.01f);
        CHECK(a.frames.back().time == frameTicks(9));
        CHECK(a.span() == TimeRange{Ticks{}, frameTicks(10)});
        CHECK(a.frameAt(frameTicks(9) + Ticks{5}, Ticks{frameTicks(1).value / 2}) == &a.frames.back());
        CHECK(a.frameAt(frameTicks(7), Ticks{frameTicks(1).value / 2}) == nullptr);
    }
}

TEST_CASE("The matte store: tracks in the analysis cache by identity", "[ai][segment][matte]") {
    const auto dir = freshDir("store");
    const MatteStore store(dir);
    const MatteTrack t = sampleTrack();
    auto missing = store.get(t.identity());
    REQUIRE(missing);
    CHECK_FALSE(missing->has_value());
    REQUIRE(store.put(t));
    auto got = store.get(t.identity());
    REQUIRE(got);
    REQUIRE(got->has_value());
    CHECK((*got)->frames.size() == t.frames.size());
    CHECK((*got)->identity() == t.identity());
    CHECK_FALSE(store.get("not an identity"));
}
