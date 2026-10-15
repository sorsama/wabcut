// Audio track roles and the effect stacks of tracks and the master buses (EffectTarget operations).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::EffectTarget;
using edit::ParamRef;

namespace {

const Track& trackIn(const ProjectState& state, SequenceId seq, TrackId track) {
    return *state.findSequence(seq)->findTrack(track);
}

const Sequence& sequenceIn(const ProjectState& state, SequenceId seq) { return *state.findSequence(seq); }

} // namespace

TEST_CASE("Audio tracks carry a role: dialogue, music or effects", "[model][roles]") {
    Timeline tl(R"(
        V1 tgt          |Aaaa|
        A1 tgt dialogue |Aaaa|
        A2 music        |Mmmm|
        A3              |....|
    )");
    CHECK(trackIn(tl.state, tl.seq, tl.track("A1")).role == TrackRole::Dialogue);
    CHECK(trackIn(tl.state, tl.seq, tl.track("A2")).role == TrackRole::Music);
    CHECK(trackIn(tl.state, tl.seq, tl.track("A3")).role == TrackRole::None);
    CHECK(trackIn(tl.state, tl.seq, tl.track("V1")).role == TrackRole::None);

    for (const TrackRole role : {TrackRole::None, TrackRole::Dialogue, TrackRole::Music, TrackRole::Effects}) {
        CHECK(trackRoleFromString(toString(role)) == role);
    }
    CHECK_FALSE(trackRoleFromString("Dialogue"));
    CHECK_FALSE(trackRoleFromString(""));

    SECTION("setTrackState changes it as a header change, also on a locked track") {
        edit::TrackStateChange change;
        change.locked = true;
        const auto locked = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A3"), change));
        edit::TrackStateChange role;
        role.role = TrackRole::Effects;
        const auto r = ok(edit::setTrackState(locked.state, tl.seq, tl.track("A3"), role));
        CHECK(trackIn(r.state, tl.seq, tl.track("A3")).role == TrackRole::Effects);
        const SequenceChange* change3 = r.changes.sequence(tl.seq);
        REQUIRE(change3);
        REQUIRE(change3->track(tl.track("A3")));
        CHECK(change3->track(tl.track("A3"))->headerChanged);
        CHECK(change3->track(tl.track("A3"))->ranges.empty());
        CHECK(diff(locked.state, r.state).sequence(tl.seq)->track(tl.track("A3"))->headerChanged);
    }
    SECTION("video tracks have no role") {
        edit::TrackStateChange role;
        role.role = TrackRole::Dialogue;
        CHECK_FALSE(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), role));
        role.role = TrackRole::None;
        CHECK(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), role)); // clearing is always fine

        auto seq = std::make_shared<Sequence>(sequenceIn(tl.state, tl.seq));
        auto v1 = std::make_shared<Track>(*seq->videoTracks[0]);
        v1->role = TrackRole::Music;
        seq->videoTracks[0] = v1;
        const ProjectState bad = tl.state.withSequence(seq);
        CHECK_FALSE(validate(bad, Validation::Structure));
    }
    SECTION("roles are saved only when set and load back") {
        const auto text = io::toJson(tl.state);
        REQUIRE(text);
        CHECK(text->find("\"role\": \"dialogue\"") != std::string::npos);
        CHECK(text->find("\"role\": \"music\"") != std::string::npos);
        CHECK(text->find("\"role\": \"none\"") == std::string::npos);
        const auto loaded = io::fromJson(*text);
        REQUIRE(loaded);
        CHECK(*loaded == tl.state);
    }
}

