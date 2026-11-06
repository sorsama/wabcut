// OpenFX host (0.7): from a project to the picture the compositor draws - the evaluator collects a clip's OpenFX effects
// (parameters at the frame's curve time, keyframes included), buildCompositeLayers runs them on the decoded frame and
// hands the compositor the result, and an effect that is missing or fails leaves the layer as it was.

#include "OfxFixture.h"

#include <wabcut/model/Track.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/PreparedComposite.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

std::shared_ptr<MediaItem> fakeVideo(ProjectState& s, std::string name) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->name = name;
    item->locator.path = "C:/media/" + name + ".mp4"; // never opened: evaluation only describes layers
    VideoStreamInfo v;
    v.index = 0;
    v.width = 64;
    v.height = 48;
    v.frameRate = kRate30;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

Clip clipAt(ProjectState& s, MediaId media, std::int64_t start, std::int64_t frames) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {framesToTicks(start, kRate30), framesToTicks(frames, kRate30)};
    return c;
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

Sequence sequenceOf(ProjectState& s, Clip clip) {
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList({std::move(clip)});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate30;
    seq.width = 64;
    seq.height = 48;
    seq.videoTracks = {std::make_shared<const Track>(std::move(t))};
    return seq;
}

Ticks frame(std::int64_t f) { return framesToTicks(f, kRate30); }

} // namespace

TEST_CASE("OpenFX pipeline: the evaluator collects a clip's OpenFX effects instead of bypassing them", "[ofxhost][pipeline]") {
    TestOfx ofx;
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 0, 60);
    EffectInstance gain = effectOf(s, kGain, {{"gain", ParamValue(0.5)}, {"mode", ParamValue(1.0)}});
    gain.setData(EffectData{"label", std::string("hi there")});
    clip.setEffects({gain, effectOf(s, "wabcut.blur.gaussian"), effectOf(s, "plugin.ofx.not.installed"),
                     effectOf(s, kInvert8)});
    const Sequence seq = sequenceOf(s, clip);
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, frame(10), layers);
    REQUIRE(layers.size() == 1);
    const LayerSpec& layer = layers[0];

    REQUIRE(layer.external.size() == 2); // the gain and the invert, in stack order
    CHECK(layer.external[0].effect == kGain);
    CHECK(layer.external[0].id == gain.id);
    CHECK(layer.external[1].effect == kInvert8);
    // Every parameter of the descriptor, at its value or default; text settings too.
    const auto value = [&](const std::string& key) {
        for (const auto& [k, v] : layer.external[0].params) {
            if (k == key) {
                return v;
            }
        }
        FAIL("no parameter " << key);
        return 0.0;
    };
    CHECK(value("gain") == Approx(0.5));
    CHECK(value("mode") == Approx(1.0));
    CHECK(value("offset") == Approx(0.0));
    CHECK(value("tint.g") == Approx(1.0));
    CHECK(value("center.x") == Approx(0.5)); // the plug-in's default (normalised)
    REQUIRE(layer.external[0].texts.size() == 1);
    CHECK(layer.external[0].texts[0].first == "label");
    CHECK(layer.external[0].texts[0].second == "hi there");
    // The clip's time in frames (the plug-in's time), at the sequence's rate.
    CHECK(layer.external[0].timeFrames == Approx(10.0));
    CHECK(layer.external[0].fps == Approx(30.0));
    // The built-in effect is the compositor's; the plug-in that is not installed is bypassed and counted.
    REQUIRE(layer.effects.size() == 1);
    CHECK(layer.effects[0].type == render::EffectType::GaussianBlur);
    CHECK(layer.bypassedEffects == 1);
}

