// audio.essentialSound and audio.remix (A.7) through the CommandEngine with fake analysis: a preset builds the expected
// stack (the record, then one marked effect per setting), applying again replaces it and keeps the clip's own effects,
// "none" clears it, loudness is matched, music ducks and remixes, sound effects pan; every request is one undo step and
// dryRun changes nothing. Remix puts the pieces on the track with constant-gain joins, or one take.

#include "CommandHarness.h"
#include "MusicTestSignals.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/audio/edit/Loudness.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

/// A clip's loudness: -35 LUFS plus its volume and the gain effects of its stack (no other effect changes it here).
class FakeLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        ++calls;
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
    int calls = 0;
};

class FakeSpeech final : public SpeechDetector {
public:
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId> tracks, SpeechSource,
                                                        std::optional<TimeRange>, command::ExecutionControl*) override {
        askedTracks.assign(tracks.begin(), tracks.end());
        return std::vector<TimeRange>{TimeRange{Ticks::fromSeconds(4), Ticks::fromSeconds(3)}, TimeRange{Ticks::fromSeconds(20), Ticks::fromSeconds(5)}};
    }
    std::vector<TrackId> askedTracks;
};

/// The synthetic song's analysis for any media; takes are written as small files.
class FakeMusic final : public MusicAnalyzer {
public:
    explicit FakeMusic(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::expected<MusicAnalysis, Error> analyze(const MediaItem&, int, std::stop_token, const std::function<void(double)>& progress) override {
        ++analyses;
        if (progress) {
            progress(1.0);
        }
        return defaultSongAnalysis();
    }
    std::expected<Take, Error> render(const MediaItem& item, int, const RemixPlan& plan, std::stop_token,
                                      const std::function<void(double)>&) override {
        ++renders;
        Take t;
        const auto file = m_dir / (item.name + "-remix.wav");
        std::ofstream(file) << "remix";
        t.path = pathToUtf8(file);
        t.info.path = t.path;
        t.info.duration = plan.duration;
        AudioStreamInfo a;
        a.index = 0;
        a.codec = "pcm_f32le";
        a.sampleRate = Rate{48000, 1};
        a.channels = 2;
        a.duration = plan.duration;
        t.info.audio.push_back(a);
        t.fingerprint.size = 99;
        t.fingerprint.sampleHash = "0123456789abcdef";
        t.elapsed = 0.5;
        return t;
    }
    int analyses = 0;
    int renders = 0;

private:
    std::filesystem::path m_dir;
};

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio_edit" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// V1: 60 s of picture; A1: 20 s of dialogue (no role yet); A2: the 80 s song (media M, 100 s long).
std::string picture() {
    return "V1 tgt |V" + std::string(1499, 'v') + "|\n" + "A1 tgt |D" + std::string(499, 'd') + "|\n" + "A2     |M" + std::string(1999, 'm') + "|\n";
}

struct Fixture {
    explicit Fixture(const std::string& name, bool withServices = true)
        : dir(scratch(name)), loudness(std::make_shared<FakeLoudness>()), speech(std::make_shared<FakeSpeech>()),
          music(std::make_shared<FakeMusic>(dir)),
          h(Timeline(picture(), {{'V', 3000}, {'D', 3000}, {'M', 2500}}), policy(withServices), ai::edit::editingCommands()) {}

    command::EnginePolicy policy(bool withServices) {
        if (!withServices) {
            return command::EnginePolicy{command::CapabilitySet::all(), 64, nullptr};
        }
        auto services = std::make_shared<AudioServices>(loudness, speech);
        services->music = music;
        return command::EnginePolicy{command::CapabilitySet::all(), 64, services};
    }
    const Clip& clip(ClipId id) const { return h.sequence(h.state()).findClip(id)->clip(); }
    const Track& track(std::string_view label) const { return *h.sequence(h.state()).findTrack(h.tl.track(label)); }
    std::vector<std::string> stackOf(ClipId id) const {
        std::vector<std::string> out;
        for (const EffectInstance& e : clip(id).effects()) {
            const EffectData* section = e.findData(kSectionKey);
            out.push_back(e.effect + (section != nullptr ? ":" + *section->text() : ""));
        }
        return out;
    }

