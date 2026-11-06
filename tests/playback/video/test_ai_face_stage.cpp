// AI Face Retouch (card 132) in the evaluator. A clip with AI Face Retouch keeps its effect for the compositor and
// buildCompositeLayers hands it the frame's face map from the installed SourceFrameProcessor as the effect's second picture (an
// opaque raster whose red, green and blue are the skin, mouth and eye coverage, sRGB-encoded so the shader reads them as linear
// coverage, placed like the frame); without a face map the effect leaves the picture and the layer says so; with every amount at 0
// the processor is not even asked.

#include "PlaybackTestSupport.h"

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/SourceStage.h>
#include <wabcut/render/EffectLibrary.h>
#include <wabcut/render/FaceMap.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

std::shared_ptr<MediaItem> mediaOf(ProjectState& s, int w, int h) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = w;
    v.height = h;
    v.frameRate = kRate60;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

struct Fixture {
    ProjectState state;
    Sequence seq;
    std::shared_ptr<MediaItem> media;

    Fixture(int w, int h, const std::function<void(ProjectState&, Clip&)>& setup) {
        media = mediaOf(state, w, h);
        state.media = {media};
        seq.id = state.ids.allocate<SequenceId>();
        seq.frameRate = kRate60;
        seq.width = 1920;
        seq.height = 1080;
        Clip c;
        c.id = state.ids.allocate<ClipId>();
        c.media = media->id;
        c.timelineRange = {Ticks{}, framesToTicks(120, kRate60)};
        setup(state, c);
        Track t;
        t.id = state.ids.allocate<TrackId>();
        t.clips = ClipList({c});
        seq.videoTracks = {std::make_shared<const Track>(std::move(t))};
    }

    std::vector<LayerSpec> layers(std::int64_t frame) const {
        std::vector<LayerSpec> out;
        evaluateVideoLayers(state, seq, framesToTicks(frame, kRate60), out);
        return out;
    }
};

media::VideoFrame frameOf(int w, int h, Ticks pts) {
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4, 128);
    media::VideoFrame f;
    f.pts = pts;
    f.duration = framesToTicks(1, kRate60);
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::RGBA8;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{pixels->data(), w * 4, w, h};
    cpu.owner = pixels;
    f.payload = std::move(cpu);
    return f;
}

/// A processor with a face map (8 x 4: skin on the left half, mouth in one corner, eyes in another) to give, or none.
class FaceProcessor final : public SourceFrameProcessor {
public:
    bool haveMap = true;
    int calls = 0;
    Ticks lastPts{};
    SourceStageResult process(const SourceStageQuery&) override { return {}; }
    FaceResult faces(const FaceQuery& q) override {
        ++calls;
        lastPts = q.decoded ? q.decoded->pts : Ticks{};
        if (!haveMap) {
            return {nullptr, true};
        }
        auto map = std::make_shared<FaceMap>();
        map->width = 8;
        map->height = 4;
        map->coverage.assign(8 * 4 * 3, 0);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 8; ++x) {
                std::uint8_t* px = &map->coverage[(static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)) * 3];
                px[0] = x < 4 ? 255 : 0;               // skin
                px[1] = x == 7 && y == 3 ? 128 : 0;    // mouth, half covered in one corner
                px[2] = x == 6 && y == 0 ? 255 : 0;    // eyes
            }
        }
        return {map, false};
    }
};

struct InstalledProcessor {
    explicit InstalledProcessor(std::shared_ptr<SourceFrameProcessor> p) { setSourceFrameProcessor(std::move(p)); }
    ~InstalledProcessor() { setSourceFrameProcessor(nullptr); }
};

EffectInstance faceRetouch(ProjectState& s, double smoothing = 60.0, double teeth = 40.0) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::string(kAiFaceRetouchEffect);
    e.set("smoothing", ParamValue(smoothing));
    e.set("teeth", ParamValue(teeth));
    return e;
}

const render::CanvasSize kCanvas{1920, 1080, 1.0};

std::vector<render::CompositeLayer> build(const Fixture& f, std::int64_t frame) {
    auto specs = f.layers(frame);
    std::vector<media::VideoFrame> frames{frameOf(1920, 1080, framesToTicks(frame, kRate60))};
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
    return out;
}

} // namespace

TEST_CASE("AI Face Retouch stays a compositor effect and the layer is flagged for its face map", "[playback][evaluate][ai][faces]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({faceRetouch(s)}); });
    const auto specs = f.layers(10);
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].faceRetouch);
    CHECK_FALSE(specs[0].refocus);
    CHECK(specs[0].ai.empty()); // not a source-stage effect: the decoded picture is not replaced
    REQUIRE(specs[0].effects.size() == 1);
    CHECK(specs[0].effects[0].type == render::EffectType::Library);
    CHECK(specs[0].effects[0].op == static_cast<std::uint16_t>(render::LibraryOp::FaceRetouch));
    CHECK(specs[0].effects[0].params[0] == Approx(60.0)); // Skin Smoothing, keyframable: read at the frame's time
    CHECK(specs[0].effects[0].params[5] == Approx(40.0)); // Teeth Whitening
    // A clip without it is not flagged; Skin Smoothing (not AI) is an ordinary effect.
    Fixture plain(1920, 1080, [](ProjectState&, Clip&) {});
    CHECK_FALSE(plain.layers(10)[0].faceRetouch);
}