TEST_CASE("Track effect stacks: add, set, animate, move, remove", "[model][effects][track]") {
    Timeline tl(R"(
        V1 tgt   |Aaaaaaaa|
        A1 tgt   |Aaaaaaaa|
        A2 lock  |Bbbbbbbb|
    )");
    const EffectTarget a1 = EffectTarget::ofTrack(tl.track("A1"));
    auto r = ok(edit::addEffect(tl.state, tl.seq, a1, "wabcut.audio.compressor", {{"ratio", ParamValue(8.0)}}));
    REQUIRE(r.createdEffects.size() == 1);
    const EffectInstanceId comp = r.createdEffects[0];
    {
        const Track& t = trackIn(r.state, tl.seq, tl.track("A1"));
        REQUIRE(t.effects.size() == 1);
        CHECK(t.effects[0].effect == "wabcut.audio.compressor");
        CHECK(effectParameterAt(t.effects[0], "ratio", Ticks{}) == 8.0);
        CHECK(effectParameterAt(t.effects[0], "threshold", Ticks{}) == -20.0); // default
        CHECK(isEvaluable(t.effects[0], EffectKind::Audio));
        // Clips are untouched; the change is a header change of A1.
        CHECK(trackIn(r.state, tl.seq, tl.track("A1")).clips == trackIn(tl.state, tl.seq, tl.track("A1")).clips);
        const TrackChange* change = r.changes.sequence(tl.seq)->track(tl.track("A1"));
        REQUIRE(change);
        CHECK(change->headerChanged);
    }
    r = ok(edit::addEffect(r.state, tl.seq, a1, "wabcut.audio.eq3", {}, 0));
    const EffectInstanceId eq = r.createdEffects[0];
    CHECK(edit::effectStackOf(sequenceIn(r.state, tl.seq), a1)->front().id == eq);

    SECTION("parameters and keyframes in timeline time") {
        edit::EffectChange change;
        change.enabled = false;
        change.params = {{"midGain", ParamValue(6.0)}};
        auto set = ok(edit::setEffect(r.state, tl.seq, a1, eq, change));
        const EffectInstance& e = trackIn(set.state, tl.seq, tl.track("A1")).effects[0];
        CHECK_FALSE(e.enabled);
        CHECK(e.find("midGain")->constant() == 6.0);

        const ParamRef threshold = ParamRef::ofEffect(comp, "threshold");
        set = ok(edit::addKeyframe(set.state, tl.seq, a1, threshold, Keyframe{f(2), -30.0}));
        set = ok(edit::addKeyframe(set.state, tl.seq, a1, threshold, Keyframe{f(6), -10.0}));
        const EffectInstance& c = trackIn(set.state, tl.seq, tl.track("A1")).effects[1];
        REQUIRE(c.find("threshold")->isCurve());
        CHECK(effectParameterAt(c, "threshold", f(4)) == -20.0);
        set = ok(edit::moveKeyframe(set.state, tl.seq, a1, threshold, 1, f(7)));
        set = ok(edit::setKeyframe(set.state, tl.seq, a1, threshold, 0,
                                   edit::KeyframeChange{-40.0, Interpolation::Hold, std::nullopt, std::nullopt}));
        CHECK(effectParameterAt(trackIn(set.state, tl.seq, tl.track("A1")).effects[1], "threshold", f(6)) == -40.0);
        set = ok(edit::removeKeyframe(set.state, tl.seq, a1, threshold, 0));
        set = ok(edit::setAnimated(set.state, tl.seq, a1, threshold, false, f(0)));
        CHECK(trackIn(set.state, tl.seq, tl.track("A1")).effects[1].find("threshold")->isConstant());
        set = ok(edit::setParameter(set.state, tl.seq, a1, ParamRef::ofEffect(comp, "makeup"), ParamValue(3.0)));
        CHECK(effectParameterAt(trackIn(set.state, tl.seq, tl.track("A1")).effects[1], "makeup", Ticks{}) == 3.0);

        CHECK_FALSE(edit::setParameter(r.state, tl.seq, a1, ParamRef::ofEffect(comp, "ratio"), ParamValue(50.0)));
        CHECK_FALSE(edit::setParameter(r.state, tl.seq, a1, ParamRef::ofEffect(comp, "bogus"), ParamValue(1.0)));
        // A.3: a track's intrinsic parameters are its fader (volume, pan; test_track_faders.cpp); others stay clip-only.
        CHECK(edit::setParameter(r.state, tl.seq, a1, ParamRef::of(ParamId::VolumeDb), ParamValue(-6.0)));
        CHECK_FALSE(edit::setParameter(r.state, tl.seq, a1, ParamRef::of(ParamId::Opacity), ParamValue(0.5)));
        CHECK_FALSE(edit::setParameter(r.state, tl.seq, a1, ParamRef::ofEffect(EffectInstanceId{9999}, "ratio"),
                                       ParamValue(2.0)));
    }
    SECTION("move and remove") {
        auto moved = ok(edit::moveEffect(r.state, tl.seq, a1, eq, 9));
        CHECK(trackIn(moved.state, tl.seq, tl.track("A1")).effects[1].id == eq);
        auto removed = ok(edit::removeEffect(moved.state, tl.seq, a1, comp));
        CHECK(trackIn(removed.state, tl.seq, tl.track("A1")).effects.size() == 1);
        CHECK_FALSE(edit::removeEffect(removed.state, tl.seq, a1, comp));
        const auto unchanged = ok(edit::moveEffect(removed.state, tl.seq, a1, eq, 0));
        CHECK(unchanged.state == removed.state);
    }
    SECTION("kinds, locks and unknown tracks are checked") {
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, a1, "wabcut.blur.gaussian"));
        CHECK(edit::addEffect(tl.state, tl.seq, EffectTarget::ofTrack(tl.track("V1")), "wabcut.blur.gaussian"));
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, EffectTarget::ofTrack(tl.track("V1")), "wabcut.audio.gain"));
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, EffectTarget::ofTrack(tl.track("A2")), "wabcut.audio.gain"));
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, EffectTarget::ofTrack(TrackId{4242}), "wabcut.audio.gain"));
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, a1, "wabcut.volume")); // intrinsic
        CHECK_FALSE(edit::addEffect(tl.state, tl.seq, a1, "constantPower")); // a transition
    }
    SECTION("a clip target is the clip operation") {
        const ClipId clip = tl.clip("A1@0");
        const auto viaTarget = ok(edit::addEffect(tl.state, tl.seq, EffectTarget::ofClip(clip), "wabcut.audio.gain",
                                                  {{"gain", ParamValue(-3.0)}}));
        const auto viaClip =
            ok(edit::addEffect(tl.state, tl.seq, std::vector<ClipId>{clip}, "wabcut.audio.gain", {{"gain", ParamValue(-3.0)}}));
        CHECK(viaTarget.state == viaClip.state);
        CHECK(edit::effectStackOf(sequenceIn(viaTarget.state, tl.seq), EffectTarget::ofClip(clip))->size() == 1);
        CHECK_FALSE(edit::effectStackOf(sequenceIn(viaTarget.state, tl.seq), EffectTarget::ofClip(ClipId{4242})));
    }
}

