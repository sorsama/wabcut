// OpenFX host (0.7): the processor that runs a layer's OpenFX effects on its decoded frame - the picture it returns for the
// compositor, the cache, the order of the effects, bypassed effects, badges (missing, crashed) and retrying.

#include "OfxFixture.h"

#include <wabcut/ofxhost/OfxEffects.h>
#include <wabcut/playback/video/ExternalEffects.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <thread>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// BT.709 limited-range grey of luma code 126: (126 - 16) / 219 = 0.5023, 128 in 8 bits.
constexpr int kGreyCode = 128;

playback::ExternalSource sourceOf(const media::FrameHandle& frame) {
    playback::ExternalSource s;
    s.frame = &frame;
    return s;
}

media::FrameHandle rgbaFrame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a, AlphaMode alpha) {
    auto storage = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (std::size_t i = 0; i < storage->size(); i += 4) {
        (*storage)[i] = r;
        (*storage)[i + 1] = g;
        (*storage)[i + 2] = b;
        (*storage)[i + 3] = a;
    }
    media::FrameHandle f;
    f.pts = Ticks{0};
    f.duration = Ticks{1000};
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::RGBA8;
    f.alpha = alpha;
    f.color.matrix = ColorMatrix::RGB;
    f.color.range = ColorRange::Full;
    f.color.primaries = ColorPrimaries::BT709;
    f.color.transfer = TransferFunction::SRGB;
    f.source.fingerprint = 9;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{storage->data(), w * 4, w, h};
    cpu.owner = storage;
    f.payload = std::move(cpu);
    return f;
}

} // namespace

TEST_CASE("OpenFX effects: an effect runs on a decoded frame and returns the picture the compositor draws", "[ofxhost][effects]") {
    TestOfx ofx;
    const auto frame = yuvFrame(64, 48, 126, 128, 128);
    SECTION("a gain of one changes nothing but the format") {
        const auto picture = ofx.effects->process(sourceOf(frame), {externalEffect(kGain, 1, gainKeys())});
        REQUIRE(picture);
        CHECK(picture->width == 64);
        CHECK(picture->height == 48);
        CHECK(picture->layout == render::PlaneLayout::PackedRGBA);
        CHECK(picture->alpha == AlphaMode::Premultiplied);
        // The picture is in the frame's own encoding: RGB, full range, its primaries and transfer.
        CHECK(picture->color.matrix == ColorMatrix::RGB);
        CHECK(picture->color.range == ColorRange::Full);
        CHECK(picture->color.primaries == ColorPrimaries::BT709);
        CHECK(picture->color.transfer == TransferFunction::BT709);
        const std::uint8_t* p = rgbaAt(*picture, 10, 10);
        CHECK(std::abs(p[0] - kGreyCode) <= 1);
        CHECK(std::abs(p[1] - kGreyCode) <= 1);
        CHECK(std::abs(p[2] - kGreyCode) <= 1);
        CHECK(p[3] == 255);
    }
    SECTION("a gain of a half halves it") {
        const auto picture = ofx.effects->process(sourceOf(frame), {externalEffect(kGain, 1, gainKeys({{"gain", 0.5}}))});
        REQUIRE(picture);
        const std::uint8_t* p = rgbaAt(*picture, 63, 47);
        CHECK(std::abs(p[0] - 64) <= 1);
        CHECK(std::abs(p[1] - 64) <= 1);
        CHECK(std::abs(p[2] - 64) <= 1);
    }
    SECTION("a tint scales the channels apart") {
        const auto picture = ofx.effects->process(
            sourceOf(frame), {externalEffect(kGain, 1, gainKeys({{"tint.r", 1.0}, {"tint.g", 0.5}, {"tint.b", 0.0}}))});
        REQUIRE(picture);
        const std::uint8_t* p = rgbaAt(*picture, 3, 3);
        CHECK(std::abs(p[0] - kGreyCode) <= 1);
        CHECK(std::abs(p[1] - 64) <= 1);
        CHECK(p[2] == 0);
    }
}

