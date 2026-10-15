// Evaluation of the C.3 finishing features (docs/architecture.md, "Evaluator obligations"): keyframe curves in
// media time, speed / reverse / freeze through the clip's time mapping, effect stacks (order, disabled and bypassed
// instances, animated parameters) and transitions with handles.

#include "PlaybackTestSupport.h"

#include <wabcut/model/Effects.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

Ticks frames(std::int64_t n) {
    return framesToTicks(n, kRate60);
}

std::shared_ptr<MediaItem> fakeMedia(ProjectState& s) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
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

Clip clipOf(ProjectState& s, MediaId media, std::int64_t start, std::int64_t length, std::int64_t sourceIn) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {frames(start), frames(length)};
    c.sourceIn = frames(sourceIn);
    return c;
}

Curve linearCurve(std::int64_t t0, double v0, std::int64_t t1, double v1) {
    Curve curve;
    curve.keys = {Keyframe{frames(t0), v0}, Keyframe{frames(t1), v1}};
    return curve;
}

EffectInstance instanceOf(ProjectState& s, std::string id, std::vector<std::pair<std::string, ParamValue>> params,
                          bool enabled = true, int version = 1) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    e.version = version;
    e.enabled = enabled;
    for (auto& [key, value] : params) {
        e.set(key, std::move(value));
    }
    return e;
}

} // namespace

TEST_CASE("Keyframes: opacity and position follow curves in media time", "[playback][evaluate][finishing]") {
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    // The clip shows source frames 30.. from timeline frame 60; keys at source 30 and 90.
    Clip clip = clipOf(s, media->id, 60, 120, 30);
    ClipProperties p;
    p[ParamId::Opacity] = ParamValue(linearCurve(30, 0.0, 90, 1.0));
    p[ParamId::PositionX] = ParamValue(linearCurve(30, -400.0, 90, 400.0));
    clip.setProperties(p);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList(std::vector<Clip>{clip});
    seq.videoTracks = {std::make_shared<const Track>(t)};
    std::vector<LayerSpec> layers;

    evaluateVideoLayers(s, seq, frames(60), layers); // source 30: opacity 0 -> skipped
    CHECK(layers.empty());
    evaluateVideoLayers(s, seq, frames(90), layers); // source 60: half way
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].opacity == Approx(0.5));
    CHECK(layers[0].transform.position.x == Approx(0.0));
    evaluateVideoLayers(s, seq, frames(105), layers); // source 75
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].opacity == Approx(0.75));
    CHECK(layers[0].transform.position.x == Approx(200.0));
    evaluateVideoLayers(s, seq, frames(150), layers); // past the last key: clamped
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].opacity == Approx(1.0));
    CHECK(layers[0].transform.position.x == Approx(400.0));

    // Slip by +10 source frames: the keys stick to the picture, so the same timeline time reads 10 frames later.
    Clip slipped = clip;
    slipped.sourceIn = frames(40);
    t.clips = ClipList(std::vector<Clip>{slipped});
    seq.videoTracks = {std::make_shared<const Track>(t)};
    evaluateVideoLayers(s, seq, frames(90), layers); // source 70
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].opacity == Approx(40.0 / 60.0));
    CHECK(layers[0].sourceTime == frames(70));
}

