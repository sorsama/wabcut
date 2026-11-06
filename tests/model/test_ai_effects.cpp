// AI video effects (0.7): the descriptors of AI Restore and AI Denoise, the chain of a stack (stages, Auto scale, Amount),
// its hash (the cache's identity: everything but Amount) and the project round trip of their settings.

#include "TimelineFixture.h"

#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

using namespace wabcut;
using Catch::Approx;

namespace {

EffectInstance instance(std::uint64_t id, std::string_view effect) {
    EffectInstance e;
    e.id = EffectInstanceId{id};
    e.effect = std::string(effect);
    return e;
}

} // namespace

TEST_CASE("AI Restore and AI Denoise are registered video effects with valid defaults", "[model][effects][ai]") {
    const EffectRegistry& registry = EffectRegistry::builtin();
    const EffectDescriptor* restore = registry.find(kAiRestoreEffect);
    const EffectDescriptor* denoise = registry.find(kAiDenoiseEffect);
    REQUIRE(restore != nullptr);
    REQUIRE(denoise != nullptr);
    CHECK(restore->kind == EffectKind::Video);
    CHECK(denoise->category == "AI");
    CHECK_FALSE(restore->intrinsic);
    CHECK(isAiVideoEffect(restore->id));
    CHECK_FALSE(isAiVideoEffect("wabcut.blur.gaussian"));
    for (const EffectDescriptor* d : {restore, denoise}) {
        const EffectParamInfo* amount = d->param("amount");
        REQUIRE(amount != nullptr);
        CHECK(amount->animatable); // Amount is the keyframable mix
        CHECK(amount->defaultValue == 100.0);
        for (const EffectParamInfo& p : d->params) {
            if (p.key != "amount") {
                CHECK_FALSE(p.animatable); // what the models make is a constant; the pictures are kept per setting
            }
            CHECK(p.defaultValue >= p.minValue);
            CHECK(p.defaultValue <= p.maxValue);
        }
    }
    CHECK(restore->param("scale")->isChoice());
    CHECK(restore->param("scale")->optionLabel(0) == "Auto");
    CHECK(restore->param("scale")->optionLabel(3) == "4x");
    CHECK(restore->param("quality")->optionLabel(1) == "High quality");
    CHECK(restore->param("tile")->optionLabels().size() == 4);

    // A stack of defaults, and of the extremes, is a valid stack.
    EffectStack stack{instance(1, kAiRestoreEffect), instance(2, kAiDenoiseEffect)};
    CHECK_FALSE(effectStackProblem(stack, EffectKind::Video, 100).has_value());
    stack[0].set("scale", ParamValue(3.0));
    stack[0].set("deblur", ParamValue(100.0));
    stack[1].set("strength", ParamValue(100.0));
    CHECK_FALSE(effectStackProblem(stack, EffectKind::Video, 100).has_value());
    stack[0].set("scale", ParamValue(4.0)); // out of the menu
    CHECK(effectStackProblem(stack, EffectKind::Video, 100).has_value());
}

TEST_CASE("Auto scale fills the canvas and no more", "[model][effects][ai]") {
    CHECK(aiAutoScale(2160, 1080) == 1);
    CHECK(aiAutoScale(1080, 1080) == 1);
    CHECK(aiAutoScale(1000, 1080) == 2);
    CHECK(aiAutoScale(540, 1080) == 2);
    CHECK(aiAutoScale(480, 1080) == 4); // 960 lines is not within 10 % of 1080
    CHECK(aiAutoScale(270, 1080) == 4);
    CHECK(aiAutoScale(0, 1080) == 2); // unknown
    CHECK(aiAutoScale(720, 0) == 2);
}

