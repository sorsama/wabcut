// Sequence evaluation (pure): visible layers, source times, motion, culling; the composite pool; the
// transports.

#include "PlaybackTestSupport.h"

#include <wabcut/playback/video/CompositePool.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/TransportClock.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using Catch::Approx;

namespace {

/// A project over synthetic media descriptions (no files are opened by evaluation).
struct FakeProject {
    ProjectState state;
    const Sequence* seq = nullptr;
};

std::shared_ptr<MediaItem> fakeMedia(ProjectState& s, int w, int h, AlphaMode alpha = AlphaMode::None,
                                     int rotation = 0) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = w;
    v.height = h;
    v.frameRate = kRate60;
    v.alpha = alpha;
    v.rotationDegrees = rotation;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

Clip clipOf(ProjectState& s, MediaId media, std::int64_t start, std::int64_t frames, std::int64_t sourceIn,
            ClipProperties props = {}) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {framesToTicks(start, kRate60), framesToTicks(frames, kRate60)};
    c.sourceIn = framesToTicks(sourceIn, kRate60);
    c.setProperties(props);
    return c;
}

TrackPtr trackOf(ProjectState& s, std::vector<Clip> clips, bool enabled = true) {
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.enabled = enabled;
    t.clips = ClipList(std::move(clips));
    return std::make_shared<const Track>(std::move(t));
}

} // namespace

TEST_CASE("A still image shows its one picture for the whole clip (RC.1)", "[playback][evaluate]") {
    // A still's clip runs 5 s by default over a media of one 40 ms frame: preview and export asked the decoder for
    // source time 1 s, 2 s, ... and exporting a video essay's B-roll still failed ("Seeking ... failed").
    ProjectState s;
    auto still = fakeMedia(s, 1920, 1080);
    still->info.isStillImage = true;
    still->info.video.front().duration = framesToTicks(1, kRate60);
    still->info.duration = still->info.video.front().duration;
    s.media = {still};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.videoTracks = {trackOf(s, {clipOf(s, still->id, 0, 300, 0)})};
    std::vector<LayerSpec> layers;
    for (const std::int64_t frame : {0, 59, 60, 299}) {
        evaluateVideoLayers(s, seq, framesToTicks(frame, kRate60), layers);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].sourceTime == Ticks{});
    }
}
TEST_CASE("Evaluation lists visible layers bottom to top with source times", "[playback][evaluate]") {
    ProjectState s;
    auto a = fakeMedia(s, 1920, 1080);
    auto b = fakeMedia(s, 1280, 720);
    s.media = {a, b};
    ClipProperties pip;
    pip.set(ParamId::Scale, 0.5);
    pip.set(ParamId::PositionX, 200);
    pip.set(ParamId::PositionY, -100);
    pip.set(ParamId::Rotation, 15);
    pip.set(ParamId::AnchorX, 10);
    pip.set(ParamId::AnchorY, 20);
    pip.set(ParamId::Opacity, 0.6);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.videoTracks = {trackOf(s, {clipOf(s, a->id, 0, 120, 30)}),
                       trackOf(s, {clipOf(s, b->id, 60, 60, 5, pip)})};
    std::vector<LayerSpec> layers;

    evaluateVideoLayers(s, seq, framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].sourceTime == framesToTicks(40, kRate60)); // sourceIn 30 + 10
    CHECK(layers[0].track == 0);

    evaluateVideoLayers(s, seq, framesToTicks(70, kRate60), layers);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].media == a->id); // V1 first
    CHECK(layers[1].media == b->id);
    CHECK(layers[1].sourceTime == framesToTicks(15, kRate60)); // sourceIn 5 + (70 - 60)
    CHECK(layers[1].transform.scale == 0.5);
    CHECK(layers[1].transform.position.x == 200);
    CHECK(layers[1].transform.rotationDegrees == 15);
    CHECK(layers[1].transform.anchor.y == 20);
    CHECK(layers[1].opacity == Approx(0.6));
    CHECK(layers[1].item == b.get());

    evaluateVideoLayers(s, seq, framesToTicks(130, kRate60), layers);
    CHECK(layers.empty()); // gap after both clips
}

