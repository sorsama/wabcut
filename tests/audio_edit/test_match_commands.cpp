// audio.eqMatch, audio.eqMatchReset and audio.levelMatch through the CommandEngine, with a fake render (0.7, card 045): the
// EQ Match learned from the difference of two spectra and applied to the target clips as a marked Graphic Equalizer (30
// Bands) in one undo step, learning again without the earlier match, the refusals (silent, too short, no reference), a
// media file as the reference, the level matched to the reference's loudness or true peak with a headroom and a limiter, and
// the learned curve surviving a save and a load of the project.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/audio/edit/Loudness.h>
#include <wabcut/audio/edit/MatchCurve.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <string>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

constexpr std::string_view kEqId = "wabcut.audio.graphicEq30";

/// What a signal sounds like to the fake render: its average spectrum and its levels.
struct Signal {
    std::array<double, kMatchBandCount> spectrum{};
    double lufs = -23.0;
    double truePeak = -10.0;
    double rms = -27.0;
    double crest = 12.0;
    double activeSeconds = 10.0;
};

/// A tilted spectrum: `base` dB at the 1 kHz band, `tilt` dB per band.
std::array<double, kMatchBandCount> tilted(double base, double tilt) {
    std::array<double, kMatchBandCount> out{};
    for (int b = 0; b < kMatchBandCount; ++b) {
        out[static_cast<std::size_t>(b)] = base + tilt * (b - 15);
    }
    return out;
}

/// The render of a clip: its media's signal, the stack's Graphic EQ sliders added to the spectrum and its gain effects
/// to the levels (the way preview and export would hear them).
class FakeMatch final : public LoudnessAnalyzer {
public:
    explicit FakeMatch(std::map<MediaId, Signal> media) : m_media(std::move(media)) {}

    std::expected<LoudnessStats, Error> measure(const ProjectState&, SequenceId, const LoudnessTarget&, std::optional<TimeRange>,
                                                command::ExecutionControl*) override {
        return std::unexpected(Error{ErrorCode::Unsupported, "not used"});
    }

    std::expected<MatchStats, Error> measureMatch(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                  std::optional<TimeRange>, command::ExecutionControl*) override {
        ++calls;
        const Sequence& seq = *state.findSequence(sequence);
        const Clip* clip = nullptr;
        const EffectStack* above = nullptr; // the stack of the track or the master over the clip
        if (target.scope == LoudnessTarget::Scope::Clip) {
            clip = &seq.findClip(target.clip)->clip();
        } else if (target.scope == LoudnessTarget::Scope::Track) {
            const Track& t = *seq.findTrack(target.track);
            clip = t.clips.empty() ? nullptr : &t.clips.front(); // a track of the fake is its first clip
            above = &t.effects;
        } else {
            for (const TrackPtr& t : seq.audioTracks) {
                if (!t->clips.empty()) {
                    clip = &t->clips.front(); // the mix of the fake: its first clip
                    break;
                }
            }
            above = &seq.audioEffects;
        }
        MatchStats out;
        out.channels = 2;
        out.sampleRate = 48000;
        if (clip == nullptr) {
            return out;
        }
        Signal s = m_media.at(clip->media);
        double gain = clip->properties().value(ParamId::VolumeDb);
        for (const EffectInstance& e : clip->effects()) {
            if (!e.enabled) {
                continue;
            }
            if (e.effect == "wabcut.audio.gain") {
                gain += effectParameterAt(e, "gain", Ticks{});
            } else if (e.effect == kEqId) {
                for (int b = 0; b < kMatchBandCount; ++b) {
                    s.spectrum[static_cast<std::size_t>(b)] += effectParameterAt(e, "band" + std::to_string(b + 1), Ticks{});
                }
            }
        }
        if (above != nullptr) {
            for (const EffectInstance& e : *above) {
                if (e.enabled && e.effect == "wabcut.audio.gain") {
                    gain += effectParameterAt(e, "gain", Ticks{});
                }
            }
        }
        out.loudness.integratedLufs = s.lufs + gain;
        out.loudness.truePeakDbtp = s.truePeak + gain;
        out.loudness.duration = Ticks{10 * kTicksPerSecond};
        out.rmsDbfs = s.rms + gain;
        out.crestDb = s.crest;
        out.spectrum.levelDb = s.spectrum;
        out.spectrum.seconds = 10.0;
        out.spectrum.activeSeconds = s.activeSeconds;
        out.spectrum.sampleRate = 48000;
        return out;
    }