TEST_CASE("A stack's chain keeps the enabled stages that change something, in order", "[model][effects][ai]") {
    EffectInstance restore = instance(1, kAiRestoreEffect);
    restore.set("scale", ParamValue(2.0)); // 2x
    EffectInstance noop = instance(2, kAiRestoreEffect);
    noop.set("scale", ParamValue(1.0)); // 1x
    noop.set("deblur", ParamValue(0.0));
    EffectInstance denoise = instance(3, kAiDenoiseEffect);
    denoise.set("strength", ParamValue(70.0));
    EffectInstance off = instance(4, kAiDenoiseEffect);
    off.enabled = false;
    EffectInstance other = instance(5, "wabcut.blur.gaussian");

    const AiChain chain = aiChainOf({restore, noop, other, denoise, off}, Ticks{}, 540, 1080);
    REQUIRE(chain.stages.size() == 2);
    CHECK(chain.stages[0].effect == kAiRestoreEffect);
    CHECK(chain.stages[0].scale == 2);
    CHECK(chain.stages[0].deblur == 50); // the default
    CHECK(chain.stages[1].effect == kAiDenoiseEffect);
    CHECK(chain.stages[1].denoise == 70);
    CHECK(chain.stages[1].grain == 15); // Denoise keeps some grain by default
    CHECK(chain.outputScale() == 2);
    CHECK(aiChainLabel(chain) == "Restore 2x, Denoise");
    CHECK(aiChainOf({}, Ticks{}, 540, 1080).empty());
    CHECK(aiChainOf({other}, Ticks{}, 540, 1080).empty());
    // Grain preserved needs a denoise to preserve.
    EffectInstance deblurOnly = instance(6, kAiRestoreEffect);
    deblurOnly.set("scale", ParamValue(1.0));
    deblurOnly.set("grain", ParamValue(80.0));
    const AiChain d = aiChainOf({deblurOnly}, Ticks{}, 540, 1080);
    REQUIRE(d.stages.size() == 1);
    CHECK(d.stages[0].grain == 0);
}

TEST_CASE("The chain's Amount is the product of the stages' Amounts at the curve time, 0 means none", "[model][effects][ai]") {
    EffectInstance a = instance(1, kAiRestoreEffect);
    a.set("scale", ParamValue(2.0));
    a.set("amount", ParamValue(80.0));
    EffectInstance b = instance(2, kAiDenoiseEffect);
    b.set("amount", ParamValue(50.0));
    CHECK(aiChainOf({a, b}, Ticks{}, 540, 1080).amount == Approx(0.4));
    a.set("amount", ParamValue(0.0));
    CHECK(aiChainOf({a, b}, Ticks{}, 540, 1080).empty()); // one stage at 0 % shows the picture as it is
    Curve fade;
    fade.keys = {Keyframe{Ticks{}, 100.0}, Keyframe{Ticks::fromSeconds(2), 0.0}};
    EffectInstance c = instance(3, kAiRestoreEffect);
    c.set("scale", ParamValue(2.0));
    c.set("amount", ParamValue(fade));
    CHECK(aiChainOf({c}, Ticks{}, 540, 1080).amount == Approx(1.0));
    CHECK(aiChainOf({c}, Ticks::fromSeconds(1), 540, 1080).amount == Approx(0.5));
    // The processing chain ignores Amount altogether.
    CHECK(aiProcessingChainOf({a, b}, 540, 1080).stages.size() == 2);
}

TEST_CASE("The chain hash is what the pictures depend on: every setting but Amount", "[model][effects][ai]") {
    const auto hashWith = [](const std::string& key, double value) {
        EffectInstance e = instance(1, kAiRestoreEffect);
        e.set("scale", ParamValue(2.0));
        e.set(key, ParamValue(value));
        return aiChainHash(aiProcessingChainOf({e}, 540, 1080));
    };
    const std::uint64_t base = hashWith("deblur", 50.0);
    CHECK(base != 0);
    CHECK(hashWith("deblur", 50.0) == base);
    CHECK(hashWith("deblur", 60.0) != base);
    CHECK(hashWith("denoise", 20.0) != base);
    CHECK(hashWith("quality", 1.0) != base);
    CHECK(hashWith("tile", 2.0) != base);
    CHECK(hashWith("amount", 10.0) == base); // Amount is applied when the picture is shown
    CHECK(aiChainHash(AiChain{}) == 0);

    // Auto resolved differently is a different chain (another media height, another canvas).
    EffectInstance e = instance(1, kAiRestoreEffect);
    CHECK(aiChainHash(aiProcessingChainOf({e}, 540, 1080)) != aiChainHash(aiProcessingChainOf({e}, 2160, 1080)));
    // The same settings in another order are another chain (denoise then restore is not restore then denoise).
    EffectInstance d = instance(2, kAiDenoiseEffect);
    CHECK(aiChainHash(aiProcessingChainOf({e, d}, 540, 1080)) != aiChainHash(aiProcessingChainOf({d, e}, 540, 1080)));
}

