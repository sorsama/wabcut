// The audio commands through the CommandEngine, with fake analysis: roles, track and master stacks, loudness
// normalisation (measured and refined), the master limiter and auto-ducking (one undo step, dryRun preview).

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/audio/edit/Loudness.h>
#include <wabcut/command/Schema.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

Ticks s(double seconds) { return Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))}; }

/// Loudness as a simple model of the render: each media letter has a loudness; a clip adds its volume and the gain
/// effects of its stack, and a compressor halves what is above -20 LUFS (so gain does not move loudness one for
/// one); a track and the mix are the power sum of their parts plus their own gain effects.
class FakeLoudness final : public LoudnessAnalyzer {
public:
    explicit FakeLoudness(std::map<MediaId, double> media) : m_media(std::move(media)) {}

    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        ++calls;
        const Sequence& seq = *state.findSequence(sequence);
        double l = -std::numeric_limits<double>::infinity();
        switch (target.scope) {
        case LoudnessTarget::Scope::Clip:
            l = clipLoudness(seq.findClip(target.clip)->clip());
            break;
        case LoudnessTarget::Scope::Track:
            l = trackLoudness(*seq.findTrack(target.track));
            break;
        case LoudnessTarget::Scope::Mix: {
            double power = 0.0;
            for (const TrackPtr& t : seq.audioTracks) {
                power += std::pow(10.0, trackLoudness(*t) / 10.0);
            }
            l = 10.0 * std::log10(power) + gains(seq.audioEffects);
            break;
        }
        }
        LoudnessStats stats;
        stats.integratedLufs = l;
        stats.truePeakDbtp = l + 10.0;
        stats.duration = s(10.0);
        return stats;
    }

    int calls = 0;

private:
    static double gains(const EffectStack& stack) {
        double g = 0.0;
        for (const EffectInstance& e : stack) {
            if (e.enabled && e.effect == "wabcut.audio.gain") {
                g += effectParameterAt(e, "gain", Ticks{});
            }
        }
        return g;
    }
    static bool compresses(const EffectStack& stack) {
        return std::any_of(stack.begin(), stack.end(), [](const EffectInstance& e) { return e.effect == "wabcut.audio.compressor"; });
    }
    double clipLoudness(const Clip& c) const {
        double l = m_media.at(c.media) + c.properties().value(ParamId::VolumeDb) + gains(c.effects());
        if (compresses(c.effects()) && l > -20.0) {
            l = -20.0 + (l + 20.0) / 2.0;
        }
        return l;
    }
    double trackLoudness(const Track& t) const {
        double power = 0.0;
        for (const Clip& c : t.clips) {
            power += std::pow(10.0, clipLoudness(c) / 10.0);
        }
        return 10.0 * std::log10(power) + gains(t.effects);
    }

    std::map<MediaId, double> m_media;
};

class FakeSpeech final : public SpeechDetector {
public:
    explicit FakeSpeech(std::vector<TimeRange> speech) : m_speech(std::move(speech)) {}
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId> tracks,
                                                        SpeechSource source, std::optional<TimeRange>,
                                                        command::ExecutionControl*) override {
        askedTracks.assign(tracks.begin(), tracks.end());
        askedSource = source;
        return m_speech;
    }
    std::vector<TrackId> askedTracks;
    SpeechSource askedSource = SpeechSource::Auto;

private:
    std::vector<TimeRange> m_speech;
};

constexpr std::string_view kPicture = R"(
    V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    A3              |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
)";

struct Fixture {
    Fixture()
        : tl(kPicture),
          loudness(std::make_shared<FakeLoudness>(std::map<MediaId, double>{
              {tl.media('A'), -30.0}, {tl.media('M'), -24.0}, {tl.media('B'), -40.0}})),
          speech(std::make_shared<FakeSpeech>(std::vector<TimeRange>{TimeRange::fromStartEnd(s(1.0), s(2.0))})),
          h(tl, command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(loudness, speech)},
            ai::edit::editingCommands()) {}

    const Sequence& seq() const { return h.sequence(h.state()); }
    const Track& track(int a) const { return *seq().audioTracks[static_cast<std::size_t>(a - 1)]; }

    Timeline tl;
    std::shared_ptr<FakeLoudness> loudness;
    std::shared_ptr<FakeSpeech> speech;
    CommandHarness h;
};

} // namespace

