// Effect presets (0.2, EffectPresets.h): Save Preset, Premiere's three preset types, fixed effects, LUT files, the
// built-in Presets bin and the library and preset files (untrusted input).

#include "TimelineFixture.h"

#include <wabcut/model/EffectPresets.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Matchers::WithinAbs;

namespace {

Curve keys(std::initializer_list<std::pair<Ticks, double>> list) {
    Curve c;
    for (const auto& [t, v] : list) {
        c.keys.push_back(Keyframe{t, v, Interpolation::Linear, {}, {}});
    }
    return c;
}

const EffectInstance& lastEffect(const ProjectState& state, SequenceId seq, ClipId clip) {
    return state.findSequence(seq)->findClip(clip)->clip().effects().back();
}

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId clip) { return state.findSequence(seq)->findClip(clip)->clip(); }

std::vector<Ticks> keyTimes(const ParamValue* value) {
    std::vector<Ticks> out;
    REQUIRE(value);
    REQUIRE(value->curve());
    for (const Keyframe& k : value->curve()->keys) {
        out.push_back(k.time);
    }
    return out;
}

/// A clip at frame 10 of V1 (20 frames, source In 5) with a Gaussian Blur keyed at its frames 2 and 10, and a 40-frame
/// clip at frame 40 (source In 0) to apply presets to.
struct PresetFixture {
    Timeline tl{R"(
        V1 tgt |..........A5aaaaaaaaaaaaaaaaaa..........Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A1 tgt |..........A5aaaaaaaaaaaaaaaaaa..........Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )"};
    ProjectState state;
    ClipId source;
    ClipId target;

    PresetFixture() {
        source = tl.clip("V1@10");
        target = tl.clip("V1@40");
        // Curve time = source time: the clip shows source frame 5 at its In point.
        state = ok(edit::addEffect(tl.state, tl.seq, std::vector<ClipId>{source}, "wabcut.blur.gaussian",
                                   {EffectParam{"radius", ParamValue(keys({{f(7), 40.0}, {f(15), 0.0}}))}}))
                    .state;
    }

    EffectPreset save(EffectPresetType type, edit::PresetSelection selection = {{0}, {}}) const {
        auto p = edit::makeEffectPreset(state, clipOf(state, tl.seq, source), selection, "Blur in", type);
        INFO((p ? std::string() : p.error().message));
        REQUIRE(p);
        return *p;
    }
};

} // namespace

TEST_CASE("Save Preset keeps keyframes relative to the clip's In point", "[model][effects][presets]") {
    PresetFixture fx;
    const EffectPreset p = fx.save(EffectPresetType::Scale);
    CHECK(p.name == "Blur in");
    CHECK(p.duration == f(20));
    REQUIRE(p.effects.size() == 1);
    CHECK(p.effects[0].effect == "wabcut.blur.gaussian");
    CHECK(keyTimes(p.effects[0].find("radius")) == std::vector<Ticks>{f(2), f(10)});
    CHECK_FALSE(effectPresetProblem(p));
    CHECK(p.kind() == EffectKind::Video);
}

TEST_CASE("Preset types place keyframes as Premiere does", "[model][effects][presets]") {
    PresetFixture fx;
    const auto applied = [&](EffectPresetType type) {
        auto r = edit::applyEffectPreset(fx.state, fx.tl.seq, std::vector<ClipId>{fx.target}, fx.save(type));
        INFO((r ? std::string() : r.error().message));
        REQUIRE(r);
        requireValid(r->edit.state);
        REQUIRE(r->clips.size() == 1);
        CHECK(r->clips[0].applied == std::vector<std::string>{"Gaussian Blur"});
        CHECK(r->edit.createdEffects.size() == 1);
        return keyTimes(lastEffect(r->edit.state, fx.tl.seq, fx.target).find("radius"));
    };
    // The target is 40 frames from source 0: Anchor to In Point keeps 2 and 10 frames from the In point, Scale doubles
    // them with the length, Anchor to Out Point keeps their distance from the Out point (18 and 10 frames).
    CHECK(applied(EffectPresetType::AnchorToInPoint) == std::vector<Ticks>{f(2), f(10)});
    CHECK(applied(EffectPresetType::Scale) == std::vector<Ticks>{f(4), f(20)});
    CHECK(applied(EffectPresetType::AnchorToOutPoint) == std::vector<Ticks>{f(22), f(30)});
}