TEST_CASE("Disabled tracks and clips and transparent layers are skipped", "[playback][evaluate]") {
    ProjectState s;
    auto a = fakeMedia(s, 640, 360);
    s.media = {a};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    Clip off = clipOf(s, a->id, 0, 60, 0);
    off.enabled = false;
    ClipProperties invisible;
    invisible.set(ParamId::Opacity, 0.0);
    seq.videoTracks = {trackOf(s, {clipOf(s, a->id, 0, 60, 0)}, false), trackOf(s, {off}),
                       trackOf(s, {clipOf(s, a->id, 0, 60, 0, invisible)}),
                       trackOf(s, {clipOf(s, a->id, 0, 60, 0)})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, Ticks{}, layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].track == 3);
    // Offline media and media missing from the project are skipped too.
    auto offline = std::make_shared<MediaItem>(*a);
    offline->locator.offline = true;
    ProjectState withOffline = s;
    withOffline.media = {offline};
    evaluateVideoLayers(withOffline, seq, Ticks{}, layers);
    CHECK(layers.empty());
    CHECK(sourcePathOf(*offline) == nullptr);
    REQUIRE(sourcePathOf(*a) != nullptr);
    CHECK(*sourcePathOf(*a) == "fake.mp4");
    auto proxied = std::make_shared<MediaItem>(*a);
    proxied->locator.proxy = ProxyMedia{"proxy.mp4", {}};
    CHECK(*sourcePathOf(*proxied, true) == "proxy.mp4");
    CHECK(*sourcePathOf(*proxied, false) == "fake.mp4");
    Sequence orphan;
    orphan.videoTracks = {trackOf(s, {clipOf(s, MediaId{999}, 0, 60, 0)})};
    evaluateVideoLayers(s, orphan, Ticks{}, layers);
    CHECK(layers.empty());
}

TEST_CASE("Opaque full-canvas layers occlude what is below", "[playback][evaluate]") {
    ProjectState s;
    auto full = fakeMedia(s, 1920, 1080);
    auto withAlpha = fakeMedia(s, 1920, 1080, AlphaMode::Straight);
    auto small = fakeMedia(s, 1280, 720);
    s.media = {full, withAlpha, small};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    std::vector<LayerSpec> layers;

    seq.videoTracks = {trackOf(s, {clipOf(s, small->id, 0, 60, 0)}),
                       trackOf(s, {clipOf(s, full->id, 0, 60, 0)})};
    evaluateVideoLayers(s, seq, Ticks{}, layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].media == full->id);
    evaluateVideoLayers(s, seq, Ticks{}, layers, false);
    CHECK(layers.size() == 2); // culling off

    seq.videoTracks = {trackOf(s, {clipOf(s, full->id, 0, 60, 0)}),
                       trackOf(s, {clipOf(s, withAlpha->id, 0, 60, 0)})};
    evaluateVideoLayers(s, seq, Ticks{}, layers);
    CHECK(layers.size() == 2); // alpha on top: both visible

    ClipProperties half;
    half.set(ParamId::Opacity, 0.5);
    seq.videoTracks = {trackOf(s, {clipOf(s, full->id, 0, 60, 0)}),
                       trackOf(s, {clipOf(s, full->id, 0, 60, 0, half)})};
    evaluateVideoLayers(s, seq, Ticks{}, layers);
    CHECK(layers.size() == 2);

    // A 720p frame scaled 1.5x exactly covers 1080p; rotated 180 degrees too; 45 degrees does not.
    CHECK(coversCanvas({{0, 0}, 1.5, 0.0, {0, 0}}, 1280, 720, 1.0, {1920, 1080}));
    CHECK(coversCanvas({{0, 0}, 1.5, 180.0, {0, 0}}, 1280, 720, 1.0, {1920, 1080}));
    CHECK_FALSE(coversCanvas({{0, 0}, 3.0, 45.0, {0, 0}}, 1280, 720, 1.0, {1920, 1080}));
    CHECK_FALSE(coversCanvas({{10, 0}, 1.5, 0.0, {0, 0}}, 1280, 720, 1.0, {1920, 1080}));
}