TEST_CASE("The audio commands are in the editing catalog", "[audio][edit][commands]") {
    command::CommandRegistry registry;
    registerAudioCommands(registry);
    CHECK(registry.all().size() == 37); // + A.3: track.setVolume, track.setPan, sequence.setMasterGain, audio.moveKeyframe;
                                        // + A.4: audio.enhanceSpeech, audio.separateStems; + A.5: audio.syncClips;
                                        // + A.6: plugins.list, plugins.scan; + A.7: audio.essentialSound, audio.remix
                                        // + A.8: audio.remixRender, track.setRecordArm, record.takes, record.useTake
                                        // + A.9: audio.setChannels, setMediaChannels, setChannelVolume, fillChannel,
                                        //   splitToMono, channels, channelLevels, track.setFormat, setSurroundPan,
                                        //   sequence.setMasterFormat
    for (const command::CommandSpec& spec : registry.all()) {
        INFO(spec.name);
        CHECK(ai::edit::editingCommands().knows(spec.name));
        CHECK(command::unsupportedSchemaKeywords(spec.params).empty());
        CHECK(command::validateAgainstSchema(spec.params, spec.example).empty());
    }
}

TEST_CASE("track.setRole sets an audio track's role", "[audio][edit][commands]") {
    Fixture f;
    const std::size_t undo = f.h.session.undoCount();
    f.h.ok("track.setRole", {{"track", "A3"}, {"role", "effects"}});
    CHECK(f.track(3).role == TrackRole::Effects);
    CHECK(f.h.session.undoCount() == undo + 1);
    f.h.ok("track.setRole", {{"track", "A3"}, {"role", "none"}});
    CHECK(f.track(3).role == TrackRole::None);
    CHECK_FALSE(f.h.errorOf("track.setRole", {{"track", "V1"}, {"role", "music"}}).empty());
    CHECK(f.h.errorOf("track.setRole", {{"track", "A1"}, {"role", "podcast"}}) == "InvalidParams");
}

