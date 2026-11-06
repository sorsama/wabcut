// Card 137, auto-mix of several voices: the planner (which microphone is lowered when, never under its own words) and
// audio.autoMix through the CommandEngine with fake speech detection (one undo step, dry run, per-voice report, gate,
// refusals).

#include "CommandHarness.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/audio/edit/AutoMix.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
TimeRange span(double from, double to) { return TimeRange::fromStartEnd(s(from), s(to)); }

AutoMixOptions options() {
    AutoMixOptions o; // amount -12, attack 0.1, release 0.5, hold 0.4, lead 0.08, minSpeech 0.2
    return o;
}

double offsetAt(const std::vector<DuckRegion>& regions, const AutoMixOptions& o, double seconds) {
    return duckOffsetDb(regions, duckingOptionsOf(o), s(seconds));
}

/// Speech detection by track: each track returns what the test gave it.
class PerTrackSpeech final : public SpeechDetector {
public:
    void set(TrackId track, std::vector<TimeRange> speech) { m_speech[track.value] = std::move(speech); }
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId> tracks,
                                                        SpeechSource source, std::optional<TimeRange>, command::ExecutionControl*) override {
        asked.emplace_back(tracks.begin(), tracks.end());
        askedSource = source;
        std::vector<TimeRange> out;
        for (const TrackId t : tracks) {
            const auto it = m_speech.find(t.value);
            if (it != m_speech.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
        return out;
    }
    std::vector<std::vector<TrackId>> asked;
    SpeechSource askedSource = SpeechSource::Auto;

private:
    std::map<std::uint64_t, std::vector<TimeRange>> m_speech;
};

/// Twelve seconds of picture (a character is a frame at 25 fps): the video and the host's mic, the guest's mic, the music.
std::string picture() {
    const auto row = [](const char* head, char media) {
        return std::string(head) + "|" + media + std::string(299, static_cast<char>(media + 32)) + "|\n";
    };
    return "\n" + row("    V1 tgt          ", 'A') + row("    A1 tgt          ", 'A') + row("    A2              ", 'B') +
           row("    A3 music        ", 'M');
}

struct Fixture {
    Fixture()
        : tl(picture()),
          speech(std::make_shared<PerTrackSpeech>()),
          h(tl, command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(nullptr, speech)},
            ai::edit::editingCommands()) {}

    const Sequence& seq() const { return h.sequence(h.state()); }
    const Clip& clip(const char* name) const { return h.sequence(h.state()).findClip(h.clip(name))->clip(); }
    ParamValue volume(const char* name) const { return clip(name).properties()[ParamId::VolumeDb]; }
    double volumeAt(const char* name, double seconds) const {
        const Clip& c = clip(name);
        return c.properties()[ParamId::VolumeDb].valueAt(c.curveTimeAt(s(seconds)));
    }

    Timeline tl;
    std::shared_ptr<PerTrackSpeech> speech;
    CommandHarness h;
};

} // namespace

TEST_CASE("Auto-mix lowers each microphone while the other voice speaks and brings it up before its own words",
          "[audio][edit][automix]") {
    const AutoMixOptions o = options();
    // The host speaks 1-3 s, the guest 4-6 s: they take turns.
    const std::vector<std::vector<TimeRange>> speech{{span(1.0, 3.0)}, {span(4.0, 6.0)}};

    const auto host = autoMixRegions(speech, 0, o);
    REQUIRE(host.size() == 1);
    // Down from where the host's tail ends (3.4 s), so not under it; the guest speaks 4-6 s and is held to 6.4 s.
    CHECK(host[0].downStart == s(3.9));
    CHECK(host[0].speech == span(4.0, 6.4));
    CHECK(host[0].upStart == s(6.4));
    CHECK(host[0].upEnd == s(6.9));
    CHECK(offsetAt(host, o, 2.0) == 0.0);  // the host speaking: full level
    CHECK(offsetAt(host, o, 3.9) == 0.0);
    CHECK(offsetAt(host, o, 3.95) == Approx(-6.0));
    CHECK(offsetAt(host, o, 5.0) == -12.0);
    CHECK(offsetAt(host, o, 6.65) == Approx(-6.0));
    CHECK(offsetAt(host, o, 7.0) == 0.0);

    const auto guest = autoMixRegions(speech, 1, o);
    REQUIRE(guest.size() == 1);
    CHECK(guest[0].downStart == s(0.9));
    CHECK(guest[0].speech.start == s(1.0));
    // The host is held to 3.4 s, then the 0.5 s ramp up (to 3.9 s), 0.1 s before the guest's first word at 4 s.
    CHECK(guest[0].upStart == s(3.4));
    CHECK(guest[0].upEnd == s(3.9));
    CHECK(offsetAt(guest, o, 2.0) == -12.0);
    CHECK(offsetAt(guest, o, 3.9) == 0.0);
    CHECK(offsetAt(guest, o, 4.0) == 0.0);
    CHECK(offsetAt(guest, o, 5.0) == 0.0);
}