TEST_CASE("Codex P1-6: curves stay in media time through trims, splits, 2x, reverse and holds",
          "[playback][evaluate][finishing]") {
    // The review's scenario: sourceIn = 5 s, opacity keys (5 s, 0.2) and (10 s, 0.8); 2 s into the clip shows
    // source 7 s, so the opacity is 0.2 + 0.6 * 2 / 5 = 0.44 (an evaluator in clip-relative time would read 0.2).
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    const auto sec = [](double v) { return static_cast<std::int64_t>(v * 60.0); }; // seconds -> 60 fps frames
    const auto opacityAt = [&](std::vector<Clip> clips, double seconds) {
        Track t;
        t.id = TrackId{9001};
        t.clips = ClipList(std::move(clips));
        seq.videoTracks = {std::make_shared<const Track>(t)};
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(s, seq, frames(sec(seconds)), layers);
        REQUIRE(layers.size() == 1);
        return layers[0].opacity;
    };
    const auto keyed = [&](std::int64_t start, std::int64_t length, std::int64_t sourceIn) {
        Clip c = clipOf(s, media->id, start, length, sourceIn);
        ClipProperties p;
        p[ParamId::Opacity] = ParamValue(linearCurve(sec(5), 0.2, sec(10), 0.8));
        c.setProperties(p);
        return c;
    };

    // The clip at 0 s, sourceIn 5 s, 5 s long.
    CHECK(opacityAt({keyed(0, sec(5), sec(5))}, 2.0) == Approx(0.44));
    // Trimmed: the In point moved 1 s later (timeline start and source In together): the same picture keeps its
    // opacity; the animation does not restart.
    CHECK(opacityAt({keyed(sec(1), sec(4), sec(6))}, 2.0) == Approx(0.44));
    // Split at 1 s: the right-hand piece continues the animation.
    CHECK(opacityAt({keyed(0, sec(1), sec(5)), keyed(sec(1), sec(4), sec(6))}, 2.0) == Approx(0.44));
    CHECK(opacityAt({keyed(0, sec(1), sec(5)), keyed(sec(1), sec(4), sec(6))}, 0.5) == Approx(0.26));
    // 2x: 1 s into the clip already shows source 7 s.
    Clip fast = keyed(0, sec(2.5), sec(5));
    fast.speed = {2, 1};
    CHECK(opacityAt({fast}, 1.0) == Approx(0.44));
    // Reversed (source 10 s -> 5 s): source 7 s is shown 3 s in.
    Clip backwards = keyed(0, sec(5), sec(5));
    backwards.reversed = true;
    CHECK(opacityAt({backwards}, 3.0) == Approx(0.44).margin(0.01));
    // Frame hold at source 5 s: the picture stays, the curve runs on (Clip::curveTimeAt's held-frame semantics).
    Clip hold = keyed(0, sec(5), sec(5));
    hold.speed = {0, 1};
    CHECK(opacityAt({hold}, 2.0) == Approx(0.44));
}

TEST_CASE("Speed: 2x, reverse and freeze map timeline time to source time", "[playback][evaluate][finishing]") {
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    const auto evaluate = [&](const Clip& clip, std::int64_t frame) {
        Track t;
        t.id = TrackId{9000};
        t.clips = ClipList(std::vector<Clip>{clip});
        seq.videoTracks = {std::make_shared<const Track>(t)};
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(s, seq, frames(frame), layers);
        REQUIRE(layers.size() == 1);
        return layers[0];
    };

    Clip fast = clipOf(s, media->id, 0, 60, 100);
    fast.speed = {2, 1};
    CHECK(evaluate(fast, 0).sourceTime == frames(100));
    CHECK(evaluate(fast, 10).sourceTime == frames(120));
    CHECK(evaluate(fast, 59).sourceTime == frames(218));

    Clip slow = clipOf(s, media->id, 0, 60, 100);
    slow.speed = {1, 2};
    CHECK(evaluate(slow, 11).sourceTime == frames(100) + Ticks{frames(11).value / 2});

    // Reversed: the first frame shows the last source frame of [sourceIn, sourceOut).
    Clip reversed = clipOf(s, media->id, 0, 60, 100);
    reversed.reversed = true;
    const Ticks top = reversed.sourceOut() - Ticks{1};
    CHECK(evaluate(reversed, 0).sourceTime == top);
    CHECK(evaluate(reversed, 10).sourceTime == top - frames(10));
    CHECK(evaluate(reversed, 59).sourceTime == top - frames(59));
    CHECK(evaluate(reversed, 59).sourceTime >= frames(100));

    // Freeze frame: the held frame throughout, while parameters keep animating from it (curve time runs on).
    Clip freeze = clipOf(s, media->id, 0, 60, 100);
    freeze.speed = {0, 1};
    ClipProperties p;
    p[ParamId::Scale] = ParamValue(linearCurve(100, 1.0, 160, 2.0));
    freeze.setProperties(p);
    CHECK(evaluate(freeze, 0).sourceTime == frames(100));
    CHECK(evaluate(freeze, 45).sourceTime == frames(100));
    CHECK(evaluate(freeze, 0).transform.scale == Approx(1.0));
    CHECK(evaluate(freeze, 30).transform.scale == Approx(1.5));
}

