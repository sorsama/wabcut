// 0.2: the evaluator's mapping of Premiere's transition library (EffectsLibrary.cpp descriptors) to the compositor's
// transitions (render::LayerTransition): every registered video transition is implemented, Start and End remap the
// progress, options and colours arrive in the compositor's units. And the effects the evaluator does more for than
// read parameters: Posterize Time, Echo, the Transform effect's shutter, the text effects, and the effects that read
// another track (Gradient Wipe, Difference Matte).

#include <wabcut/model/Animations.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Synthetic.h>
#include <wabcut/model/Track.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/EffectLibrary.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

Transition transitionOf(std::string effect, std::vector<EffectParam> params = {}) {
    Transition t;
    t.id = TransitionId{0x1234567};
    t.effect = std::move(effect);
    t.params = std::move(params);
    return t;
}

/// A media item over a file that is never opened (evaluation only describes layers).
std::shared_ptr<MediaItem> fakeVideo(ProjectState& s, std::string name) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->name = name;
    item->locator.path = "C:/media/" + name + ".mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = 1920;
    v.height = 1080;
    v.frameRate = kRate60;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

std::shared_ptr<MediaItem> fillOf(ProjectState& s, double gray) {
    auto item = std::make_shared<MediaItem>(*syntheticMediaItem(
        SyntheticKind::ColorMatte, {{"color.r", gray}, {"color.g", gray}, {"color.b", gray}}, SyntheticFormat{1920, 1080, kRate60}));
    item->id = s.ids.allocate<MediaId>();
    return item;
}

Clip clipAt(ProjectState& s, MediaId media, std::int64_t start, std::int64_t frames, std::int64_t sourceIn = 0) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {framesToTicks(start, kRate60), framesToTicks(frames, kRate60)};
    c.sourceIn = framesToTicks(sourceIn, kRate60);
    return c;
}

TrackPtr trackWith(ProjectState& s, std::vector<Clip> clips, bool enabled = true, std::vector<Transition> transitions = {}) {
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.enabled = enabled;
    t.clips = ClipList(std::move(clips));
    t.transitions = std::move(transitions);
    return std::make_shared<const Track>(std::move(t));
}

EffectInstance effectOf(ProjectState& s, std::string id, std::vector<std::pair<std::string, ParamValue>> params = {}) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    for (auto& [key, value] : params) {
        e.set(key, std::move(value));
    }
    return e;
}

Sequence sequenceOf(ProjectState& s, std::vector<TrackPtr> tracks) {
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.width = 1920;
    seq.height = 1080;
    seq.videoTracks = std::move(tracks);
    return seq;
}

Ticks frame(std::int64_t f) {
    return framesToTicks(f, kRate60);
}

} // namespace

TEST_CASE("Transition library: every video transition of the registry is implemented (0.2)", "[playback][evaluate][library]") {
    int count = 0;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind != EffectKind::VideoTransition) {
            continue;
        }
        INFO(d.id);
        ++count;
        if (isAnimationTransition(d.id)) {
            // 0.2 Animations move the layer itself (transitionAnimationAt), they mix nothing.
            CHECK_FALSE(playback::transitionMixAt(transitionOf(std::string(d.id)), 0.25));
            CHECK(playback::transitionAnimationAt(transitionOf(std::string(d.id)), 0.25, false, {1920, 1080}, {1920, 1080}));
            continue;
        }
        const auto mix = playback::transitionMixAt(transitionOf(std::string(d.id)), 0.25);
        REQUIRE(mix);
        CHECK(mix->progress == Approx(0.25));
    }
    CHECK(count >= 51);
    CHECK_FALSE(playback::transitionMixAt(transitionOf("vendor.unknown"), 0.5));
    CHECK_FALSE(playback::transitionMixAt(transitionOf("constantPower"), 0.5)); // an audio transition
}

