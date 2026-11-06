// Stream A, batch A10 (0.7), model: the Graph Editor's easing presets (card 96) and Motion Blur's shutter (card 58).

#include <wabcut/model/Effects.h>
#include <wabcut/model/KeyframeEasing.h>
#include <wabcut/model/MotionBlur.h>
#include <wabcut/model/Parameters.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using Catch::Approx;

namespace {

/// Seconds (fractions too) as ticks; Ticks::fromSeconds takes whole seconds.
Ticks secs(double s) {
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

Curve twoKeys(double from, double to, double seconds = 2.0) {
    Curve c;
    c.keys.push_back(Keyframe{Ticks{}, from, Interpolation::Linear, {}, {}});
    c.keys.push_back(Keyframe{secs(seconds), to, Interpolation::Linear, {}, {}});
    return c;
}

} // namespace

TEST_CASE("Easing presets: a segment shaped by a preset draws that preset's curve", "[model][easing][a10]") {
    const auto presets = edit::easingPresets();
    REQUIRE(presets.size() >= 30);
    // Ids are unique and findable.
    std::set<std::string> ids;
    for (const edit::EasingPreset& p : presets) {
        CHECK(ids.insert(std::string(p.id)).second);
        CHECK(edit::findEasingPreset(p.id) == &p);
        CHECK(p.x1 >= 0.0);
        CHECK(p.x2 <= 1.0);
    }
    CHECK(edit::findEasingPreset("nope") == nullptr);

    // Each preset, applied to a 20 -> 80 segment over two seconds, evaluates as 20 + 60 * easing(u) at every quarter.
    for (const edit::EasingPreset& p : presets) {
        INFO(p.id);
        Curve c = twoKeys(20.0, 80.0);
        REQUIRE(edit::applyEasing(c, 0, p));
        CHECK(c.isWellFormed());
        CHECK(c.keys[0].interpolation == Interpolation::Bezier);
        CHECK(c.keys[1].interpolation == Interpolation::Bezier);
        CHECK(c.valueAt(c.keys[0].time) == Approx(20.0));
        CHECK(c.valueAt(c.keys[1].time) == Approx(80.0));
        for (const double u : {0.15, 0.25, 0.5, 0.75, 0.9}) {
            const Ticks t = secs(2.0 * u);
            // The handles keep influence at least 1 % (a handle sits off its key), so the curve is the preset's within a hair
            // (a preset that leaves the key at once, like Ease Out Circ, differs most: under 2 % of the 60 units).
            CHECK(c.valueAt(t) == Approx(20.0 + 60.0 * edit::easingValue(p, u)).margin(1.2));
        }
    }
}

TEST_CASE("Easing presets: Linear is straight, Ease In starts slow, Ease Out lands slow, Back overshoots", "[model][easing][a10]") {
    const auto at = [](std::string_view id, double u) {
        Curve c = twoKeys(0.0, 100.0);
        REQUIRE(edit::applyEasing(c, 0, *edit::findEasingPreset(id)));
        return c.valueAt(secs(2.0 * u));
    };
    CHECK(at("linear", 0.25) == Approx(25.0).margin(0.2));
    CHECK(at("linear", 0.5) == Approx(50.0).margin(0.2));
    CHECK(at("easeIn", 0.25) < 15.0);
    CHECK(at("easeOut", 0.25) > 35.0);
    CHECK(at("easeInOut", 0.5) == Approx(50.0).margin(0.5));
    CHECK(at("easeInOut", 0.1) < 10.0);
    CHECK(at("easeInOut", 0.9) > 90.0);
    // Back goes past the end on the way out and below the start on the way in.
    CHECK(at("easeOutBack", 0.7) > 100.0);
    CHECK(at("easeInBack", 0.2) < 0.0);
}

TEST_CASE("Easing presets: only the one segment changes; a Hold key stays a step", "[model][easing][a10]") {
    // Three keys: 0 (linear), 50 (linear), 100 (linear). Ease the first segment: the second keeps its straight line.
    Curve c;
    c.keys = {Keyframe{Ticks{}, 0.0, Interpolation::Linear, {}, {}},
              Keyframe{secs(1), 50.0, Interpolation::Linear, {}, {}},
              Keyframe{secs(2), 100.0, Interpolation::Linear, {}, {}}};
    const Curve before = c;
    REQUIRE(edit::applyEasing(c, 0, *edit::findEasingPreset("easeInOutCubic")));
    CHECK(c.valueAt(secs(0.25)) < before.valueAt(secs(0.25)));
    for (const double s : {1.25, 1.5, 1.75})
        CHECK(c.valueAt(secs(s)) == Approx(before.valueAt(secs(s))).margin(1e-6));
    CHECK(c.isWellFormed());

    // The last key has no segment after it.
    CHECK_FALSE(edit::applyEasing(c, 2, *edit::findEasingPreset("easeIn")));
    CHECK_FALSE(edit::applyEasing(c, 7, *edit::findEasingPreset("easeIn")));

    // The arriving key is a Hold: it keeps being a step, and the leaving side of the first key is shaped.
    Curve held;
    held.keys = {Keyframe{Ticks{}, 0.0, Interpolation::Linear, {}, {}},
                 Keyframe{secs(1), 50.0, Interpolation::Hold, {}, {}},
                 Keyframe{secs(2), 100.0, Interpolation::Linear, {}, {}}};
    REQUIRE(edit::applyEasing(held, 0, *edit::findEasingPreset("easeOut")));
    CHECK(held.keys[1].interpolation == Interpolation::Hold);
    CHECK(held.valueAt(secs(1.5)) == Approx(50.0)); // the step after the Hold key is untouched

    // Several keys at once: every segment after a listed key.
    Curve many;
    for (int i = 0; i < 5; ++i)
        many.keys.push_back(Keyframe{secs(i), 10.0 * i, Interpolation::Linear, {}, {}});
    const std::vector<std::size_t> indices{0, 2, 4}; // 4 is the last key: no segment
    CHECK(edit::applyEasing(many, indices, *edit::findEasingPreset("easeIn")) == 2);
    CHECK(many.valueAt(secs(0.5)) < 5.0);
    CHECK(many.valueAt(secs(1.5)) == Approx(15.0).margin(1e-6)); // segment 1 untouched
    CHECK(many.valueAt(secs(2.5)) < 25.0);
    // Two keys on the same time cannot be eased.
    Curve flat = twoKeys(1.0, 1.0);
    CHECK(edit::applyEasing(flat, 0, *edit::findEasingPreset("easeIn")));
    CHECK(flat.valueAt(secs(1)) == Approx(1.0));
}

TEST_CASE("Motion Blur: the shutter's instants are centred by default and 0 means no blur", "[model][motionblur][a10]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kMotionBlurEffect);
    REQUIRE(d != nullptr);
    CHECK(d->displayName == "Motion Blur");
    CHECK(d->category == "Blur & Sharpen");
    CHECK(d->kind == EffectKind::Video);
    REQUIRE(d->params.size() == 4);
    CHECK(d->param("shutterAngle")->defaultValue == 180.0);
    CHECK(d->param("shutterPhase")->defaultValue == -90.0);
    CHECK(d->param("samples")->defaultValue == 16.0);
    CHECK(d->param("previewSamples")->defaultValue == 8.0);

    ShutterSettings s;
    const std::vector<double> offsets = shutterOffsets(s, 4);
    REQUIRE(offsets.size() == 4);
    // 180 degrees centred on the frame: from -1/4 to +1/4 of a frame, the middles of four equal parts.
    CHECK(offsets[0] == Approx(-0.1875));
    CHECK(offsets[3] == Approx(0.1875));
    CHECK(offsets[0] + offsets[3] == Approx(0.0).margin(1e-12));
    CHECK(std::is_sorted(offsets.begin(), offsets.end()));
    // 360 degrees opening at the frame instant covers the whole frame after it.
    s.angle = 360.0;
    s.phase = 0.0;
    const std::vector<double> full = shutterOffsets(s, 2);
    CHECK(full[0] == Approx(0.25));
    CHECK(full[1] == Approx(0.75));
    // No angle: the one instant 0; no instants asked for: none.
    s.angle = 0.0;
    CHECK(shutterOffsets(s, 9) == std::vector<double>{0.0});
    CHECK(shutterOffsets(ShutterSettings{}, 0).empty());
}

TEST_CASE("Motion Blur: the clip's blur is its first enabled Motion Blur with an angle, and how many instants it needs", "[model][motionblur][a10]") {
    const auto instance = [](std::uint64_t id, double angle, bool enabled = true) {
        EffectInstance e;
        e.id = EffectInstanceId{id};
        e.effect = std::string(kMotionBlurEffect);
        e.enabled = enabled;
        e.set("shutterAngle", ParamValue(angle));
        return e;
    };
    EffectStack none;
    CHECK_FALSE(motionBlurOf(none, Ticks{}).has_value());
    EffectStack stack{instance(1, 90.0)};
    const auto found = motionBlurOf(stack, Ticks{});
    REQUIRE(found.has_value());
    CHECK(found->angle == 90.0);
    CHECK(found->phase == -90.0); // the default
    CHECK(found->samples == 16);
    // An angle of 0 turns it off; so does a disabled instance; the first instance decides.
    CHECK_FALSE(motionBlurOf(EffectStack{instance(1, 0.0)}, Ticks{}).has_value());
    CHECK_FALSE(motionBlurOf(EffectStack{instance(1, 90.0, false)}, Ticks{}).has_value());
    CHECK_FALSE(motionBlurOf(EffectStack{instance(1, 0.0), instance(2, 90.0)}, Ticks{}).has_value());
    CHECK(motionBlurOf(EffectStack{instance(1, 90.0, false), instance(2, 45.0)}, Ticks{})->angle == 45.0);
    // Ranges: an angle past 720 is held at 720.
    CHECK(motionBlurOf(EffectStack{instance(1, 5000.0)}, Ticks{})->angle == 720.0);

    // The angle can be keyed: off at the start, on later.
    EffectInstance keyed = instance(3, 0.0);
    keyed.set("shutterAngle", ParamValue(Curve{{Keyframe{Ticks{}, 0.0}, Keyframe{secs(1), 180.0}}}));
    CHECK_FALSE(motionBlurOf(EffectStack{keyed}, Ticks{}).has_value());
    CHECK(motionBlurOf(EffectStack{keyed}, secs(1))->angle == Approx(180.0));

    // Instants: none for a layer that hardly moves, one per pixel and a half, at most the quality.
    ShutterSettings s;
    CHECK(blurSampleCount(s, 0.0, false) == 1);
    CHECK(blurSampleCount(s, 0.2, false) == 1);
    CHECK(blurSampleCount(s, 0.3, false) == 2);
    CHECK(blurSampleCount(s, 6.0, false) == 5);
    CHECK(blurSampleCount(s, 500.0, false) == 16); // export quality
    CHECK(blurSampleCount(s, 500.0, true) == 8);   // preview quality
    s.previewSamples = 40;
    CHECK(blurSampleCount(s, 500.0, true) == 16);  // never above the export quality
    s.angle = 0.0;
    CHECK(blurSampleCount(s, 500.0, false) == 1);
}