    int calls = 0;

private:
    std::map<MediaId, Signal> m_media;
};

constexpr std::string_view kPicture = R"(
    V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    A3              |Sssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss|
    A4              |Hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh|
)";

struct Fixture {
    Fixture()
        : tl(kPicture),
          render(std::make_shared<FakeMatch>(std::map<MediaId, Signal>{
              {tl.media('A'), Signal{tilted(-60.0, 0.0), -30.0, -20.0, -34.0, 14.0, 10.0}},     // the target: flat, quiet
              {tl.media('M'), Signal{tilted(-56.0, 0.3), -24.0, -14.0, -28.0, 14.0, 10.0}},     // the reference: brighter, louder
              {tl.media('S'), Signal{tilted(-60.0, 0.0), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -95.0, 0.0, 0.2}}, // silent: 0.2 s audible
              {tl.media('H'), Signal{tilted(-50.0, 0.0), -6.0, 4.0, -12.0, 16.0, 10.0}}})),     // hot: a true peak over full scale
          h(tl, command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(render, nullptr)},
            ai::edit::editingCommands()) {}

    const Sequence& seq() const { return h.sequence(h.state()); }
    const Track& track(int a) const { return *seq().audioTracks[static_cast<std::size_t>(a - 1)]; }
    ClipId target() const { return h.clip("A1@0"); }
    ClipId music() const { return h.clip("A2@0"); }
    /// The EQ Match instance of the target clip, or null.
    const EffectInstance* matchOf(ClipId id) const {
        for (const EffectInstance& e : seq().findClip(id)->clip().effects()) {
            if (e.effect == kEqId && e.findData("eqMatch") != nullptr) {
                return &e;
            }
        }
        return nullptr;
    }

    Timeline tl;
    std::shared_ptr<FakeMatch> render;
    CommandHarness h;
};

Json clipsOf(std::initializer_list<ClipId> ids) {
    Json out = Json::array();
    for (const ClipId id : ids) {
        out.push_back(id.value);
    }
    return out;
}

} // namespace

TEST_CASE("audio.eqMatch gives the target a marked Graphic EQ from the difference of the spectra, in one undo step", "[audio][edit][match]") {
    Fixture f;
    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.music().value}}},
                                            {"clips", clipsOf({f.target()})},
                                            {"smoothing", 0}});
    CHECK(f.h.session.undoCount() == undo + 1);
    REQUIRE(r.createdIds.size() == 1);
    const EffectInstance* match = f.matchOf(f.target());
    REQUIRE(match != nullptr);
    CHECK(match->id.value == r.createdIds[0]);
    // The reference tilts 0.3 dB per band over the target: the curve is that tilt with its mean removed (bands 5 to 28).
    for (int b = kMatchFirstBand; b < kMatchFirstBand + kMatchBands; ++b) {
        INFO("band " << (b + 1));
        CHECK(effectParameterAt(*match, "band" + std::to_string(b + 1), Ticks{}) == Approx(0.3 * (b - 15.5)).margin(0.02));
    }
    // The bands beside the matched ones fade out, those far away are untouched.
    CHECK(std::fabs(effectParameterAt(*match, "band1", Ticks{})) < 0.3 * 11.5);
    CHECK(r.data["applied"] == true);
    CHECK(r.data["curve"]["bands"].size() == 30);
    CHECK(r.data["curve"]["levelOffsetDb"].get<double>() == Approx(4.15).margin(0.01)); // the level it leaves alone
    CHECK(r.data["reference"]["stats"]["integratedLufs"].get<double>() == Approx(-24.0));
    CHECK(r.data["target"]["stats"]["integratedLufs"].get<double>() == Approx(-30.0));
    // The Graphic Equalizer is an ordinary effect of the clip's stack and the spectra ride along in its data.
    REQUIRE(match->findData("eqMatchReference") != nullptr);
    REQUIRE(match->findData("eqMatchTarget") != nullptr);
    CHECK(match->findData("eqMatchReference")->points()->size() >= 20);
    // Undo takes it off again in one step.
    REQUIRE(f.h.session.undo());
    CHECK(f.matchOf(f.target()) == nullptr);
}