TEST_CASE("AI Refocus is a registered compositor effect, not a source-stage one (card 181)", "[model][effects][ai][refocus]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kAiRefocusEffect);
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::Video);
    CHECK(d->category == "AI");
    CHECK(d->displayName == "AI Refocus");
    // The settings of the blur, in the compositor's order; every one keyframable but the Show menu.
    const std::vector<std::string_view> keys{"focus", "range", "amount", "foreground", "background", "view"};
    REQUIRE(d->params.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        INFO(keys[i]);
        CHECK(d->params[i].key == keys[i]);
        CHECK(d->params[i].animatable == (keys[i] != "view"));
        CHECK(d->params[i].defaultValue >= d->params[i].minValue);
        CHECK(d->params[i].defaultValue <= d->params[i].maxValue);
    }
    CHECK(d->param("focus")->defaultValue == 50.0);
    CHECK(d->param("range")->defaultValue == 20.0);
    CHECK(d->param("foreground")->defaultValue == 100.0);
    CHECK(d->param("view")->optionLabel(1.0) == "Depth Map");
    CHECK(d->param("view")->optionLabel(2.0) == "Blur Map");
    // It is not an effect that replaces the picture: the chains Restore and Denoise form never contain it.
    CHECK_FALSE(isAiVideoEffect(kAiRefocusEffect));
    CHECK(isAiRefocusEffect(kAiRefocusEffect));
    EffectStack stack{instance(1, kAiRefocusEffect), instance(2, kAiDenoiseEffect)};
    const AiChain source = aiChainOf(stack, Ticks{}, 1080, 1080);
    REQUIRE(source.stages.size() == 1);
    CHECK(source.stages.front().effect == kAiDenoiseEffect);
    // Its depth chain: one stage however many Refocus effects there are; none while it is disabled.
    const AiChain depth = aiDepthChainOf(stack);
    REQUIRE(depth.stages.size() == 1);
    CHECK(depth.stages.front().effect == kAiRefocusEffect);
    CHECK(depth.stages.front().isSideProduct());
    CHECK(aiChainHash(depth) == aiChainHash(aiDepthChain()));
    stack.push_back(instance(3, kAiRefocusEffect));
    CHECK(aiDepthChainOf(stack) == depth);
    stack[0].enabled = false;
    stack[2].enabled = false;
    CHECK(aiDepthChainOf(stack).empty());
    CHECK(aiDepthChainOf({}).empty());
    // The settings do not enter the depth maps' identity: moving the focus never makes them again.
    EffectStack focused{instance(1, kAiRefocusEffect)};
    focused[0].set("focus", ParamValue(80.0));
    focused[0].set("amount", ParamValue(10.0));
    CHECK(aiChainHash(aiDepthChainOf(focused)) == aiChainHash(aiDepthChainOf({instance(1, kAiRefocusEffect)})));
}

TEST_CASE("AI Frame Interpolation is a time interpolation with a name, a chain and a cache of its own (card 133)",
          "[model][effects][ai][interpolation]") {
    const AiChain chain = aiInterpolationChain();
    REQUIRE(chain.stages.size() == 1);
    CHECK(chain.stages.front().effect == kAiInterpolateStage);
    CHECK(chain.stages.front().isSideProduct());
    CHECK(aiChainHash(chain) != 0);
    CHECK(aiChainHash(chain) != aiChainHash(aiDepthChain()));
    CHECK(chain.outputScale() == 1);
    CHECK(aiChainLabel(chain) == "Frame interpolation");
    CHECK(aiChainLabel(aiDepthChain()) == "Depth maps");
}