TEST_CASE("Transition library: options, Start and End reach the compositor (0.2)", "[playback][evaluate][library]") {
    SECTION("Start and End remap the progress") {
        const auto mix = playback::transitionMixAt(
            transitionOf("irisRound", {{"end", ParamValue(0.8)}, {"start", ParamValue(0.2)}}), 0.5);
        REQUIRE(mix);
        CHECK(mix->kind == render::TransitionKind::IrisRound);
        CHECK(mix->progress == Approx(0.5));
        const auto quarter = playback::transitionMixAt(
            transitionOf("irisRound", {{"end", ParamValue(0.8)}, {"start", ParamValue(0.2)}}), 0.25);
        CHECK(quarter->progress == Approx(0.35));
    }
    SECTION("border, reverse, anti-aliasing, centre and direction") {
        const auto mix = playback::transitionMixAt(
            transitionOf("irisBox", {{"antiAliasing", ParamValue(2.0)},
                                     {"borderColor.b", ParamValue(0.0)},
                                     {"borderColor.g", ParamValue(1.0)},
                                     {"borderColor.r", ParamValue(1.0)},
                                     {"borderWidth", ParamValue(8.0)},
                                     {"center.x", ParamValue(0.25)},
                                     {"reverse", ParamValue(1.0)}}),
            0.4);
        REQUIRE(mix);
        CHECK(mix->borderWidth == 8.0);
        CHECK(mix->borderColor[0] == Approx(1.0)); // sRGB 1 is linear 1
        CHECK(mix->borderColor[2] == 0.0);
        CHECK(mix->reverse);
        CHECK(mix->antiAliasing == 2);
        CHECK(mix->center[0] == 0.25);
        CHECK(mix->center[1] == 0.5); // the default
        CHECK(mix->seed == 0x234567u); // the id's low 24 bits (exact in the shader's float)
    }
    SECTION("custom settings by kind") {
        CHECK(playback::transitionMixAt(transitionOf("venetianBlinds"), 0.5)->custom[0] == 6.0);
        CHECK(playback::transitionMixAt(transitionOf("randomBlocks"), 0.5)->custom[1] == 20.0);
        CHECK(playback::transitionMixAt(transitionOf("randomWipe"), 0.5)->direction == 1); // top to bottom
        CHECK(playback::transitionMixAt(transitionOf("bandWipe"), 0.5)->custom[0] == 7.0);
        const auto checker = playback::transitionMixAt(transitionOf("checkerWipe", {{"cells.y", ParamValue(3.0)}}), 0.5);
        CHECK(checker->custom[0] == 8.0);
        CHECK(checker->custom[1] == 3.0);
        CHECK(playback::transitionMixAt(transitionOf("pinwheel"), 0.5)->custom[0] == 8.0);
        const auto gradient = playback::transitionMixAt(transitionOf("gradientWipe", {{"gradient", ParamValue(2.0)}}), 0.5);
        CHECK(gradient->custom[0] == 2.0);
        CHECK(gradient->custom[1] == 10.0); // Softness
        const auto glitch = playback::transitionMixAt(transitionOf("glitch"), 0.5);
        CHECK(glitch->kind == render::TransitionKind::Glitch);
        CHECK(glitch->custom[0] == 50.0);
        CHECK(glitch->custom[1] == 12.0);
        CHECK(playback::transitionMixAt(transitionOf("push", {{"direction", ParamValue(3.0)}}), 0.5)->direction == 3);
    }
    SECTION("dip colours are linear light; Film Dissolve is the linear dissolve") {
        const auto dip = playback::transitionMixAt(
            transitionOf("dipToColor", {{"color.b", ParamValue(0.0)}, {"color.g", ParamValue(0.5)}, {"color.r", ParamValue(1.0)}}),
            0.5);
        REQUIRE(dip);
        CHECK(dip->kind == render::TransitionKind::DipToColor);
        CHECK(dip->color[0] == Approx(1.0));
        CHECK(dip->color[1] == Approx(0.214041).margin(1e-5)); // sRGB 0.5
        CHECK(playback::transitionMixAt(transitionOf("dipToWhite"), 0.5)->color[1] == 1.0);
        CHECK(playback::transitionMixAt(transitionOf("dipToBlack"), 0.5)->color[1] == 0.0);
        CHECK(playback::transitionMixAt(transitionOf("filmDissolve"), 0.5)->kind == render::TransitionKind::CrossDissolve);
        const auto flash = playback::transitionMixAt(transitionOf("flash"), 0.5);
        CHECK(flash->color[0] == Approx(1.0));
        CHECK(flash->custom[0] == 1.0); // Intensity 100 %
    }
}

