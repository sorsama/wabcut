// Streamer voice and Game under voice (A.10) without rendering: the streamerVoice preset's stack (sections in order,
// values in their effects' ranges), the presets built before it unchanged, thresholds that follow the loudness target,
// audio.essentialSound on a whole track (one undo step, dryRun), the stack staying editable; the game/voice track
// chooser, the gameUnderVoice timing and audio.autoDuck's "preset" through the CommandEngine with fake analysis.
// The measurements on rendered fixtures are in test_streamer_audio_media.cpp.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/audio/edit/GameAudio.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

Ticks s(double seconds) { return Ticks{std::llround(seconds * static_cast<double>(kTicksPerSecond))}; }

/// The value of a built effect's parameter (they are constants).
double paramOf(const BuiltEffect& b, std::string_view key) {
    for (const EffectParam& p : b.params) {
        if (p.key == key) {
            return p.value.constant();
        }
    }
    FAIL("no parameter " << key << " on " << b.effect);
    return 0.0;
}

std::vector<std::string> sectionsOf(const std::vector<BuiltEffect>& stack) {
    std::vector<std::string> out;
    for (const BuiltEffect& b : stack) {
        out.push_back(b.effect + ":" + b.section);
    }
    return out;
}

const BuiltEffect& sectionIn(const std::vector<BuiltEffect>& stack, std::string_view section) {
    for (const BuiltEffect& b : stack) {
        if (b.section == section) {
            return b;
        }
    }
    FAIL("no section " << section);
    return stack.front();
}

/// The fixture's audio tracks named (the picture gives roles, not names).
Timeline named(Timeline tl, std::initializer_list<std::pair<const char*, const char*>> names) {
    for (const auto& [label, name] : names) {
        edit::TrackStateChange change;
        change.name = std::string(name);
        tl.state = test::ok(edit::setTrackState(tl.state, tl.seq, tl.track(label), change)).state;
    }
    return tl;
}

/// A clip's loudness: -35 LUFS plus its volume and the gain effects of its stack (no other effect changes it here).
class FakeLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        LoudnessStats stats;
        if (target.scope == LoudnessTarget::Scope::Clip) {
            const Clip& c = state.findSequence(sequence)->findClip(target.clip)->clip();
            double l = -35.0 + c.properties().value(ParamId::VolumeDb);
            for (const EffectInstance& e : c.effects()) {
                if (e.enabled && e.effect == "wabcut.audio.gain") {
                    l += effectParameterAt(e, "gain", Ticks{});
                }
            }
            stats.integratedLufs = l;
            stats.truePeakDbtp = l + 10.0;
        }
        stats.duration = Ticks::fromSeconds(10);
        return stats;
    }
};

class FakeSpeech final : public SpeechDetector {
public:
    explicit FakeSpeech(std::vector<TimeRange> speech) : m_speech(std::move(speech)) {}
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId> tracks, SpeechSource,
                                                        std::optional<TimeRange>, command::ExecutionControl*) override {
        askedTracks.assign(tracks.begin(), tracks.end());
        return m_speech;
    }
    std::vector<TrackId> askedTracks;

private:
    std::vector<TimeRange> m_speech;
};

command::EnginePolicy policyWith(std::shared_ptr<LoudnessAnalyzer> loudness, std::shared_ptr<SpeechDetector> speech) {
    return command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(std::move(loudness), std::move(speech))};
}

const std::vector<std::string> kStreamerStack{
    "wabcut.audio.gain:loudness",          "wabcut.audio.filter:repair.rumble",     "wabcut.audio.denoise:repair.noise",
    "wabcut.audio.expander:repair.gate",   "wabcut.audio.deess:repair.deess",       "wabcut.audio.filter:clarity.band",
    "wabcut.audio.eq3:clarity.eq",         "wabcut.audio.compressor:clarity.dynamics", "wabcut.audio.limiter:clarity.limiter"};

} // namespace

// ---- The presets ---------------------------------------------------------------------------------------------------

