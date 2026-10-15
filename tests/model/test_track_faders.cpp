// A.3: the track fader and panner (Track::volume / Track::pan) and the master fader (Sequence::masterVolume): the
// target-taking parameter and keyframe operations, validation, change detection, and project files (absent until
// set: other projects stay byte-identical).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using edit::EffectTarget;
using edit::ParamRef;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
    A2     |Mmmmmmmm|
)";

const Track& trackIn(const ProjectState& state, SequenceId seq, TrackId track) {
    return *state.findSequence(seq)->findTrack(track);
}

Curve keys(std::initializer_list<std::pair<std::int64_t, double>> list) {
    Curve c;
    for (const auto& [frame, v] : list) {
        c.keys.push_back(Keyframe{f(frame), v});
    }
    return c;
}

} // namespace

TEST_CASE("Track and master faders: set, keyframe, move and remove through the target operations", "[model][faders]") {
    Timeline tl(kPicture);
    const auto a1 = EffectTarget::ofTrack(tl.track("A1"));
    const auto volume = ParamRef::of(ParamId::VolumeDb);
    const auto pan = ParamRef::of(ParamId::Pan);

    auto r = ok(edit::setParameter(tl.state, tl.seq, a1, volume, ParamValue(-6.0)));
    CHECK(trackIn(r.state, tl.seq, tl.track("A1")).volume == ParamValue(-6.0));
    CHECK(trackIn(r.state, tl.seq, tl.track("A2")).volume == ParamValue()); // untouched
    r = ok(edit::setParameter(r.state, tl.seq, a1, pan, ParamValue(-0.25)));
    CHECK(trackIn(r.state, tl.seq, tl.track("A1")).pan == ParamValue(-0.25));

    // Automation: keys in timeline time; the stopwatch on a constant starts a curve holding it.
    r = ok(edit::addKeyframe(r.state, tl.seq, a1, volume, Keyframe{f(2), -12.0}));
    r = ok(edit::addKeyframe(r.state, tl.seq, a1, volume, Keyframe{f(6), 0.0}));
    const Curve* curve = trackIn(r.state, tl.seq, tl.track("A1")).volume.curve();
    REQUIRE(curve);
    REQUIRE(curve->keys.size() == 2);
    CHECK(trackIn(r.state, tl.seq, tl.track("A1")).volume.valueAt(f(4)) == -6.0);
    r = ok(edit::moveKeyframe(r.state, tl.seq, a1, volume, 1, f(8)));
    CHECK(trackIn(r.state, tl.seq, tl.track("A1")).volume.curve()->keys[1].time == f(8));
    r = ok(edit::removeKeyframe(r.state, tl.seq, a1, volume, 0));
    r = ok(edit::removeKeyframe(r.state, tl.seq, a1, volume, 0)); // the last one leaves its value as a constant
    CHECK(trackIn(r.state, tl.seq, tl.track("A1")).volume == ParamValue(0.0));

    // The master fader.
    r = ok(edit::setParameter(r.state, tl.seq, EffectTarget::masterAudio(), volume, ParamValue(-1.5)));
    CHECK(r.state.findSequence(tl.seq)->masterVolume == ParamValue(-1.5));
    r = ok(edit::setAnimated(r.state, tl.seq, EffectTarget::masterAudio(), volume, true, f(3)));
    REQUIRE(r.state.findSequence(tl.seq)->masterVolume.curve());
    CHECK(r.state.findSequence(tl.seq)->masterVolume.curve()->keys[0].value == -1.5);
    CHECK(validate(r.state, Validation::Structure));

    // What is not a fader.
    CHECK_FALSE(edit::setParameter(tl.state, tl.seq, EffectTarget::masterAudio(), pan, ParamValue(0.5)));     // no master pan
    CHECK_FALSE(edit::setParameter(tl.state, tl.seq, EffectTarget::masterVideo(), volume, ParamValue(-3.0)));
    CHECK_FALSE(edit::setParameter(tl.state, tl.seq, EffectTarget::ofTrack(tl.track("V1")), volume, ParamValue(-3.0)));
    CHECK_FALSE(edit::setParameter(tl.state, tl.seq, a1, ParamRef::of(ParamId::Opacity), ParamValue(0.5)));
    CHECK_FALSE(edit::setParameter(tl.state, tl.seq, a1, volume, ParamValue(30.0)));  // +24 dB at most
    CHECK_FALSE(edit::setParameter(tl.state, tl.seq, a1, pan, ParamValue(-1.5)));
    edit::TrackStateChange lock;
    lock.locked = true;
    const auto locked = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), lock));
    CHECK_FALSE(edit::setParameter(locked.state, tl.seq, a1, volume, ParamValue(-3.0)));
    // Setting what is already there changes nothing (no undo step).
    const auto same = edit::setParameter(tl.state, tl.seq, a1, volume, ParamValue(0.0));
    REQUIRE(same);
    CHECK(same->state.findSequence(tl.seq) == tl.state.findSequence(tl.seq));
}