TEST_CASE("Transition library: choices must be one of their options (0.2)", "[playback][evaluate][library]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find("irisRound");
    REQUIRE(d);
    const EffectParamInfo* aa = d->param("antiAliasing");
    REQUIRE(aa);
    CHECK(aa->isChoice());
    CHECK(aa->optionLabels().size() == 4);
    CHECK(aa->optionLabel(0.0) == "Off");
    CHECK(aa->optionLabel(3.0) == "High");
    CHECK(aa->optionLabel(1.5).empty());
    CHECK(aa->optionLabel(4.0).empty());
}

TEST_CASE("Effect library: Posterize Time holds the clip on its own frame rate (0.2)", "[playback][evaluate][library]") {
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 30, 120, 100);
    clip.setEffects({effectOf(s, "wabcut.time.posterizeTime", {{"frameRate", ParamValue(12.0)}})});
    const Sequence seq = sequenceOf(s, {trackWith(s, {clip})});
    std::vector<LayerSpec> layers;
    // 12 fps in a 60 fps sequence: each picture held for 5 frames, counted from the clip's start (frame 30).
    const std::vector<std::pair<std::int64_t, std::int64_t>> held = {{30, 100}, {34, 100}, {35, 105}, {39, 105}, {41, 110}};
    for (const auto& [at, source] : held) {
        INFO("frame " << at);
        evaluateVideoLayers(s, seq, frame(at), layers);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].sourceTime == frame(source));
        CHECK(layers[0].effects.empty()); // not a pass
        CHECK(layers[0].bypassedEffects == 0);
    }
}

TEST_CASE("Effect library: Echo puts the clip's earlier frames in a hidden group the effect reads (0.2)",
          "[playback][evaluate][library]") {
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    const auto withEcho = [&](double op, double decay) {
        Clip clip = clipAt(s, media->id, 0, 120);
        clip.setEffects({effectOf(s, "wabcut.time.echo",
                                  {{"count", ParamValue(3.0)},
                                   {"decay", ParamValue(decay)},
                                   {"echoTime", ParamValue(-0.05)}, // 3 frames at 60 fps
                                   {"operator", ParamValue(op)}})});
        return sequenceOf(s, {trackWith(s, {clip})});
    };
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, withEcho(0.0, 0.5), frame(60), layers);
    REQUIRE(layers.size() == 5); // the echo group, its 3 echoes (farthest first), the clip
    CHECK(layers[0].matte);
    CHECK(layers[0].nested);
    CHECK(layers[0].matteEffect == 0);
    CHECK(layers[0].nestedCount == 3);
    std::set<std::uint64_t> decoders{sessionKey(layers[4])};
    for (int k = 0; k < 3; ++k) {
        const LayerSpec& echo = layers[static_cast<std::size_t>(1 + k)];
        INFO("echo " << 3 - k);
        CHECK(echo.sourceTime == frame(60 - 3 * (3 - k)));
        CHECK(echo.opacity == Approx(std::pow(0.5, 3 - k)));
        CHECK(echo.blend == BlendMode::Add);
        CHECK(echo.effects.empty());
        decoders.insert(sessionKey(echo));
    }
    CHECK(decoders.size() == 4); // every picture reads its own decoder
    CHECK(layers[4].sourceTime == frame(60));
    REQUIRE(layers[4].effects.size() == 1);
    CHECK(layers[4].effects[0].type == render::EffectType::Library);
    CHECK(layers[4].effects[0].op == static_cast<std::uint16_t>(render::LibraryOp::Echo));

    // Near the clip's start only the echoes inside it exist; Blend weighs every picture over their total.
    evaluateVideoLayers(s, withEcho(6.0, 1.0), frame(4), layers);
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].nestedCount == 1);
    CHECK(layers[1].sourceTime == frame(1));
    CHECK(layers[1].opacity == Approx(0.25));
    CHECK(layers[2].effects[0].params[2] == Approx(0.25)); // the current frame's share
    // Maximum and Minimum are Lighten and Darken inside the group.
    evaluateVideoLayers(s, withEcho(1.0, 1.0), frame(60), layers);
    CHECK(layers[1].blend == BlendMode::Lighten);
    evaluateVideoLayers(s, withEcho(2.0, 1.0), frame(60), layers);
    CHECK(layers[1].blend == BlendMode::Darken);
}