TEST_CASE("OpenFX effects: the picture is not turned over on the way through the plug-in", "[ofxhost][effects]") {
    TestOfx ofx;
    // White above, black below; the plug-in paints the bottom row green.
    const auto frame = yuvFrame(64, 48, 235, 128, 128, 7, 0, 16);
    const auto picture = ofx.effects->process(sourceOf(frame), {externalEffect(kGain, 1, gainKeys({{"bottomMark", 1.0}}))});
    REQUIRE(picture);
    const std::uint8_t* top = rgbaAt(*picture, 20, 0);
    CHECK(top[0] == 255);
    CHECK(top[1] == 255);
    CHECK(top[2] == 255);
    const std::uint8_t* below = rgbaAt(*picture, 20, 46);
    CHECK(below[0] == 0);
    CHECK(below[1] == 0);
    const std::uint8_t* last = rgbaAt(*picture, 20, 47);
    CHECK(last[0] == 0);
    CHECK(last[1] == 255);
    CHECK(last[2] == 0);
}

TEST_CASE("OpenFX effects: alpha - straight RGBA is premultiplied for the plug-in and stays premultiplied", "[ofxhost][effects]") {
    TestOfx ofx;
    const auto frame = rgbaFrame(16, 16, 200, 100, 50, 128, AlphaMode::Straight);
    const auto picture = ofx.effects->process(sourceOf(frame), {externalEffect(kGain, 1, gainKeys())});
    REQUIRE(picture);
    const std::uint8_t* p = rgbaAt(*picture, 4, 4);
    CHECK(std::abs(p[0] - 100) <= 1); // 200 * 128 / 255
    CHECK(std::abs(p[1] - 50) <= 1);
    CHECK(std::abs(p[2] - 25) <= 1);
    CHECK(p[3] == 128);
    // An opaque RGBA8 frame passes at its own values.
    const auto opaque = rgbaFrame(16, 16, 51, 102, 153, 255, AlphaMode::None);
    const auto flat = ofx.effects->process(sourceOf(opaque), {externalEffect(kGain, 2, gainKeys())});
    REQUIRE(flat);
    CHECK(std::abs(rgbaAt(*flat, 1, 1)[0] - 51) <= 1);
    CHECK(std::abs(rgbaAt(*flat, 1, 1)[2] - 153) <= 1);
    CHECK(rgbaAt(*flat, 1, 1)[3] == 255);
}

TEST_CASE("OpenFX effects: results are cached by frame and parameters", "[ofxhost][effects][cache]") {
    TestOfx ofx;
    const auto frame = yuvFrame(32, 24, 126, 128, 128);
    const auto effect = externalEffect(kGain, 1, gainKeys({{"gain", 0.5}}));

    const auto first = ofx.effects->process(sourceOf(frame), {effect});
    REQUIRE(first);
    auto stats = ofx.effects->cacheStats();
    CHECK(stats.misses == 1);
    CHECK(stats.hits == 0);
    CHECK(stats.pictures == 1);
    CHECK(stats.entries == 1);
    CHECK(stats.bytes == 32u * 24u * 4u);

    // The same frame and values again: the plug-in does not run, and the very same raster comes back (so the GPU keeps its copy).
    const auto second = ofx.effects->process(sourceOf(frame), {effect});
    REQUIRE(second);
    stats = ofx.effects->cacheStats();
    CHECK(stats.hits == 1);
    CHECK(stats.pictures == 1);
    CHECK(second->identity == first->identity);

    // Another value, another frame or another time of the clip: a new picture.
    REQUIRE(ofx.effects->process(sourceOf(frame), {externalEffect(kGain, 1, gainKeys({{"gain", 0.25}}))}));
    CHECK(ofx.effects->cacheStats().pictures == 2);
    const auto later = yuvFrame(32, 24, 126, 128, 128, 7, 5000);
    REQUIRE(ofx.effects->process(sourceOf(later), {effect}));
    CHECK(ofx.effects->cacheStats().pictures == 3);

    // A frame of no known identity is never cached.
    const auto anonymous = yuvFrame(32, 24, 126, 128, 128, 0);
    REQUIRE(ofx.effects->process(sourceOf(anonymous), {effect}));
    REQUIRE(ofx.effects->process(sourceOf(anonymous), {effect}));
    CHECK(ofx.effects->cacheStats().pictures == 5);

    ofx.effects->clearCache();
    CHECK(ofx.effects->cacheStats().entries == 0);
    REQUIRE(ofx.effects->process(sourceOf(frame), {effect}));
    CHECK(ofx.effects->cacheStats().pictures == 6);

    // The budget bounds it.
    ofx.effects->setCacheBudget(1000); // less than one picture
    REQUIRE(ofx.effects->process(sourceOf(frame), {externalEffect(kGain, 1, gainKeys({{"gain", 0.75}}))}));
    CHECK(ofx.effects->cacheStats().entries == 0);
}