TEST_CASE("Colour mattes, black and transparent video are fills of their frame size, nothing decoded (C.15)",
          "[playback][evaluate]") {
    ProjectState s;
    auto made = syntheticMediaItem(SyntheticKind::ColorMatte, {{"color.r", 1.0}, {"color.g", 0.5}, {"color.b", 0.0}},
                                   SyntheticFormat{1280, 720, kRate60});
    REQUIRE(made);
    auto matte = std::make_shared<MediaItem>(std::move(*made));
    matte->id = s.ids.allocate<MediaId>();
    auto black = std::make_shared<MediaItem>(*syntheticMediaItem(SyntheticKind::BlackVideo, {}, SyntheticFormat{1280, 720, kRate60}));
    black->id = s.ids.allocate<MediaId>();
    auto clear = std::make_shared<MediaItem>(*syntheticMediaItem(SyntheticKind::TransparentVideo, {}, SyntheticFormat{1280, 720, kRate60}));
    clear->id = s.ids.allocate<MediaId>();
    s.media = {matte, black, clear};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    ClipProperties half;
    half.set(ParamId::Scale, 0.5);
    seq.videoTracks = {trackOf(s, {clipOf(s, black->id, 0, 60, 0)}), trackOf(s, {clipOf(s, matte->id, 0, 60, 0, half)}),
                       trackOf(s, {clipOf(s, clear->id, 0, 60, 0)})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 3);
    for (const LayerSpec& layer : layers) {
        CHECK_FALSE(needsDecode(layer));
        REQUIRE(layer.fill);
        CHECK(layer.fillSize == render::CanvasSize{1280, 720});
    }
    CHECK(*layers[0].fill == std::array<std::uint8_t, 4>{0, 0, 0, 255});
    CHECK(*layers[1].fill == std::array<std::uint8_t, 4>{255, 128, 0, 255});
    CHECK(layers[1].transform.scale == 0.5);
    CHECK(*layers[2].fill == std::array<std::uint8_t, 4>{0, 0, 0, 0});

    std::vector<media::VideoFrame> frames(layers.size());
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 3);
    REQUIRE(out[1].view);
    CHECK(out[1].view->width == 1280);
    CHECK(out[1].view->height == 720);
    const std::uint8_t* px = out[1].view->planes[0].data;
    CHECK((px[0] == 255 && px[1] == 128 && px[2] == 0 && px[3] == 255));
    CHECK(out[1].transform.scale == 0.5);
}

TEST_CASE("A Track Matte Key reads its matte track's clip as a hidden group, whatever that track's output (C.15, 0.1.2)",
          "[playback][evaluate]") {
    ProjectState s;
    const auto fill = [&](SyntheticKind kind, double r) {
        auto item = std::make_shared<MediaItem>(
            *syntheticMediaItem(kind, {{"color.r", r}, {"color.g", r}, {"color.b", r}}, SyntheticFormat{1280, 720, kRate60}));
        item->id = s.ids.allocate<MediaId>();
        return item;
    };
    auto white = fill(SyntheticKind::ColorMatte, 1.0);
    auto red = fill(SyntheticKind::ColorMatte, 0.3);
    s.media = {white, red};
    const auto withKey = [&](double matteTrack, double compositeUsing) {
        EffectInstance key;
        key.id = s.ids.allocate<EffectInstanceId>();
        key.effect = "wabcut.key.trackMatte";
        key.set("matteTrack", ParamValue(matteTrack));
        key.set("compositeUsing", ParamValue(compositeUsing));
        Clip keyed = clipOf(s, red->id, 0, 60, 0);
        keyed.setEffects({key});
        Sequence seq;
        seq.id = s.ids.allocate<SequenceId>();
        // V1 the matte (its output off: still read), V2 the keyed clip, V3 empty.
        seq.videoTracks = {trackOf(s, {clipOf(s, white->id, 0, 60, 0)}, false), trackOf(s, {keyed}), trackOf(s, {})};
        return seq;
    };
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, withKey(1.0, 1.0), framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 3); // the matte group with V1's clip inside, then V2; V1 itself is off
    CHECK(layers[0].matte);
    CHECK(layers[0].nested);
    CHECK(layers[0].nestedCount == 1);
    CHECK(layers[1].media == white->id);
    CHECK(layers[2].media == red->id);
    REQUIRE(layers[2].effects.size() == 1);
    CHECK(layers[2].effects[0].type == render::EffectType::TrackMatte);
    CHECK(layers[2].effects[0].params[1] == 1.0); // luma
    CHECK(layers[2].effects[0].params[3] == 1.0); // the matte track is valid
    // The composite: the matte is not a layer; the key's effect holds its composite.
    std::vector<media::VideoFrame> frames(layers.size());
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].effects.size() == 1);
    REQUIRE(out[0].effects[0].matte);
    CHECK(out[0].effects[0].matte->layers.size() == 1);

    // No such track, or the layer's own: the key shows nothing (valid 0) and reads no matte.
    for (const double track : {9.0, 2.0}) {
        evaluateVideoLayers(s, withKey(track, 0.0), framesToTicks(10, kRate60), layers);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].effects[0].params[3] == 0.0);
    }
    // An empty matte track (V3): valid, nothing to read - an empty matte.
    evaluateVideoLayers(s, withKey(3.0, 0.0), framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].effects[0].params[3] == 1.0);
    out.clear(); // buildCompositeLayers appends
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].effects[0].matte);
}