TEST_CASE("The master audio bus has its own effect stack", "[model][effects][master]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )");
    const EffectTarget master = EffectTarget::masterAudio();
    auto r = ok(edit::addEffect(tl.state, tl.seq, master, "wabcut.audio.limiter", {{"ceiling", ParamValue(-1.0)}}));
    REQUIRE(r.createdEffects.size() == 1);
    const Sequence& seq = sequenceIn(r.state, tl.seq);
    REQUIRE(seq.audioEffects.size() == 1);
    CHECK(seq.effects.empty());
    CHECK(r.changes.sequence(tl.seq)->settingsChanged);
    CHECK(diff(tl.state, r.state).sequence(tl.seq)->settingsChanged);

    CHECK_FALSE(edit::addEffect(tl.state, tl.seq, master, "wabcut.vignette"));
    CHECK(edit::addEffect(tl.state, tl.seq, EffectTarget::masterVideo(), "wabcut.vignette"));
    CHECK_FALSE(edit::addEffect(tl.state, tl.seq, EffectTarget::masterVideo(), "wabcut.audio.limiter"));
    CHECK(edit::addEffect(r.state, tl.seq, master, "wabcut.audio.denoise", {{"amount", ParamValue(0.5)}}));

    // Keyframes on master effects are timeline times.
    const ParamRef ceiling = ParamRef::ofEffect(r.createdEffects[0], "ceiling");
    auto k = ok(edit::addKeyframe(r.state, tl.seq, master, ceiling, Keyframe{f(1), -3.0}));
    k = ok(edit::addKeyframe(k.state, tl.seq, master, ceiling, Keyframe{f(3), -1.0}));
    CHECK(effectParameterAt(sequenceIn(k.state, tl.seq).audioEffects[0], "ceiling", f(2)) == -2.0);
    // Lookahead is a constant.
    CHECK_FALSE(edit::addKeyframe(r.state, tl.seq, master, ParamRef::ofEffect(r.createdEffects[0], "lookahead"),
                                  Keyframe{f(1), 2.0}));

    SECTION("saved and validated") {
        const auto text = io::toJson(k.state);
        REQUIRE(text);
        CHECK(text->find("\"audioEffects\"") != std::string::npos);
        const auto loaded = io::fromJson(*text);
        REQUIRE(loaded);
        CHECK(*loaded == k.state);
        // Files without the field (schema 4 and older) load with an empty stack and are not written with one.
        const auto plain = io::toJson(tl.state);
        REQUIRE(plain);
        CHECK(plain->find("\"audioEffects\"") == std::string::npos);

        auto bad = std::make_shared<Sequence>(sequenceIn(tl.state, tl.seq));
        ProjectState state = tl.state;
        bad->audioEffects = {EffectInstance{state.ids.allocate<EffectInstanceId>(), "wabcut.vignette", 1, true, {}, {}}};
        state = state.withSequence(bad);
        CHECK_FALSE(validate(state, Validation::Structure));
    }
}

TEST_CASE("Reduce Noise is an audio effect with an amount", "[model][effects]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find("wabcut.audio.denoise");
    REQUIRE(d);
    CHECK(d->kind == EffectKind::Audio);
    CHECK_FALSE(d->intrinsic);
    CHECK(d->capabilities.has(EffectCapability::AudioStateful));
    REQUIRE(d->param("amount"));
    CHECK(d->param("amount")->defaultValue == 1.0);
    CHECK(d->param("amount")->unit == ParamUnit::Fraction);
}