TEST_CASE("The streamerVoice preset builds its stack in order: the gate after Reduce Noise and the limiter last",
          "[audio][edit][essential][streamer]") {
    const EssentialSoundPreset* preset = findEssentialSoundPreset(SoundType::Dialogue, "streamerVoice");
    REQUIRE(preset);
    CHECK(essentialSoundPresets(SoundType::Dialogue)[0].id == "default");
    CHECK(essentialSoundPresets(SoundType::Dialogue)[1].id == "streamerVoice"); // the gaming editor's first choice
    const EssentialSoundOptions& o = preset->options;
    CHECK(o.loudness);
    CHECK(o.targetLufs == -16.0);
    CHECK_FALSE(o.enhanceSpeech); // RNNoise: runs anywhere without a model
    CHECK(o.reduceNoise == 0.8);
    CHECK(o.rumbleHz == 80.0);
    CHECK(o.gate == 0.5);
    CHECK(o.deess == 0.45);
    CHECK(o.eq == "streamerVoice");
    CHECK(o.dynamics == 0.6);
    CHECK(o.dynamicsMakeup);
    CHECK(o.limiter);
    CHECK(o.limiterCeiling == -1.0);
    const std::span<const std::string_view> eqs = clarityEqPresets();
    CHECK(std::ranges::find(eqs, std::string_view("streamerVoice")) != eqs.end());

    const std::vector<BuiltEffect> stack = buildEssentialSoundStack(SoundType::Dialogue, o);
    CHECK(sectionsOf(stack) == kStreamerStack);
    // At -16 LUFS the level-dependent thresholds sit 7 dB above their -23 LUFS values.
    const BuiltEffect& gate = sectionIn(stack, "repair.gate");
    CHECK(paramOf(gate, "threshold") == Approx(-24.0)); // 8 dB under the voice's loudness
    CHECK(paramOf(gate, "ratio") == Approx(6.0));
    CHECK(paramOf(gate, "range") == Approx(26.0));
    CHECK(paramOf(gate, "attack") == Approx(1.0));
    CHECK(paramOf(gate, "release") == Approx(160.0));
    const BuiltEffect& deess = sectionIn(stack, "repair.deess");
    CHECK(paramOf(deess, "threshold") == Approx(-18.0 - 22.0 * 0.45 + 7.0));
    CHECK(paramOf(deess, "reduction") == Approx(4.0 + 12.0 * 0.45));
    CHECK(paramOf(sectionIn(stack, "repair.noise"), "amount") == Approx(0.8));
    CHECK(paramOf(sectionIn(stack, "repair.rumble"), "lowCut") == 80.0);
    CHECK(paramOf(sectionIn(stack, "clarity.band"), "lowCut") == 90.0);
    CHECK(paramOf(sectionIn(stack, "clarity.band"), "slope") == 24.0);
    const BuiltEffect& eq = sectionIn(stack, "clarity.eq");
    CHECK(paramOf(eq, "lowFrequency") == 300.0);
    CHECK(paramOf(eq, "lowGain") == Approx(-2.5));
    CHECK(paramOf(eq, "midFrequency") == 3200.0);
    CHECK(paramOf(eq, "midGain") == Approx(3.5));
    CHECK(paramOf(eq, "highFrequency") == 10000.0);
    CHECK(paramOf(eq, "highGain") == Approx(2.0));
    const BuiltEffect& comp = sectionIn(stack, "clarity.dynamics");
    CHECK(paramOf(comp, "threshold") == Approx(-14.0 - 16.0 * 0.6 + 7.0));
    CHECK(paramOf(comp, "ratio") == Approx(1.5 + 4.5 * 0.6));
    // The makeup gives back what a voice at the target (peaks 9 dB over its loudness) loses: 9.6 dB over the threshold.
    CHECK(paramOf(comp, "makeup") == Approx(9.6 * (1.0 - 1.0 / 4.2)));
    const BuiltEffect& limiter = stack.back();
    CHECK(limiter.effect == "wabcut.audio.limiter");
    CHECK(paramOf(limiter, "ceiling") == -1.0);
    // Every value is one its effect takes.
    for (const BuiltEffect& b : stack) {
        const EffectDescriptor* d = EffectRegistry::builtin().find(b.effect);
        REQUIRE(d);
        for (const EffectParam& p : b.params) {
            INFO(b.effect << " " << p.key);
            const EffectParamInfo* info = d->param(p.key);
            REQUIRE(info);
            CHECK(p.value.constant() >= info->minValue);
            CHECK(p.value.constant() <= info->maxValue);
        }
    }
    // Enhance Speech instead of RNNoise is an option of the preset.
    EssentialSoundOptions heavier = o;
    heavier.enhanceSpeech = true;
    heavier.reduceNoise = 0.0;
    const std::vector<BuiltEffect> enhanced = buildEssentialSoundStack(SoundType::Dialogue, heavier);
    CHECK(enhanced[1].section == "repair.enhance");
    CHECK(std::ranges::none_of(enhanced, [](const BuiltEffect& b) { return b.section == "repair.noise"; }));
}