TEST_CASE("Auto-mix never lowers a voice under its own words also when it talks over the other", "[audio][edit][automix]") {
    const AutoMixOptions o = options();
    // The host speaks 1-5 s, the guest 4-8 s: they overlap for a second.
    const std::vector<std::vector<TimeRange>> speech{{span(1.0, 5.0)}, {span(4.0, 8.0)}};

    const auto guest = autoMixRegions(speech, 1, o);
    REQUIRE(guest.size() == 1);
    CHECK(guest[0].upEnd < s(4.0)); // full level before the guest's first word
    CHECK(guest[0].upEnd == s(3.92));
    for (double t = 4.0; t <= 8.4; t += 0.05) {
        INFO("t = " << t);
        CHECK(offsetAt(guest, o, t) == 0.0);
    }
    const auto host = autoMixRegions(speech, 0, o);
    REQUIRE(host.size() == 1);
    // Nothing is lowered while both speak (4-5 s), and the host goes down only after its own held tail (5.4 s).
    for (double t = 1.0; t <= 5.4; t += 0.05) {
        INFO("t = " << t);
        CHECK(offsetAt(host, o, t) == 0.0);
    }
    CHECK(host[0].downStart == s(5.4));
    CHECK(host[0].speech.start == s(5.5));
    CHECK(offsetAt(host, o, 6.5) == -12.0);
}

TEST_CASE("Auto-mix ignores blips and bridges short gaps and leaves stretches too short to ramp alone", "[audio][edit][automix]") {
    AutoMixOptions o = options();
    SECTION("a cough on the other microphone lowers nothing") {
        const std::vector<std::vector<TimeRange>> speech{{span(1.0, 3.0)}, {span(5.0, 5.1)}};
        CHECK(autoMixRegions(speech, 0, o).empty());
    }
    SECTION("a gap shorter than a ramp down and up is bridged") {
        const std::vector<std::vector<TimeRange>> speech{{span(10.0, 11.0)}, {span(1.0, 2.0), span(2.9, 4.0)}};
        const auto host = autoMixRegions(speech, 0, o);
        REQUIRE(host.size() == 1);
        CHECK(host[0].speech == span(1.0, 4.4));
        // A longer gap is two regions.
        const std::vector<std::vector<TimeRange>> far{{span(10.0, 11.0)}, {span(1.0, 2.0), span(3.2, 4.0)}};
        CHECK(autoMixRegions(far, 0, o).size() == 2);
    }
    SECTION("the voice's own words in a gap keep the two stretches apart") {
        // The guest speaks 1-2 s and 3-4 s; the host answers in between (2.2-2.8 s).
        const std::vector<std::vector<TimeRange>> speech{{span(2.2, 2.8)}, {span(1.0, 2.0), span(3.0, 4.0)}};
        const auto host = autoMixRegions(speech, 0, o);
        REQUIRE(host.size() == 2);
        CHECK(host[0].upEnd <= s(2.2 - 0.08 + 1e-9)); // up before the host's words
        CHECK(host[1].downStart >= s(3.2));            // down after the host's held words end (3.2 s)
        CHECK(offsetAt(host, o, 2.5) == 0.0);
    }
    SECTION("a stretch shorter than the ramps is dropped") {
        o.hold = Ticks{};
        const std::vector<std::vector<TimeRange>> speech{{span(1.0, 1.3)}, {span(1.3, 3.0)}};
        CHECK(autoMixRegions(speech, 1, o).empty()); // 0.3 s of the host before the guest starts: not worth a dip
    }
}