TEST_CASE("audio.eqMatch with apply false only learns", "[audio][edit][match]") {
    Fixture f;
    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}, {"apply", false}});
    CHECK(f.h.session.undoCount() == undo);
    CHECK(f.matchOf(f.target()) == nullptr);
    CHECK(r.data["applied"] == false);
    CHECK(r.data["curve"]["bands"][15]["referenceDb"].is_number());
    CHECK(r.createdIds.empty());
}

TEST_CASE("A second EQ Match replaces the first and learns without it", "[audio][edit][match]") {
    Fixture f;
    const Json params{{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}, {"smoothing", 0}};
    f.h.ok("audio.eqMatch", params);
    const double first = effectParameterAt(*f.matchOf(f.target()), "band20", Ticks{});
    const auto again = f.h.ok("audio.eqMatch", params);
    int matches = 0;
    for (const EffectInstance& e : f.seq().findClip(f.target())->clip().effects()) {
        matches += e.effect == kEqId ? 1 : 0;
    }
    CHECK(matches == 1);
    // Not learned from its own correction: the same curve, not twice as much.
    CHECK(effectParameterAt(*f.matchOf(f.target()), "band20", Ticks{}) == Approx(first).margin(1e-6));
    CHECK(again.data["target"]["stats"]["integratedLufs"].get<double>() == Approx(-30.0));
    // A user's own Graphic EQ is no EQ Match and stays.
    f.h.ok("clip.addEffect", {{"clips", clipsOf({f.target()})}, {"effect", "wabcut.audio.graphicEq30"}});
    f.h.ok("audio.eqMatch", params);
    int equalizers = 0;
    for (const EffectInstance& e : f.seq().findClip(f.target())->clip().effects()) {
        equalizers += e.effect == kEqId ? 1 : 0;
    }
    CHECK(equalizers == 2);
}

TEST_CASE("audio.eqMatch sits after the normalisation gain and before the dynamics", "[audio][edit][match]") {
    Fixture f;
    f.h.ok("clip.addEffect", {{"clips", clipsOf({f.target()})}, {"effect", "wabcut.audio.compressor"}});
    f.h.ok("clip.addEffect", {{"clips", clipsOf({f.target()})}, {"effect", "wabcut.audio.gain"}, {"params", {{"gain", 3}}}, {"index", 0}});
    f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}});
    const EffectStack& stack = f.seq().findClip(f.target())->clip().effects();
    REQUIRE(stack.size() == 3);
    CHECK(stack[0].effect == "wabcut.audio.gain");
    CHECK(stack[1].effect == kEqId);
    CHECK(stack[2].effect == "wabcut.audio.compressor");
}

