// AI video effects (0.7) in the evaluator: a clip's AI effects become LayerSpec::ai (Auto resolved against the media and the
// canvas, Amount at the frame's curve time) and never reach the compositor as stack effects; buildCompositeLayers hands the
// decoded picture to the installed SourceFrameProcessor, keeps the unprocessed one (and says so) where there is no processed
// picture, and places a picture of another size where the original would be.

#include "PlaybackTestSupport.h"

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/playback/video/SourceStage.h>

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

EffectInstance restore(ProjectState& s, double scale, double deblur, double denoise, double amount = 100.0, bool enabled = true) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::string(kAiRestoreEffect);
    e.enabled = enabled;
    e.set("scale", ParamValue(scale));
    e.set("deblur", ParamValue(deblur));
    e.set("denoise", ParamValue(denoise));
    e.set("amount", ParamValue(amount));
    return e;
}

struct Fixture {
    ProjectState state;
    Sequence seq;
    std::shared_ptr<MediaItem> media;

    Fixture(int mediaWidth, int mediaHeight, std::vector<EffectInstance> effects(ProjectState&)) {
        media = mediaOf(state, mediaWidth, mediaHeight);
        state.media = {media};
        seq.id = state.ids.allocate<SequenceId>();
        seq.frameRate = kRate60;
        seq.width = 1920;
        seq.height = 1080;
        Clip c;
        c.id = state.ids.allocate<ClipId>();
        c.media = media->id;
        c.timelineRange = {Ticks{}, framesToTicks(120, kRate60)};
        c.setEffects(effects(state));
        Track t;
        t.id = state.ids.allocate<TrackId>();
        t.clips = ClipList({c});
        seq.videoTracks = {std::make_shared<const Track>(std::move(t))};
    }

    std::vector<LayerSpec> layers(std::int64_t frame = 10) const {
        std::vector<LayerSpec> out;
        evaluateVideoLayers(state, seq, framesToTicks(frame, kRate60), out);
        return out;
    }
};

/// An RGBA8 frame of a given size with real memory.
media::VideoFrame frameOf(int w, int h, std::int64_t ptsFrame = 10) {
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4, 128);
    media::VideoFrame f;
    f.pts = framesToTicks(ptsFrame, kRate60);
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

/// A processor that hands back a picture of a chosen size, or says it is missing.
class FakeProcessor final : public SourceFrameProcessor {
public:
    int outWidth = 0;
    int outHeight = 0;
    bool missing = false;
    int calls = 0;
    AiChain lastChain;
    Ticks lastDecodedPts{};
    SourceStageResult process(const SourceStageQuery& q) override {
        ++calls;
        lastChain = *q.chain;
        lastDecodedPts = q.decoded ? q.decoded->pts : Ticks{};
        SourceStageResult r;
        if (missing) {
            r.missing = true;
        } else {
            r.frame = frameOf(outWidth, outHeight);
        }
        return r;
    }
};

struct InstalledProcessor {
    explicit InstalledProcessor(std::shared_ptr<SourceFrameProcessor> p) { setSourceFrameProcessor(std::move(p)); }
    ~InstalledProcessor() { setSourceFrameProcessor(nullptr); }
};

} // namespace

TEST_CASE("A clip's AI effects become its AI chain, not compositor effects", "[playback][evaluate][ai]") {
    Fixture f(960, 540, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 0, 50, 0)}; });
    const auto layers = f.layers();
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].effects.empty());
    CHECK(layers[0].bypassedEffects == 0);
    REQUIRE(layers[0].ai.stages.size() == 1);
    CHECK(layers[0].ai.stages[0].effect == kAiRestoreEffect);
    CHECK(layers[0].ai.stages[0].scale == 2); // Auto: 540 lines on a 1080 line canvas
    CHECK(layers[0].ai.stages[0].deblur == 50);
    CHECK(layers[0].ai.amount == 1.0);
}