TEST_CASE("OpenFX effects: the effects of a stack run in order", "[ofxhost][effects]") {
    TestOfx ofx;
    const auto frame = yuvFrame(32, 24, 126, 128, 128);
    // Gain a half (128 -> 64), then add 51/255: 64 + 51 = 115.
    const auto gainThenAdd = ofx.effects->process(
        sourceOf(frame), {externalEffect(kGain, 1, gainKeys({{"gain", 0.5}})), externalEffect(kGain, 2, gainKeys({{"mode", 1.0}, {"offset", 51.0}}))});
    REQUIRE(gainThenAdd);
    CHECK(std::abs(rgbaAt(*gainThenAdd, 5, 5)[0] - 115) <= 2);
    // Add first (128 + 51 = 179), then the gain: 90.
    const auto addThenGain = ofx.effects->process(
        sourceOf(frame), {externalEffect(kGain, 3, gainKeys({{"mode", 1.0}, {"offset", 51.0}})), externalEffect(kGain, 4, gainKeys({{"gain", 0.5}}))});
    REQUIRE(addThenGain);
    CHECK(std::abs(rgbaAt(*addThenGain, 5, 5)[0] - 90) <= 2);
}

TEST_CASE("OpenFX effects: an effect that cannot run is bypassed, the others still run", "[ofxhost][effects]") {
    TestOfx ofx;
    const auto frame = yuvFrame(32, 24, 126, 128, 128);
    // A plug-in that fails the picture, one that is not installed, and a good one.
    const auto picture = ofx.effects->process(
        sourceOf(frame), {externalEffect(kFail, 1), externalEffect("plugin.ofx.not.installed", 2), externalEffect(kGain, 3, gainKeys({{"gain", 0.5}}))});
    REQUIRE(picture);
    CHECK(std::abs(rgbaAt(*picture, 2, 2)[0] - 64) <= 1);
    // Nothing runs: an error, and the layer keeps its frame.
    const auto none = ofx.effects->process(sourceOf(frame), {externalEffect("plugin.ofx.not.installed", 4)});
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::IoError);
    // Nothing to process, and no frame, are refused.
    CHECK_FALSE(ofx.effects->process(sourceOf(frame), {}));
    CHECK_FALSE(ofx.effects->process(playback::ExternalSource{}, {externalEffect(kGain, 5)}));
}

TEST_CASE("OpenFX effects: pictures the plug-ins cannot take are refused, not damaged", "[ofxhost][effects]") {
    TestOfx ofx;
    auto hdr = yuvFrame(32, 24, 126, 128, 128);
    hdr.color.transfer = TransferFunction::PQ;
    hdr.color.primaries = ColorPrimaries::BT2020;
    hdr.color.matrix = ColorMatrix::BT2020NonConstant;
    const auto picture = ofx.effects->process(sourceOf(hdr), {externalEffect(kGain, 1, gainKeys())});
    REQUIRE_FALSE(picture);
    CHECK(picture.error().code == ErrorCode::Unsupported);
    CHECK(picture.error().message.find("HDR") != std::string::npos);
}