TEST_CASE("An adjustment layer is a group over the tracks below it, carrying its effects (C.15)", "[playback][evaluate]") {
    ProjectState s;
    auto full = fakeMedia(s, 1920, 1080);
    auto small = fakeMedia(s, 640, 360);
    auto adjustment = std::make_shared<MediaItem>(*adjustmentMediaItem(SyntheticFormat{1920, 1080, kRate60}));
    adjustment->id = s.ids.allocate<MediaId>();
    s.media = {full, small, adjustment};
    EffectInstance blur;
    blur.id = s.ids.allocate<EffectInstanceId>();
    blur.effect = "wabcut.blur.gaussian";
    blur.set("radius", ParamValue(20.0));
    const auto withAdjustment = [&](double opacity) {
        Clip adjust = clipOf(s, adjustment->id, 0, 30, 0); // frames 0..30 only
        adjust.setEffects({blur});
        ClipProperties props;
        props.set(ParamId::Opacity, opacity);
        adjust.setProperties(props);
        Sequence seq;
        seq.id = s.ids.allocate<SequenceId>();
        seq.videoTracks = {trackOf(s, {clipOf(s, full->id, 0, 60, 0)}), trackOf(s, {adjust}),
                           trackOf(s, {clipOf(s, small->id, 0, 60, 0)})};
        return seq;
    };
    std::vector<LayerSpec> layers;
    const Sequence seq = withAdjustment(1.0);
    evaluateVideoLayers(s, seq, framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 3); // the group (V1 inside), then V3 above it - not blurred
    CHECK(layers[0].nested);
    CHECK(layers[0].nestedCount == 1);
    CHECK(layers[0].nestedCanvas == render::CanvasSize{1920, 1080});
    REQUIRE(layers[0].effects.size() == 1);
    CHECK(layers[0].effects[0].type == render::EffectType::GaussianBlur);
    CHECK(layers[0].track == 1);
    CHECK(layers[1].media == full->id);
    CHECK(layers[2].media == small->id);
    CHECK_FALSE(layers[2].nested);

    evaluateVideoLayers(s, seq, framesToTicks(40, kRate60), layers); // past the adjustment layer: no group
    REQUIRE(layers.size() == 2);
    CHECK_FALSE(layers[0].nested);

    // At half opacity the tracks below show as they are, the adjusted copy mixed over them by half.
    evaluateVideoLayers(s, withAdjustment(0.5), framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 4);
    CHECK(layers[0].media == full->id);
    CHECK(layers[1].nested);
    CHECK(layers[1].opacity == 0.5);
    CHECK(layers[2].media == full->id);
    CHECK(layers[3].media == small->id);
}

TEST_CASE("A clip's blend mode reaches its layer; a blended full-canvas layer occludes nothing (C.15)", "[playback][evaluate]") {
    ProjectState s;
    auto full = fakeMedia(s, 1920, 1080);
    s.media = {full};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    Clip screen = clipOf(s, full->id, 0, 60, 0);
    screen.setBlendMode(BlendMode::Screen);
    seq.videoTracks = {trackOf(s, {clipOf(s, full->id, 0, 60, 0)}), trackOf(s, {screen})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, Ticks{}, layers);
    REQUIRE(layers.size() == 2); // Screen over V1 shows V1 through it
    CHECK(layers[0].blend == BlendMode::Normal);
    CHECK(layers[1].blend == BlendMode::Screen);
}

TEST_CASE("Stream display rotation is applied before the clip's motion", "[playback][evaluate]") {
    ProjectState s;
    auto rotated = fakeMedia(s, 640, 360, AlphaMode::None, 90);
    s.media = {rotated};
    ClipProperties p;
    p.set(ParamId::Rotation, 10.0);
    p.set(ParamId::AnchorX, 5); // displayed pixels
    p.set(ParamId::AnchorY, 0);
    Sequence seq;
    seq.videoTracks = {trackOf(s, {clipOf(s, rotated->id, 0, 60, 0, p)})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, Ticks{}, layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].transform.rotationDegrees == Approx(100.0));
    // Anchor (5, 0) displayed = (0, -5) stored for a 90-degree clockwise display rotation.
    CHECK(layers[0].transform.anchor.x == Approx(0.0));
    CHECK(layers[0].transform.anchor.y == Approx(-5.0));
}