TEST_CASE("Auto-mix gate also lowers a microphone while nobody speaks", "[audio][edit][automix]") {
    AutoMixOptions o = options();
    o.gate = true;
    const std::vector<std::vector<TimeRange>> speech{{span(1.0, 2.0)}, {}};
    const auto host = autoMixRegions(speech, 0, o, span(0.0, 10.0));
    REQUIRE(host.size() == 2);
    CHECK(offsetAt(host, o, 1.5) == 0.0);   // speaking
    CHECK(offsetAt(host, o, 6.0) == -12.0); // nobody speaks: gated
    const auto guest = autoMixRegions(speech, 1, o, span(0.0, 10.0));
    REQUIRE(guest.size() == 1);
    CHECK(offsetAt(guest, o, 1.5) == -12.0);
    CHECK(offsetAt(guest, o, 8.0) == -12.0);
    // Without the gate the silent guest is lowered only while the host speaks.
    o.gate = false;
    const auto quiet = autoMixRegions(speech, 1, o, span(0.0, 10.0));
    REQUIRE(quiet.size() == 1);
    CHECK(offsetAt(quiet, o, 1.5) == -12.0);
    CHECK(offsetAt(quiet, o, 8.0) == 0.0);
}

TEST_CASE("audio.autoMix writes volume keyframes on each voice track: one undo step with a dry run and a per-voice report",
          "[audio][edit][automix][commands]") {
    Fixture f;
    f.speech->set(f.tl.track("A1"), {span(1.0, 3.0)});
    f.speech->set(f.tl.track("A2"), {span(4.0, 6.0)});
    const Json params{{"tracks", Json::array({"A1", "A2"})}};

    // dryRun: the preview has the keys, the session does not.
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.autoMix"}, {"params", params}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.dryRun);
    CHECK(f.h.engine.revision() == revision);
    REQUIRE(preview.preview);
    CHECK(f.volume("A1@0").isConstant());

    f.speech->asked.clear();
    const std::size_t undo = f.h.session.undoCount();
    const auto r = f.h.ok("audio.autoMix", params);
    CHECK(f.h.session.undoCount() == undo + 1); // both tracks, one step
    CHECK(f.speech->asked.size() == 2);           // one detection per voice
    CHECK(f.speech->asked[0] == std::vector<TrackId>{f.tl.track("A1")});
    CHECK(f.speech->asked[1] == std::vector<TrackId>{f.tl.track("A2")});
    REQUIRE(r.data["voices"].size() == 2);
    CHECK(r.data["voices"][0]["track"] == "A1");
    CHECK(r.data["voices"][0]["speechRuns"] == 1);
    CHECK(r.data["voices"][0]["regions"].size() == 1);
    CHECK(r.data["voices"][1]["clips"].size() == 1);
    CHECK(r.data["regions"].size() == 2);
    CHECK(r.data["amountDb"] == -12.0);

    // The host is down while the guest speaks (4-6 s), up while talking; the guest is the other way round.
    REQUIRE(f.volume("A1@0").isCurve());
    CHECK(f.volumeAt("A1@0", 2.0) == 0.0);
    CHECK(f.volumeAt("A1@0", 5.0) == -12.0);
    CHECK(f.volumeAt("A1@0", 8.0) == 0.0);
    REQUIRE(f.volume("A2@0").isCurve());
    CHECK(f.volumeAt("A2@0", 2.0) == -12.0);
    CHECK(f.volumeAt("A2@0", 5.0) == 0.0);
    CHECK(f.volumeAt("A2@0", 9.0) == 0.0);
    // Music and the video clip are untouched.
    CHECK(f.volume("A3@0").isConstant());

    // Again: the keys are replaced, not doubled - nothing changes, so there is no second undo step.
    const ParamValue before = f.volume("A1@0");
    f.h.ok("audio.autoMix", params);
    CHECK(f.volume("A1@0") == before);
    CHECK(f.h.session.undoCount() == undo + 1);
    // And one undo takes both tracks back.
    f.h.session.undo();
    CHECK(f.volume("A1@0").isConstant());
    CHECK(f.volume("A2@0").isConstant());
}