TEST_CASE("The presets built before A.10 build the same stacks: no gate or limiter and thresholds set for -23 LUFS",
          "[audio][edit][essential][streamer]") {
    for (const SoundType type : {SoundType::Dialogue, SoundType::Music, SoundType::Sfx, SoundType::Ambience}) {
        for (const EssentialSoundPreset& preset : essentialSoundPresets(type)) {
            if (preset.id == "streamerVoice") {
                continue;
            }
            INFO(toString(type) << " " << preset.id);
            CHECK(preset.options.gate == 0.0);
            CHECK_FALSE(preset.options.limiter);
            CHECK_FALSE(preset.options.dynamicsMakeup);
            for (const BuiltEffect& b : buildEssentialSoundStack(type, preset.options)) {
                CHECK(b.section != "repair.gate");
                CHECK(b.section != "clarity.limiter");
                if (b.effect == "wabcut.audio.compressor") {
                    CHECK(paramOf(b, "makeup") == 0.0);
                }
            }
        }
    }
    // The level-dependent values of the dialogue presets, as their A.7 formulas give them at -23 LUFS.
    const auto stackOf = [](std::string_view id) {
        const EssentialSoundPreset* p = findEssentialSoundPreset(SoundType::Dialogue, id);
        REQUIRE(p);
        return buildEssentialSoundStack(SoundType::Dialogue, p->options);
    };
    const auto podcast = stackOf("podcastVoice");
    CHECK(sectionsOf(podcast) == std::vector<std::string>{"wabcut.audio.gain:loudness", "wabcut.audio.filter:repair.rumble",
                                                           "wabcut.audio.denoise:repair.noise", "wabcut.audio.deess:repair.deess",
                                                           "wabcut.audio.filter:clarity.band", "wabcut.audio.eq3:clarity.eq",
                                                           "wabcut.audio.compressor:clarity.dynamics"});
    CHECK(paramOf(sectionIn(podcast, "repair.deess"), "threshold") == Approx(-26.8));
    CHECK(paramOf(sectionIn(podcast, "clarity.dynamics"), "threshold") == Approx(-22.0));
    CHECK(paramOf(sectionIn(podcast, "clarity.dynamics"), "ratio") == Approx(3.75));
    CHECK(paramOf(sectionIn(stackOf("phone"), "clarity.dynamics"), "threshold") == Approx(-23.6));
    CHECK(paramOf(sectionIn(stackOf("radio"), "clarity.dynamics"), "threshold") == Approx(-25.2));
    CHECK(paramOf(sectionIn(stackOf("radio"), "repair.deess"), "threshold") == Approx(-24.6));
    CHECK(paramOf(sectionIn(stackOf("cleanUpNoisy"), "repair.deess"), "threshold") == Approx(-24.6));
    const auto roomEcho = stackOf("reduceRoomEcho");
    const BuiltEffect& echo = sectionIn(roomEcho, "repair.reverb");
    CHECK(paramOf(echo, "threshold") == Approx(-41.0));
    CHECK(paramOf(echo, "ratio") == Approx(3.0));
    CHECK(paramOf(echo, "range") == Approx(16.8));
    CHECK(paramOf(echo, "release") == Approx(160.0));
    // The other types: loudness, and width where a preset widens.
    const auto sectionsFor = [](SoundType type, std::string_view id) {
        const EssentialSoundPreset* p = findEssentialSoundPreset(type, id);
        REQUIRE(p);
        return sectionsOf(buildEssentialSoundStack(type, p->options));
    };
    const std::vector<std::string> loudnessOnly{"wabcut.audio.gain:loudness"};
    const std::vector<std::string> wide{"wabcut.audio.gain:loudness", "wabcut.audio.width:width"};
    CHECK(sectionsFor(SoundType::Music, "background") == loudnessOnly);
    CHECK(sectionsFor(SoundType::Music, "wide") == wide);
    CHECK(sectionsFor(SoundType::Sfx, "background") == loudnessOnly);
    CHECK(sectionsFor(SoundType::Ambience, "underDialogue") == loudnessOnly);
    CHECK(sectionsFor(SoundType::Ambience, "wide") == wide);
}