TEST_CASE("Auto scale does not upscale a picture that already fills the canvas (a 4K clip on a 1080p timeline)", "[playback][evaluate][ai]") {
    Fixture uhd(3840, 2160, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 0, 50, 0)}; });
    const auto layers = uhd.layers();
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].ai.stages.size() == 1);
    CHECK(layers[0].ai.stages[0].scale == 1); // deblur only
    // With deblur and denoise off as well there is nothing to do: no chain, no badge, no cost.
    Fixture nothing(3840, 2160, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 0, 0, 0)}; });
    CHECK(nothing.layers()[0].ai.empty());
    // A small clip is upscaled 4x when 2x does not reach the canvas.
    Fixture small(480, 270, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 0, 0, 0)}; });
    CHECK(small.layers()[0].ai.stages[0].scale == 4);
}

TEST_CASE("Disabled AI effects and Amount 0 leave no chain; Amount follows its keyframes", "[playback][evaluate][ai]") {
    Fixture off(960, 540, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 2, 50, 0, 100.0, false)}; });
    CHECK(off.layers()[0].ai.empty());
    Fixture zero(960, 540, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 2, 50, 0, 0.0)}; });
    CHECK(zero.layers()[0].ai.empty());
    Fixture half(960, 540, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 2, 50, 0, 50.0)}; });
    CHECK(half.layers()[0].ai.amount == Approx(0.5));

    Fixture keyed(960, 540, [](ProjectState& s) {
        EffectInstance e = restore(s, 2, 50, 0);
        Curve curve;
        curve.keys = {Keyframe{Ticks{}, 100.0}, Keyframe{framesToTicks(100, kRate60), 0.0}};
        e.set("amount", ParamValue(curve));
        return std::vector<EffectInstance>{e};
    });
    const double early = keyed.layers(0)[0].ai.amount;
    const double mid = keyed.layers(50)[0].ai.amount;
    CHECK(early == Approx(1.0));
    CHECK(mid > 0.3);
    CHECK(mid < 0.7);
    // The chain's identity ignores Amount: the same pictures serve every value.
    CHECK(aiChainHash(keyed.layers(0)[0].ai) == aiChainHash(keyed.layers(50)[0].ai));
}

TEST_CASE("The installed processor replaces the decoded picture and the layer is placed as the original would be", "[playback][evaluate][ai]") {
    Fixture f(960, 540, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 2, 50, 0)}; });
    auto processor = std::make_shared<FakeProcessor>();
    processor->outWidth = 1920;
    processor->outHeight = 1080;
    InstalledProcessor installed(processor);
    auto specs = f.layers();
    REQUIRE(specs.size() == 1);
    std::vector<media::VideoFrame> frames{frameOf(960, 540)};
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(specs, frames, out);
    REQUIRE(out.size() == 1);
    CHECK(processor->calls == 1);
    CHECK(processor->lastChain.stages.size() == 1);
    CHECK(processor->lastDecodedPts == framesToTicks(10, kRate60));
    CHECK(out[0].frame.width == 1920);
    CHECK(out[0].frame.height == 1080);
    CHECK_FALSE(out[0].aiUnprocessed);
    // The restored picture is twice as tall as the media: its scale halves so it covers the same canvas area.
    CHECK(out[0].transform.scale == Approx(specs[0].transform.scale * 0.5));
}