TEST_CASE("Effects: the stack is evaluated in order at the curve time; unknown ones are bypassed",
          "[playback][evaluate][finishing]") {
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    Clip clip = clipOf(s, media->id, 0, 120, 0);
    ClipContent content = clip.content();
    content.effects = {
        instanceOf(s, "wabcut.blur.gaussian", {{"radius", ParamValue(linearCurve(0, 0.0, 60, 30.0))}}),
        instanceOf(s, "wabcut.sharpen", {{"amount", ParamValue(100.0)}}, /*enabled*/ false),
        instanceOf(s, "acme.glow", {{"strength", ParamValue(3.0)}}),
        instanceOf(s, "wabcut.blur.gaussian", {{"radius", ParamValue(5.0)}}, true, /*version*/ 7),
        instanceOf(s, "wabcut.color.basic", {{"exposure", ParamValue(1.5)}}),
        instanceOf(s, "wabcut.crop", {{"left", ParamValue(0.25)}, {"feather", ParamValue(8.0)}}),
    };
    clip.setContent(content);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList(std::vector<Clip>{clip});
    seq.videoTracks = {std::make_shared<const Track>(t)};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, frames(30), layers);
    REQUIRE(layers.size() == 1);
    const LayerSpec& layer = layers[0];
    CHECK(layer.bypassedEffects == 2); // the unknown id and the newer version; the disabled one is not a warning
    REQUIRE(layer.effects.size() == 3);
    CHECK(layer.effects[0].type == render::EffectType::GaussianBlur);
    CHECK(layer.effects[0].params[0] == Approx(15.0)); // half way along the radius curve
    CHECK(layer.effects[0].params[1] == Approx(1.0));  // repeatEdges default
    CHECK(layer.effects[1].type == render::EffectType::BasicCorrection);
    CHECK(layer.effects[1].params[0] == Approx(1.5));
    CHECK(layer.effects[1].params[2] == Approx(100.0)); // saturation default
    CHECK(layer.effects[2].type == render::EffectType::Crop);
    CHECK(layer.effects[2].params[0] == Approx(0.25));
    CHECK(layer.effects[2].params[4] == Approx(8.0));
}

TEST_CASE("Transitions: both sides with handles, progress and one-sided mixes", "[playback][evaluate][finishing]") {
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    // A [0, 60) shows source 0..; B [60, 120) shows source 200..; a 20-frame centred dissolve at the cut.
    Clip a = clipOf(s, media->id, 0, 60, 0);
    Clip b = clipOf(s, media->id, 60, 60, 200);
    Transition dissolve;
    dissolve.id = s.ids.allocate<TransitionId>();
    dissolve.outgoing = a.id;
    dissolve.incoming = b.id;
    dissolve.before = frames(10);
    dissolve.after = frames(10);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    const auto evaluateWith = [&](std::vector<Clip> clips, Transition tr, std::int64_t frame) {
        Track t;
        t.id = TrackId{9001};
        t.clips = ClipList(std::move(clips));
        t.transitions = {std::move(tr)};
        seq.videoTracks = {std::make_shared<const Track>(t)};
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(s, seq, frames(frame), layers);
        return layers;
    };

    auto layers = evaluateWith({a, b}, dissolve, 45); // before the transition
    REQUIRE(layers.size() == 1);
    CHECK_FALSE(layers[0].transition);

    layers = evaluateWith({a, b}, dissolve, 55);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].clip == a.id);
    CHECK(layers[1].clip == b.id);
    REQUIRE(layers[0].transition);
    CHECK_FALSE(layers[1].transition);
    CHECK(layers[0].transition->layerCount == 2);
    CHECK(layers[0].transition->kind == render::TransitionKind::CrossDissolve);
    CHECK(layers[0].transition->progress == Approx(0.25));
    CHECK(layers[0].sourceTime == frames(55));
    CHECK(layers[1].sourceTime == frames(195)); // B's head handle: 5 frames before its In point

    layers = evaluateWith({a, b}, dissolve, 65);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].transition->progress == Approx(0.75));
    CHECK(layers[0].sourceTime == frames(65)); // A's tail handle, past its Out point
    CHECK(layers[1].sourceTime == frames(205));

    layers = evaluateWith({a, b}, dissolve, 70);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].clip == b.id);
    CHECK_FALSE(layers[0].transition);

    // A reversed: its handle lies below its source In.
    Clip reversedA = a;
    reversedA.reversed = true;
    reversedA.sourceIn = frames(20); // shows source 79 .. 20
    layers = evaluateWith({reversedA, b}, dissolve, 65);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].sourceTime == reversedA.sourceOut() - Ticks{1} - frames(65));
    CHECK(layers[0].sourceTime < reversedA.sourceIn);

    // Dip to white and wipe parameters.
    Transition wipe = dissolve;
    wipe.effect = "wipe";
    wipe.params = {EffectParam{"angle", ParamValue(90.0)}, EffectParam{"feather", ParamValue(0.2)}};
    layers = evaluateWith({a, b}, wipe, 60);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].transition->kind == render::TransitionKind::Wipe);
    CHECK(layers[0].transition->angleDegrees == Approx(90.0));
    CHECK(layers[0].transition->feather == Approx(0.2));
    CHECK(layers[0].transition->progress == Approx(0.5));
    Transition dip = dissolve;
    dip.effect = "dipToWhite";
    layers = evaluateWith({a, b}, dip, 52);
    REQUIRE(layers[0].transition);
    CHECK(layers[0].transition->kind == render::TransitionKind::DipToColor);
    CHECK(layers[0].transition->color[0] == Approx(1.0));

    // One-sided at A's tail ("to black"): A alone, mixed with transparent.
    Transition tail = dissolve;
    tail.incoming = ClipId{};
    tail.after = Ticks{};
    layers = evaluateWith({a}, tail, 55);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].transition);
    CHECK(layers[0].transition->layerCount == 1);
    CHECK_FALSE(layers[0].transition->incomingOnly);
    CHECK(layers[0].transition->progress == Approx(0.5));

    // One-sided at B's head ("from black").
    Transition head = dissolve;
    head.outgoing = ClipId{};
    head.before = Ticks{};
    layers = evaluateWith({b}, head, 65);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].transition);
    CHECK(layers[0].transition->incomingOnly);
    CHECK(layers[0].transition->progress == Approx(0.5));

    // An unknown transition id is bypassed: a plain cut.
    Transition unknown = dissolve;
    unknown.effect = "acme.spin";
    layers = evaluateWith({a, b}, unknown, 55);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].clip == a.id);
    layers = evaluateWith({a, b}, unknown, 62);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].clip == b.id);
}