TEST_CASE("OpenFX pipeline: keyframes of a plug-in's parameters are evaluated at the frame's time", "[ofxhost][pipeline]") {
    TestOfx ofx;
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 0, 60);
    EffectInstance gain = effectOf(s, kGain);
    Curve curve;
    curve.keys = {Keyframe{Ticks{0}, 0.0}, Keyframe{Ticks::fromSeconds(2), 2.0}}; // linear: 1 per second
    gain.set("gain", ParamValue(curve));
    clip.setEffects({gain});
    const Sequence seq = sequenceOf(s, clip);
    std::vector<LayerSpec> layers;
    for (const auto& [at, expected] : std::vector<std::pair<std::int64_t, double>>{{0, 0.0}, {15, 0.5}, {30, 1.0}, {45, 1.5}}) {
        INFO("frame " << at);
        evaluateVideoLayers(s, seq, frame(at), layers);
        REQUIRE(layers.size() == 1);
        REQUIRE(layers[0].external.size() == 1);
        double got = -1.0;
        for (const auto& [k, v] : layers[0].external[0].params) {
            if (k == "gain") {
                got = v;
            }
        }
        CHECK(got == Approx(expected).margin(0.001));
    }
}

TEST_CASE("OpenFX pipeline: the composite layer shows the plug-in's picture, in the frame's own pixel shape", "[ofxhost][pipeline]") {
    TestOfx ofx;
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 0, 60);
    clip.setEffects({effectOf(s, kGain, {{"gain", ParamValue(0.5)}, {"center.x", ParamValue(9.0)}, {"center.y", ParamValue(9.0)}})});
    const Sequence seq = sequenceOf(s, clip);
    std::vector<LayerSpec> specs;
    evaluateVideoLayers(s, seq, frame(0), specs);
    REQUIRE(specs.size() == 1);
    REQUIRE(specs[0].external.size() == 1);

    std::vector<media::VideoFrame> frames;
    frames.push_back(yuvFrame(64, 48, 126, 128, 128));
    frames.back().sampleAspect = Rate{4, 3}; // an anamorphic source: its pixels are wider than they are tall
    std::vector<render::CompositeLayer> layers;
    buildCompositeLayers(specs, frames, layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].view.has_value());
    CHECK(layers[0].viewIsMedia); // not a canvas-sized graphic: the compositor keeps the pixel aspect it carries
    const render::FrameView& view = *layers[0].view;
    CHECK(view.width == 64);
    CHECK(view.height == 48);
    CHECK(view.pixelAspect == Approx(4.0 / 3.0));
    CHECK(view.layout == render::PlaneLayout::PackedRGBA);
    const std::uint8_t* p = rgbaAt(view, 20, 20);
    CHECK(std::abs(p[0] - 64) <= 1);
    CHECK(std::abs(p[1] - 64) <= 1);
    CHECK(p[3] == 255);
    // The decoded frame stays with the layer (redactions and field options read its timing).
    CHECK(layers[0].frame.isValid());
}

TEST_CASE("OpenFX pipeline: a layer whose plug-in fails keeps showing its frame", "[ofxhost][pipeline]") {
    TestOfx ofx;
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 0, 60);
    clip.setEffects({effectOf(s, kFail)});
    const Sequence seq = sequenceOf(s, clip);
    std::vector<LayerSpec> specs;
    evaluateVideoLayers(s, seq, frame(0), specs);
    REQUIRE(specs.size() == 1);
    REQUIRE(specs[0].external.size() == 1);
    std::vector<media::VideoFrame> frames;
    frames.push_back(yuvFrame(64, 48, 126, 128, 128));
    std::vector<render::CompositeLayer> layers;
    buildCompositeLayers(specs, frames, layers);
    REQUIRE(layers.size() == 1);
    CHECK_FALSE(layers[0].view.has_value()); // bypassed: the compositor draws the decoded frame as it always did
    CHECK(layers[0].frame.isValid());
    CHECK_FALSE(layers[0].viewIsMedia);
}

TEST_CASE("OpenFX pipeline: no processor installed means every OpenFX effect is bypassed", "[ofxhost][pipeline]") {
    ProjectState s;
    auto media = fakeVideo(s, "game");
    s.media = {media};
    Clip clip = clipAt(s, media->id, 0, 60);
    clip.setEffects({effectOf(s, kGain)}); // no plug-in system here: the effect is an unknown effect
    const Sequence seq = sequenceOf(s, clip);
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, frame(0), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].external.empty());
    CHECK(layers[0].bypassedEffects == 1);
}