    std::filesystem::path dir;
    std::shared_ptr<FakeLoudness> loudness;
    std::shared_ptr<FakeSpeech> speech;
    std::shared_ptr<FakeMusic> music;
    CommandHarness h;
};

double paramOf(const EffectInstance& e, std::string_view key) { return effectParameterAt(e, key, Ticks{}); }

} // namespace

TEST_CASE("A dialogue preset builds its stack: the record, then a marked effect per setting; loudness matched; one undo step",
          "[audio][edit][essential]") {
    Fixture f("es-dialogue");
    const ClipId dialogue = f.h.clip("A1@0");
    const std::size_t undo = f.h.session.undoCount();
    auto r = f.h.ok("audio.essentialSound", {{"clips", Json::array({dialogue.value})}, {"type", "dialogue"}, {"preset", "podcastVoice"}});
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.stackOf(dialogue) == std::vector<std::string>{"wabcut.audio.essentialSound", "wabcut.audio.gain:loudness",
                                                          "wabcut.audio.filter:repair.rumble", "wabcut.audio.denoise:repair.noise",
                                                          "wabcut.audio.deess:repair.deess", "wabcut.audio.filter:clarity.band",
                                                          "wabcut.audio.eq3:clarity.eq", "wabcut.audio.compressor:clarity.dynamics"});
    const EffectStack& stack = f.clip(dialogue).effects();
    // The record: type, preset and the options applied.
    const EffectData* type = stack[0].findData("type");
    REQUIRE(type);
    CHECK(*type->text() == "dialogue");
    CHECK(*stack[0].findData("preset")->text() == "podcastVoice");
    const Json options = Json::parse(*stack[0].findData("options")->text());
    CHECK(options["clarity"]["eq"] == "podcastVoice");
    CHECK(options["repair"]["rumble"] == 80.0);
    CHECK(recordedSoundType(stack) == SoundType::Dialogue);
    // The settings: rumble at 80 Hz, the noise reduction at 0.3, the EQ preset's shape.
    CHECK(paramOf(stack[2], "lowCut") == 80.0);
    CHECK(paramOf(stack[3], "amount") == Approx(0.3));
    CHECK(paramOf(stack[6], "midGain") == Approx(3.0));
    // Loudness: -35 LUFS matched to dialogue's -23 by the Gain (+12 dB).
    CHECK(paramOf(stack[1], "gain") == Approx(12.0).margin(0.01));
    CHECK(r.data["clips"][0]["loudness"]["after"]["integratedLufs"] == -23.0);
    // A1 had no role: it is the dialogue track now.
    CHECK(f.track("A1").role == TrackRole::Dialogue);
    REQUIRE(r.data["tracks"].size() == 1);
    // Every effect is a normal one: valid, evaluable, editable.
    for (const EffectInstance& e : stack) {
        CHECK(isEvaluable(e, EffectKind::Audio));
    }
    requireValid(*f.h.state());
    // Undo takes it all back.
    f.h.session.undo();
    CHECK(f.clip(dialogue).effects().empty());
    CHECK(f.track("A1").role == TrackRole::None);
}