TEST_CASE("Level-dependent thresholds follow the loudness target; the gate and the limiter work on any dialogue preset",
          "[audio][edit][essential][streamer]") {
    EssentialSoundOptions o = findEssentialSoundPreset(SoundType::Dialogue, "podcastVoice")->options;
    o.targetLufs = -16.0;
    auto stack = buildEssentialSoundStack(SoundType::Dialogue, o);
    CHECK(paramOf(sectionIn(stack, "repair.deess"), "threshold") == Approx(-26.8 + 7.0));
    CHECK(paramOf(sectionIn(stack, "clarity.dynamics"), "threshold") == Approx(-22.0 + 7.0));
    // Without loudness the clip's level is not known: the thresholds stay as set for -23 LUFS.
    o.loudness = false;
    stack = buildEssentialSoundStack(SoundType::Dialogue, o);
    CHECK(paramOf(sectionIn(stack, "repair.deess"), "threshold") == Approx(-26.8));
    CHECK(paramOf(sectionIn(stack, "clarity.dynamics"), "threshold") == Approx(-22.0));

    // The gate on podcastVoice at -23 LUFS: after Reduce Noise, before the de-esser; amount 1 closes 4 dB under the
    // voice's loudness, deeper and sooner.
    o = findEssentialSoundPreset(SoundType::Dialogue, "podcastVoice")->options;
    o.gate = 1.0;
    o.dynamicsMakeup = true;
    o.limiter = true;
    o.limiterCeiling = -2.0;
    o.reverb = "hall";
    stack = buildEssentialSoundStack(SoundType::Dialogue, o);
    CHECK(sectionsOf(stack) == std::vector<std::string>{"wabcut.audio.gain:loudness", "wabcut.audio.filter:repair.rumble",
                                                         "wabcut.audio.denoise:repair.noise", "wabcut.audio.expander:repair.gate",
                                                         "wabcut.audio.deess:repair.deess", "wabcut.audio.filter:clarity.band",
                                                         "wabcut.audio.eq3:clarity.eq", "wabcut.audio.compressor:clarity.dynamics",
                                                         "wabcut.audio.reverb:creative.reverb", "wabcut.audio.limiter:clarity.limiter"});
    const BuiltEffect& gate = sectionIn(stack, "repair.gate");
    CHECK(paramOf(gate, "threshold") == Approx(-27.0));
    CHECK(paramOf(gate, "ratio") == Approx(10.0));
    CHECK(paramOf(gate, "range") == Approx(40.0));
    CHECK(paramOf(gate, "release") == Approx(120.0));
    CHECK(paramOf(stack.back(), "ceiling") == -2.0); // after the reverb: nothing raises a peak again
    CHECK(paramOf(sectionIn(stack, "clarity.dynamics"), "makeup") == Approx(8.0 * (1.0 - 1.0 / 3.75)));
    // Reduce Reverb's expander and the gate together: the gate first.
    o.reduceReverb = 0.6;
    stack = buildEssentialSoundStack(SoundType::Dialogue, o);
    CHECK(stack[3].section == "repair.gate");
    CHECK(stack[4].section == "repair.reverb");
    CHECK(paramOf(stack[4], "threshold") == Approx(-41.0));
}

// ---- audio.essentialSound on a track -----------------------------------------------------------------------------