TEST_CASE("audio.eqMatch refuses a silent reference, a too short target and a missing reference", "[audio][edit][match]") {
    Fixture f;
    // The reference is the (nearly) silent clip: 0.2 s of audible sound.
    CHECK(f.h.errorOf("audio.eqMatch", {{"reference", {{"clip", f.h.clip("A3@0").value}}}, {"clips", clipsOf({f.target()})}}) ==
          "EditRejected");
    CHECK(f.h.errorOf("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.h.clip("A3@0")})}}) ==
          "EditRejected");
    CHECK(f.h.errorOf("audio.eqMatch", {{"clips", clipsOf({f.target()})}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.eqMatch", {{"reference", "somewhere"}, {"clips", clipsOf({f.target()})}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}}) == "InvalidParams");
    // A host without spectra refuses.
    CommandHarness bare(Timeline(kPicture), {}, ai::edit::editingCommands());
    CHECK(bare.errorOf("audio.eqMatch", {{"reference", "mix"}, {"clips", Json::array({1})}}) == "InvalidRequest");
}

TEST_CASE("audio.eqMatch takes a media file as the reference and a range of the mix", "[audio][edit][match]") {
    Fixture f;
    const MediaId music = f.tl.media('M');
    const auto r = f.h.ok("audio.eqMatch", {{"reference", {{"media", music.value}}}, {"clips", clipsOf({f.target()})}, {"smoothing", 0}});
    REQUIRE(f.matchOf(f.target()) != nullptr);
    CHECK(r.data["reference"]["source"]["media"] == music.value);
    CHECK(effectParameterAt(*f.matchOf(f.target()), "band20", Ticks{}) == Approx(0.3 * (19 - 15.5)).margin(0.02));
    // The scratch sequence the file was measured in is not left in the project.
    CHECK(f.h.state()->sequences.size() == 1);
    CHECK(f.h.errorOf("audio.eqMatch", {{"reference", {{"media", 999999}}}, {"clips", clipsOf({f.target()})}}) == "NotFound");
    CHECK(f.h.errorOf("audio.eqMatch", {{"reference", {{"media", music.value}, {"stream", 5}}}, {"clips", clipsOf({f.target()})}}) ==
          "InvalidParams");
    // The mix, over a range.
    f.h.ok("audio.eqMatch", {{"reference", "mix"}, {"referenceFrom", tc(0)}, {"referenceTo", tc(50)}, {"clips", clipsOf({f.target()})}});
    CHECK(f.matchOf(f.target()) != nullptr);
}

TEST_CASE("audio.eqMatch matches several clips with one curve and warns of a big level difference", "[audio][edit][match]") {
    Fixture f;
    const ClipId other = f.h.clip("A3@0");
    const auto r = f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target(), f.h.clip("A4@0")})}});
    CHECK(f.matchOf(f.target()) != nullptr);
    CHECK(f.matchOf(f.h.clip("A4@0")) != nullptr);
    CHECK(f.matchOf(other) == nullptr);
    CHECK(r.createdIds.size() == 2);
    // The hot clip is 18 dB louder than the music reference's -24 LUFS only in its levels; spectrally 6 dB: no warning.
    // A level gap above 6 dB in the bands is reported.
    const auto warned = f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.h.clip("A4@0").value}}}, {"clips", clipsOf({f.target()})}, {"apply", false}});
    CHECK_FALSE(warned.warnings.empty());
}

TEST_CASE("audio.eqMatchReset takes the EQ Match off and nothing else", "[audio][edit][match]") {
    Fixture f;
    f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}});
    f.h.ok("clip.addEffect", {{"clips", clipsOf({f.target()})}, {"effect", "wabcut.audio.graphicEq30"}});
    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.eqMatchReset", {{"clips", clipsOf({f.target()})}});
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(r.data["removed"] == 1);
    CHECK(f.matchOf(f.target()) == nullptr);
    CHECK(f.seq().findClip(f.target())->clip().effects().size() == 1); // the user's own equalizer stays
    const auto none = f.h.ok("audio.eqMatchReset", {{"clips", clipsOf({f.target()})}});
    CHECK(none.data["removed"] == 0);
    CHECK_FALSE(none.warnings.empty());
}

TEST_CASE("The learned EQ curve survives saving and loading the project", "[audio][edit][match]") {
    Fixture f;
    f.h.ok("audio.eqMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}});
    const EffectInstance before = *f.matchOf(f.target());
    const auto json = io::toJson(*f.h.state());
    REQUIRE(json.has_value());
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded.has_value());
    const Sequence& seq = *loaded->findSequence(f.h.tl.seq);
    const EffectInstance* match = nullptr;
    for (const EffectInstance& e : seq.findClip(f.target())->clip().effects()) {
        match = e.findData("eqMatch") != nullptr ? &e : match;
    }
    REQUIRE(match != nullptr);
    CHECK(*match == before); // every slider, both spectra and the marker
    CHECK(match->findData("eqMatchTarget")->points()->size() == before.findData("eqMatchTarget")->points()->size());
}

// ---- Level match -----------------------------------------------------------------------------------------------------