TEST_CASE("OpenFX effects: a crashing plug-in is badged, bypassed, and tried again on request", "[ofxhost][effects][crash]") {
    TestOfx ofx;
    const auto frame = yuvFrame(32, 24, 126, 128, 128);
    const EffectInstance crashing = ofxhost::makeOfxEffect(ofx.plugin(kCrash), EffectInstanceId{50});
    const EffectInstance good = ofxhost::makeOfxEffect(ofx.plugin(kGain), EffectInstanceId{51});
    CHECK(ofx.effects->status(crashing).status == ofxhost::OfxEffectStatus::Ready);
    const std::uint64_t before = ofx.effects->statusRevision();

    // The crash does not reach this process: the layer shows its own frame (an error) and a good effect beside it runs.
    const auto lost = ofx.effects->process(sourceOf(frame), {externalEffect(kCrash, 50)});
    REQUIRE_FALSE(lost);
    const auto kept = ofx.effects->process(sourceOf(frame), {externalEffect(kCrash, 50), externalEffect(kGain, 51, gainKeys({{"gain", 0.5}}))});
    REQUIRE(kept);
    CHECK(std::abs(rgbaAt(*kept, 0, 0)[0] - 64) <= 1);

    const auto badge = ofx.effects->status(crashing);
    CHECK(badge.status == ofxhost::OfxEffectStatus::Crashed);
    CHECK(badge.message.starts_with("Crashed: the plug-in crashed"));
    CHECK(badge.message.find("0xC0000005") != std::string::npos);
    CHECK(ofx.effects->status(good).status == ofxhost::OfxEffectStatus::Ready);
    CHECK(ofx.effects->statusRevision() > before); // the badge asks to be redrawn

    // Retrying takes the badge off; the next picture starts a new helper (and crashes again).
    ofx.effects->retry(EffectInstanceId{50});
    CHECK(ofx.effects->status(crashing).status == ofxhost::OfxEffectStatus::Ready);
    CHECK_FALSE(ofx.effects->process(sourceOf(frame), {externalEffect(kCrash, 50)}));
    CHECK(ofx.effects->status(crashing).status == ofxhost::OfxEffectStatus::Crashed);
}

TEST_CASE("OpenFX effects: a plug-in that fails again and again is badged failed", "[ofxhost][effects]") {
    TestOfx ofx;
    const auto frame = yuvFrame(32, 24, 126, 128, 128);
    const EffectInstance failing = ofxhost::makeOfxEffect(ofx.plugin(kFail), EffectInstanceId{60});
    for (std::uint64_t i = 0; i < 3; ++i) {
        const auto later = yuvFrame(32, 24, 126, 128, 128, 7, static_cast<std::int64_t>(i) * 1000); // not cached: another frame
        CHECK_FALSE(ofx.effects->process(sourceOf(later), {externalEffect(kFail, 60)}));
    }
    const auto badge = ofx.effects->status(failing);
    CHECK(badge.status == ofxhost::OfxEffectStatus::Failed);
    CHECK(badge.message == "Failed: this plug-in always fails");
    CHECK(ofx.effects->problem(failing) == badge.message);
}

TEST_CASE("OpenFX effects: a plug-in this computer does not have is badged missing, with the name it was saved with", "[ofxhost][effects]") {
    TestOfx ofx;
    EffectInstance gone;
    gone.id = EffectInstanceId{70};
    gone.effect = "plugin.ofx.vendor.gone";
    gone.setData(EffectData{"plugin.name", std::string("Gone Plug-in (Vendor)")});
    CHECK_FALSE(isEvaluable(gone, EffectKind::Video)); // the model keeps it as an unknown effect
    const auto badge = ofx.effects->status(gone);
    CHECK(badge.status == ofxhost::OfxEffectStatus::Missing);
    CHECK(badge.message == "Missing plug-in: Gone Plug-in (Vendor)");
    CHECK(ofx.effects->problem(gone) == badge.message);
    // Without the saved name, the identifier.
    gone.data.clear();
    CHECK(ofx.effects->status(gone).message == "Missing plug-in: vendor.gone");
    // Not an OpenFX effect: no badge.
    EffectInstance blur;
    blur.effect = "wabcut.blur.gaussian";
    CHECK(ofx.effects->status(blur).status == ofxhost::OfxEffectStatus::NotOfx);
    CHECK(ofx.effects->problem(blur).empty());
}

TEST_CASE("OpenFX effects: the offline scope is per thread and nests", "[ofxhost][effects]") {
    CHECK_FALSE(playback::offlineVideoRender());
    {
        const playback::ScopedOfflineVideoRender outer;
        CHECK(playback::offlineVideoRender());
        {
            const playback::ScopedOfflineVideoRender inner;
            CHECK(playback::offlineVideoRender());
        }
        CHECK(playback::offlineVideoRender());
        bool other = true;
        std::thread([&other] { other = playback::offlineVideoRender(); }).join();
        CHECK_FALSE(other);
    }
    CHECK_FALSE(playback::offlineVideoRender());
}
