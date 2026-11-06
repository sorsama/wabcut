// AI Eye Contact (0.7, card 72): the effect's descriptor and chain, what the settings make of the measured eyes (model/EyeContact.h: the
// gaze ranges, blinks, turned heads, the largest shift) and the layout the compositor's parameters are baked in; the Morph Cut's Face
// Aware switch (card 131).

#include "TimelineFixture.h"

#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/EyeContact.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

/// An open eye on a 1000 px wide picture: 60 px between the corners, lids 7.5 px above and 4.5 px below the corner line, an iris of 12 px
/// radius looking into the lens (centred between the corners, at the middle of the opening).
GazeEye lensEye() {
    GazeEye e;
    e.cx = 0.40;
    e.cy = 0.30;
    e.angle = 0.0;
    e.halfWidth = 0.030;
    e.upper = 0.0075;
    e.lower = 0.0045;
    e.irisRadius = 0.012;
    e.irisX = 0.0;
    e.irisY = 0.5 * (e.lower - e.upper); // the middle of the opening
    return e;
}

/// The same eye looking `down` (a fraction of the eye's width) and `right`.
GazeEye gazing(double down, double right = 0.0) {
    GazeEye e = lensEye();
    e.irisY += down * 2.0 * e.halfWidth;
    e.irisX += right * 2.0 * e.halfWidth;
    return e;
}

} // namespace

TEST_CASE("AI Eye Contact is a registered compositor effect with its own eye-measurement chain (card 72)", "[model][effects][ai][eyecontact]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kAiEyeContactEffect);
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::Video);
    CHECK(d->category == "AI");
    CHECK(d->displayName == "AI Eye Contact");
    // The settings, in the compositor's order (render/EffectLibrary.cpp keys); all keyframable but the two menus; the labels stay short
    // (Effect Controls cuts a name past about 12 letters).
    const std::vector<std::string_view> keys{"amount", "downFrom", "downTo", "sideLimit", "faces", "view"};
    REQUIRE(d->params.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        INFO(keys[i]);
        CHECK(d->params[i].key == keys[i]);
        CHECK(d->params[i].animatable == (keys[i] != "faces" && keys[i] != "view"));
        CHECK(d->params[i].defaultValue >= d->params[i].minValue);
        CHECK(d->params[i].defaultValue <= d->params[i].maxValue);
        CHECK(d->params[i].displayName.size() <= 12);
    }
    CHECK(d->param("amount")->defaultValue == 100.0);
    CHECK(d->param("faces")->optionLabel(0.0) == "All");
    CHECK(d->param("faces")->optionLabel(1.0) == "Largest");
    CHECK(d->param("view")->optionLabel(1.0) == "Eye Map");
    // The registered defaults are the settings' defaults.
    std::array<double, 32> defaults{};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        defaults[i] = d->params[i].defaultValue;
    }
    const EyeContactSettings fromDefaults = eyeContactSettingsOf(defaults);
    const EyeContactSettings plain;
    CHECK(fromDefaults.amount == plain.amount);
    CHECK(fromDefaults.downFrom == plain.downFrom);
    CHECK(fromDefaults.downTo == plain.downTo);
    CHECK(fromDefaults.sideLimit == plain.sideLimit);
    // It does not replace the picture, it is not Refocus or Face Retouch, and it has chains of its own.
    CHECK_FALSE(isAiVideoEffect(kAiEyeContactEffect));
    CHECK(isAiEyeContactEffect(kAiEyeContactEffect));
    CHECK_FALSE(isAiFaceRetouchEffect(kAiEyeContactEffect));
    EffectStack stack{instance(1, kAiEyeContactEffect), instance(2, kAiDenoiseEffect)};
    const AiChain source = aiChainOf(stack, Ticks{}, 1080, 1080);
    REQUIRE(source.stages.size() == 1);
    CHECK(source.stages.front().effect == kAiDenoiseEffect);
    const AiChain gaze = aiGazeChainOf(stack);
    REQUIRE(gaze.stages.size() == 1);
    CHECK(gaze.stages.front().effect == kAiEyeContactEffect);
    CHECK(gaze.stages.front().isSideProduct());
    CHECK(aiChainHash(gaze) == aiChainHash(aiGazeChain()));
    CHECK(aiChainHash(gaze) != aiChainHash(aiFaceChain()));
    CHECK(aiChainHash(gaze) != aiChainHash(aiDepthChain()));
    CHECK(aiChainLabel(gaze) == "Eye measurements");
    stack.push_back(instance(3, kAiEyeContactEffect));
    CHECK(aiGazeChainOf(stack) == gaze); // one set of measurements however many Eye Contact effects there are
    stack[0].enabled = false;
    stack[2].enabled = false;
    CHECK(aiGazeChainOf(stack).empty());
    CHECK(aiGazeChainOf({}).empty());
    CHECK(aiGazeChainOf({instance(1, kAiFaceRetouchEffect)}).empty());
    CHECK(aiFaceChainOf({instance(1, kAiEyeContactEffect)}).empty());
    // The settings do not enter the measurements' identity: moving a slider never makes them again.
    EffectStack tuned{instance(1, kAiEyeContactEffect)};
    tuned[0].set("amount", ParamValue(35.0));
    tuned[0].set("downTo", ParamValue(20.0));
    tuned[0].set("sideLimit", ParamValue(60.0));
    CHECK(aiChainHash(aiGazeChainOf(tuned)) == aiChainHash(aiGazeChainOf({instance(1, kAiEyeContactEffect)})));
}