TEST_CASE("Effect library: an echo group becomes the Echo effect's second picture (0.2)", "[playback][evaluate][library]") {
    ProjectState s;
    auto gray = fillOf(s, 0.5);
    s.media = {gray};
    Clip clip = clipAt(s, gray->id, 0, 120);
    clip.setEffects({effectOf(s, "wabcut.color.brightnessContrast"),
                     effectOf(s, "wabcut.time.echo", {{"count", ParamValue(2.0)}})});
    const Sequence seq = sequenceOf(s, {trackWith(s, {clip})});
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, frame(30), layers);
    REQUIRE(layers.size() == 4);
    CHECK(layers[0].matteEffect == 1);
    std::vector<media::VideoFrame> frames(layers.size());
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].effects.size() == 2);
    CHECK_FALSE(out[0].effects[0].matte);
    REQUIRE(out[0].effects[1].matte);
    CHECK(out[0].effects[1].matte->layers.size() == 2);
}

TEST_CASE("Effect library: Gradient Wipe and Difference Matte read another track's clip (0.2)", "[playback][evaluate][library]") {
    ProjectState s;
    auto white = fillOf(s, 1.0);
    auto red = fillOf(s, 0.3);
    s.media = {white, red};
    const auto keyed = [&](double layer) {
        Clip clip = clipAt(s, red->id, 0, 60);
        clip.setEffects({effectOf(s, "wabcut.transition.gradientWipe",
                                  {{"completion", ParamValue(0.5)}, {"gradientLayer", ParamValue(layer)}}),
                         effectOf(s, "wabcut.key.differenceMatte", {{"differenceLayer", ParamValue(layer)}})});
        // V1 the other layer (its output off: still read), V2 the clip.
        return sequenceOf(s, {trackWith(s, {clipAt(s, white->id, 0, 60)}, false), trackWith(s, {clip})});
    };
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, keyed(1.0), frame(10), layers);
    REQUIRE(layers.size() == 5); // a group for each effect, then the clip
    CHECK(layers[0].matte);
    CHECK(layers[0].matteEffect == 0);
    CHECK(layers[1].media == white->id);
    CHECK(layers[2].matte);
    CHECK(layers[2].matteEffect == 1);
    CHECK(layers[4].media == red->id);
    std::vector<media::VideoFrame> frames(layers.size());
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].effects.size() == 2);
    CHECK(out[0].effects[0].matte);
    CHECK(out[0].effects[1].matte);
    // The clip's own track, no such track or none at all: nothing to read (Gradient Wipe uses the clip's luminance).
    for (const double layer : {2.0, 9.0, 0.0}) {
        evaluateVideoLayers(s, keyed(layer), frame(10), layers);
        CHECK(layers.size() == 1);
    }
}

