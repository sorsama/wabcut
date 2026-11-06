// AI Refocus (card 181) and AI Frame Interpolation (card 133) in the evaluator. A clip with AI Refocus keeps its Depth Blur
// effect for the compositor and buildCompositeLayers hands it the frame's depth map from the installed SourceFrameProcessor as the
// effect's second picture (a raster whose alpha is the distance, placed like the frame); without a depth map the effect leaves the
// picture and the layer says so. A retimed clip set to AI Frame Interpolation shows the processor's picture between its two frames
// alone on the group's canvas, and plays with Optical Flow (a flow request, a flagged layer) while the picture is missing.

#include "PlaybackTestSupport.h"

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/SourceStage.h>
#include <wabcut/render/EffectLibrary.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

/// A processor with a depth map and an interpolated picture to give, or neither.
class SideProcessor final : public SourceFrameProcessor {
public:
    bool haveDepth = true;
    bool haveInterpolated = true;
    int depthCalls = 0;
    int interpolationCalls = 0;
    Ticks lastDepthPts{};
    double lastWeight = -1.0;
    Ticks lastFirst{};
    Ticks lastSecond{};
    SourceStageResult process(const SourceStageQuery&) override { return {}; }
    DepthResult depth(const DepthQuery& q) override {
        ++depthCalls;
        lastDepthPts = q.decoded ? q.decoded->pts : Ticks{};
        if (!haveDepth) {
            return {nullptr, true};
        }
        auto map = std::make_shared<DepthMap>();
        map->width = 8;
        map->height = 4;
        map->distance.resize(32);
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 4; ++y) {
                map->distance[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(x * 36);
            }
        }
        return {map, false};
    }
    InterpolationResult interpolate(const InterpolationQuery& q) override {
        ++interpolationCalls;
        lastWeight = q.weight;
        lastFirst = q.first->pts;
        lastSecond = q.second->pts;
        InterpolationResult r;
        if (haveInterpolated) {
            r.frame = frameOf(1920, 1080, q.first->pts);
        } else {
            r.missing = true;
        }
        return r;
    }
};

struct InstalledProcessor {
    explicit InstalledProcessor(std::shared_ptr<SourceFrameProcessor> p) { setSourceFrameProcessor(std::move(p)); }
    ~InstalledProcessor() { setSourceFrameProcessor(nullptr); }
};

EffectInstance refocus(ProjectState& s) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::string(kAiRefocusEffect);
    e.set("focus", ParamValue(30.0));
    e.set("amount", ParamValue(80.0));
    return e;
}

const render::CanvasSize kCanvas{1920, 1080, 1.0};

} // namespace

TEST_CASE("AI Refocus stays a compositor effect and the layer is flagged for its depth map", "[playback][evaluate][ai][refocus]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({refocus(s)}); });
    const auto specs = f.layers(10);
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].refocus);
    CHECK(specs[0].ai.empty()); // not a source-stage effect: the decoded picture is not replaced
    REQUIRE(specs[0].effects.size() == 1);
    CHECK(specs[0].effects[0].type == render::EffectType::Library);
    CHECK(specs[0].effects[0].op == static_cast<std::uint16_t>(render::LibraryOp::DepthBlur));
    CHECK(specs[0].effects[0].params[0] == Approx(30.0)); // Focus Distance, keyframable: read at the frame's time
    CHECK(specs[0].effects[0].params[2] == Approx(80.0)); // Blur Amount
    // A clip without it is not flagged.
    Fixture plain(1920, 1080, [](ProjectState&, Clip&) {});
    CHECK_FALSE(plain.layers(10)[0].refocus);
}

TEST_CASE("The depth map becomes the second picture of the Depth Blur effect, placed like the frame", "[playback][evaluate][ai][refocus]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({refocus(s)}); });
    auto processor = std::make_shared<SideProcessor>();
    InstalledProcessor installed(processor);
    auto specs = f.layers(10);
    std::vector<media::VideoFrame> frames{frameOf(1920, 1080, framesToTicks(10, kRate60))};
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
    REQUIRE(out.size() == 1);
    CHECK(processor->depthCalls == 1);
    CHECK(processor->lastDepthPts == framesToTicks(10, kRate60)); // asked for the decoded frame's own time
    CHECK_FALSE(out[0].aiUnprocessed);
    REQUIRE(out[0].effects.size() == 1);
    const auto& matte = out[0].effects[0].matte;
    REQUIRE(matte);
    CHECK(matte->canvas == kCanvas);
    REQUIRE(matte->layers.size() == 1);
    REQUIRE(matte->layers[0].view.has_value()); // a raster whose alpha is the distance
    // The 8 x 4 map is stretched over the 1080-line frame: the layer's scale grows by 1080 / 4.
    CHECK(matte->layers[0].transform.scale == Approx(specs[0].transform.scale * 270.0));
    CHECK(matte->layers[0].transform.position.x == Approx(specs[0].transform.position.x));
    CHECK(matte->layers[0].transform.position.y == Approx(specs[0].transform.position.y));
}