TEST_CASE("audio.levelMatch moves the target's loudness to the reference's and reports before and after", "[audio][edit][match]") {
    Fixture f;
    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.levelMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}});
    CHECK(f.h.session.undoCount() == undo + 1);
    const Json& row = r.data["results"][0];
    CHECK(row["before"]["integratedLufs"].get<double>() == Approx(-30.0));
    CHECK(row["after"]["integratedLufs"].get<double>() == Approx(-24.0).margin(0.1)); // within 0.1 LU
    CHECK(row["gainDb"].get<double>() == Approx(6.0).margin(0.1));
    CHECK(row["before"]["rmsDbfs"].get<double>() == Approx(-34.0));
    CHECK(row["before"]["crestDb"].get<double>() == Approx(14.0));
    CHECK(r.data["reference"]["stats"]["integratedLufs"].get<double>() == Approx(-24.0));
    CHECK(r.data["mode"] == "loudness");
    // A true peak of -14 dBTP is under the 1 dB headroom: no warning, no limiter.
    CHECK(r.warnings.empty());
    CHECK(f.seq().findClip(f.target())->clip().effects().size() == 1);
    // Again: already there.
    const auto again = f.h.ok("audio.levelMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}});
    CHECK(again.data["results"][0]["passes"] == 0);
}

TEST_CASE("audio.levelMatch in peak mode matches the true peak", "[audio][edit][match]") {
    Fixture f;
    const auto r = f.h.ok("audio.levelMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}, {"mode", "peak"}});
    const Json& row = r.data["results"][0];
    CHECK(row["after"]["truePeakDbtp"].get<double>() == Approx(-14.0).margin(0.1));
    CHECK(row["gainDb"].get<double>() == Approx(6.0).margin(0.1)); // -20 -> -14 dBTP
}

TEST_CASE("audio.levelMatch warns when the matched level passes the headroom and limits on request", "[audio][edit][match]") {
    Fixture f;
    // The hot clip is -6 LUFS with a +4 dBTP true peak: matched to it, the target would pass the -1 dBTP headroom.
    const ClipId hot = f.h.clip("A4@0");
    const auto warned = f.h.ok("audio.levelMatch", {{"reference", {{"clip", hot.value}}}, {"clips", clipsOf({f.target()})}});
    CHECK_FALSE(warned.warnings.empty());
    CHECK(f.seq().findClip(f.target())->clip().effects().size() == 1); // gain only
    REQUIRE(f.h.session.undo());
    const auto limited = f.h.ok("audio.levelMatch", {{"reference", {{"clip", hot.value}}}, {"clips", clipsOf({f.target()})},
                                                     {"limit", true}, {"headroom", 1.5}});
    const EffectStack& stack = f.seq().findClip(f.target())->clip().effects();
    REQUIRE(stack.size() == 2);
    CHECK(stack[0].effect == "wabcut.audio.gain");
    CHECK(stack[1].effect == "wabcut.audio.limiter");
    CHECK(effectParameterAt(stack[1], "ceiling", Ticks{}) == Approx(-1.5));
    CHECK(limited.data["results"][0]["limited"] == true);
    CHECK(limited.data["headroomDb"] == 1.5);
}

TEST_CASE("audio.levelMatch matches a track and refuses a silent reference or a bad mode", "[audio][edit][match]") {
    Fixture f;
    const auto r = f.h.ok("audio.levelMatch", {{"reference", {{"clip", f.music().value}}}, {"track", "A1"}});
    CHECK(r.data["results"][0]["target"]["track"] == "A1");
    CHECK(effectParameterAt(f.track(1).effects.front(), "gain", Ticks{}) == Approx(6.0).margin(0.1));
    CHECK(f.h.errorOf("audio.levelMatch", {{"reference", {{"clip", f.h.clip("A3@0").value}}}, {"clips", clipsOf({f.target()})}}) ==
          "EditRejected");
    CHECK(f.h.errorOf("audio.levelMatch", {{"reference", {{"clip", f.music().value}}}, {"clips", clipsOf({f.target()})}, {"mode", "rms"}}) ==
          "InvalidParams");
    CHECK(f.h.errorOf("audio.levelMatch", {{"clips", clipsOf({f.target()})}}) == "InvalidParams");
}