TEST_CASE("AI Face Retouch is a registered compositor effect with its own face-map chain (card 132)", "[model][effects][ai][faces]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kAiFaceRetouchEffect);
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::Video);
    CHECK(d->category == "AI");
    CHECK(d->displayName == "AI Face Retouch");
    // The settings of the retouch, in the compositor's order; every one keyframable but the Show menu.
    const std::vector<std::string_view> keys{"smoothing", "detail", "size", "blemish", "blemishSize", "teeth", "eyes", "view"};
    REQUIRE(d->params.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        INFO(keys[i]);
        CHECK(d->params[i].key == keys[i]);
        CHECK(d->params[i].animatable == (keys[i] != "view"));
        CHECK(d->params[i].defaultValue >= d->params[i].minValue);
        CHECK(d->params[i].defaultValue <= d->params[i].maxValue);
    }
    // The defaults do something visible but gentle; every amount can go to 0 (which changes nothing) and up to 100.
    for (const std::string_view amount : {"smoothing", "blemish", "teeth", "eyes"}) {
        INFO(amount);
        CHECK(d->param(amount)->minValue == 0.0);
        CHECK(d->param(amount)->maxValue == 100.0);
        CHECK(d->param(amount)->defaultValue > 0.0);
        CHECK(d->param(amount)->defaultValue <= 50.0);
    }
    CHECK(d->param("view")->optionLabel(0.0) == "Result");
    CHECK(d->param("view")->optionLabel(1.0) == "Skin Map");
    CHECK(d->param("view")->optionLabel(2.0) == "Mouth Map");
    CHECK(d->param("view")->optionLabel(3.0) == "Eye Map");
    // It is not an effect that replaces the picture, and it is not Refocus.
    CHECK_FALSE(isAiVideoEffect(kAiFaceRetouchEffect));
    CHECK_FALSE(isAiRefocusEffect(kAiFaceRetouchEffect));
    CHECK(isAiFaceRetouchEffect(kAiFaceRetouchEffect));
    EffectStack stack{instance(1, kAiFaceRetouchEffect), instance(2, kAiDenoiseEffect)};
    const AiChain source = aiChainOf(stack, Ticks{}, 1080, 1080);
    REQUIRE(source.stages.size() == 1);
    CHECK(source.stages.front().effect == kAiDenoiseEffect);
    // Its face chain: one stage however many Face Retouch effects there are; none while they are disabled.
    const AiChain faces = aiFaceChainOf(stack);
    REQUIRE(faces.stages.size() == 1);
    CHECK(faces.stages.front().effect == kAiFaceRetouchEffect);
    CHECK(faces.stages.front().isSideProduct());
    CHECK(aiChainHash(faces) == aiChainHash(aiFaceChain()));
    CHECK(aiChainHash(faces) != aiChainHash(aiDepthChain()));
    CHECK(aiChainLabel(faces) == "Face maps");
    stack.push_back(instance(3, kAiFaceRetouchEffect));
    CHECK(aiFaceChainOf(stack) == faces);
    stack[0].enabled = false;
    stack[2].enabled = false;
    CHECK(aiFaceChainOf(stack).empty());
    CHECK(aiFaceChainOf({}).empty());
    // A Refocus is not a Face Retouch: each has its own maps.
    CHECK(aiFaceChainOf({instance(1, kAiRefocusEffect)}).empty());
    CHECK(aiDepthChainOf({instance(1, kAiFaceRetouchEffect)}).empty());
    // The settings do not enter the face maps' identity: moving a slider never makes them again.
    EffectStack tuned{instance(1, kAiFaceRetouchEffect)};
    tuned[0].set("smoothing", ParamValue(90.0));
    tuned[0].set("teeth", ParamValue(0.0));
    tuned[0].set("blemishSize", ParamValue(25.0));
    CHECK(aiChainHash(aiFaceChainOf(tuned)) == aiChainHash(aiFaceChainOf({instance(1, kAiFaceRetouchEffect)})));
}