TEST_CASE("Applying a preset follows the target clip's speed and direction", "[model][effects][presets]") {
    PresetFixture fx;
    const EffectPreset p = fx.save(EffectPresetType::AnchorToInPoint);
    // At 200 %, two frames of the clip are four frames of curve (source) time.
    edit::SpeedChange change;
    change.speed = Speed{2, 1};
    ProjectState fast = ok(edit::setSpeed(fx.state, fx.tl.seq, std::vector<ClipId>{fx.target}, change, edit::LinkMode::IgnoreLinks)).state;
    auto r = edit::applyEffectPreset(fast, fx.tl.seq, std::vector<ClipId>{fx.target}, p);
    REQUIRE(r);
    requireValid(r->edit.state);
    const Clip& c = clipOf(r->edit.state, fx.tl.seq, fx.target);
    CHECK(keyTimes(c.effects().back().find("radius")) == std::vector<Ticks>{c.sourceIn + f(4), c.sourceIn + f(20)});
    // The blur reads 40 at the clip's third frame wherever the clip is.
    CHECK_THAT(effectParameterAt(c.effects().back(), "radius", c.curveTimeAt(c.start() + f(2))), WithinAbs(40.0, 1e-9));
}

TEST_CASE("Fixed effects replace the clip's values and skip the other kind", "[model][effects][presets]") {
    PresetFixture fx;
    ProjectState s = ok(edit::setParameter(fx.state, fx.tl.seq, fx.source, edit::ParamRef::of(ParamId::Scale),
                                           ParamValue(keys({{f(5), 0.0}, {f(10), 0.5}}))))
                         .state;
    s = ok(edit::setParameter(s, fx.tl.seq, fx.source, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.8))).state;
    edit::ClipAttributeChange blend;
    blend.blendMode = BlendMode::Screen;
    s = ok(edit::setClipAttributes(s, fx.tl.seq, std::vector<ClipId>{fx.source}, blend)).state;
    auto p = edit::makeEffectPreset(s, clipOf(s, fx.tl.seq, fx.source), {{}, {"wabcut.motion", "wabcut.opacity"}}, "Pop",
                                    EffectPresetType::AnchorToInPoint);
    REQUIRE(p);
    REQUIRE(p->effects.size() == 2);
    CHECK(p->effects[0].effect == "wabcut.motion");
    CHECK(p->effects[1].findData(kPresetBlendModeKey)->text()->compare("screen") == 0);

    const ClipId audio = fx.tl.clip("A1@40");
    auto r = edit::applyEffectPreset(s, fx.tl.seq, std::vector<ClipId>{fx.target, audio}, *p);
    REQUIRE(r);
    requireValid(r->edit.state);
    const Clip& v = clipOf(r->edit.state, fx.tl.seq, fx.target);
    CHECK(keyTimes(&v.properties()[ParamId::Scale]) == std::vector<Ticks>{Ticks{}, f(5)});
    CHECK(v.properties()[ParamId::Opacity].constant() == 0.8);
    CHECK(v.blendMode() == BlendMode::Screen);
    REQUIRE(r->clips.size() == 2);
    CHECK(r->clips[1].applied.empty());
    CHECK(r->clips[1].skipped.size() == 2);
    // A video preset on audio clips alone applies to nothing.
    CHECK_FALSE(edit::applyEffectPreset(s, fx.tl.seq, std::vector<ClipId>{audio}, *p));
    // Motion of an audio clip cannot be saved.
    CHECK_FALSE(edit::makeEffectPreset(s, clipOf(s, fx.tl.seq, audio), {{}, {"wabcut.motion"}}, "x", EffectPresetType::Scale));
}

