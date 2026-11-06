// Motion Blur in the evaluator (0.7, model/MotionBlur.h; docs/effects.md "Motion Blur"): a clip with the Motion Blur effect gets
// its Motion at each instant of the shutter (LayerSpec::blur -> render::CompositeLayer::blur), in curve time so a speed ramp
// lengthens the smear; an angle of 0, a still clip or a disabled effect leave the layer exactly as it was; the preview takes
// Preview Samples, the export Samples; the effect is no pass of its own.

#include <wabcut/model/Effects.h>
#include <wabcut/model/MotionBlur.h>
#include <wabcut/model/Track.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

std::shared_ptr<MediaItem> fakeVideo(ProjectState& s) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->name = "game";
    item->locator.path = "C:/media/game.mp4";
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

Ticks frame(std::int64_t f) {
    return framesToTicks(f, kRate60);
}

EffectInstance blurEffect(ProjectState& s, double angle, bool enabled = true, int samples = 16, int previewSamples = 8) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::string(kMotionBlurEffect);
    e.enabled = enabled;
    e.set("shutterAngle", ParamValue(angle));
    e.set("samples", ParamValue(static_cast<double>(samples)));
    e.set("previewSamples", ParamValue(static_cast<double>(previewSamples)));
    return e;
}

/// A 2 s clip at 60 fps whose Position X runs `perSecond` pixels a second (media time), played at `speed`, optionally with
/// the effect.
Sequence sequenceOf(ProjectState& s, const std::shared_ptr<MediaItem>& media, double perSecond, Speed speed,
                    std::optional<EffectInstance> effect) {
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = media->id;
    clip.speed = speed;
    clip.timelineRange = {Ticks{}, frame(120)};
    ClipProperties props;
    props.set(ParamId::PositionX, 0.0);
    if (perSecond != 0.0) {
        Curve move;
        move.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{Ticks::fromSeconds(30), perSecond * 30.0}};
        props[ParamId::PositionX] = ParamValue(move);
    }
    clip.setProperties(props);
    if (effect)
        clip.setEffects({*effect});
    Track track;
    track.id = s.ids.allocate<TrackId>();
    track.clips = ClipList({clip});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.width = 1920;
    seq.height = 1080;
    seq.videoTracks = {std::make_shared<const Track>(std::move(track))};
    return seq;
}

std::vector<LayerSpec> evaluate(const ProjectState& s, const Sequence& seq, Ticks time, bool preview = false) {
    std::vector<LayerSpec> layers;
    PreviewSettings settings;
    evaluateVideoLayers(s, seq, time, layers, true, preview ? &settings : nullptr, nullptr);
    return layers;
}

} // namespace

TEST_CASE("Motion Blur: a keyframed move gets a placement per instant of the shutter, centred on the frame", "[playback][evaluate][motionblur]") {
    ProjectState s;
    const auto media = fakeVideo(s);
    s.media = {media};
    // 19,200 pixels a second = 320 a frame: a 180 degree shutter travels 160 pixels, far more than the 16 instants allow.
    const Sequence seq = sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 180.0));
    const auto layers = evaluate(s, seq, frame(30));
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].effects.empty()); // the effect is no pass: the evaluator reads it
    REQUIRE(layers[0].blur.size() == 16);
    CHECK(layers[0].transform.position.x == Approx(9600.0));
    // Quarter of a frame before to a quarter after, the middles of 16 equal parts: 320 * (+-0.25 * 15/16) from the frame's place.
    CHECK(layers[0].blur.front().position.x == Approx(9600.0 - 320.0 * 0.234375));
    CHECK(layers[0].blur.back().position.x == Approx(9600.0 + 320.0 * 0.234375));
    double mean = 0;
    for (const render::LayerTransform& t : layers[0].blur)
        mean += t.position.x / 16.0;
    CHECK(mean == Approx(9600.0));
    CHECK(std::is_sorted(layers[0].blur.begin(), layers[0].blur.end(),
                         [](const render::LayerTransform& a, const render::LayerTransform& b) { return a.position.x < b.position.x; }));
    // The preview takes Preview Samples (8 here), the export Samples (16); both are the same smear.
    const auto preview = evaluate(s, seq, frame(30), true);
    REQUIRE(preview[0].blur.size() == 8);
    CHECK(preview[0].blur.front().position.x == Approx(9600.0 - 320.0 * 0.21875));
    // A shutter of 360 degrees opening at the frame (phase 0) smears forward one whole frame.
    EffectInstance forward = blurEffect(s, 360.0);
    forward.set("shutterPhase", ParamValue(0.0));
    const auto ahead = evaluate(s, sequenceOf(s, media, 19200.0, Speed{1, 1}, forward), frame(30));
    CHECK(ahead[0].blur.front().position.x > 9600.0);
    CHECK(ahead[0].blur.back().position.x == Approx(9600.0 + 320.0 * (1.0 - 0.5 / 16)));
}

TEST_CASE("Motion Blur: the number of instants follows how far the layer moves", "[playback][evaluate][motionblur]") {
    ProjectState s;
    const auto media = fakeVideo(s);
    s.media = {media};
    // 480 pixels a second = 8 a frame: a 180 degree shutter travels 4 pixels: ceil(4 / 1.5) + 1 = 4 instants.
    const auto slow = evaluate(s, sequenceOf(s, media, 480.0, Speed{1, 1}, blurEffect(s, 180.0)), frame(30));
    CHECK(slow[0].blur.size() == 4);
    // Twice as fast: 8 pixels, ceil(8 / 1.5) + 1 = 7.
    const auto fast = evaluate(s, sequenceOf(s, media, 960.0, Speed{1, 1}, blurEffect(s, 180.0)), frame(30));
    CHECK(fast[0].blur.size() == 7);
    // Quality caps it: Samples 5 allows no more than five instants however far it travels.
    const auto capped = evaluate(s, sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 180.0, true, 5)), frame(30));
    CHECK(capped[0].blur.size() == 5);
    // A clip that moves a hundredth of a pixel gets no blur at all.
    const auto barely = evaluate(s, sequenceOf(s, media, 0.6, Speed{1, 1}, blurEffect(s, 180.0)), frame(30));
    CHECK(barely[0].blur.empty());
}