TEST_CASE("Skin Smoothing is a registered Blur and Sharpen effect with the controls of the card (card 100)", "[model][effects][retouch]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find("wabcut.retouch.skinSmoothing");
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::Video);
    CHECK(d->category == "Blur & Sharpen");
    CHECK(d->displayName == "Skin Smoothing");
    const std::vector<std::string_view> keys{"amount", "detail", "size", "tone", "range", "softness", "view"};
    REQUIRE(d->params.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        INFO(keys[i]);
        CHECK(d->params[i].key == keys[i]);
        CHECK(d->params[i].animatable == (keys[i] != "view")); // every control can be keyframed but the Show menu
        CHECK(d->params[i].defaultValue >= d->params[i].minValue);
        CHECK(d->params[i].defaultValue <= d->params[i].maxValue);
    }
    CHECK(d->param("amount")->minValue == 0.0);
    CHECK(d->param("amount")->defaultValue > 0.0);
    CHECK(d->param("tone")->defaultValue == 123.0); // the skin line of the vectorscope
    CHECK(d->param("view")->optionLabel(1.0) == "Skin Mask");
    // It is an ordinary effect: not a source stage, no maps to make.
    CHECK_FALSE(isAiVideoEffect("wabcut.retouch.skinSmoothing"));
    CHECK(aiFaceChainOf({instance(1, "wabcut.retouch.skinSmoothing")}).empty());
}

TEST_CASE("Skin Smoothing and AI Face Retouch are saved with the project and come back as they were", "[model][effects][retouch][io]") {
    test::Timeline tl("V1 |Aaaaaaaa|");
    const ClipId clip = tl.clip("V1@0");
    ProjectState s = tl.state;
    const auto skin = test::ok(edit::addEffect(s, tl.seq, std::span(&clip, 1), "wabcut.retouch.skinSmoothing"));
    s = skin.state;
    edit::EffectChange tuned;
    tuned.params = {EffectParam{"amount", ParamValue(65.0)}, EffectParam{"tone", ParamValue(131.0)}, EffectParam{"view", ParamValue(1.0)}};
    s = test::ok(edit::setEffect(s, tl.seq, clip, skin.createdEffects.front(), tuned)).state;
    const auto face = test::ok(edit::addEffect(s, tl.seq, std::span(&clip, 1), std::string(kAiFaceRetouchEffect)));
    s = face.state;
    edit::EffectChange retouched;
    retouched.params = {EffectParam{"teeth", ParamValue(12.0)}, EffectParam{"blemishSize", ParamValue(18.0)}, EffectParam{"view", ParamValue(2.0)}};
    s = test::ok(edit::setEffect(s, tl.seq, clip, face.createdEffects.front(), retouched)).state;
    const auto json = io::toJson(s);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    const auto found = loaded->findSequence(tl.seq)->findClip(clip);
    REQUIRE(found);
    const auto& effects = found->clip().effects();
    REQUIRE(effects.size() == 2);
    CHECK(effects[0].effect == "wabcut.retouch.skinSmoothing");
    CHECK(effectParameterAt(effects[0], "amount", Ticks{}, -1.0) == Approx(65.0));
    CHECK(effectParameterAt(effects[0], "tone", Ticks{}, -1.0) == Approx(131.0));
    CHECK(effectParameterAt(effects[0], "view", Ticks{}, -1.0) == Approx(1.0));
    CHECK(effectParameterAt(effects[0], "detail", Ticks{}, -1.0) == Approx(30.0)); // what was not touched keeps its default
    CHECK(effects[1].effect == kAiFaceRetouchEffect);
    CHECK(effectParameterAt(effects[1], "teeth", Ticks{}, -1.0) == Approx(12.0));
    CHECK(effectParameterAt(effects[1], "blemishSize", Ticks{}, -1.0) == Approx(18.0));
    CHECK(effectParameterAt(effects[1], "view", Ticks{}, -1.0) == Approx(2.0));
    CHECK(effectParameterAt(effects[1], "smoothing", Ticks{}, -1.0) == Approx(50.0));
    // The face maps do not depend on those settings: the loaded project asks for the same maps.
    EffectStack before(s.findSequence(tl.seq)->findClip(clip)->clip().effects().begin(), s.findSequence(tl.seq)->findClip(clip)->clip().effects().end());
    EffectStack after(effects.begin(), effects.end());
    CHECK(aiChainHash(aiFaceChainOf(after)) == aiChainHash(aiFaceChainOf(before)));
    CHECK_FALSE(aiFaceChainOf(after).empty());
}