TEST_CASE("Presets refer to LUT files by path and bring them into the project", "[model][effects][presets][color]") {
    PresetFixture fx;
    ProjectAsset lut;
    lut.kind = std::string(kAssetKindLut);
    lut.name = "Look.cube";
    lut.locator.path = "C:/Looks/Look.cube";
    auto added = edit::addAsset(fx.state, lut);
    REQUIRE(added);
    ProjectState s = ok(edit::addEffect(added->state, fx.tl.seq, std::vector<ClipId>{fx.source}, "wabcut.color.grade")).state;
    const EffectInstanceId grade = clipOf(s, fx.tl.seq, fx.source).effects().back().id;
    edit::EffectChange change;
    change.data.push_back(EffectData{"creative.look", assetReference(added->id)});
    s = ok(edit::setEffect(s, fx.tl.seq, fx.source, grade, change)).state;
    auto p = edit::makeEffectPreset(s, clipOf(s, fx.tl.seq, fx.source), {{1}, {}}, "My look", EffectPresetType::Scale);
    REQUIRE(p);
    CHECK(*p->effects[0].findData("creative.look")->text() == "file:C:/Looks/Look.cube");

    // In a project that has the file, its asset is used; in one that has not, an asset is added.
    auto same = edit::applyEffectPreset(s, fx.tl.seq, std::vector<ClipId>{fx.target}, *p);
    REQUIRE(same);
    CHECK(same->edit.state.assets.size() == 1);
    CHECK(*lastEffect(same->edit.state, fx.tl.seq, fx.target).findData("creative.look")->text() == assetReference(added->id));
    auto fresh = edit::applyEffectPreset(fx.state, fx.tl.seq, std::vector<ClipId>{fx.target}, *p);
    REQUIRE(fresh);
    requireValid(fresh->edit.state);
    REQUIRE(fresh->edit.state.assets.size() == 1);
    CHECK(fresh->edit.state.assets[0].locator.path == "C:/Looks/Look.cube");
    CHECK(fresh->edit.state.assets[0].name == "Look.cube");
    CHECK(fresh->edit.changes.assetsChanged);
}

TEST_CASE("The built-in Presets bin is valid and frame-aware", "[model][effects][presets]") {
    const auto presets = builtinEffectPresets(1920, 1080);
    std::set<std::string> names;
    for (const EffectPreset& p : presets) {
        INFO(p.name);
        CHECK_FALSE(effectPresetProblem(p));
        CHECK(names.insert(p.name).second);
    }
    for (const char* name : {"Bevel Edges Thick", "Bevel Edges Thin", "Fast Blur In", "Fast Blur Out", "Mosaic In", "Mosaic Out",
                             "PiP 25% UL", "PiP 25% LR Scale In", "PiP 40% LL", "PiP 25% Motion UL to LR", "Solarize In",
                             "Solarize Out", "Twirl In", "Twirl Out", "Horizontal Image Pan Left to Right", "Teal & Orange",
                             "Sony S-Log3 (S-Gamut3.Cine) to Rec.709", "Slow Push In"}) {
        INFO(name);
        CHECK(findBuiltinEffectPreset(name));
    }
    CHECK(isLumetriPreset(*findBuiltinEffectPreset("Mono Silver")));
    CHECK_FALSE(isLumetriPreset(*findBuiltinEffectPreset("Twirl In")));
    // PiP 25% UL: a quarter-size picture 5 % in from the top-left corner, in sequence pixels from the centre.
    const auto pip = findBuiltinEffectPreset("pip 25% ul", 1280, 720);
    REQUIRE(pip);
    CHECK_THAT(pip->effects[0].find("motion.scale")->constant(), WithinAbs(0.25, 1e-12));
    CHECK_THAT(pip->effects[0].find("motion.position.x")->constant(), WithinAbs(-0.325 * 1280, 1e-9));
    CHECK_THAT(pip->effects[0].find("motion.position.y")->constant(), WithinAbs(-0.325 * 720, 1e-9));

    PresetFixture fx;
    auto r = edit::applyEffectPreset(fx.state, fx.tl.seq, std::vector<ClipId>{fx.target}, *findBuiltinEffectPreset("Twirl Out"));
    REQUIRE(r);
    requireValid(r->edit.state);
    // Anchored to the Out point: the twirl winds up over the clip's last second (25 frames of 40).
    CHECK(keyTimes(lastEffect(r->edit.state, fx.tl.seq, fx.target).find("angle")) == std::vector<Ticks>{f(15), f(40)});
}