TEST_CASE("The face map becomes the second picture of the Face Retouch effect as coverage, placed like the frame", "[playback][evaluate][ai][faces]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({faceRetouch(s)}); });
    auto processor = std::make_shared<FaceProcessor>();
    InstalledProcessor installed(processor);
    const auto out = build(f, 10);
    REQUIRE(out.size() == 1);
    CHECK(processor->calls == 1);
    CHECK(processor->lastPts == framesToTicks(10, kRate60)); // asked for the decoded frame's own time
    CHECK_FALSE(out[0].aiUnprocessed);
    REQUIRE(out[0].effects.size() == 1);
    const auto& matte = out[0].effects[0].matte;
    REQUIRE(matte);
    CHECK(matte->canvas == kCanvas);
    REQUIRE(matte->layers.size() == 1);
    REQUIRE(matte->layers[0].view.has_value());
    // The 8 x 4 map is stretched over the 1080-line frame: the layer's scale grows by 1080 / 4.
    const auto specs = f.layers(10);
    CHECK(matte->layers[0].transform.scale == Approx(specs[0].transform.scale * 270.0));
    CHECK(matte->layers[0].transform.position.x == Approx(specs[0].transform.position.x));
    CHECK(matte->layers[0].transform.position.y == Approx(specs[0].transform.position.y));
    // The raster: RGBA8, opaque, the coverage sRGB-encoded (so the shader's linear red, green and blue are the coverage).
    const render::FrameView& view = *matte->layers[0].view;
    REQUIRE(view.planeCount == 1);
    CHECK(view.width == 8);
    CHECK(view.height == 4);
    CHECK(view.color.transfer == TransferFunction::SRGB);
    const auto byteAt = [&](int x, int y, int channel) {
        return view.planes[0].data[static_cast<std::size_t>(y) * static_cast<std::size_t>(view.planes[0].stride) + static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(channel)];
    };
    CHECK(byteAt(0, 0, 0) == 255);                       // skin: full coverage stays full
    CHECK(byteAt(5, 0, 0) == 0);                         // not skin
    CHECK(byteAt(7, 3, 1) == render::faceMapByte(128));  // half the mouth: the sRGB byte of 0.5 in linear, not 128
    CHECK(byteAt(7, 3, 1) > 180);
    CHECK(byteAt(7, 3, 1) < 200);
    CHECK(byteAt(6, 0, 2) == 255);                       // an eye
    CHECK(byteAt(0, 0, 3) == 255);                       // opaque everywhere
    CHECK(byteAt(7, 3, 3) == 255);
}

TEST_CASE("The face map raster is the coverage encoded for linear reading and refuses a wrong size", "[playback][evaluate][ai][faces]") {
    CHECK(render::faceMapByte(0) == 0);
    CHECK(render::faceMapByte(255) == 255);
    for (int c = 1; c < 256; ++c) {
        CHECK(render::faceMapByte(static_cast<std::uint8_t>(c)) >= render::faceMapByte(static_cast<std::uint8_t>(c - 1))); // monotonic
    }
    // Decoded with the sRGB curve, every byte gives back its coverage within one 8-bit step of the linear scale.
    for (int c = 0; c < 256; ++c) {
        const double e = render::faceMapByte(static_cast<std::uint8_t>(c)) / 255.0;
        const double linear = e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
        CHECK(std::abs(linear * 255.0 - c) < 1.2 + 0.02 * c);
    }
    const std::vector<std::uint8_t> short_(10, 255);
    const auto empty = render::makeFaceMapRaster(4, 4, short_);
    REQUIRE(empty);
    CHECK(empty->rgba()[0] == 0); // a map of the wrong size covers nothing
    CHECK(empty->rgba()[3] == 255);
}

TEST_CASE("Without a face map the effect leaves the picture alone and the layer says it is not processed", "[playback][evaluate][ai][faces]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({faceRetouch(s)}); });
    SECTION("the processor has none for this frame") {
        auto processor = std::make_shared<FaceProcessor>();
        processor->haveMap = false;
        InstalledProcessor installed(processor);
        const auto out = build(f, 10);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        REQUIRE(out[0].effects.size() == 1);
        CHECK_FALSE(out[0].effects[0].matte);
    }
    SECTION("no processor is installed (a build without the AI helper)") {
        const auto out = build(f, 10);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        CHECK_FALSE(out[0].effects[0].matte);
    }
    SECTION("a clip without Face Retouch never asks") {
        Fixture plain(1920, 1080, [](ProjectState&, Clip&) {});
        auto processor = std::make_shared<FaceProcessor>();
        InstalledProcessor installed(processor);
        const auto out = build(plain, 10);
        REQUIRE(out.size() == 1);
        CHECK(processor->calls == 0);
        CHECK_FALSE(out[0].aiUnprocessed);
    }
}

TEST_CASE("A Face Retouch with every amount at 0 asks for nothing and flags nothing", "[playback][evaluate][ai][faces]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) {
        EffectInstance e = faceRetouch(s, 0.0, 0.0);
        e.set("blemish", ParamValue(0.0));
        e.set("eyes", ParamValue(0.0));
        c.setEffects({e});
    });
    auto processor = std::make_shared<FaceProcessor>();
    InstalledProcessor installed(processor);
    const auto out = build(f, 10);
    REQUIRE(out.size() == 1);
    CHECK(processor->calls == 0);
    CHECK_FALSE(out[0].aiUnprocessed);
    CHECK_FALSE(out[0].effects[0].matte);
    // Showing a map is something to see even with the amounts at 0.
    Fixture view(1920, 1080, [](ProjectState& s, Clip& c) {
        EffectInstance e = faceRetouch(s, 0.0, 0.0);
        e.set("blemish", ParamValue(0.0));
        e.set("eyes", ParamValue(0.0));
        e.set("view", ParamValue(1.0));
        c.setEffects({e});
    });
    const auto shown = build(view, 10);
    REQUIRE(shown.size() == 1);
    CHECK(processor->calls == 1);
    CHECK(shown[0].effects[0].matte);
}