TEST_CASE("A measured eye that looks into the lens is left exactly as it is", "[model][eyecontact]") {
    const EyeContactSettings settings;
    const EyeCorrection c = correctEye(lensEye(), 0.0, settings);
    CHECK_FALSE(c.active);
    CHECK(c.shiftX == 0.0);
    CHECK(c.shiftY == 0.0);
    CHECK(c.gazeX == Approx(0.0).margin(1e-12));
    CHECK(c.gazeY == Approx(0.0).margin(1e-12));
    CHECK(c.openness == Approx(0.2));
    // Amount 0 changes nothing whatever the eye does.
    EyeContactSettings off = settings;
    off.amount = 0.0;
    CHECK_FALSE(correctEye(gazing(0.12), 0.0, off).active);
    // An eye that is not a measurement (no opening, an iris bigger than the eye) is left alone.
    GazeEye none;
    CHECK_FALSE(none.valid());
    CHECK_FALSE(correctEye(none, 0.0, settings).active);
    GazeEye huge = lensEye();
    huge.irisRadius = 0.05;
    CHECK_FALSE(correctEye(huge, 0.0, settings).active);
    GazeEye nan = lensEye();
    nan.irisX = std::nan("");
    CHECK_FALSE(correctEye(nan, 0.0, settings).active);
}

TEST_CASE("An iris looking down moves towards the lens by Amount", "[model][eyecontact]") {
    EyeContactSettings settings;
    const GazeEye notes = gazing(0.10); // 10 % of the eye's width below where it would look into the lens
    const EyeCorrection full = correctEye(notes, 0.0, settings);
    REQUIRE(full.active);
    CHECK(full.gazeY == Approx(0.10));
    CHECK(full.weight == Approx(1.0));
    // Up by the whole offset, not sideways: the iris ends where it looks into the lens.
    CHECK(full.shiftX == Approx(0.0).margin(1e-12));
    CHECK(full.shiftY == Approx(-0.10 * 2.0 * notes.halfWidth));
    EyeContactSettings half = settings;
    half.amount = 50.0;
    const EyeCorrection partial = correctEye(notes, 0.0, half);
    CHECK(partial.shiftY == Approx(0.5 * full.shiftY));
    // Sideways offsets are corrected too, by the same rule.
    const EyeCorrection aside = correctEye(gazing(0.08, -0.12), 0.0, settings);
    REQUIRE(aside.active);
    CHECK(aside.shiftX > 0.0); // looking to the picture's left: back to the right
    CHECK(aside.shiftY < 0.0);
}