TEST_CASE("Effects library and preset files read back and refuse bad input", "[model][effects][presets][io]") {
    PresetFixture fx;
    EffectsLibrary library;
    EffectPreset a = fx.save(EffectPresetType::AnchorToOutPoint);
    a.bin = "Mine/Blurs";
    a.description = "Ein Weichzeichner";
    library.presets.push_back(a);
    library.presetBins = {"Mine", "Mine/Blurs"};
    library.customBins.push_back({"Gaming", {"effect:wabcut.blur.gaussian", "preset:Blur in", "preset:Twirl In"}});
    library.favorites = {"effect:crossDissolve"};
    REQUIRE_FALSE(effectsLibraryProblem(library));
    const std::string text = io::effectsLibraryToJson(library);
    auto back = io::effectsLibraryFromJson(text);
    INFO((back ? std::string() : back.error().message));
    REQUIRE(back);
    CHECK(*back == library);

    const std::vector<EffectPreset> list{a};
    auto file = io::effectPresetsFileFromJson(io::effectPresetsFileToJson(list));
    REQUIRE(file);
    CHECK(*file == list);

    const auto refused = [](std::string_view json) { return !io::effectsLibraryFromJson(json).has_value(); };
    CHECK(refused("not json"));
    CHECK(refused(R"({"format": "wabcut-graphic-template", "version": 1})"));
    CHECK(io::effectsLibraryFromJson(R"({"format": "wabcut-effects-library", "version": 9})").error().code ==
          ErrorCode::VersionMismatch);
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "favorites": ["nonsense"]})"));
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "customBins": [{"name": ""}]})"));
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "presets": [{"name": "Twirl In", "duration": 100,
        "effects": [{"id": 1, "effect": "wabcut.distort.twirl", "params": {}}]}]})")); // a built-in's name
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "presets": [{"name": "x", "duration": 0,
        "effects": [{"id": 1, "effect": "wabcut.distort.twirl", "params": {}}]}]})")); // no duration
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "presets": [{"name": "x", "duration": 100, "type": "wobble",
        "effects": [{"id": 1, "effect": "wabcut.distort.twirl", "params": {}}]}]})"));
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "presets": [{"name": "x", "duration": 100,
        "effects": [{"id": 1, "effect": "wabcut.motion", "params": {"motion.scale": {"value": -1}}}]}]})")); // out of range
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "presets": [{"name": "x", "duration": 100,
        "effects": [{"id": 1, "effect": "wabcut.motion", "params": {}}, {"id": 2, "effect": "wabcut.volume", "params": {}}]}]})"));
    CHECK(refused(R"({"format": "wabcut-effects-library", "version": 1, "presetBins": ["a//b"]})"));
    std::string deep = R"({"format": "wabcut-effects-library", "version": 1, "presetBins": [")";
    for (int i = 0; i < 9; ++i) {
        deep += (i ? "/b" : "b");
    }
    CHECK(refused(deep + "\"]}"));
}