TEST_CASE("Applying again replaces Essential Sound's effects and keeps the clip's own; options change a preset; none clears",
          "[audio][edit][essential]") {
    Fixture f("es-replace");
    const ClipId dialogue = f.h.clip("A1@0");
    f.h.ok("clip.addEffect", {{"clips", Json::array({dialogue.value})}, {"effect", "wabcut.audio.limiter"}});
    f.h.ok("audio.essentialSound", {{"clips", Json::array({dialogue.value})}, {"type", "dialogue"}, {"preset", "cleanUpNoisy"}});
    CHECK(f.stackOf(dialogue) == std::vector<std::string>{"wabcut.audio.essentialSound", "wabcut.audio.gain:loudness",
                                                          "wabcut.audio.filter:repair.rumble", "wabcut.audio.denoise:repair.noise",
                                                          "wabcut.audio.deess:repair.deess", "wabcut.audio.limiter"});
    // Again, another preset with options: 60 Hz hum, no loudness, a hall; the type is remembered (no "type").
    f.h.ok("audio.essentialSound", {{"clips", Json::array({dialogue.value})},
                                    {"preset", "phone"},
                                    {"options", Json{{"loudness", false}, {"repair", Json{{"hum", 60}}}, {"creative", Json{{"reverb", "hall"}, {"amount", 0.4}}}}}});
    CHECK(f.stackOf(dialogue) == std::vector<std::string>{"wabcut.audio.essentialSound", "wabcut.audio.dehum:repair.hum",
                                                          "wabcut.audio.filter:clarity.band", "wabcut.audio.eq3:clarity.eq",
                                                          "wabcut.audio.compressor:clarity.dynamics", "wabcut.audio.reverb:creative.reverb",
                                                          "wabcut.audio.limiter"});
    const EffectStack& stack = f.clip(dialogue).effects();
    CHECK(paramOf(stack[1], "frequency") == 60.0);
    CHECK(paramOf(stack[2], "lowCut") == 300.0);
    CHECK(paramOf(stack[2], "highCut") == 3400.0);
    CHECK(paramOf(stack[5], "mix") == Approx(0.4));
    CHECK(paramOf(stack[5], "decay") == 2600.0);
    // audio.normalize on the clip adjusts a gain right after the record, not before it.
    f.h.ok("audio.normalize", {{"clips", Json::array({dialogue.value})}, {"lufs", -20.0}});
    CHECK(f.clip(dialogue).effects()[0].effect == "wabcut.audio.essentialSound");
    CHECK(f.clip(dialogue).effects()[1].effect == "wabcut.audio.gain");
    CHECK(paramOf(f.clip(dialogue).effects()[1], "gain") == Approx(15.0).margin(0.15));
    // None: Essential Sound's effects go (the normalisation gain is the user's now: no section), the limiter stays.
    auto cleared = f.h.ok("audio.essentialSound", {{"clips", Json::array({dialogue.value})}, {"type", "none"}});
    CHECK(cleared.data["clips"][0]["removed"] == 6);
    CHECK(f.stackOf(dialogue) == std::vector<std::string>{"wabcut.audio.gain", "wabcut.audio.limiter"});
}