TEST_CASE("audio.autoMix uses the dialogue-role tracks by default and takes an amount and a gate and says what it cannot do",
          "[audio][edit][automix][commands]") {
    Fixture f;
    f.speech->set(f.tl.track("A1"), {span(1.0, 3.0)});
    f.speech->set(f.tl.track("A2"), {span(4.0, 6.0)});

    // No roles and no tracks: nothing to mix; one voice is not a mix either.
    CHECK(f.h.errorOf("audio.autoMix") == "InvalidParams");
    f.h.ok("track.setRole", {{"track", "A1"}, {"role", "dialogue"}});
    CHECK(f.h.errorOf("audio.autoMix") == "InvalidParams");
    CHECK(f.h.errorOf("audio.autoMix", {{"tracks", Json::array({"A1"})}}) == "InvalidParams");
    // Two dialogue-role tracks are the voices.
    f.h.ok("track.setRole", {{"track", "A2"}, {"role", "dialogue"}});
    const auto r = f.h.ok("audio.autoMix", {{"amount", -20}, {"source", "energy"}});
    CHECK(f.speech->askedSource == SpeechSource::Energy);
    CHECK(r.data["amountDb"] == -20.0);
    CHECK(f.volumeAt("A1@0", 5.0) == -20.0);
}

TEST_CASE("audio.autoMix gate and locked tracks and silence are reported", "[audio][edit][automix][commands]") {
    Fixture f;
    const Json tracks = Json::array({"A1", "A2"});
    SECTION("no speech anywhere: a warning, nothing changes") {
        const std::size_t undo = f.h.session.undoCount();
        const auto r = f.h.ok("audio.autoMix", {{"tracks", tracks}});
        REQUIRE_FALSE(r.warnings.empty());
        CHECK(r.warnings[0].find("no speech") != std::string::npos);
        CHECK(f.volume("A1@0").isConstant());
        CHECK(f.h.session.undoCount() == undo);
    }
    SECTION("voices that never speak apart: nothing lowered, said so") {
        f.speech->set(f.tl.track("A1"), {span(1.0, 3.0)});
        f.speech->set(f.tl.track("A2"), {span(1.0, 3.0)});
        const auto r = f.h.ok("audio.autoMix", {{"tracks", tracks}});
        REQUIRE_FALSE(r.warnings.empty());
        CHECK(r.warnings[0].find("never speak apart") != std::string::npos);
        CHECK(f.volume("A1@0").isConstant());
        CHECK(f.volume("A2@0").isConstant());
    }
    SECTION("a locked voice track is skipped with a warning, the other is still mixed") {
        f.speech->set(f.tl.track("A1"), {span(1.0, 3.0)});
        f.speech->set(f.tl.track("A2"), {span(4.0, 6.0)});
        f.h.ok("track.setState", {{"track", "A1"}, {"locked", true}});
        const auto r = f.h.ok("audio.autoMix", {{"tracks", tracks}});
        bool said = false;
        for (const std::string& w : r.warnings) {
            said = said || w.find("locked") != std::string::npos;
        }
        CHECK(said);
        CHECK(f.volume("A1@0").isConstant());
        CHECK(f.volume("A2@0").isCurve());
    }
    SECTION("the gate lowers a microphone while nobody speaks, over the sequence or the given range") {
        f.speech->set(f.tl.track("A1"), {span(1.0, 3.0)});
        f.speech->set(f.tl.track("A2"), {span(4.0, 6.0)});
        f.h.ok("audio.autoMix", {{"tracks", tracks}, {"gate", true}});
        CHECK(f.volumeAt("A1@0", 9.0) == -12.0); // nobody speaks at 9 s
        CHECK(f.volumeAt("A1@0", 2.0) == 0.0);
        CHECK(f.volumeAt("A2@0", 9.0) == -12.0);
    }
}