TEST_CASE("Gazes outside the ranges, blinks and turned heads fade out and never switch", "[model][eyecontact]") {
    const EyeContactSettings settings; // down 5..45 %, side 25 %
    // Looking up (thinking) or already in the lens: below Down From.
    CHECK_FALSE(correctEye(gazing(-0.10), 0.0, settings).active);
    CHECK(correctEye(gazing(0.0), 0.0, settings).weight == Approx(0.0).margin(1e-9));
    // Looking far down: beyond Down To (the keyboard): left alone; in between it ramps down over 10 points.
    CHECK(correctEye(gazing(0.30), 0.0, settings).weight == Approx(1.0));
    const double ramp = correctEye(gazing(0.50), 0.0, settings).weight;
    CHECK(ramp > 0.0);
    CHECK(ramp < 1.0);
    CHECK(correctEye(gazing(0.57), 0.0, settings).weight == Approx(0.0).margin(1e-9));
    // A glance to a side beyond Side Limit is deliberate.
    CHECK(correctEye(gazing(0.10, 0.10), 0.0, settings).weight == Approx(1.0));
    CHECK(correctEye(gazing(0.10, 0.45), 0.0, settings).weight == Approx(0.0).margin(1e-9));
    // A narrower range: the same gaze is outside it.
    EyeContactSettings narrow = settings;
    narrow.downTo = 8.0;
    CHECK(correctEye(gazing(0.20), 0.0, narrow).weight == Approx(0.0).margin(1e-9));
    CHECK(correctEye(gazing(0.05), 0.0, narrow).weight == Approx(1.0));
    // A blink: the opening closes under 7 % of the eye's width (fully above 14 %).
    GazeEye blinking = gazing(0.10);
    blinking.upper = 0.0015;
    blinking.lower = 0.0015;
    CHECK(correctEye(blinking, 0.0, settings).openness == Approx(0.05));
    CHECK_FALSE(correctEye(blinking, 0.0, settings).active);
    GazeEye halfClosed = gazing(0.10);
    halfClosed.upper = 0.0030;
    halfClosed.lower = 0.0027;
    const double halfWeight = correctEye(halfClosed, 0.0, settings).weight;
    CHECK(halfWeight > 0.0);
    CHECK(halfWeight < 1.0);
    // A head turned away (the nose off the middle between the cheeks): from 0.30 fading out, off at 0.55.
    CHECK(correctEye(gazing(0.10), 0.20, settings).weight == Approx(1.0));
    const double turned = correctEye(gazing(0.10), 0.42, settings).weight;
    CHECK(turned > 0.0);
    CHECK(turned < 1.0);
    CHECK_FALSE(correctEye(gazing(0.10), 0.6, settings).active);
}

TEST_CASE("The shift never exceeds one and a tenth iris radii", "[model][eyecontact]") {
    EyeContactSettings settings;
    settings.downTo = 60.0;
    const GazeEye far = gazing(0.40); // 24 px below the lens place for an iris of 12 px radius
    const EyeCorrection c = correctEye(far, 0.0, settings);
    REQUIRE(c.active);
    CHECK(std::hypot(c.shiftX, c.shiftY) == Approx(1.1 * far.irisRadius));
    CHECK(c.shiftY < 0.0); // still the right way
}