TEST_CASE("Effect library: Timecode Clip Name and Simple Text draw their text on the canvas (0.2)",
          "[playback][evaluate][library]") {
    ProjectState s;
    auto media = fakeVideo(s, "interview");
    media->info.startTimecode = Ticks::fromSeconds(3600);
    s.media = {media};
    const auto with = [&](EffectInstance effect) {
        Clip clip = clipAt(s, media->id, 0, 600);
        clip.setName("Interview A");
        clip.setEffects({std::move(effect)});
        return sequenceOf(s, {trackWith(s, {clip})});
    };
    std::vector<LayerSpec> layers;
    const auto secondAt = [&](const Sequence& seq, std::int64_t at) -> std::shared_ptr<const render::PreparedComposite> {
        evaluateVideoLayers(s, seq, frame(at), layers);
        REQUIRE(layers.size() == 1);
        REQUIRE(layers[0].effects.size() == 1);
        return layers[0].effects[0].matte;
    };
    const Sequence timecode = with(effectOf(s, "wabcut.video.timecode"));
    const auto first = secondAt(timecode, 10);
    REQUIRE(first);
    CHECK(first->canvas == render::CanvasSize{1920, 1080});
    REQUIRE(first->layers.size() == 1);
    REQUIRE(first->layers[0].view);
    CHECK(first->layers[0].view->width == 1920);
    const auto same = secondAt(timecode, 10);
    const auto next = secondAt(timecode, 11);
    REQUIRE(same);
    REQUIRE(next);
    CHECK(same->layers[0].view->planes[0].data == first->layers[0].view->planes[0].data); // the same text, kept
    CHECK(next->layers[0].view->planes[0].data != first->layers[0].view->planes[0].data); // the next frame's timecode

    CHECK(secondAt(with(effectOf(s, "wabcut.video.clipName")), 10));
    EffectInstance simple = effectOf(s, "wabcut.video.simpleText", {{"background", ParamValue(0.5)}});
    simple.setData(EffectData{"text", std::string("Chapter 2: The Fall")});
    const auto custom = secondAt(with(simple), 10);
    const auto standard = secondAt(with(effectOf(s, "wabcut.video.simpleText")), 10);
    REQUIRE(custom);
    REQUIRE(standard); // "Simple Text" when no text is set
    CHECK(custom->layers[0].view->planes[0].data != standard->layers[0].view->planes[0].data);
    // Image Matte Key without an image, or with one that is not in the project: no second picture.
    CHECK_FALSE(secondAt(with(effectOf(s, "wabcut.key.imageMatte")), 10));
    EffectInstance missing = effectOf(s, "wabcut.key.imageMatte");
    missing.setData(EffectData{"image", std::string("media:999")});
    CHECK_FALSE(secondAt(with(missing), 10));
}

TEST_CASE("Effect library: the Transform effect's shutter reads the transform later in the frame (0.2)",
          "[playback][evaluate][library]") {
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    const auto with = [&](double useComposition, double shutter) {
        Clip clip = clipAt(s, media->id, 0, 120);
        clip.setEffects({effectOf(s, "wabcut.distort.transform",
                                  {{"rotation", ParamValue(Curve{{Keyframe{Ticks{}, 0.0}, Keyframe{Ticks::fromSeconds(1), 60.0}}})},
                                   {"shutterAngle", ParamValue(shutter)},
                                   {"useCompositionShutter", ParamValue(useComposition)}})});
        return sequenceOf(s, {trackWith(s, {clip})});
    };
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, with(0.0, 180.0), frame(30), layers);
    REQUIRE(layers.size() == 1);
    const auto& q = layers[0].effects.at(0).params;
    CHECK(q[9] == Approx(30.0));  // the rotation now
    CHECK(q[22] == Approx(30.5)); // and half a frame later
    CHECK(q[23] == 1.0);
    CHECK(q[16] == Approx(q[2])); // the position, not animated
    evaluateVideoLayers(s, with(1.0, 180.0), frame(30), layers);
    CHECK(layers[0].effects.at(0).params[23] == 0.0); // the composition's shutter: none in a sequence
    evaluateVideoLayers(s, with(0.0, 0.0), frame(30), layers);
    CHECK(layers[0].effects.at(0).params[23] == 0.0);
}