TEST_CASE("Without a depth map the effect leaves the picture alone and the layer says it is not processed", "[playback][evaluate][ai][refocus]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({refocus(s)}); });
    SECTION("the processor has none for this frame") {
        auto processor = std::make_shared<SideProcessor>();
        processor->haveDepth = false;
        InstalledProcessor installed(processor);
        auto specs = f.layers(10);
        std::vector<media::VideoFrame> frames{frameOf(1920, 1080, framesToTicks(10, kRate60))};
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        REQUIRE(out[0].effects.size() == 1);
        CHECK_FALSE(out[0].effects[0].matte);
    }
    SECTION("no processor is installed (a build without the AI helper)") {
        auto specs = f.layers(10);
        std::vector<media::VideoFrame> frames{frameOf(1920, 1080, framesToTicks(10, kRate60))};
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        CHECK_FALSE(out[0].effects[0].matte);
    }
    SECTION("a clip without Refocus never asks") {
        Fixture plain(1920, 1080, [](ProjectState&, Clip&) {});
        auto processor = std::make_shared<SideProcessor>();
        InstalledProcessor installed(processor);
        auto specs = plain.layers(10);
        std::vector<media::VideoFrame> frames{frameOf(1920, 1080, framesToTicks(10, kRate60))};
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
        REQUIRE(out.size() == 1);
        CHECK(processor->depthCalls == 0);
        CHECK_FALSE(out[0].aiUnprocessed);
    }
}

namespace {

/// The clip at a quarter of its speed with AI Frame Interpolation: sequence frame 1 shows source time 1/240 s, a quarter of the
/// way from the media's frame 0 to its frame 1 (60 fps).
Fixture slowMotion(TimeInterpolation mode) {
    return Fixture(1920, 1080, [mode](ProjectState&, Clip& c) {
        c.speed = Speed{1, 4};
        ClipContent content = c.content();
        content.timeInterpolation = mode;
        c.setContent(std::move(content));
    });
}

std::vector<media::VideoFrame> twoFrames() {
    return {{}, frameOf(1920, 1080, Ticks{}), frameOf(1920, 1080, framesToTicks(1, kRate60))};
}

} // namespace

TEST_CASE("AI Frame Interpolation shows the model's picture between the two frames, alone on the group's canvas", "[playback][evaluate][ai][interpolation]") {
    const Fixture f = slowMotion(TimeInterpolation::AiInterpolation);
    auto processor = std::make_shared<SideProcessor>();
    InstalledProcessor installed(processor);
    auto specs = f.layers(1);
    REQUIRE(specs.size() == 3);
    CHECK(specs[0].interpolation == TimeInterpolation::AiInterpolation);
    CHECK(specs[0].sourceTime == Ticks{kTicksPerSecond / 240});
    std::vector<media::VideoFrame> frames = twoFrames();
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].nested);
    CHECK_FALSE(out[0].aiUnprocessed);
    REQUIRE(out[0].nested->layers.size() == 1); // the model's picture alone: no cross-dissolve of the two frames
    CHECK_FALSE(out[0].nested->layers[0].transition.has_value());
    CHECK(processor->interpolationCalls >= 1);
    CHECK(processor->lastWeight == Approx(0.25).margin(1e-6));
    CHECK(processor->lastFirst == Ticks{});
    CHECK(processor->lastSecond == framesToTicks(1, kRate60));
}

TEST_CASE("Where the AI picture is missing the clip plays with Optical Flow and the layer says so", "[playback][evaluate][ai][interpolation]") {
    const Fixture f = slowMotion(TimeInterpolation::AiInterpolation);
    struct Flow final : MotionSource {
        std::shared_ptr<const motion::FlowPair> motion(const MotionRequest&) override {
            ++asked;
            return nullptr;
        }
        int asked = 0;
    } flow;
    SECTION("the processor has no picture for this time") {
        auto processor = std::make_shared<SideProcessor>();
        processor->haveInterpolated = false;
        InstalledProcessor installed(processor);
        auto specs = f.layers(1);
        std::vector<media::VideoFrame> frames = twoFrames();
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, &flow, kCanvas);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        REQUIRE(out[0].nested);
        REQUIRE(out[0].nested->layers.size() == 2); // the two frames mixed, as Optical Flow does
        REQUIRE(out[0].nested->layers[0].transition);
        CHECK(out[0].nested->layers[0].transition->progress == Approx(0.25));
        CHECK(flow.asked == 1); // the flow stands in for the missing picture
    }
    SECTION("no processor is installed") {
        auto specs = f.layers(1);
        std::vector<media::VideoFrame> frames = twoFrames();
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, &flow, kCanvas);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        CHECK(out[0].nested->layers.size() == 2);
    }
    SECTION("with the picture there, no flow is computed") {
        auto processor = std::make_shared<SideProcessor>();
        InstalledProcessor installed(processor);
        auto specs = f.layers(1);
        std::vector<media::VideoFrame> frames = twoFrames();
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, &flow, kCanvas);
        CHECK(flow.asked == 0);
        CHECK_FALSE(out[0].aiUnprocessed);
    }
}

TEST_CASE("A time on a frame needs no interpolation, and other modes never ask the processor", "[playback][evaluate][ai][interpolation]") {
    auto processor = std::make_shared<SideProcessor>();
    InstalledProcessor installed(processor);
    {
        // Sequence frame 4 at a quarter of the speed is exactly the media's frame 1: nothing falls between frames.
        const Fixture f = slowMotion(TimeInterpolation::AiInterpolation);
        auto specs = f.layers(4);
        std::vector<media::VideoFrame> frames = twoFrames();
        frames[1] = frameOf(1920, 1080, framesToTicks(1, kRate60));
        frames[2] = frameOf(1920, 1080, framesToTicks(2, kRate60));
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
        REQUIRE(out.size() == 1);
        CHECK(processor->interpolationCalls == 0);
        CHECK_FALSE(out[0].aiUnprocessed);
        REQUIRE(out[0].nested);
        CHECK(out[0].nested->layers.size() == 1);
    }
    for (const TimeInterpolation mode : {TimeInterpolation::FrameBlending, TimeInterpolation::OpticalFlow}) {
        const Fixture f = slowMotion(mode);
        auto specs = f.layers(1);
        std::vector<media::VideoFrame> frames = twoFrames();
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
        CHECK(processor->interpolationCalls == 0);
    }
}