TEST_CASE("Animations: In and Out on one clip both stay and only the animated parameters change", "[model][effects][presets][animations]") {
    PresetFixture fx;
    // The target sits at a PiP position; Pop and Spin move scale and rotation only.
    ProjectState s = ok(edit::setParameter(fx.state, fx.tl.seq, fx.target, edit::ParamRef::of(ParamId::PositionX), ParamValue(300.0))).state;
    for (const char* name : {"Fade In", "Fade Out", "Pop In", "Pop Out", "Spin In", "Spin Out", "Pull In", "Pull Out",
                             "Travel In from Left", "Travel Out to Bottom", "Spring In", "Spring Out", "PiP 25% UL Spin In",
                             "PiP 40% LR Spin Out"}) {
        INFO(name);
        REQUIRE(findBuiltinEffectPreset(name));
        CHECK_FALSE(effectPresetProblem(*findBuiltinEffectPreset(name)));
    }
    auto in = edit::applyEffectPreset(s, fx.tl.seq, std::vector<ClipId>{fx.target}, *findBuiltinEffectPreset("Pop In"));
    REQUIRE(in);
    auto both = edit::applyEffectPreset(in->edit.state, fx.tl.seq, std::vector<ClipId>{fx.target}, *findBuiltinEffectPreset("Pop Out"));
    REQUIRE(both);
    requireValid(both->edit.state);
    const Clip& v = clipOf(both->edit.state, fx.tl.seq, fx.target);
    // 40 frames at 25 fps: Pop In keys at 0, 8 (0.32 s), 12 (0.5 s); Pop Out at 27.5, 32 and 40 frames.
    const std::vector<Ticks> scale = keyTimes(&v.properties()[ParamId::Scale]);
    REQUIRE(scale.size() == 6);
    CHECK(scale.front() == Ticks{});
    CHECK(scale.back() == f(40));
    CHECK(v.properties()[ParamId::PositionX].constant() == 300.0); // untouched
    CHECK(v.parameterAt(ParamId::Scale, v.start()) == 0.0);
    CHECK(v.parameterAt(ParamId::Scale, v.start() + f(20)) == 1.0);
    // A Scale preset still replaces the whole curve.
    auto pan = edit::applyEffectPreset(both->edit.state, fx.tl.seq, std::vector<ClipId>{fx.target},
                                       *findBuiltinEffectPreset("Horizontal Image Pan Left to Right"));
    REQUIRE(pan);
    CHECK(clipOf(pan->edit.state, fx.tl.seq, fx.target).properties()[ParamId::Scale].constant() == 1.2);
}

// ---- 0.4 Ken Burns moves (Image Pans) ---------------------------------------------------------------------------------