namespace {

Transition animation(ProjectState& s, std::string effect, ClipId outgoing, ClipId incoming, std::int64_t before,
                     std::int64_t after, std::vector<EffectParam> params = {}) {
    Transition t;
    t.id = s.ids.allocate<TransitionId>();
    t.effect = std::move(effect);
    t.outgoing = outgoing;
    t.incoming = incoming;
    t.before = frame(before);
    t.after = frame(after);
    t.params = std::move(params);
    return t;
}

double easeOut(double u) { return 1.0 - std::pow(1.0 - u, 3.0); }

/// Where Basic 3D then Transform (the Fold animation's effects, as render/reference LibraryDistort.cpp maps them) read a
/// screen point of a w x h frame from: the frame point drawn there.
render::Vec2 foldSource(const std::vector<render::LayerEffect>& fx, double w, double h, render::Vec2 screen) {
    REQUIRE(fx.size() == 2);
    const auto& t = fx[1].params; // Transform: anchor, position (fractions), scale 100 %
    const render::Vec2 f{screen.x - (t[2] - t[0]) * w, screen.y - (t[3] - t[1]) * h};
    const double focal = 1.6 * std::max(w, h);
    const double sw = fx[0].params[0] * std::numbers::pi / 180.0;
    const double ti = fx[0].params[1] * std::numbers::pi / 180.0;
    const double ex[3] = {std::cos(sw), 0.0, -std::sin(sw)};
    const double ey[3] = {std::sin(ti) * std::sin(sw), std::cos(ti), std::sin(ti) * std::cos(sw)};
    const double n[3] = {ex[1] * ey[2] - ex[2] * ey[1], ex[2] * ey[0] - ex[0] * ey[2], ex[0] * ey[1] - ex[1] * ey[0]};
    const double d[3] = {f.x - w / 2, f.y - h / 2, focal};
    const double k = focal * n[2] / (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]); // ray from (0, 0, -focal) to the plane z.n = 0
    const double x[3] = {d[0] * k, d[1] * k, -focal + d[2] * k};
    return {w / 2 + x[0] * ex[0] + x[1] * ex[1] + x[2] * ex[2], h / 2 + x[0] * ey[0] + x[1] * ey[1] + x[2] * ey[2]};
}

} // namespace

TEST_CASE("Animations: a clip animates in at its In point and out at its Out point; Start and End apply (0.2)",
          "[playback][evaluate][library][animations]") {
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 0, 120);
    const Transition pop = animation(s, "animationPop", ClipId{}, clip.id, 0, 30);
    const Transition travel = animation(s, "animationTravel", clip.id, ClipId{}, 30, 0, {{"side", ParamValue(1.0)}});
    const Sequence seq = sequenceOf(s, {trackWith(s, {clip}, true, {pop, travel})});
    std::vector<LayerSpec> layers;

    evaluateVideoLayers(s, seq, frame(0), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].transform.scale == Approx(0.0));
    CHECK(layers[0].opacity == Approx(0.0));
    CHECK_FALSE(layers[0].transition); // nothing is mixed
    evaluateVideoLayers(s, seq, frame(15), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].transform.scale == Approx(1.1 * easeOut(0.5 / 0.64))); // on the way to its overshoot
    CHECK(layers[0].opacity == Approx(1.0));
    evaluateVideoLayers(s, seq, frame(60), layers);
    CHECK(layers[0].transform.scale == Approx(1.0));
    CHECK(layers[0].transform.position.x == Approx(0.0));
    // Travel out to the right over the last 30 frames: half way at frame 105 (the In animation backwards).
    evaluateVideoLayers(s, seq, frame(105), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].transform.position.x == Approx(1920.0 * (1.0 - easeOut(0.5))));
    CHECK(layers[0].transform.position.y == Approx(0.0));
    CHECK(layers[0].transform.scale == Approx(1.0));

    // Start and End: Start 50 % begins half way in.
    Transition fade = pop;
    fade.effect = "animationFade";
    fade.params = {{"start", ParamValue(0.5)}};
    const Sequence faded = sequenceOf(s, {trackWith(s, {clip}, true, {fade})});
    evaluateVideoLayers(s, faded, frame(0), layers);
    CHECK(layers[0].opacity == Approx(0.5));

    // Spin turns on the way out the way it came in; Counterclockwise the other way.
    const Transition spinIn = animation(s, "animationSpin", ClipId{}, clip.id, 0, 30);
    CHECK(playback::transitionAnimationAt(spinIn, 0.0, false, {1920, 1080}, {1920, 1080})->rotationDegrees == Approx(-180.0));
    CHECK(playback::transitionAnimationAt(spinIn, 1.0, true, {1920, 1080}, {1920, 1080})->rotationDegrees == Approx(180.0));
    Transition ccw = spinIn;
    ccw.params = {{"turn", ParamValue(1.0)}};
    CHECK(playback::transitionAnimationAt(ccw, 0.0, false, {1920, 1080}, {1920, 1080})->rotationDegrees == Approx(180.0));
    // Spring rises from below the frame.
    const Transition spring = animation(s, "animationSpring", ClipId{}, clip.id, 0, 30);
    CHECK(playback::transitionAnimationAt(spring, 0.0, false, {1920, 1080}, {1920, 1080})->offset.y == Approx(1080.0));
    CHECK(playback::transitionAnimationAt(spring, 1.0, false, {1920, 1080}, {1920, 1080})->offset.y == Approx(0.0));
}

