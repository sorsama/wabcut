// Effect stacks: the registry, the stack operations, validation, and round trips that keep unknown effects.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

std::vector<ClipId> one(ClipId id) { return {id}; }

} // namespace

TEST_CASE("The registry describes the finishing shelf", "[model][effects]") {
    const EffectRegistry& registry = EffectRegistry::builtin();
    for (const char* id : {"wabcut.motion", "wabcut.opacity", "wabcut.volume", "wabcut.panner", "wabcut.crop",
                           "wabcut.blur.gaussian", "wabcut.color.basic", "wabcut.vignette", "wabcut.sharpen",
                           "wabcut.dropShadow", "wabcut.audio.gain", "wabcut.audio.eq3", "wabcut.audio.compressor",
                           "wabcut.audio.limiter", "crossDissolve", "dipToBlack", "dipToWhite", "wipe",
                           "constantPower", "constantGain"}) {
        INFO(id);
        CHECK(registry.find(id));
    }
    std::set<std::string_view> ids;
    for (const EffectDescriptor& d : registry.all()) {
        INFO(d.id);
        CHECK(ids.insert(d.id).second);
        CHECK(d.version >= 1);
        CHECK_FALSE(d.displayName.empty());
        CHECK_FALSE(d.description.empty());
        std::set<std::string_view> keys;
        for (const EffectParamInfo& p : d.params) {
            CHECK(keys.insert(p.key).second);
            CHECK(p.minValue <= p.defaultValue);
            CHECK(p.defaultValue <= p.maxValue);
        }
        if (d.intrinsic) {
            // Intrinsic parameters are exactly the clip parameter table's keys.
            for (const EffectParamInfo& p : d.params) {
                REQUIRE(parameterByKey(p.key));
                CHECK(parameterInfo(*parameterByKey(p.key)).defaultValue == p.defaultValue);
            }
        }
    }
    CHECK(registry.find("wabcut.audio.limiter")->capabilities.has(EffectCapability::AudioLookahead));
    CHECK(registry.find("wabcut.blur.gaussian")->capabilities.has(EffectCapability::Neighborhood));
    CHECK_FALSE(registry.find("vendor.unknown"));
}

TEST_CASE("Effect stacks: add, set, move, remove", "[model][effects]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
    )");
    const ClipId video = tl.clip("V1@0");
    auto r = ok(edit::addEffect(tl.state, tl.seq, one(video), "wabcut.blur.gaussian", {{"radius", ParamValue(12.0)}}));
    REQUIRE(r.createdEffects.size() == 1);
    const EffectInstanceId blur = r.createdEffects[0];
    CHECK(r.changes.sequences[0].roles.has(PropertyRole::Effects));
    {
        const Clip& c = clipIn(r.state, tl.seq, "V1@0");
        REQUIRE(c.effects().size() == 1);
        CHECK(c.effects()[0].effect == "wabcut.blur.gaussian");
        CHECK(effectParameterAt(c.effects()[0], "radius", Ticks{}) == 12.0);
        CHECK(effectParameterAt(c.effects()[0], "repeatEdges", Ticks{}) == 1.0); // descriptor default
        CHECK(isEvaluable(c.effects()[0], EffectKind::Video));
    }
    r = ok(edit::addEffect(r.state, tl.seq, one(video), "wabcut.color.basic", {}, 0));
    const EffectInstanceId color = r.createdEffects[0];
    CHECK(clipIn(r.state, tl.seq, "V1@0").effects()[0].id == color); // inserted first

    SECTION("set parameters and enable state; animate an effect parameter") {
        edit::EffectChange change;
        change.enabled = false;
        change.params = {{"saturation", ParamValue(0.0)}};
        auto set = ok(edit::setEffect(r.state, tl.seq, video, color, change));
        const EffectInstance& e = clipIn(set.state, tl.seq, "V1@0").effects()[0];
        CHECK_FALSE(e.enabled);
        CHECK(e.find("saturation")->constant() == 0.0);
        set = ok(edit::addKeyframe(set.state, tl.seq, video, edit::ParamRef::ofEffect(blur, "radius"),
                                   Keyframe{f(4), 30.0}));
        const EffectInstance& b = clipIn(set.state, tl.seq, "V1@0").effects()[1];
        CHECK(b.find("radius")->isCurve());
        change.params = {{"saturation", ParamValue(500.0)}};
        CHECK_FALSE(edit::setEffect(r.state, tl.seq, video, color, change)); // out of range
        change.params = {{"bogus", ParamValue(1.0)}};
        CHECK_FALSE(edit::setEffect(r.state, tl.seq, video, color, change));
        // repeatEdges is a switch: constants only.
        CHECK_FALSE(edit::addKeyframe(set.state, tl.seq, video, edit::ParamRef::ofEffect(blur, "repeatEdges"),
                                      Keyframe{f(1), 0.0}));
    }
    SECTION("move and remove") {
        auto moved = ok(edit::moveEffect(r.state, tl.seq, video, color, 5));
        CHECK(clipIn(moved.state, tl.seq, "V1@0").effects()[1].id == color);
        auto removed = ok(edit::removeEffect(moved.state, tl.seq, video, blur));
        CHECK(clipIn(removed.state, tl.seq, "V1@0").effects().size() == 1);
        CHECK_FALSE(edit::removeEffect(removed.state, tl.seq, video, blur));
    }
    SECTION("effects belong to the right kind of clip") {
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, one(tl.clip("A1@0")), "wabcut.blur.gaussian"));
        CHECK(edit::addEffect(tl.state, tl.seq, one(tl.clip("A1@0")), "wabcut.audio.compressor"));
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, one(video), "wabcut.opacity")); // intrinsic
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, one(video), "crossDissolve")); // a transition
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, one(video), "vendor.glow"));
    }
    SECTION("a razor cut shares the stack until one half changes") {
        const auto cut = ok(edit::addEdit(r.state, tl.seq, f(4), std::vector<TrackId>{tl.track("V1")}));
        CHECK(clipIn(cut.state, tl.seq, "V1@0").effects() == clipIn(cut.state, tl.seq, "V1@4").effects());
        const auto off = ok(edit::setEffect(cut.state, tl.seq, clipAt(cut.state, tl.seq, "V1@4"), blur,
                                            edit::EffectChange{false, {}}));
        CHECK(clipIn(off.state, tl.seq, "V1@0").effects()[1].enabled);
        CHECK_FALSE(clipIn(off.state, tl.seq, "V1@4").effects()[1].enabled);
    }
}