TEST_CASE("Track and master effect stacks through audio.*", "[audio][edit][commands]") {
    Fixture f;
    auto added = f.h.ok("audio.addEffect", {{"target", "master"}, {"effect", "wabcut.audio.limiter"}, {"params", {{"ceiling", -2}}}});
    REQUIRE(added.createdIds.size() == 1);
    const std::uint64_t limiter = added.createdIds[0];
    added = f.h.ok("audio.addEffect", {{"target", "A1"}, {"effect", "wabcut.audio.eq3"}, {"index", 0}});
    const std::uint64_t eq = added.createdIds[0];
    CHECK(f.seq().audioEffects.size() == 1);
    CHECK(f.track(1).effects.size() == 1);

    const auto listed = f.h.ok("audio.effects", {{"target", "master"}});
    REQUIRE(listed.data["effects"].size() == 1);
    CHECK(listed.data["effects"][0]["effect"] == "wabcut.audio.limiter");
    CHECK(listed.data["effects"][0]["params"]["ceiling"] == -2.0);

    f.h.ok("audio.setEffect", {{"target", "A1"}, {"effect", eq}, {"enabled", false}, {"params", {{"midGain", -6}}}});
    CHECK_FALSE(f.track(1).effects[0].enabled);
    f.h.ok("audio.addKeyframe", {{"target", "master"}, {"effect", limiter}, {"param", "ceiling"}, {"time", tc(25)}, {"value", -3}});
    f.h.ok("audio.addKeyframe", {{"target", "master"}, {"effect", limiter}, {"param", "ceiling"}, {"time", tc(75)}, {"value", -1}});
    CHECK(effectParameterAt(f.seq().audioEffects[0], "ceiling", s(2.0)) == Approx(-2.0));
    f.h.ok("audio.removeKeyframe", {{"target", "master"}, {"effect", limiter}, {"param", "ceiling"}, {"index", 0}});
    f.h.ok("audio.moveEffect", {{"target", "A1"}, {"effect", eq}, {"index", 3}});
    f.h.ok("audio.removeEffect", {{"target", "A1"}, {"effect", eq}});
    CHECK(f.track(1).effects.empty());

    CHECK(f.h.errorOf("audio.addEffect", {{"target", "V1"}, {"effect", "wabcut.audio.gain"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.addEffect", {{"target", "master"}, {"effect", "wabcut.vignette"}}) == "InvalidParams");
    CHECK_FALSE(f.h.errorOf("audio.addKeyframe", {{"target", "master"}, {"effect", limiter}, {"param", "lookahead"},
                                                  {"time", tc(10)}, {"value", 2}})
                    .empty()); // constant only
}

TEST_CASE("audio.loudness measures the mix, tracks and clips through the host's analyzer", "[audio][edit][loudness]") {
    Fixture f;
    const auto r = f.h.ok("audio.loudness", {{"targets", Json::array({"mix", Json{{"track", "A2"}}, Json{{"clip", f.h.clip("A3@0").value}}})}});
    const Json& results = r.data["results"];
    REQUIRE(results.size() == 3);
    CHECK(results[1]["target"]["track"] == "A2");
    CHECK(results[1]["integratedLufs"] == -24.0);
    CHECK(results[2]["integratedLufs"] == -40.0);
    CHECK(results[0]["integratedLufs"].get<double>() > -24.0); // the power sum of the three tracks

    CommandHarness bare(Timeline(kPicture), {}, ai::edit::editingCommands());
    CHECK(bare.errorOf("audio.loudness") == "InvalidRequest");
    CHECK(bare.errorOf("audio.autoDuck") == "InvalidRequest");
}

TEST_CASE("audio.normalize hits the preset by measuring and refining, in one undo step", "[audio][edit][loudness]") {
    Fixture f;
    SECTION("the mix to a preset, with the master true-peak limiter") {
        const std::size_t undo = f.h.session.undoCount();
        const auto r = f.h.ok("audio.normalize", {{"preset", "podcast"}, {"limit", true}});
        CHECK(f.h.session.undoCount() == undo + 1);
        const EffectStack& master = f.seq().audioEffects;
        REQUIRE(master.size() == 2);
        CHECK(master[0].effect == "wabcut.audio.gain"); // before the limiter
        CHECK(master[1].effect == "wabcut.audio.limiter");
        CHECK(effectParameterAt(master[1], "ceiling", Ticks{}) == -1.0);
        const Json& result = r.data["results"][0];
        CHECK(result["after"]["integratedLufs"].get<double>() == Approx(-16.0).margin(0.1));
        CHECK(r.data["targetLufs"] == -16.0);
        // Again: already there, nothing moves, still one gain.
        const auto again = f.h.ok("audio.normalize", {{"preset", "podcast"}, {"limit", true}});
        CHECK(again.data["results"][0]["passes"] == 0);
        CHECK(f.seq().audioEffects.size() == 2);
    }
    SECTION("each clip on its own, refined past a compressor") {
        const ClipId a = f.h.clip("A1@0");
        const auto comp = f.h.ok("clip.addEffect", {{"clips", Json::array({a.value})}, {"effect", "wabcut.audio.compressor"}});
        const auto r = f.h.ok("audio.normalize",
                              {{"clips", Json::array({a.value, f.h.clip("A3@0").value})}, {"lufs", -16.0}, {"maxPasses", 8}});
        const Json& results = r.data["results"];
        REQUIRE(results.size() == 2);
        // The compressed clip needs several passes (gain above -20 LUFS only counts half).
        CHECK(results[0]["passes"].get<int>() > 2);
        CHECK(results[0]["after"]["integratedLufs"].get<double>() == Approx(-16.0).margin(0.1));
        CHECK(results[1]["after"]["integratedLufs"].get<double>() == Approx(-16.0).margin(0.1));
        CHECK(results[1]["gainDb"].get<double>() == Approx(24.0)); // -40 -> -16
        const Clip& clipA = f.seq().findClip(a)->clip();
        REQUIRE(clipA.effects().size() == 2);
        CHECK(clipA.effects()[0].effect == "wabcut.audio.gain"); // inserted before the compressor
        CHECK(clipA.effects()[1].id.value == comp.createdIds[0]);
    }
    SECTION("a track to a custom target") {
        const auto r = f.h.ok("audio.normalize", {{"track", "A2"}, {"lufs", -20.0}});
        CHECK(r.data["results"][0]["gainDb"].get<double>() == Approx(4.0));
        CHECK(f.track(2).effects.size() == 1);
        CHECK(f.h.errorOf("audio.normalize", {{"track", "A2"}, {"preset", "youtube"}, {"limit", true}}) == "InvalidParams");
    }
}

TEST_CASE("audio.masterLimiter adds the preset, then updates it", "[audio][edit][loudness]") {
    Fixture f;
    const auto first = f.h.ok("audio.masterLimiter", {{"ceiling", -1.0}});
    REQUIRE(f.seq().audioEffects.size() == 1);
    f.h.ok("audio.masterLimiter", {{"ceiling", -2.0}, {"release", 100.0}});
    REQUIRE(f.seq().audioEffects.size() == 1);
    CHECK(f.seq().audioEffects[0].id.value == first.createdIds[0]);
    CHECK(effectParameterAt(f.seq().audioEffects[0], "ceiling", Ticks{}) == -2.0);
    CHECK(effectParameterAt(f.seq().audioEffects[0], "release", Ticks{}) == 100.0);
}

TEST_CASE("audio.autoDuck writes music volume keyframes under speech: one undo step, previewable, repeatable",
          "[audio][edit][duck]") {
    Fixture f;
    const Json params{{"amount", -15}, {"attack", 0.2}, {"release", 0.5}, {"hold", 0.3}};
    const ClipId music = f.h.clip("A2@0");

    // dryRun: the preview has the keys, the session does not.
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.autoDuck"}, {"params", params}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.dryRun);
    CHECK_FALSE(preview.committed);
    CHECK(f.h.engine.revision() == revision);
    REQUIRE(preview.preview);
    CHECK(preview.preview->findSequence(f.tl.seq)->findClip(music)->clip().properties()[ParamId::VolumeDb].isCurve());
    CHECK(f.seq().findClip(music)->clip().properties()[ParamId::VolumeDb].isConstant());

    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.autoDuck", params);
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.speech->askedTracks == std::vector<TrackId>{f.tl.track("A1")});
    REQUIRE(r.data["regions"].size() == 1);
    REQUIRE(r.data["clips"].size() == 1);
    const Clip& ducked = f.seq().findClip(music)->clip();
    const ParamValue& volume = ducked.properties()[ParamId::VolumeDb];
    REQUIRE(volume.isCurve());
    CHECK(volume.valueAt(ducked.curveTimeAt(s(0.5))) == 0.0);
    CHECK(volume.valueAt(ducked.curveTimeAt(s(0.9))) == Approx(-7.5));
    CHECK(volume.valueAt(ducked.curveTimeAt(s(1.5))) == -15.0);
    CHECK(volume.valueAt(ducked.curveTimeAt(s(2.55))) == Approx(-7.5));
    CHECK(volume.valueAt(ducked.curveTimeAt(s(3.0))) == 0.0);
    // Dialogue and other tracks are untouched.
    CHECK(f.seq().findClip(f.h.clip("A1@0"))->clip().properties()[ParamId::VolumeDb].isConstant());
    CHECK(f.seq().findClip(f.h.clip("A3@0"))->clip().properties()[ParamId::VolumeDb].isConstant());

    // Running it again replaces the keys instead of ducking twice.
    const ParamValue before = volume;
    f.h.ok("audio.autoDuck", params);
    CHECK(f.seq().findClip(music)->clip().properties()[ParamId::VolumeDb] == before);

    // Explicit tracks work without roles; no dialogue at all is an error.
    f.h.ok("track.setRole", {{"track", "A1"}, {"role", "none"}});
    CHECK(f.h.errorOf("audio.autoDuck") == "InvalidParams");
    f.h.ok("audio.autoDuck", {{"dialogue", Json::array({"A1"})}, {"music", Json::array({"A3"})}, {"source", "energy"}});
    CHECK(f.speech->askedSource == SpeechSource::Energy);
    CHECK(f.seq().findClip(f.h.clip("A3@0"))->clip().properties()[ParamId::VolumeDb].isCurve());
}

TEST_CASE("track.setVolume, track.setPan and sequence.setMasterGain: one undo step each, keyframes with a time",
          "[audio][edit][faders]") {
    Fixture f;
    const std::size_t undo = f.h.session.undoCount();
    auto r = f.h.ok("track.setVolume", {{"track", "A2"}, {"db", -6}});
    CHECK(r.data["target"] == "A2");
    CHECK(r.data["value"] == -6.0);
    CHECK(f.track(2).volume == ParamValue(-6.0));
    CHECK(f.h.session.undoCount() == undo + 1);
    f.h.ok("track.setPan", {{"track", "A3"}, {"pan", -0.4}});
    CHECK(f.track(3).pan == ParamValue(-0.4));
    f.h.ok("sequence.setMasterGain", {{"db", -1.5}});
    CHECK(f.seq().masterVolume == ParamValue(-1.5));
    CHECK(f.h.session.undoCount() == undo + 3);

    // Undo and redo, one step each.
    REQUIRE(f.h.session.undo());
    CHECK(f.seq().masterVolume == ParamValue());
    REQUIRE(f.h.session.undo());
    CHECK(f.track(3).pan == ParamValue());
    REQUIRE(f.h.session.redo());
    CHECK(f.track(3).pan == ParamValue(-0.4));

    // With a time: automation keys (timeline time), a curve after the second.
    f.h.ok("track.setVolume", {{"track", "A2"}, {"db", -18}, {"time", "00:00:02:00"}});
    f.h.ok("track.setVolume", {{"track", "A2"}, {"db", 0}, {"time", "00:00:04:00"}, {"interpolation", "easeInOut"}});
    const Curve* curve = f.track(2).volume.curve();
    REQUIRE(curve);
    REQUIRE(curve->keys.size() == 2);
    CHECK(curve->keys[0].value == -18.0);
    CHECK(curve->keys[1].interpolation == Interpolation::EaseInOut);
    // A constant replaces the automation.
    f.h.ok("track.setVolume", {{"track", "A2"}, {"db", -3}});
    CHECK(f.track(2).volume == ParamValue(-3.0));

    CHECK(f.h.errorOf("track.setVolume", {{"track", "master"}, {"db", -3}}) == "InvalidParams");
    CHECK(f.h.errorOf("track.setVolume", {{"track", "V1"}, {"db", -3}}) == "InvalidParams");
    CHECK(f.h.errorOf("track.setVolume", {{"track", "A2"}, {"db", 40}}) == "InvalidParams"); // the schema's range
    CHECK(f.h.errorOf("track.setPan", {{"track", "A2"}, {"pan", 2}}) == "InvalidParams");
    f.h.ok("track.setState", {{"track", "A2"}, {"locked", true}});
    CHECK_FALSE(f.h.errorOf("track.setVolume", {{"track", "A2"}, {"db", -9}}).empty());
}

TEST_CASE("Fader keyframes through audio.addKeyframe, audio.moveKeyframe and audio.removeKeyframe", "[audio][edit][faders]") {
    Fixture f;
    const std::size_t undo = f.h.session.undoCount();
    f.h.ok("audio.addKeyframe", {{"target", "A1"}, {"param", "volume"}, {"time", "00:00:01:00"}, {"value", -10}});
    f.h.ok("audio.addKeyframe", {{"target", "A1"}, {"param", "volume"}, {"time", "00:00:03:00"}, {"value", 2}});
    f.h.ok("audio.addKeyframe", {{"target", "A1"}, {"param", "pan"}, {"time", "00:00:01:00"}, {"value", 0.5}});
    f.h.ok("audio.addKeyframe", {{"target", "master"}, {"param", "volume"}, {"time", "00:00:00:00"}, {"value", -4}});
    CHECK(f.h.session.undoCount() == undo + 4);
    const auto moved = f.h.ok("audio.moveKeyframe", {{"target", "A1"}, {"param", "volume"}, {"index", 1}, {"time", "00:00:04:00"}});
    CHECK(f.h.session.undoCount() == undo + 5);
    REQUIRE(f.track(1).volume.curve());
    CHECK(f.track(1).volume.curve()->keys[1].time == s(4.0));
    f.h.ok("audio.removeKeyframe", {{"target", "A1"}, {"param", "volume"}, {"index", 0}});
    REQUIRE(f.track(1).volume.curve());
    CHECK(f.track(1).volume.curve()->keys.size() == 1);
    CHECK(f.seq().masterVolume.curve() != nullptr);
    REQUIRE(f.h.session.undo()); // the removal
    CHECK(f.track(1).volume.curve()->keys.size() == 2);

    // audio.effects lists the faders after the stack.
    const auto listed = f.h.ok("audio.effects", {{"target", "A1"}});
    CHECK(listed.data["volume"]["keys"].size() == 2);
    CHECK(listed.data["pan"]["keys"].size() == 1);
    CHECK(f.h.ok("audio.effects", {{"target", "master"}}).data["volume"]["keys"].size() == 1);
    CHECK_FALSE(f.h.ok("audio.effects", {{"target", "master"}}).data.contains("pan"));

    CHECK(f.h.errorOf("audio.addKeyframe", {{"target", "master"}, {"param", "pan"}, {"time", 0}, {"value", 0}}) == "EditRejected");
    CHECK(f.h.errorOf("audio.addKeyframe", {{"target", "A1"}, {"param", "gain"}, {"time", 0}, {"value", 0}}) == "InvalidParams");
}

TEST_CASE("audio.autoDuck reports the planned keyframes and the speech regions they come from, also in a dry run",
          "[audio][edit][duck][faders]") {
    Fixture f;
    const auto preview = f.h.run(Json{{"command", "audio.autoDuck"}, {"params", Json{{"amount", -15}, {"attack", 0.2},
                                                                                      {"release", 0.5}, {"hold", 0.3}}},
                                      {"dryRun", true}});
    REQUIRE(preview.ok());
    const Json& data = preview.data;
    REQUIRE(data["regions"].size() == 1);
    const Json& region = data["regions"][0];
    CHECK(region["ticks"]["speech"] == Json::array({s(1.0).value, s(2.0).value}));
    CHECK(region["ticks"]["down"] == s(0.8).value);
    CHECK(region["ticks"]["upStart"] == s(2.3).value);
    CHECK(region["ticks"]["up"] == s(2.8).value);
    REQUIRE(data["clips"].size() == 1);
    const Json& clip = data["clips"][0];
    CHECK(clip["keys"] == clip["keyframes"].size()); // the count stays for older readers
    CHECK(clip["regions"] == Json::array({0}));
    std::int64_t last = -1;
    int down = 0;
    for (const Json& k : clip["keyframes"]) {
        INFO(k.dump());
        REQUIRE(k["ticks"].is_number_integer());
        CHECK(k["ticks"].get<std::int64_t>() > last); // in timeline order
        last = k["ticks"].get<std::int64_t>();
        CHECK(k["time"].is_string());
        CHECK(k.contains("curveTicks"));
        CHECK(k["interpolation"] == "linear");
        if (k["db"] == -15.0) {
            ++down;
            CHECK(k["region"] == 0);
        }
    }
    CHECK(down == 2); // speech start .. up-ramp start
    CHECK(f.seq().findClip(f.h.clip("A2@0"))->clip().properties()[ParamId::VolumeDb].isConstant()); // nothing applied
}