TEST_CASE("Effects and transitions never occlude the layers below", "[playback][evaluate][finishing]") {
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    Clip below = clipOf(s, media->id, 0, 60, 0);
    Clip above = clipOf(s, media->id, 0, 60, 0);
    ClipContent content = above.content();
    content.effects = {instanceOf(s, "wabcut.crop", {{"left", ParamValue(0.5)}})};
    above.setContent(content);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    Track v1;
    v1.id = s.ids.allocate<TrackId>();
    v1.clips = ClipList(std::vector<Clip>{below});
    Track v2;
    v2.id = s.ids.allocate<TrackId>();
    v2.clips = ClipList(std::vector<Clip>{above});
    seq.videoTracks = {std::make_shared<const Track>(v1), std::make_shared<const Track>(v2)};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, frames(10), layers);
    CHECK(layers.size() == 2); // the cropped full-frame layer does not hide V1
}

TEST_CASE("Titles: title clips evaluate to a drawn layer with their style, nothing is decoded",
          "[playback][evaluate][finishing][titles]") {
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    const SequenceId seqId = created->id;
    const TrackId v1 = created->state.findSequence(seqId)->videoTracks[0]->id;
    const std::vector<EffectParam> style = {
        {"size", ParamValue(linearCurve(0, 60.0, 60, 120.0))}, // keyframed size (media time = clip time)
        {"fill.red", ParamValue(1.0)},
        {"fill.green", ParamValue(0.8)},
        {"fill.blue", ParamValue(0.0)},
        {"stroke.width", ParamValue(4.0)},
        {"shadow.opacity", ParamValue(0.6)},
        {"shadow.distance", ParamValue(12.0)},
        {"shadow.red", ParamValue(linearCurve(0, 0.2, 60, 0.8))},
        {"shadow.green", ParamValue(0.25)},
        {"shadow.blue", ParamValue(0.75)},
        {"alignment", ParamValue(0.0)},
    };
    auto titled = edit::addTitle(created->state, seqId, v1, Ticks{}, Ticks::fromSeconds(4), "Hello\nWorld", style,
                                 edit::TitleFont{"Arial", "Bold"});
    REQUIRE(titled);
    const ProjectState& s = titled->state;
    const Sequence& seq = *s.findSequence(seqId);
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, framesToTicks(30, kRate60), layers);
    REQUIRE(layers.size() == 1);
    const LayerSpec& layer = layers[0];
    CHECK_FALSE(needsDecode(layer));
    REQUIRE(layer.title);
    CHECK(layer.title->text == "Hello\nWorld");
    CHECK(layer.title->fontFamily == "Arial");
    CHECK(layer.title->fontStyle == "Bold");
    CHECK(layer.title->sizePx == Approx(90.0)); // half way along the size curve
    CHECK(layer.title->fill[1] == Approx(0.8));
    CHECK(layer.title->strokeWidth == Approx(4.0));
    CHECK(layer.title->align == render::TitleAlign::Left);
    CHECK(layer.title->frameWidth == seq.width);
    REQUIRE(layer.effects.size() == 1); // the style's shadow
    CHECK(layer.effects[0].type == render::EffectType::DropShadow);
    CHECK(layer.effects[0].params[0] == Approx(0.6));
    CHECK(layer.effects[0].params[2] == Approx(12.0));
    CHECK(layer.effects[0].params[4] == Approx(0.5));
    CHECK(layer.effects[0].params[5] == Approx(0.25));
    CHECK(layer.effects[0].params[6] == Approx(0.75));
    CHECK(layer.bypassedEffects == 0);

    // The shared composite builder draws it (no frame needed).
    std::vector<media::VideoFrame> frames(layers.size());
    std::vector<render::CompositeLayer> composite;
    buildCompositeLayers(layers, frames, composite);
    REQUIRE(composite.size() == 1);
    REQUIRE(composite[0].view);
    CHECK(composite[0].view->isValid());
    CHECK(composite[0].view->width == seq.width);
    CHECK(composite[0].effects.size() == 1);
}