TEST_CASE("Baking a face writes only the eyes that need a correction, in the compositor's layout", "[model][eyecontact]") {
    GazeFace face;
    face.eyes[0] = gazing(0.10);
    face.eyes[1] = lensEye(); // the other eye already looks into the lens
    face.eyes[1].cx = 0.52;
    face.turn = 0.0;
    std::array<double, 32> params{};
    params[0] = 100.0;
    params[1] = 5.0;
    params[2] = 45.0;
    params[3] = 25.0;
    params[kEyeParamView] = 1.0;
    params[7] = 123.0; // an unused slot between the settings and the eyes is not touched
    const EyeContactSettings settings = eyeContactSettingsOf(params);
    REQUIRE(bakeEyeContact(params, face, settings));
    CHECK(params[kEyeParamCount] == 1.0);
    CHECK(params[kEyeParamView] == 1.0);
    CHECK(params[7] == 123.0);
    const std::size_t at = kEyeParamFirst;
    CHECK(params[at + 0] == Approx(face.eyes[0].cx));
    CHECK(params[at + 1] == Approx(face.eyes[0].cy));
    CHECK(params[at + 3] == Approx(face.eyes[0].halfWidth));
    CHECK(params[at + 4] == Approx(face.eyes[0].upper));
    CHECK(params[at + 5] == Approx(face.eyes[0].lower));
    CHECK(params[at + 6] == Approx(face.eyes[0].irisX).margin(1e-12));
    CHECK(params[at + 7] == Approx(face.eyes[0].irisY));
    CHECK(params[at + 8] == Approx(face.eyes[0].irisRadius));
    CHECK(params[at + 9] == Approx(0.0).margin(1e-12));
    CHECK(params[at + 10] == Approx(-0.10 * 2.0 * face.eyes[0].halfWidth));
    // Both eyes: two entries, the eyes' own numbers each, within the 32 parameters.
    face.eyes[1] = gazing(0.06, 0.04);
    face.eyes[1].cx = 0.52;
    std::array<double, 32> both = params;
    REQUIRE(bakeEyeContact(both, face, settings));
    CHECK(both[kEyeParamCount] == 2.0);
    CHECK(both[kEyeParamFirst + kEyeParamStride + 0] == Approx(0.52));
    CHECK(both[kEyeParamFirst + kEyeParamStride + 10] < 0.0);
    CHECK(kEyeParamFirst + 2 * kEyeParamStride <= 31);
    // Nothing to correct: false, no eyes, the descriptor's values as they were.
    GazeFace calm;
    calm.eyes[0] = lensEye();
    calm.eyes[1] = lensEye();
    std::array<double, 32> none = params;
    CHECK_FALSE(bakeEyeContact(none, calm, settings));
    CHECK(none[kEyeParamCount] == 0.0);
    CHECK(none[0] == 100.0);
    // A face turned away is left alone as a whole.
    GazeFace away = face;
    away.turn = 0.8;
    std::array<double, 32> turnedAway = params;
    CHECK_FALSE(bakeEyeContact(turnedAway, away, settings));
}

TEST_CASE("AI Eye Contact is saved with the project and comes back as it was", "[model][effects][eyecontact][io]") {
    test::Timeline tl("V1 |Aaaaaaaa|");
    const ClipId clip = tl.clip("V1@0");
    ProjectState s = tl.state;
    const auto added = test::ok(edit::addEffect(s, tl.seq, std::span(&clip, 1), std::string(kAiEyeContactEffect)));
    s = added.state;
    edit::EffectChange tuned;
    tuned.params = {EffectParam{"amount", ParamValue(72.0)}, EffectParam{"downTo", ParamValue(33.0)}, EffectParam{"faces", ParamValue(1.0)},
                    EffectParam{"view", ParamValue(1.0)}};
    s = test::ok(edit::setEffect(s, tl.seq, clip, added.createdEffects.front(), tuned)).state;
    const auto json = io::toJson(s);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    const auto found = loaded->findSequence(tl.seq)->findClip(clip);
    REQUIRE(found);
    const auto& effects = found->clip().effects();
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].effect == kAiEyeContactEffect);
    CHECK(effectParameterAt(effects[0], "amount", Ticks{}, -1.0) == Approx(72.0));
    CHECK(effectParameterAt(effects[0], "downTo", Ticks{}, -1.0) == Approx(33.0));
    CHECK(effectParameterAt(effects[0], "faces", Ticks{}, -1.0) == Approx(1.0));
    CHECK(effectParameterAt(effects[0], "view", Ticks{}, -1.0) == Approx(1.0));
    CHECK(effectParameterAt(effects[0], "downFrom", Ticks{}, -1.0) == Approx(5.0)); // what was not touched keeps its default
    EffectStack after(effects.begin(), effects.end());
    CHECK_FALSE(aiGazeChainOf(after).empty());
}

TEST_CASE("Morph Cut has a Face Aware switch that is on by default (card 131)", "[model][effects][morphcut]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kMorphCutTransition);
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::VideoTransition);
    REQUIRE(d->params.size() == 1);
    CHECK(d->params[0].key == "faceAware");
    CHECK(d->params[0].displayName == "Face Aware");
    CHECK(d->params[0].defaultValue == 1.0);
    CHECK_FALSE(d->params[0].animatable); // a switch: constants only
    CHECK(d->params[0].minValue == 0.0);
    CHECK(d->params[0].maxValue == 1.0);
}