TEST_CASE("Essential Sound: a dry run changes nothing; errors commit nothing; without analysis loudness is skipped",
          "[audio][edit][essential]") {
    Fixture f("es-dry");
    const ClipId dialogue = f.h.clip("A1@0");
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.essentialSound"},
                                      {"params", Json{{"clips", Json::array({dialogue.value})}, {"type", "dialogue"}}},
                                      {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.dryRun);
    CHECK(f.h.engine.revision() == revision);
    CHECK(f.clip(dialogue).effects().empty());
    CHECK(preview.data["clips"][0]["effects"].size() == 2); // the record and the loudness gain
    const std::size_t undo = f.h.session.undoCount();
    CHECK(f.h.errorOf("audio.essentialSound", {{"clips", Json::array({dialogue.value})}, {"type", "dialogue"}, {"preset", "wide"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.essentialSound", {{"clips", Json::array({dialogue.value})}}) == "InvalidParams"); // no type, no role
    CHECK(f.h.errorOf("audio.essentialSound", {{"clips", Json::array({dialogue.value, f.h.clip("A2@0").value})}, {"type", "dialogue"},
                                               {"options", Json{{"duration", 30}}}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.essentialSound", {{"clips", Json::array({dialogue.value})}, {"type", "dialogue"},
                                               {"options", Json{{"repair", Json{{"hum", 30}}}}}}) == "InvalidParams");
    CHECK(f.h.session.undoCount() == undo);
    // A host without analysis: the stack goes on, the loudness is not matched (a warning).
    Fixture bare("es-bare", false);
    auto r = bare.h.ok("audio.essentialSound", {{"clips", Json::array({bare.h.clip("A1@0").value})}, {"type", "dialogue"}});
    CHECK_FALSE(r.warnings.empty());
    CHECK(paramOf(bare.clip(bare.h.clip("A1@0")).effects()[1], "gain") == 0.0);
}

TEST_CASE("Music ducks under the dialogue and sound effects take a position", "[audio][edit][essential]") {
    Fixture f("es-music");
    const ClipId dialogue = f.h.clip("A1@0");
    const ClipId music = f.h.clip("A2@0");
    f.h.ok("audio.essentialSound", {{"clips", Json::array({dialogue.value})}, {"type", "dialogue"}});
    auto r = f.h.ok("audio.essentialSound", {{"clips", Json::array({music.value})}, {"type", "music"}, {"preset", "background"},
                                             {"options", Json{{"width", 1.25}}}});
    CHECK(f.stackOf(music) == std::vector<std::string>{"wabcut.audio.essentialSound", "wabcut.audio.gain:loudness", "wabcut.audio.width:width"});
    CHECK(paramOf(f.clip(music).effects()[1], "gain") == Approx(10.0).margin(0.01)); // -35 to music's -25
    // The dialogue track (A1, dialogue now) was asked for speech; the music has volume keyframes under it.
    REQUIRE(f.speech->askedTracks.size() == 1);
    CHECK(f.speech->askedTracks[0] == f.h.tl.track("A1"));
    const ParamValue volume = f.clip(music).properties()[ParamId::VolumeDb];
    REQUIRE(volume.isCurve());
    CHECK(volume.curve()->keys.size() >= 8); // two duck regions
    CHECK(r.data["regions"].size() == 2);
    CHECK(f.track("A2").role == TrackRole::Music);
    // A sound effect: the clip's panner.
    auto sfx = f.h.ok("audio.essentialSound", {{"clips", Json::array({music.value})}, {"type", "sfx"}, {"options", Json{{"pan", -0.4}}}});
    CHECK(f.clip(music).properties().value(ParamId::Pan) == Approx(-0.4));
    CHECK(recordedSoundType(f.clip(music).effects()) == SoundType::Sfx);
}

TEST_CASE("audio.remix puts the music's pieces on its track with constant-gain joins, within a beat of the target",
          "[audio][edit][remix]") {
    Fixture f("remix-edits");
    const ClipId music = f.h.clip("A2@0");
    const Ticks start = f.clip(music).start();
    const std::size_t undo = f.h.session.undoCount();
    auto r = f.h.ok("audio.remix", {{"clip", music.value}, {"duration", 48}}); // 80 s less a verse and a chorus
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.music->analyses == 1);
    const Track& a2 = f.track("A2");
    const std::size_t pieces = r.data["segments"].size();
    REQUIRE(pieces >= 2);
    CHECK(a2.clips.size() == pieces);
    CHECK(a2.clips.front().id == music); // the first piece keeps the clip's id
    CHECK(a2.clips.front().start() == start);
    // Back to back, each a copy of the clip (its name, level and effects).
    for (std::size_t k = 1; k < a2.clips.size(); ++k) {
        CHECK(a2.clips[k].start() == a2.clips[k - 1].end());
        CHECK(a2.clips[k].sharesContentWith(a2.clips[0]));
    }
    const double seconds = (a2.clips.back().end() - start).toSeconds();
    CHECK(std::fabs(seconds - 48.0) <= defaultSong().beatSeconds());
    CHECK(r.data["seconds"] == Approx(seconds).margin(0.001));
    // A crossfade on every join.
    CHECK(a2.transitions.size() == pieces - 1);
    for (const Transition& t : a2.transitions) {
        CHECK(t.effect == "constantGain");
        // Centred on the cut (it ends on the landing beat's attack), even when shorter than two video frames.
        CHECK(t.before.value > 0);
        CHECK(t.before == t.after);
    }
    for (const Json& j : r.data["joins"]) {
        CHECK(j["confidence"].get<double>() > 0.8); // the song repeats itself: a seamless join
        CHECK_FALSE(j["transition"].is_null());
    }
    CHECK(r.warnings.empty());
    requireValid(*f.h.state());
    // Undo: the one clip again.
    f.h.session.undo();
    CHECK(f.track("A2").clips.size() == 1);
    CHECK(f.track("A2").transitions.empty());

    // dryRun: the plan, nothing changed.
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.remix"}, {"params", Json{{"clip", music.value}, {"fit", "video"}}}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(f.h.engine.revision() == revision);
    CHECK(std::fabs(preview.data["seconds"].get<double>() - 60.0) <= defaultSong().beatSeconds()); // V1 ends at 60 s
    CHECK(f.track("A2").clips.size() == 1);
}

TEST_CASE("audio.remix: a longer remix that would run into the next clip, a linked clip and no analysis are refused; a take",
          "[audio][edit][remix]") {
    Fixture f("remix-take");
    const ClipId music = f.h.clip("A2@0");
    // Room: a clip on A2 at 90 s.
    {
        Fixture blocked("remix-blocked");
        const std::string p = "V1 |V" + std::string(1499, 'v') + "|\nA1 |D" + std::string(499, 'd') + "|\nA2 |M" + std::string(1999, 'm') +
                              std::string(250, '.') + "Xxxx|\n";
        CommandHarness h(Timeline(p, {{'V', 3000}, {'D', 3000}, {'M', 2500}}), blocked.policy(true), ai::edit::editingCommands());
        CHECK(h.errorOf("audio.remix", {{"clip", h.clip("A2@0").value}, {"duration", 120}}) == "EditRejected");
        CHECK(h.errorOf("audio.remix", {{"clip", h.clip("A2@0").value}, {"duration", 50}}).empty()); // shorter: fine
    }
    CHECK(f.h.errorOf("audio.remix", {{"clip", music.value}}) == "InvalidParams"); // no target
    Fixture bare("remix-bare", false);
    CHECK(bare.h.errorOf("audio.remix", {{"clip", bare.h.clip("A2@0").value}, {"duration", 50}}) == "InvalidRequest");

    // The take (audio.remixRender): one new media item, the clip plays it.
    const std::size_t mediaBefore = f.h.state()->media.size();
    auto r = f.h.ok("audio.remixRender", {{"clip", music.value}, {"duration", 100}});
    CHECK(f.music->renders == 1);
    CHECK(f.h.state()->media.size() == mediaBefore + 1);
    const Clip& clip = f.clip(music);
    const MediaItem* take = f.h.state()->findMedia(clip.media);
    REQUIRE(take);
    REQUIRE(take->derived);
    CHECK(take->derived->operation == "remix");
    CHECK(take->derived->source == f.h.tl.media('M'));
    CHECK(clip.sourceIn == Ticks{});
    CHECK(std::fabs(clip.duration().toSeconds() - 100.0) <= defaultSong().beatSeconds());
    CHECK(f.track("A2").clips.size() == 1);
    CHECK(r.data["output"] == "take");
    requireValid(*f.h.state());
}

TEST_CASE("Essential Sound's music duration remixes the clip; its pieces carry the stack", "[audio][edit][essential][remix]") {
    Fixture f("es-remix");
    const ClipId music = f.h.clip("A2@0");
    auto r = f.h.ok("audio.essentialSound", {{"clips", Json::array({music.value})}, {"type", "music"}, {"options", Json{{"duration", 45}}}});
    const Track& a2 = f.track("A2");
    REQUIRE(a2.clips.size() >= 2);
    for (const Clip& piece : a2.clips) {
        CHECK(recordedSoundType(piece.effects()) == SoundType::Music);
    }
    CHECK(std::fabs((a2.clips.back().end() - a2.clips.front().start()).toSeconds() - 45.0) <= defaultSong().beatSeconds());
    CHECK(r.data["clips"][0].contains("remix"));
    requireValid(*f.h.state());
}

namespace {

/// Renders count; the take files are small stand-ins.
class CountingEnhancer final : public SpeechEnhancer {
public:
    explicit CountingEnhancer(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::string modelVersion() const override { return "fake-dfn@1"; }
    std::expected<Take, Error> render(const MediaItem& item, int stream, std::stop_token, const std::function<void(double)>&) override {
        ++renders;
        Take t;
        const auto file = m_dir / (item.name + "-a" + std::to_string(stream) + ".wav");
        std::ofstream(file) << "take";
        t.path = pathToUtf8(file);
        t.model = modelVersion();
        return t;
    }
    int renders = 0;

private:
    std::filesystem::path m_dir;
};

} // namespace

TEST_CASE("Commands that add or change media are project edits: remix's take is audio.remixRender (A.8 audit)",
          "[audio][edit][essential][remix][capability]") {
    // What each audio command may touch (MCP scopes, the assistant's allow-list and read-only MCP depend on it).
    const command::CommandRegistry& registry = ai::edit::editingCommands();
    const std::map<std::string, command::Capability> expected{
        {"audio.remix", command::Capability::EditSequence},          // pieces on the timeline only
        {"audio.remixRender", command::Capability::EditProject},     // adds the take as a media item
        {"audio.enhanceSpeech", command::Capability::EditProject},   // records the take on the media item
        {"audio.separateStems", command::Capability::EditProject},   // adds stems and a bin
        {"audio.essentialSound", command::Capability::EditSequence}, // effects, keyframes, track roles, remix pieces
        {"audio.syncClips", command::Capability::EditSequence},
        {"track.setRecordArm", command::Capability::EditSequence},
        {"record.takes", command::Capability::Read},
        {"record.useTake", command::Capability::EditSequence},
    };
    for (const auto& [name, capability] : expected) {
        INFO(name);
        const command::CommandSpec* spec = registry.find(name);
        REQUIRE(spec);
        CHECK(spec->capability == capability);
    }
    // A client allowed sequence edits only (the assistant, MCP without project edits) cannot render a remix or a take.
    Fixture f("cap-audit");
    const ClipId music = f.h.clip("A2@0");
    auto services = std::make_shared<AudioServices>(f.loudness, f.speech);
    services->music = f.music;
    auto enhancer = std::make_shared<CountingEnhancer>(f.dir);
    services->enhancer = enhancer;
    CommandHarness sequenceOnly(Timeline(picture(), {{'V', 3000}, {'D', 3000}, {'M', 2500}}),
                                command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence}),
                                                      64, services},
                                ai::edit::editingCommands());
    CHECK(sequenceOnly.errorOf("audio.remixRender", {{"clip", music.value}, {"duration", 50}}) == "PermissionDenied");
    CHECK(sequenceOnly.errorOf("audio.enhanceSpeech", {{"clips", Json::array({sequenceOnly.clip("A1@0").value})}}) == "PermissionDenied");
    CHECK(sequenceOnly.errorOf("audio.remix", {{"clip", music.value}, {"duration", 48}}).empty());
    CHECK(f.music->renders == 0);
    // audio.remix no longer takes an output: a take is audio.remixRender.
    CHECK(f.h.errorOf("audio.remix", {{"clip", music.value}, {"duration", 48}, {"output", "take"}}) == "InvalidParams");
    // Essential Sound's Enhance Speech applies the effect and never renders the take (that changes the media item).
    auto r = sequenceOnly.ok("audio.essentialSound", {{"clips", Json::array({sequenceOnly.clip("A1@0").value})},
                                                      {"type", "dialogue"},
                                                      {"options", Json{{"loudness", false}, {"repair", Json{{"enhance", true}}}}}});
    CHECK(enhancer->renders == 0);
    const Clip& dialogue = sequenceOnly.sequence(sequenceOnly.state()).findClip(sequenceOnly.clip("A1@0"))->clip();
    CHECK(dialogue.effects()[1].effect == "wabcut.audio.enhanceSpeech");
    REQUIRE(r.data["takes"].size() == 1);
    CHECK(r.data["takes"][0]["state"] == "missing");
    CHECK_FALSE(r.warnings.empty());
    for (const MediaPtr& m : sequenceOnly.state()->media) {
        CHECK(m->audioTakes.empty());
    }
}