TEST_CASE("Unknown effects are kept, bypassed and saved unchanged", "[model][effects][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectState state = tl.state;
    EffectInstance glow;
    glow.id = state.ids.allocate<EffectInstanceId>();
    glow.effect = "vendor.glow";
    glow.version = 3;
    glow.set("amount", ParamValue(5.0));
    glow.set("pulse", ParamValue(Curve{{Keyframe{f(0), 0.0}, Keyframe{f(3), 1e6}}}));
    glow.mask = "mask:7";
    EffectInstance future;
    future.id = state.ids.allocate<EffectInstanceId>();
    future.effect = "wabcut.blur.gaussian";
    future.version = 99; // newer than this build knows
    future.set("radius", ParamValue(5000.0)); // outside today's range: not checked for a newer version
    future.set("quality", ParamValue(2.0));
    {
        auto seq = std::make_shared<Sequence>(*state.findSequence(tl.seq));
        auto track = std::make_shared<Track>(*seq->videoTracks[0]);
        track->clips.mutableAt(0).setEffects({glow, future});
        track->effects = {EffectInstance{state.ids.allocate<EffectInstanceId>(), "wabcut.vignette", 1, true, {}, {}}};
        seq->effects = {EffectInstance{state.ids.allocate<EffectInstanceId>(), "vendor.lut", 1, true, {}, {}}};
        seq->videoTracks[0] = track;
        state = state.withSequence(seq);
    }
    requireValid(state);
    const Clip& c = clipIn(state, tl.seq, "V1@0");
    CHECK_FALSE(isEvaluable(c.effects()[0], EffectKind::Video));
    CHECK_FALSE(isEvaluable(c.effects()[1], EffectKind::Video));
    CHECK(effectParameterAt(c.effects()[0], "missing", Ticks{}, -1.0) == -1.0);

    const auto text = io::toJson(state);
    REQUIRE(text);
    CHECK(text->find("vendor.glow") != std::string::npos);
    CHECK(text->find("mask:7") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == state);

    // Unknown effects can be disabled or removed, but their parameters stay as they are.
    CHECK(edit::setEffect(state, tl.seq, c.id, glow.id, edit::EffectChange{false, {}}));
    CHECK_FALSE(edit::setEffect(state, tl.seq, c.id, glow.id, edit::EffectChange{std::nullopt, {{"amount", ParamValue(1.0)}}}));
    CHECK(edit::removeEffect(state, tl.seq, c.id, glow.id));
}

TEST_CASE("Effect stacks are validated", "[model][effects]") {
    const auto problem = [](EffectStack stack, EffectKind kind) { return effectStackProblem(stack, kind, 100); };
    const auto instance = [](std::uint64_t id, std::string effect) {
        EffectInstance e;
        e.id = EffectInstanceId{id};
        e.effect = std::move(effect);
        return e;
    };
    CHECK_FALSE(problem({instance(1, "wabcut.crop"), instance(2, "vendor.x")}, EffectKind::Video));
    CHECK(problem({instance(1, "wabcut.crop"), instance(1, "wabcut.sharpen")}, EffectKind::Video)); // duplicate id
    CHECK(problem({instance(100, "wabcut.crop")}, EffectKind::Video));                              // not below ids.next
    CHECK(problem({instance(1, "wabcut.opacity")}, EffectKind::Video));                             // intrinsic
    CHECK(problem({instance(1, "wabcut.crop")}, EffectKind::Audio));                                // wrong kind
    CHECK(problem({instance(1, "")}, EffectKind::Video));
    EffectInstance crop = instance(1, "wabcut.crop");
    crop.set("left", ParamValue(2.0));
    CHECK(problem({crop}, EffectKind::Video)); // out of range
    EffectInstance unsorted = instance(1, "vendor.x");
    unsorted.params = {{"b", ParamValue(1.0)}, {"a", ParamValue(1.0)}};
    CHECK(problem({unsorted}, EffectKind::Video));
}