TEST_CASE("Without a processed picture the decoded one stays and the layer says so", "[playback][evaluate][ai]") {
    Fixture f(960, 540, [](ProjectState& s) { return std::vector<EffectInstance>{restore(s, 2, 50, 0)}; });
    SECTION("the processor has none for this frame") {
        auto processor = std::make_shared<FakeProcessor>();
        processor->missing = true;
        InstalledProcessor installed(processor);
        auto specs = f.layers();
        std::vector<media::VideoFrame> frames{frameOf(960, 540)};
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        CHECK(out[0].frame.width == 960);
        CHECK(out[0].transform.scale == Approx(specs[0].transform.scale));
    }
    SECTION("no processor is installed (a build without the AI helper)") {
        auto specs = f.layers();
        std::vector<media::VideoFrame> frames{frameOf(960, 540)};
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        CHECK(out[0].frame.width == 960);
    }
    SECTION("a clip without AI effects never asks and is never flagged") {
        Fixture plain(960, 540, [](ProjectState&) { return std::vector<EffectInstance>{}; });
        auto processor = std::make_shared<FakeProcessor>();
        InstalledProcessor installed(processor);
        auto specs = plain.layers();
        std::vector<media::VideoFrame> frames{frameOf(960, 540)};
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(specs, frames, out);
        REQUIRE(out.size() == 1);
        CHECK(processor->calls == 0);
        CHECK_FALSE(out[0].aiUnprocessed);
    }
}

TEST_CASE("Chains: the AI effects of a stack run in stack order, other effects do not interrupt them", "[playback][evaluate][ai]") {
    Fixture f(960, 540, [](ProjectState& s) {
        EffectInstance denoise;
        denoise.id = s.ids.allocate<EffectInstanceId>();
        denoise.effect = std::string(kAiDenoiseEffect);
        denoise.set("strength", ParamValue(60.0));
        EffectInstance blur;
        blur.id = s.ids.allocate<EffectInstanceId>();
        blur.effect = "wabcut.blur.gaussian";
        blur.set("radius", ParamValue(4.0));
        return std::vector<EffectInstance>{denoise, blur, restore(s, 2, 0, 0, 50.0)};
    });
    const auto layers = f.layers();
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].ai.stages.size() == 2);
    CHECK(layers[0].ai.stages[0].effect == kAiDenoiseEffect);
    CHECK(layers[0].ai.stages[0].denoise == 60);
    CHECK(layers[0].ai.stages[1].effect == kAiRestoreEffect);
    CHECK(layers[0].ai.amount == Approx(0.5)); // the product of the effects' Amounts
    REQUIRE(layers[0].effects.size() == 1);     // the blur only
    CHECK(layers[0].effects[0].type == render::EffectType::GaussianBlur);
    CHECK(aiChainLabel(layers[0].ai) == "Denoise, Restore 2x");
}

TEST_CASE("The scheduler counts the layers that show an unprocessed AI picture (the monitor's badge)", "[playback][scheduler][ai]") {
    namespace pt = wabcut::playback::test;
    constexpr const char* kClip = "h264_1080p60_aac.mp4";
    if (!pt::haveMedia(kClip)) {
        SKIP("test media missing");
    }
    const auto shownAiLayers = [&](bool withAiEffect) {
        pt::TestProject project = pt::makeProject({{kClip, 0, 0, 400, 100}});
        if (withAiEffect) {
            auto added = edit::addEffect(project.state, project.sequence, std::span(&project.clips.front(), 1), std::string(kAiRestoreEffect));
            REQUIRE(added.has_value());
            project.state = std::move(added->state);
        }
        EditSession session(project.state);
        PlaybackController controller;
        controller.setSession(&session);
        controller.setSequence(project.sequence);
        const Ticks target = framesToTicks(20, kRate60);
        controller.seek(target);
        const std::uint64_t epoch = controller.mailbox()->epoch();
        bool presented = false;
        pt::consume(*controller.mailbox(), std::chrono::milliseconds(5000), [&](const std::vector<pt::Presented>& seen) {
            presented = seen.back().epoch == epoch && seen.back().target == target;
            return presented;
        });
        REQUIRE(presented);
        return controller.stats().aiUnprocessedLayers;
    };
    auto processor = std::make_shared<FakeProcessor>();
    processor->missing = true; // no processed picture for any frame
    const InstalledProcessor installed(processor);
    CHECK(shownAiLayers(true) == 1);  // AI Restore at its defaults (deblur) on a 1080p clip on a 1080p timeline: nothing made yet
    CHECK(shownAiLayers(false) == 0); // a clip without AI effects never shows the badge
}