TEST_CASE("Composite pool reuses composites once released", "[playback][pool]") {
    CompositePool pool(2, 4);
    auto a = pool.acquire();
    auto b = pool.acquire();
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a != b);
    CHECK(pool.inUse() == 2);
    CHECK(pool.acquire() == nullptr); // both still referenced
    a->targetTime = Ticks{42};
    a->layers.resize(3);
    const auto* raw = a.get();
    a.reset();
    auto again = pool.acquire();
    REQUIRE(again);
    CHECK(again.get() == raw);
    CHECK(again->targetTime == Ticks{});
    CHECK(again->layers.empty());
    CHECK(again->layers.capacity() >= 3); // storage kept
}

TEST_CASE("Free-run transport: exact integer time under play, speed, pause and seek",
          "[playback][transport]") {
    auto t = makeFreeRunTransport(Ticks::fromSeconds(2));
    const std::int64_t now = steadyNowNs();
    CHECK(t->transportTime(now) == Ticks::fromSeconds(2));
    CHECK_FALSE(t->isPlaying());
    t->play();
    const Ticks a = t->transportTime(now + 1'000'000'000);
    const Ticks b = t->transportTime(now + 2'000'000'000);
    CHECK(b - a == Ticks::fromSeconds(1)); // 1x: one second of timeline per second
    t->setSpeed({2, 1});
    const std::int64_t later = steadyNowNs();
    CHECK(t->transportTime(later + 1'500'000'000) - t->transportTime(later + 500'000'000) ==
          Ticks::fromSeconds(2));
    t->setSpeed({-1, 4});
    CHECK(t->effectiveSpeed() == TransportSpeed{-1, 4});
    const std::int64_t r = steadyNowNs();
    CHECK(t->transportTime(r) - t->transportTime(r + 4'000'000'000) == Ticks::fromSeconds(1));
    t->pause();
    CHECK(t->effectiveSpeed().isZero());
    const Ticks held = t->transportTime(steadyNowNs());
    CHECK(t->transportTime(steadyNowNs() + 5'000'000'000) == held);
    t->seek(Ticks::fromSeconds(7));
    CHECK(t->transportTime(steadyNowNs()) == Ticks::fromSeconds(7));
    // Reverse play never goes below zero.
    t->seek(Ticks{});
    t->setSpeed({-8, 1});
    t->play();
    CHECK(t->transportTime(steadyNowNs() + 1'000'000'000) == Ticks{});
    // Zero/invalid speeds are ignored.
    t->setSpeed({0, 1});
    t->setSpeed({1, 0});
    CHECK(t->effectiveSpeed() == TransportSpeed{-8, 1});
}

TEST_CASE("An Ultra Key reaches the compositor with its 20 parameters in descriptor order (0.1.1)", "[playback][evaluate]") {
    ProjectState s;
    auto media = fakeMedia(s, 1920, 1080);
    s.media = {media};
    EffectInstance key;
    key.id = s.ids.allocate<EffectInstanceId>();
    key.effect = "wabcut.key.ultra";
    key.set("keyColor.g", ParamValue(0.7));
    key.set("choke", ParamValue(12.0));
    key.set("luminance", ParamValue(140.0));
    Clip clip = clipOf(s, media->id, 0, 60, 0);
    clip.setEffects({key});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.videoTracks = {trackOf(s, {clip})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, framesToTicks(10, kRate60), layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].effects.size() == 1);
    const render::LayerEffect& e = layers[0].effects[0];
    CHECK(e.type == render::EffectType::UltraKey);
    CHECK(e.params[0] == 0.0);    // output: composite (the default)
    CHECK(e.params[1] == 0.0);    // key colour r, g, b
    CHECK(e.params[2] == 0.7);
    CHECK(e.params[3] == 0.0);
    CHECK(e.params[4] == 45.0);   // transparency (default)
    CHECK(e.params[8] == 10.0);   // pedestal (default)
    CHECK(e.params[9] == 12.0);   // choke
    CHECK(e.params[12] == 50.0);  // mid point (default)
    CHECK(e.params[17] == 100.0); // saturation (default)
    CHECK(e.params[19] == 140.0); // luminance
    CHECK(layers[0].bypassedEffects == 0);
}