namespace {

/// A1 "Mic": two clips of the commentary (8 s from 0 s, 8 s from 8 s); A2 "Game": the game audio; A3: empty.
Timeline streamTimeline() {
    return named(Timeline("V1 |V" + std::string(399, 'v') + "|\n" + "A1 dialogue |D" + std::string(199, 'd') + "D200" +
                              std::string(196, 'd') + "|\n" + "A2 effects |G" + std::string(399, 'g') + "|\n" + "A3 |" +
                              std::string(400, '.') + "|\n",
                          {{'V', 1000}, {'D', 1000}, {'G', 1000}}),
                 {{"A1", "Mic"}, {"A2", "Game"}});
}

struct StreamFixture {
    StreamFixture()
        : loudness(std::make_shared<FakeLoudness>()),
          speech(std::make_shared<FakeSpeech>(std::vector<TimeRange>{TimeRange::fromStartEnd(s(1.0), s(2.0))})),
          h(streamTimeline(), policyWith(loudness, speech), ai::edit::editingCommands()) {}
    const Clip& clip(ClipId id) const { return h.sequence(h.state()).findClip(id)->clip(); }
    std::vector<std::string> stackOf(ClipId id) const {
        std::vector<std::string> out;
        for (const EffectInstance& e : clip(id).effects()) {
            const EffectData* section = e.findData(kSectionKey);
            out.push_back(e.effect + (section != nullptr ? ":" + *section->text() : ""));
        }
        return out;
    }
    std::shared_ptr<FakeLoudness> loudness;
    std::shared_ptr<FakeSpeech> speech;
    CommandHarness h;
};

std::vector<std::string> withRecord(const std::vector<std::string>& sections) {
    std::vector<std::string> out{std::string(kEssentialSoundEffect)};
    out.insert(out.end(), sections.begin(), sections.end());
    return out;
}

const EffectInstance* effectIn(const Clip& clip, std::string_view section) {
    for (const EffectInstance& e : clip.effects()) {
        if (const EffectData* d = e.findData(kSectionKey); d != nullptr && *d->text() == section) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("audio.essentialSound applies streamerVoice to every clip of the mic track: one undo step; dryRun changes nothing",
          "[audio][edit][essential][streamer]") {
    StreamFixture f;
    const ClipId first = f.h.clip("A1@0");
    const ClipId second = f.h.clip("A1@200");
    const Json params{{"tracks", Json::array({"A1"})}, {"type", "dialogue"}, {"preset", "streamerVoice"}};

    // dryRun: the plan, nothing applied.
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.essentialSound"}, {"params", params}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.dryRun);
    CHECK(f.h.engine.revision() == revision);
    CHECK(f.clip(first).effects().empty());
    REQUIRE(preview.data["clips"].size() == 2);
    CHECK(preview.data["clips"][0]["effects"].size() == kStreamerStack.size() + 1); // the record, then the stack

    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.essentialSound", params);
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(r.data["preset"] == "streamerVoice");
    CHECK(f.stackOf(first) == withRecord(kStreamerStack));
    CHECK(f.stackOf(second) == withRecord(kStreamerStack));
    // The record keeps the options, the new ones included.
    const Json options = Json::parse(*f.clip(first).effects()[0].findData("options")->text());
    CHECK(options["targetLufs"] == -16.0);
    CHECK(options["repair"]["gate"] == 0.5);
    CHECK(options["repair"]["noise"] == 0.8);
    CHECK(options["clarity"]["eq"] == "streamerVoice");
    CHECK(options["clarity"]["makeup"] == true);
    CHECK(options["clarity"]["limiter"] == true);
    CHECK(options["clarity"]["limiterCeiling"] == -1.0);
    // Loudness: each clip matched to -16 LUFS (the fake is linear: +19 dB in one pass).
    for (const Json& row : r.data["clips"]) {
        CHECK(row["track"] == "A1");
        CHECK(row["loudness"]["after"]["integratedLufs"] == -16.0);
        CHECK(row["loudness"]["passes"] == 1);
    }
    // The game track and its clip are untouched.
    CHECK(f.clip(f.h.clip("A2@0")).effects().empty());
    requireValid(*f.h.state());
    f.h.session.undo();
    CHECK(f.clip(first).effects().empty());
    CHECK(f.clip(second).effects().empty());

    // "clips" and "tracks" together: each clip once.
    const auto both = f.h.ok("audio.essentialSound", {{"clips", Json::array({first.value})},
                                                      {"tracks", Json::array({"A1"})},
                                                      {"type", "dialogue"},
                                                      {"preset", "streamerVoice"}});
    CHECK(both.data["clips"].size() == 2);
    // Refusals commit nothing.
    const std::size_t before = f.h.session.undoCount();
    CHECK(f.h.errorOf("audio.essentialSound", {{"type", "dialogue"}, {"preset", "streamerVoice"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.essentialSound", {{"tracks", Json::array({"V1"})}, {"type", "dialogue"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.essentialSound", {{"tracks", Json::array({"A3"})}, {"type", "dialogue"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.essentialSound", {{"tracks", Json::array({"A9"})}, {"type", "dialogue"}}) == "NotFound");
    CHECK(f.h.errorOf("audio.essentialSound", {{"tracks", Json::array({"A1"})}, {"type", "dialogue"},
                                               {"options", Json{{"repair", Json{{"gate", 2}}}}}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.essentialSound", {{"tracks", Json::array({"A1"})}, {"type", "dialogue"},
                                               {"options", Json{{"clarity", Json{{"limiterCeiling", 3}}}}}}) == "InvalidParams");
    CHECK(f.h.session.undoCount() == before);
}

TEST_CASE("Streamer voice stays editable: options change only its effects and a user effect after it is kept",
          "[audio][edit][essential][streamer]") {
    StreamFixture f;
    const ClipId mic = f.h.clip("A1@0");
    f.h.ok("audio.essentialSound", {{"clips", Json::array({mic.value})}, {"type", "dialogue"}, {"preset", "streamerVoice"}});
    // The user's own effect after the stack, and an edit of the gate's threshold: ordinary effects, edited as such.
    const auto added = f.h.ok("clip.addEffect", {{"clips", Json::array({mic.value})}, {"effect", "wabcut.audio.reverb"}});
    REQUIRE(added.createdIds.size() == 1);
    const std::uint64_t userReverb = added.createdIds[0];
    const EffectInstance* gate = effectIn(f.clip(mic), "repair.gate");
    REQUIRE(gate);
    f.h.ok("clip.setEffect", {{"clip", mic.value}, {"effect", gate->id.value}, {"params", Json{{"threshold", -30}}}});
    CHECK(effectParameterAt(*effectIn(f.clip(mic), "repair.gate"), "threshold", Ticks{}) == -30.0);
    for (const EffectInstance& e : f.clip(mic).effects()) {
        CHECK(isEvaluable(e, EffectKind::Audio));
    }
    // Applied again with options (no "type": the record says dialogue): a firmer gate, no limiter. Only Essential
    // Sound's effects change; the reverb stays, after them.
    f.h.ok("audio.essentialSound",
           {{"clips", Json::array({mic.value})},
            {"preset", "streamerVoice"},
            {"options", Json{{"repair", Json{{"gate", 0.8}}}, {"clarity", Json{{"limiter", false}}}}}});
    std::vector<std::string> expected(kStreamerStack.begin(), kStreamerStack.end() - 1);
    expected = withRecord(expected);
    expected.emplace_back("wabcut.audio.reverb");
    CHECK(f.stackOf(mic) == expected);
    CHECK(f.clip(mic).effects().back().id.value == userReverb);
    CHECK(effectParameterAt(*effectIn(f.clip(mic), "repair.gate"), "threshold", Ticks{}) == Approx(-23.0 - 12.0 + 6.4 + 7.0));
    CHECK(effectParameterAt(*effectIn(f.clip(mic), "repair.gate"), "range", Ticks{}) == Approx(12.0 + 28.0 * 0.8));
    // A section turned off stays a normal disabled effect; "none" takes Essential Sound away and keeps the reverb.
    const EffectInstance* deess = effectIn(f.clip(mic), "repair.deess");
    REQUIRE(deess);
    f.h.ok("clip.setEffect", {{"clip", mic.value}, {"effect", deess->id.value}, {"enabled", false}});
    CHECK_FALSE(effectIn(f.clip(mic), "repair.deess")->enabled);
    f.h.ok("audio.essentialSound", {{"clips", Json::array({mic.value})}, {"type", "none"}});
    CHECK(f.stackOf(mic) == std::vector<std::string>{"wabcut.audio.reverb"});
}

// ---- Game under voice ----------------------------------------------------------------------------------------------

TEST_CASE("chooseGameVoiceTracks: the mic and the game by name or else by role; a Discord track stays out",
          "[audio][edit][duck][streamer]") {
    const std::string clip = "|Aaaa|";
    const std::string picture = "A1 effects " + clip + "\nA2 dialogue " + clip + "\nA3 dialogue " + clip + "\nA4 music " + clip +
                                "\nA5 " + clip + "\nA6 effects " + clip + "\n";
    const Timeline roles(picture);
    const Timeline tl = named(Timeline(picture), {{"A1", "Game"}, {"A2", "Mic"}, {"A3", "Discord"}, {"A4", "Music"},
                                                  {"A5", "Track 5"}, {"A6", "Sound FX"}});
    // By name: the mic is the voice; the game and the music duck. Discord is neither, and the effects-role "Sound FX"
    // keeps its level (a track is named for the game).
    GameVoiceTracks chosen = chooseGameVoiceTracks(tl.sequence());
    CHECK(chosen.voice == std::vector<TrackId>{tl.track("A2")});
    CHECK(chosen.game == std::vector<TrackId>{tl.track("A1"), tl.track("A4")});
    chosen = chooseGameVoiceTracks(tl.sequence(), false);
    CHECK(chosen.game == std::vector<TrackId>{tl.track("A1")});
    // By role (no names): every dialogue track is the voice; the effects and music tracks duck, in track order.
    chosen = chooseGameVoiceTracks(roles.sequence());
    CHECK(chosen.voice == std::vector<TrackId>{roles.track("A2"), roles.track("A3")});
    CHECK(chosen.game == std::vector<TrackId>{roles.track("A1"), roles.track("A4"), roles.track("A6")});

    // Name forms: any case, a number or a note after the word; not inside another word. A voice-named track is never
    // on the game side, whatever its role.
    const std::string five = "A1 " + clip + "\nA2 " + clip + "\nA3 " + clip + "\nA4 effects " + clip + "\nA5 " + clip + "\n";
    const Timeline forms = named(Timeline(five), {{"A1", "  MIC 2 "}, {"A2", "Desktop Audio"}, {"A3", "Gamer tag"},
                                                  {"A4", "Microphone (Yeti)"}, {"A5", "Microwave"}});
    chosen = chooseGameVoiceTracks(forms.sequence());
    CHECK(chosen.voice == std::vector<TrackId>{forms.track("A1"), forms.track("A4")});
    CHECK(chosen.game == std::vector<TrackId>{forms.track("A2")});
    const Timeline obs = named(Timeline(five), {{"A1", "Mic/Aux"}, {"A2", "Game (Track 1)"}, {"A3", "commentary"},
                                                {"A4", "Gameplay"}, {"A5", "music"}});
    chosen = chooseGameVoiceTracks(obs.sequence());
    CHECK(chosen.voice == std::vector<TrackId>{obs.track("A1"), obs.track("A3")});
    CHECK(chosen.game == std::vector<TrackId>{obs.track("A2"), obs.track("A4"), obs.track("A5")});
    // Nothing to go on: empty lists.
    chosen = chooseGameVoiceTracks(Timeline("A1 " + clip + "\n").sequence());
    CHECK(chosen.voice.empty());
    CHECK(chosen.game.empty());
}

TEST_CASE("gameUnderVoiceDucking: -10 dB with a short lead-in; pauses under 1.27 s stay down", "[audio][edit][duck][streamer]") {
    const DuckingOptions o = gameUnderVoiceDucking();
    CHECK(o.amountDb == -10.0);
    CHECK(o.attack == s(0.12));
    CHECK(o.hold == s(0.35));
    CHECK(o.release == s(0.8));
    CHECK(o.minSpeech == s(0.15));
    const auto span = [](double from, double to) { return TimeRange::fromStartEnd(s(from), s(to)); };
    // A 1.2 s pause between sentences is bridged (the game would be back at 4.15 s and down again from 4.08 s); 1.3 s
    // is not. A 0.1 s click does nothing; 0.15 s counts.
    CHECK(duckRegions(std::vector<TimeRange>{span(2.0, 3.0), span(4.2, 5.0)}, o).size() == 1);
    CHECK(duckRegions(std::vector<TimeRange>{span(2.0, 3.0), span(4.3, 5.0)}, o).size() == 2);
    CHECK(duckRegions(std::vector<TimeRange>{span(2.0, 2.1)}, o).empty());
    CHECK(duckRegions(std::vector<TimeRange>{span(2.0, 2.15)}, o).size() == 1);
    // The envelope: down by the speech start, down through the hold, up over the release.
    const auto regions = duckRegions(std::vector<TimeRange>{span(2.0, 3.0)}, o);
    CHECK(duckOffsetDb(regions, o, s(1.87)) == 0.0);
    CHECK(duckOffsetDb(regions, o, s(1.94)) == Approx(-5.0));
    CHECK(duckOffsetDb(regions, o, s(2.0)) == -10.0);
    CHECK(duckOffsetDb(regions, o, s(3.3)) == -10.0);
    CHECK(duckOffsetDb(regions, o, s(3.75)) == Approx(-5.0));
    CHECK(duckOffsetDb(regions, o, s(4.15)) == 0.0);
}

namespace {

/// A1 "Mic", A2 "Discord" (both dialogue), A3 "Game" (effects), A4 "Music" (music): 4 s each.
Timeline gameTimeline(bool names = true) {
    const std::string clip = "|A" + std::string(99, 'a') + "|";
    Timeline tl("A1 dialogue " + clip + "\nA2 dialogue " + clip + "\nA3 effects " + clip + "\nA4 music " + clip + "\n");
    return names ? named(std::move(tl), {{"A1", "Mic"}, {"A2", "Discord"}, {"A3", "Game"}, {"A4", "Music"}}) : tl;
}

struct DuckFixture {
    explicit DuckFixture(Timeline tl = gameTimeline())
        : speech(std::make_shared<FakeSpeech>(std::vector<TimeRange>{TimeRange::fromStartEnd(s(1.0), s(2.0))})),
          h(std::move(tl), policyWith(std::make_shared<FakeLoudness>(), speech), ai::edit::editingCommands()) {}
    const Clip& clipOn(std::string_view label) const { return h.sequence(h.state()).findClip(h.clip(std::string(label) + "@0"))->clip(); }
    double volumeAt(std::string_view label, double t) const {
        const Clip& c = clipOn(label);
        return c.properties()[ParamId::VolumeDb].valueAt(c.curveTimeAt(s(t)));
    }
    std::shared_ptr<FakeSpeech> speech;
    CommandHarness h;
};

} // namespace

TEST_CASE("audio.autoDuck gameUnderVoice ducks the game and the music under the mic: the report names the tracks",
          "[audio][edit][duck][streamer]") {
    DuckFixture f;
    const Json params{{"preset", "gameUnderVoice"}};
    // dryRun: the plan and the tracks, nothing applied.
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.autoDuck"}, {"params", params}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(f.h.engine.revision() == revision);
    CHECK(preview.data["preset"] == "gameUnderVoice");
    CHECK(f.clipOn("A3").properties()[ParamId::VolumeDb].isConstant());

    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.autoDuck", params);
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.speech->askedTracks == std::vector<TrackId>{f.h.tl.track("A1")}); // the mic, not Discord
    CHECK(r.data["dialogue"] == Json::array({Json{{"track", "A1"}, {"name", "Mic"}}}));
    CHECK(r.data["music"] == Json::array({Json{{"track", "A3"}, {"name", "Game"}}, Json{{"track", "A4"}, {"name", "Music"}}}));
    CHECK(r.data["amountDb"] == -10.0);
    REQUIRE(r.data["clips"].size() == 2);
    REQUIRE(r.data["regions"].size() == 1);
    CHECK(r.data["regions"][0]["ticks"]["down"] == s(0.88).value);
    CHECK(r.data["regions"][0]["ticks"]["upStart"] == s(2.35).value);
    CHECK(r.data["regions"][0]["ticks"]["up"] == s(3.15).value);
    // The game: up, down over 0.12 s by the speech start, down through the hold, up over 0.8 s.
    for (const char* label : {"A3", "A4"}) {
        INFO(label);
        CHECK(f.volumeAt(label, 0.5) == 0.0);
        CHECK(f.volumeAt(label, 0.94) == Approx(-5.0));
        CHECK(f.volumeAt(label, 1.5) == -10.0);
        CHECK(f.volumeAt(label, 2.3) == -10.0);
        CHECK(f.volumeAt(label, 2.75) == Approx(-5.0));
        CHECK(f.volumeAt(label, 3.5) == 0.0);
    }
    // The voices keep their level.
    CHECK(f.clipOn("A1").properties()[ParamId::VolumeDb].isConstant());
    CHECK(f.clipOn("A2").properties()[ParamId::VolumeDb].isConstant());

    // Given parameters win: a deeper duck under Discord, and the game only.
    const auto custom = f.h.ok("audio.autoDuck", {{"preset", "gameUnderVoice"}, {"dialogue", Json::array({"A2"})},
                                                  {"music", Json::array({"A3"})}, {"amount", -14}});
    CHECK(f.speech->askedTracks == std::vector<TrackId>{f.h.tl.track("A2")});
    CHECK(custom.data["music"].size() == 1);
    CHECK(f.volumeAt("A3", 1.5) == -14.0);
    CHECK(f.volumeAt("A3", 2.3) == -14.0); // the preset's hold is kept
    CHECK(f.h.errorOf("audio.autoDuck", {{"preset", "loud"}}) == "InvalidParams");
}

TEST_CASE("audio.autoDuck gameUnderVoice: the music can stay up; roles stand in for names; nothing to duck is refused",
          "[audio][edit][duck][streamer]") {
    {
        DuckFixture f;
        const auto r = f.h.ok("audio.autoDuck", {{"preset", "gameUnderVoice"}, {"includeMusic", false}});
        CHECK(r.data["music"] == Json::array({Json{{"track", "A3"}, {"name", "Game"}}}));
        CHECK(f.clipOn("A4").properties()[ParamId::VolumeDb].isConstant());
        CHECK(f.volumeAt("A3", 1.5) == -10.0);
    }
    {
        // No names: the dialogue-role tracks are the voice (both), the effects and music tracks duck.
        DuckFixture f(gameTimeline(false));
        const auto r = f.h.ok("audio.autoDuck", {{"preset", "gameUnderVoice"}});
        CHECK(f.speech->askedTracks == std::vector<TrackId>{f.h.tl.track("A1"), f.h.tl.track("A2")});
        CHECK(r.data["music"].size() == 2);
        // The default preset is what it was: dialogue-role speech, music-role tracks only, -12 dB.
        const auto plain = f.h.ok("audio.autoDuck");
        CHECK(plain.data["preset"] == "default");
        CHECK(plain.data["music"] == Json::array({Json{{"track", "A4"}, {"name", ""}}}));
        CHECK(plain.data["amountDb"] == -12.0);
    }
    {
        // Nothing to duck under, nothing to duck.
        DuckFixture voiceless(named(Timeline("A1 |Aaaa|\nA2 effects |Bbbb|\n"), {{"A2", "Game"}}));
        CHECK(voiceless.h.errorOf("audio.autoDuck", {{"preset", "gameUnderVoice"}}) == "InvalidParams");
        DuckFixture gameless(named(Timeline("A1 |Aaaa|\nA2 |Bbbb|\n"), {{"A1", "Mic"}}));
        CHECK(gameless.h.errorOf("audio.autoDuck", {{"preset", "gameUnderVoice"}}) == "InvalidParams");
        CHECK(gameless.h.errorOf("audio.autoDuck", {{"preset", "gameUnderVoice"}, {"music", Json::array({"A2"})}}).empty());
    }
}