TEST_CASE("Image Pans: Slow Push In and Pull Out are eased scale moves across the clip", "[model][effects][presets][kenburns]") {
    // Three 40-frame clips (1.6 s at 25 fps): a Scale preset stretches its keys to each clip's length.
    Timeline tl{R"(
        V1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbCccccccccccccccccccccccccccccccccccccccc|
    )"};
    const std::vector<ClipId> clips{tl.clip("V1@0"), tl.clip("V1@40"), tl.clip("V1@80")};
    const auto push = findBuiltinEffectPreset("Slow Push In");
    REQUIRE(push);
    CHECK(push->bin == "Image Pans");
    CHECK(push->type == EffectPresetType::Scale);
    // Only the scale is set: rotation, anchor and position stay the clip's.
    REQUIRE(push->effects.size() == 1);
    CHECK(push->effects[0].find("motion.rotation") == nullptr);
    CHECK(push->effects[0].find("motion.position.x") == nullptr);

    // One edit (one undo step) puts the move on every clip.
    ProjectState s = ok(edit::setParameter(tl.state, tl.seq, clips[1], edit::ParamRef::of(ParamId::Rotation), ParamValue(10.0))).state;
    auto r = edit::applyEffectPreset(s, tl.seq, clips, *push);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    requireValid(r->edit.state);
    CHECK(r->clips.size() == 3);
    for (const ClipId id : clips) {
        const Clip& c = clipOf(r->edit.state, tl.seq, id);
        const ParamValue& scale = c.properties()[ParamId::Scale];
        REQUIRE(scale.curve());
        REQUIRE(scale.curve()->keys.size() == 2);
        CHECK(scale.curve()->keys.front().interpolation == Interpolation::EaseInOut);
        CHECK_THAT(c.parameterAt(ParamId::Scale, c.start()), WithinAbs(1.0, 1e-9));
        CHECK_THAT(c.parameterAt(ParamId::Scale, c.end()), WithinAbs(1.12, 1e-9));
        // A slow start: a quarter of the way through, well under a quarter of the move.
        CHECK(c.parameterAt(ParamId::Scale, c.start() + f(10)) < 1.03);
        CHECK(c.parameterAt(ParamId::Scale, c.start() + f(20)) > 1.04);
    }
    CHECK(clipOf(r->edit.state, tl.seq, clips[1]).properties()[ParamId::Rotation].constant() == 10.0); // untouched

    // Pull Out is the reverse.
    const auto pull = findBuiltinEffectPreset("Slow Pull Out");
    REQUIRE(pull);
    auto back = edit::applyEffectPreset(tl.state, tl.seq, std::vector<ClipId>{clips[0]}, *pull);
    REQUIRE(back);
    const Clip& c = clipOf(back->edit.state, tl.seq, clips[0]);
    CHECK_THAT(c.parameterAt(ParamId::Scale, c.start()), WithinAbs(1.12, 1e-9));
    CHECK_THAT(c.parameterAt(ParamId::Scale, c.end()), WithinAbs(1.0, 1e-9));
    CHECK(c.parameterAt(ParamId::Scale, c.start() + f(10)) > 1.09);
}

TEST_CASE("Image Pans: directional push-ins never show the frame edge", "[model][effects][presets][kenburns]") {
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    for (const char* name : {"Push In to Top", "Push In to Bottom", "Push In Left to Right", "Push In Right to Left",
                             "Slow Push In", "Slow Pull Out"}) {
        INFO(name);
        const auto p = findBuiltinEffectPreset(name, kWidth, kHeight);
        REQUIRE(p);
        CHECK_FALSE(effectPresetProblem(*p));
        REQUIRE(p->effects.size() == 1);
        const EffectInstance& m = p->effects[0];
        const ParamValue* scale = m.find("motion.scale");
        REQUIRE(scale);
        REQUIRE(scale->curve());
        const ParamValue* x = m.find("motion.position.x");
        const ParamValue* y = m.find("motion.position.y");
        // Position keys share the scale keys' times and easing.
        for (const ParamValue* pos : {x, y}) {
            if (!pos)
                continue;
            REQUIRE(pos->curve());
            REQUIRE(pos->curve()->keys.size() == scale->curve()->keys.size());
            for (std::size_t i = 0; i < pos->curve()->keys.size(); ++i) {
                CHECK(pos->curve()->keys[i].time == scale->curve()->keys[i].time);
                CHECK(pos->curve()->keys[i].interpolation == scale->curve()->keys[i].interpolation);
            }
        }
        // At 10 times across the preset: the picture still covers the frame.
        for (int i = 0; i <= 9; ++i) {
            const Ticks t{p->duration.value * i / 9};
            const double s = scale->valueAt(t);
            CHECK(s >= 1.0 - 1e-9);
            const double xv = x ? x->valueAt(t) : 0.0;
            const double yv = y ? y->valueAt(t) : 0.0;
            CHECK(std::abs(xv) <= (s - 1.0) / 2.0 * kWidth + 1e-6);
            CHECK(std::abs(yv) <= (s - 1.0) / 2.0 * kHeight + 1e-6);
        }
    }
}