TEST_CASE("Animations: at an edit point the incoming clip animates in over the outgoing one (0.2)",
          "[playback][evaluate][library][animations]") {
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip a = clipAt(s, media->id, 0, 60);
    Clip b = clipAt(s, media->id, 60, 60, 300);
    const Transition flip = animation(s, "animationFlip", a.id, b.id, 15, 15);
    const Sequence seq = sequenceOf(s, {trackWith(s, {a, b}, true, {flip})});
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, frame(60), layers);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].clip == a.id);
    CHECK(layers[0].effects.empty());
    CHECK(layers[0].opacity == 1.0);
    CHECK(layers[1].clip == b.id);
    REQUIRE(layers[1].effects.size() == 1);
    CHECK(layers[1].effects[0].type == render::EffectType::Library);
    CHECK(layers[1].effects[0].op == static_cast<std::uint16_t>(render::LibraryOp::Basic3D));
    CHECK(layers[1].effects[0].params[0] == Approx(-90.0 * (1.0 - easeOut(0.5)))); // swivel
    CHECK(layers[1].effects[0].params[1] == 0.0);
    // Before the cut B shows its handle frames, animating in already.
    evaluateVideoLayers(s, seq, frame(50), layers);
    REQUIRE(layers.size() == 2);
    CHECK(layers[1].sourceTime == frame(290));
    CHECK(layers[1].effects[0].params[0] == Approx(-90.0 * (1.0 - easeOut(5.0 / 30.0))));

    // Block: Block Dissolve's completion runs from 1 to 0; Pull: from 160 %.
    const Transition block = animation(s, "animationBlock", a.id, b.id, 15, 15, {{"blockSize", ParamValue(32.0)}});
    const auto blocks = playback::transitionAnimationAt(block, 0.25, false, {1920, 1080}, {1920, 1080});
    REQUIRE(blocks->effects.size() == 1);
    CHECK(blocks->effects[0].op == static_cast<std::uint16_t>(render::LibraryOp::BlockDissolve));
    CHECK(blocks->effects[0].params[0] == Approx(0.75));
    CHECK(blocks->effects[0].params[1] == 32.0);
    const Transition pull = animation(s, "animationPull", a.id, b.id, 15, 15);
    CHECK(playback::transitionAnimationAt(pull, 0.0, false, {1920, 1080}, {1920, 1080})->scale == Approx(1.6));
}

TEST_CASE("Animations: Fold keeps its hinge edge in place while the clip swings in 3D (0.2)",
          "[playback][evaluate][library][animations]") {
    ProjectState s;
    for (const auto& [hinge, edge] : std::vector<std::pair<double, render::Vec2>>{
             {0.0, {960.0, 0.0}}, {1.0, {960.0, 1080.0}}, {2.0, {0.0, 540.0}}, {3.0, {1920.0, 540.0}}}) {
        INFO("hinge " << hinge);
        const Transition fold = animation(s, "animationFold", ClipId{}, ClipId{s.ids.allocate<ClipId>()}, 0, 30,
                                          {{"hinge", ParamValue(hinge)}});
        for (const double progress : {0.2, 0.5, 0.8}) {
            INFO("progress " << progress);
            const auto anim = playback::transitionAnimationAt(fold, progress, false, {1920, 1080}, {1920, 1080});
            REQUIRE(anim);
            const double angle = 90.0 * (1.0 - easeOut(progress));
            CHECK(std::abs(anim->effects[0].params[0]) + std::abs(anim->effects[0].params[1]) == Approx(angle));
            // The middle of the hinge edge shows the middle of the frame's edge.
            const render::Vec2 source = foldSource(anim->effects, 1920.0, 1080.0, edge);
            CHECK(source.x == Approx(edge.x).margin(1e-6));
            CHECK(source.y == Approx(edge.y).margin(1e-6));
        }
    }
}