TEST_CASE("Motion Blur: shutter 0, a disabled effect and a still clip leave the layer exactly as it was", "[playback][evaluate][motionblur]") {
    ProjectState s;
    const auto media = fakeVideo(s);
    s.media = {media};
    const Sequence bare = sequenceOf(s, media, 19200.0, Speed{1, 1}, std::nullopt);
    const auto reference = evaluate(s, bare, frame(30));
    REQUIRE(reference.size() == 1);
    CHECK(reference[0].blur.empty());
    std::vector<std::pair<const char*, Sequence>> variants;
    variants.emplace_back("angle 0", sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 0.0)));
    variants.emplace_back("disabled", sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 180.0, false)));
    for (const auto& [name, sequence] : variants) {
        INFO(name);
        const auto layers = evaluate(s, sequence, frame(30));
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].blur.empty());
        CHECK(layers[0].effects.empty());
        CHECK(layers[0].transform == reference[0].transform);
        CHECK(layers[0].opacity == reference[0].opacity);
        CHECK(layers[0].sourceTime == reference[0].sourceTime);
    }
    // A clip that does not move: the effect changes nothing.
    const auto still = evaluate(s, sequenceOf(s, media, 0.0, Speed{1, 1}, blurEffect(s, 180.0)), frame(30));
    REQUIRE(still.size() == 1);
    CHECK(still[0].blur.empty());
    // Keyed on: no blur while the angle is 0, blur once it opens.
    EffectInstance keyed = blurEffect(s, 0.0);
    keyed.set("shutterAngle", ParamValue(Curve{{Keyframe{Ticks{}, 0.0}, Keyframe{Ticks::fromSeconds(1), 180.0}}}));
    const Sequence ramp = sequenceOf(s, media, 19200.0, Speed{1, 1}, keyed);
    CHECK(evaluate(s, ramp, frame(0))[0].blur.empty());
    CHECK_FALSE(evaluate(s, ramp, frame(60))[0].blur.empty());
}

TEST_CASE("Motion Blur: a speed ramp makes the keys race and the smear longer", "[playback][evaluate][motionblur]") {
    ProjectState s;
    const auto media = fakeVideo(s);
    s.media = {media};
    // 480 pixels a second of media time. At 200 % the clip covers it twice as fast: 16 pixels a frame, a 180 degree shutter
    // travels 8; at 100 %, 4. The instants are the keyed Position X at curve time (the first source frame of the clip is 0).
    const auto at = [&](Speed speed) { return evaluate(s, sequenceOf(s, media, 480.0, speed, blurEffect(s, 180.0, true, 64, 64)), frame(30)); };
    const auto normal = at(Speed{1, 1});
    const auto doubled = at(Speed{2, 1});
    REQUIRE(normal[0].blur.size() >= 2);
    REQUIRE(doubled[0].blur.size() >= 2);
    const auto span = [](const std::vector<render::LayerTransform>& blur) { return blur.back().position.x - blur.front().position.x; };
    // Half a frame of a 480 px/s move is 4 pixels at 100 % and 8 at 200 %, less the outer half of the end instants.
    CHECK(span(normal[0].blur) == Approx(4.0 * (1.0 - 1.0 / static_cast<double>(normal[0].blur.size()))).margin(0.05));
    CHECK(span(doubled[0].blur) == Approx(8.0 * (1.0 - 1.0 / static_cast<double>(doubled[0].blur.size()))).margin(0.05));
    CHECK(span(doubled[0].blur) > 1.5 * span(normal[0].blur));
    // The frame's own place moved with the speed too.
    CHECK(doubled[0].transform.position.x == Approx(2.0 * normal[0].transform.position.x));
}

TEST_CASE("Motion Blur: with equal Samples and Preview Samples the preview and the export place the layer alike", "[playback][evaluate][motionblur]") {
    ProjectState s;
    const auto media = fakeVideo(s);
    s.media = {media};
    const Sequence seq = sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 180.0, true, 12, 12));
    const auto exported = evaluate(s, seq, frame(30), false);
    const auto previewed = evaluate(s, seq, frame(30), true);
    REQUIRE(exported.size() == 1);
    REQUIRE(previewed.size() == 1);
    REQUIRE(exported[0].blur.size() == 12);
    CHECK(previewed[0].blur == exported[0].blur);
    CHECK(previewed[0].transform == exported[0].transform);
    // Different numbers of samples are the same smear sampled more or less finely: the ends of the shutter agree to within
    // one sample's share of it.
    const auto coarse = evaluate(s, sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 180.0, true, 16, 4)), frame(30), true);
    const auto fine = evaluate(s, sequenceOf(s, media, 19200.0, Speed{1, 1}, blurEffect(s, 180.0, true, 16, 4)), frame(30), false);
    REQUIRE(coarse[0].blur.size() == 4);
    REQUIRE(fine[0].blur.size() == 16);
    const double shutter = 320.0 * 0.5; // pixels travelled in half a frame
    CHECK(coarse[0].blur.front().position.x - fine[0].blur.front().position.x == Approx(shutter * (1.0 / 8.0 - 1.0 / 32.0)).margin(1e-6));
}