TEST_CASE("Captions: the cue under the playhead is the top layer; the chosen track; karaoke",
          "[playback][evaluate][finishing][captions]") {
    ProjectState s;
    auto media = fakeMedia(s);
    s.media = {media};
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.width = 1280;
    seq.height = 720;
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList(std::vector<Clip>{clipOf(s, media->id, 0, 120, 0)});
    seq.videoTracks = {std::make_shared<const Track>(t)};
    const auto track = [&](std::string name, bool enabled, std::vector<CaptionCue> cues) {
        CaptionTrack c;
        c.id = s.ids.allocate<CaptionTrackId>();
        c.name = std::move(name);
        c.enabled = enabled;
        c.cues = std::move(cues);
        return c;
    };
    const CaptionTrack english = track("English", true, {{frames(0), frames(60), "Hello big world"}, {frames(90), frames(30), "Second"}});
    const CaptionTrack hidden = track("Hidden", false, {{frames(0), frames(120), "Disabled"}});
    const CaptionTrack thai = track("Thai", true, {{frames(0), frames(120), "สวัสดีครับ"}});
    seq.captionTracks = {english, hidden, thai};

    std::vector<LayerSpec> layers;
    CaptionSettings captions;
    evaluateVideoLayers(s, seq, frames(10), layers, true, nullptr, &captions);
    REQUIRE(layers.size() == 2);
    REQUIRE(layers.back().caption);
    CHECK_FALSE(needsDecode(layers.back()));
    CHECK(layers.back().item == nullptr);
    CHECK(layers.back().caption->text == "Hello big world");
    CHECK(layers.back().caption->frameWidth == 1280);
    CHECK(layers.back().caption->frameHeight == 720);
    CHECK(layers.back().caption->highlightEnd == -1);
    evaluateVideoLayers(s, seq, frames(70), layers, true, nullptr, &captions); // between cues
    CHECK(layers.size() == 1);
    evaluateVideoLayers(s, seq, frames(95), layers, true, nullptr, &captions);
    REQUIRE(layers.back().caption);
    CHECK(layers.back().caption->text == "Second");
    evaluateVideoLayers(s, seq, frames(10), layers); // no caption settings: no captions
    CHECK(layers.size() == 1);

    // The chosen track; a disabled one falls back to the first enabled.
    captions.track = thai.id;
    CHECK(evaluateCaption(seq, frames(10), captions)->text == "สวัสดีครับ");
    captions.track = hidden.id;
    CHECK(evaluateCaption(seq, frames(10), captions)->text == "Hello big world");
    captions.track.reset();

    // Karaoke, estimated: the cue's 60 frames shared by letters (Hello 5, big 3, world 5).
    captions.karaoke = true;
    CHECK(evaluateCaption(seq, frames(0), captions)->highlightEnd == 5);   // "Hello"
    CHECK(evaluateCaption(seq, frames(30), captions)->highlightEnd == 9);  // "Hello big"
    CHECK(evaluateCaption(seq, frames(59), captions)->highlightEnd == 15); // all three
    // With word timings (a linked transcript): they decide.
    auto words = std::make_shared<CaptionWordTimings>();
    words->cues[{english.id.value, frames(0).value}] = {{frames(0), frames(10)}, {frames(40), frames(45)}, {frames(50), frames(60)}};
    captions.words = words;
    CHECK(evaluateCaption(seq, frames(30), captions)->highlightEnd == 5);
    CHECK(evaluateCaption(seq, frames(45), captions)->highlightEnd == 9);
    CHECK(evaluateCaption(seq, frames(52), captions)->highlightEnd == 15);
    // Timings that do not match the cue's words are ignored (estimate again).
    words->cues[{english.id.value, frames(0).value}] = {{frames(0), frames(10)}};
    CHECK(evaluateCaption(seq, frames(30), captions)->highlightEnd == 9);
}