TEST_CASE("Fader changes: a track header change and a sequence setting in the ChangeSet", "[model][faders][changeset]") {
    Timeline tl(kPicture);
    const auto r = ok(edit::setParameter(tl.state, tl.seq, EffectTarget::ofTrack(tl.track("A2")), ParamRef::of(ParamId::Pan),
                                         ParamValue(0.4)));
    const ChangeSet changes = diff(tl.state, r.state);
    const SequenceChange* seq = changes.sequence(tl.seq);
    REQUIRE(seq);
    const TrackChange* track = seq->track(tl.track("A2"));
    REQUIRE(track);
    CHECK(track->headerChanged);
    CHECK(track->ranges.empty());
    CHECK(seq->track(tl.track("A1")) == nullptr);
    const auto master = ok(edit::setParameter(tl.state, tl.seq, EffectTarget::masterAudio(), ParamRef::of(ParamId::VolumeDb),
                                              ParamValue(-2.0)));
    CHECK(diff(tl.state, master.state).sequence(tl.seq)->settingsChanged);
}

TEST_CASE("Fader validation: in range, well formed, audio tracks only", "[model][faders][validate]") {
    Timeline tl(kPicture);
    const auto withTrack = [&](const char* label, auto change) {
        Sequence seq = *tl.state.findSequence(tl.seq);
        for (TrackPtr& t : label[0] == 'V' ? seq.videoTracks : seq.audioTracks) {
            if (t->id == tl.track(label)) {
                Track copy = *t;
                change(copy);
                t = std::make_shared<const Track>(std::move(copy));
            }
        }
        return tl.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    };
    CHECK(validate(withTrack("A1", [](Track& t) { t.volume = ParamValue(keys({{0, -60.0}, {4, 6.0}})); }), Validation::Structure));
    CHECK_FALSE(validate(withTrack("A1", [](Track& t) { t.volume = ParamValue(25.0); }), Validation::Structure));
    CHECK_FALSE(validate(withTrack("A1", [](Track& t) { t.pan = ParamValue(keys({{0, 0.0}, {4, 2.0}})); }), Validation::Structure));
    CHECK_FALSE(validate(withTrack("A1", [](Track& t) { t.volume = ParamValue(keys({{4, 0.0}, {2, 0.0}})); }), Validation::Structure));
    CHECK_FALSE(validate(withTrack("V1", [](Track& t) { t.volume = ParamValue(-6.0); }), Validation::Structure));
    Sequence seq = *tl.state.findSequence(tl.seq);
    seq.masterVolume = ParamValue(-200.0);
    CHECK_FALSE(validate(tl.state.withSequence(std::make_shared<const Sequence>(std::move(seq))), Validation::Structure));
}

TEST_CASE("Fader project files: absent until set (old projects stay byte-identical), exact round trips",
          "[model][faders][io]") {
    Timeline tl(kPicture);
    // A project without faders: saving it again gives the same bytes, and no fader key appears.
    const auto original = io::toJson(tl.state);
    REQUIRE(original);
    CHECK(original->find("\"volume\"") == std::string::npos);
    CHECK(original->find("\"pan\"") == std::string::npos);
    CHECK(original->find("\"masterVolume\"") == std::string::npos);
    const auto loaded = io::fromJson(*original);
    REQUIRE(loaded);
    CHECK(*io::toJson(*loaded) == *original);
    CHECK(trackIn(*loaded, tl.seq, tl.track("A1")).volume == ParamValue());
    CHECK(loaded->findSequence(tl.seq)->masterVolume == ParamValue());

    // Faders and automation round-trip exactly.
    auto r = ok(edit::setParameter(tl.state, tl.seq, EffectTarget::ofTrack(tl.track("A1")), ParamRef::of(ParamId::VolumeDb),
                                   ParamValue(keys({{0, -3.0}, {5, -18.5}}))));
    r = ok(edit::setParameter(r.state, tl.seq, EffectTarget::ofTrack(tl.track("A2")), ParamRef::of(ParamId::Pan), ParamValue(0.75)));
    r = ok(edit::setParameter(r.state, tl.seq, EffectTarget::masterAudio(), ParamRef::of(ParamId::VolumeDb), ParamValue(-0.5)));
    const auto saved = io::toJson(r.state);
    REQUIRE(saved);
    CHECK(saved->find("\"masterVolume\"") != std::string::npos);
    const auto back = io::fromJson(*saved);
    REQUIRE(back);
    CHECK(trackIn(*back, tl.seq, tl.track("A1")).volume == trackIn(r.state, tl.seq, tl.track("A1")).volume);
    CHECK(trackIn(*back, tl.seq, tl.track("A2")).pan == ParamValue(0.75));
    CHECK(back->findSequence(tl.seq)->masterVolume == ParamValue(-0.5));
    CHECK(*io::toJson(*back) == *saved);
    // The schema did not change: older readers ignore the keys (they read 0 dB / centre).
    CHECK(saved->find("\"schemaVersion\": " + std::to_string(io::kSchemaVersion)) != std::string::npos);

    // A malformed fader in a file is rejected, not half-loaded.
    std::string bad = *saved;
    const std::string pan = "\"pan\": {\n";
    const auto at = bad.find(pan);
    REQUIRE(at != std::string::npos);
    bad.replace(bad.find("0.75", at), 4, "7.5");
    CHECK_FALSE(io::fromJson(bad));
}
