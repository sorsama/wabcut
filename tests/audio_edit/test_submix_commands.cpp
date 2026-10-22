// p02 audio: submix tracks, track output assignment and sends: the model operations (SubmixOps.h), the project file
// (schema 17), validation of untrusted files, and the commands (sequence.addSubmixTrack, track.setOutput,
// track.addSend / setSend / removeSend, audio.routing, and the existing track commands on a submix), one undo step each.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/SubmixOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaa|
    A1 tgt |Aaaaaaaaaa|
    A2     |Bbbbbbbbbb|
)";

CommandHarness harness() {
    Timeline tl(kPicture);
    tl.linkMatching();
    return CommandHarness(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 64, nullptr},
                          ai::edit::editingCommands());
}

const Sequence& seqOf(CommandHarness& h) { return h.sequence(h.state()); }

} // namespace

TEST_CASE("Submix operations: add, assign, send, refuse loops, delete reassigns to the Mix", "[audio][edit][submix]") {
    Timeline tl(kPicture);
    ProjectState state = tl.state;
    const SequenceId seq = tl.seq;
    auto added = edit::addSubmixTrack(state, seq, ChannelFormat::Stereo, "Voices");
    REQUIRE(added);
    REQUIRE(added->createdTracks.size() == 1);
    const TrackId s1 = added->createdTracks[0];
    state = added->state;
    added = edit::addSubmixTrack(state, seq, ChannelFormat::Mono);
    REQUIRE(added);
    const TrackId s2 = added->createdTracks[0];
    state = added->state;
    const Sequence* s = state.findSequence(seq);
    REQUIRE(s->submixTracks.size() == 2);
    CHECK(s->findSubmix(s1)->name == "Voices");
    CHECK(s->findSubmix(s2)->format == ChannelFormat::Mono);
    CHECK(s->findTrack(s1) == nullptr); // submixes are not among the tracks clips go on
    CHECK_FALSE(usesSubmixRouting(*s));

    const TrackId a1 = s->audioTracks[0]->id;
    const TrackId a2 = s->audioTracks[1]->id;
    auto routed = edit::setTrackOutput(state, seq, a1, s1);
    REQUIRE(routed);
    state = routed->state;
    CHECK(usesSubmixRouting(*state.findSequence(seq)));
    // The ChangeSet sees a header change of A1 (the player re-routes from it).
    const ChangeSet changes = diff(tl.state, state);
    REQUIRE(changes.sequence(seq) != nullptr);

    // S2 into S1: fine; then S1 into S2 would loop.
    state = edit::setTrackOutput(state, seq, s2, s1)->state;
    const auto loop = edit::setTrackOutput(state, seq, s1, s2);
    REQUIRE_FALSE(loop);
    CHECK(loop.error().message.find("loop") != std::string::npos);
    CHECK_FALSE(edit::setTrackOutput(state, seq, s1, s1));
    // A video track has no output; an output must be a submix.
    CHECK_FALSE(edit::setTrackOutput(state, seq, state.findSequence(seq)->videoTracks[0]->id, s1));
    CHECK_FALSE(edit::setTrackOutput(state, seq, a1, a2));

    // Sends: at most five, to submixes, from audio tracks.
    TrackSend send;
    send.submix = s2;
    send.level = ParamValue(-6.0);
    for (int i = 0; i < 5; ++i) {
        auto r = edit::addTrackSend(state, seq, a2, send);
        REQUIRE(r);
        state = r->state;
    }
    CHECK_FALSE(edit::addTrackSend(state, seq, a2, send));
    CHECK_FALSE(edit::addTrackSend(state, seq, s1, send)); // not from a submix
    TrackSend bad = send;
    bad.submix = a1;
    CHECK_FALSE(edit::addTrackSend(state, seq, a1, bad));
    bad = send;
    bad.level = ParamValue(40.0);
    CHECK_FALSE(edit::setTrackSend(state, seq, a2, 0, bad));
    TrackSend pre = send;
    pre.preFader = true;
    state = edit::setTrackSend(state, seq, a2, 1, pre)->state;
    CHECK(state.findSequence(seq)->findTrack(a2)->sends[1].preFader);
    state = edit::removeTrackSend(state, seq, a2, 0)->state;
    CHECK(state.findSequence(seq)->findTrack(a2)->sends.size() == 4);
    REQUIRE(validate(state, Validation::Structure));

    // Deleting S2: S2's sends go, nothing loops, A1 still goes to S1; deleting S1: A1 goes to the Mix.
    state = edit::removeSubmixTrack(state, seq, s2)->state;
    CHECK(state.findSequence(seq)->findTrack(a2)->sends.empty());
    CHECK(state.findSequence(seq)->findTrack(a1)->output == s1);
    state = edit::removeTrack(state, seq, s1)->state; // the generic delete reaches submixes too
    CHECK_FALSE(state.findSequence(seq)->findTrack(a1)->output.isValid());
    CHECK(state.findSequence(seq)->submixTracks.empty());
    REQUIRE(validate(state, Validation::Structure));
}

TEST_CASE("Submix routing survives a save and load and bad routing is refused on load", "[audio][edit][submix][io]") {
    Timeline tl(kPicture);
    ProjectState state = tl.state;
    const SequenceId seq = tl.seq;
    state = edit::addSubmixTrack(state, seq, ChannelFormat::Mono, "Verb")->state;
    const TrackId s1 = state.findSequence(seq)->submixTracks[0]->id;
    const TrackId a1 = state.findSequence(seq)->audioTracks[0]->id;
    state = edit::setTrackOutput(state, seq, a1, s1)->state;
    TrackSend send;
    send.submix = s1;
    send.preFader = true;
    send.muted = true;
    send.level = ParamValue(Curve{{Keyframe{Ticks{}, -12.0}, Keyframe{Ticks::fromSeconds(1), 0.0}}});
    send.pan = ParamValue(-0.5);
    state = edit::addTrackSend(state, seq, state.findSequence(seq)->audioTracks[1]->id, send)->state;
    const auto text = io::toJson(state);
    REQUIRE(text);
    CHECK(text->find("\"submixTracks\"") != std::string::npos);
    const auto back = io::fromJson(*text);
    REQUIRE(back);
    CHECK(*back->findSequence(seq) == *state.findSequence(seq));

    SECTION("an output that is not a submix") {
        std::string damaged = *text;
        const std::string key = "\"output\": " + std::to_string(s1.value);
        const auto at = damaged.find(key);
        REQUIRE(at != std::string::npos);
        damaged.replace(at, key.size(), "\"output\": " + std::to_string(a1.value));
        CHECK_FALSE(io::fromJson(damaged));
    }
    SECTION("a submix with clips") {
        nlohmann::json j = nlohmann::json::parse(*text);
        auto& s = j["sequences"][0];
        s["submixTracks"][0]["clips"] = s["audioTracks"][0]["clips"];
        CHECK_FALSE(io::fromJson(j.dump()));
    }
    SECTION("two submixes in a loop") {
        nlohmann::json j = nlohmann::json::parse(*text);
        auto& s = j["sequences"][0];
        nlohmann::json second = s["submixTracks"][0];
        second["id"] = j["nextId"];
        j["nextId"] = j["nextId"].get<std::uint64_t>() + 1;
        second["output"] = s["submixTracks"][0]["id"];
        s["submixTracks"][0]["output"] = second["id"];
        s["submixTracks"].push_back(second);
        CHECK_FALSE(io::fromJson(j.dump()));
    }
    SECTION("six sends") {
        nlohmann::json j = nlohmann::json::parse(*text);
        auto& sends = j["sequences"][0]["audioTracks"][1]["sends"];
        while (sends.size() < 6) {
            sends.push_back(sends[0]);
        }
        CHECK_FALSE(io::fromJson(j.dump()));
    }
}

TEST_CASE("Submix commands: add, assign, send, report, and the track commands on a submix, one undo step each",
          "[audio][edit][submix][commands]") {
    CommandHarness h = harness();
    std::size_t undo = h.session.undoCount();
    auto made = h.ok("sequence.addSubmixTrack", {{"format", "stereo"}, {"name", "Voices"}});
    CHECK(made.data["track"] == "S1");
    CHECK(h.session.undoCount() == ++undo);
    h.ok("sequence.addSubmixTrack", {{"format", "mono"}});
    ++undo;
    REQUIRE(seqOf(h).submixTracks.size() == 2);

    auto out = h.ok("track.setOutput", {{"track", "A1"}, {"output", "S1"}});
    CHECK(out.data["output"] == "S1");
    CHECK(seqOf(h).audioTracks[0]->output == seqOf(h).submixTracks[0]->id);
    CHECK(h.session.undoCount() == ++undo);
    CHECK(h.errorOf("track.setOutput", {{"track", "S1"}, {"output", "S1"}}) == "EditRejected");
    CHECK(h.errorOf("track.setOutput", {{"track", "A1"}, {"output", "A2"}}) == "InvalidParams");
    h.ok("track.setOutput", {{"track", "S2"}, {"output", "S1"}});
    ++undo;
    CHECK(h.errorOf("track.setOutput", {{"track", "S1"}, {"output", "S2"}}) == "EditRejected");

    auto sent = h.ok("track.addSend", {{"track", "A2"}, {"submix", "S2"}, {"level", -6}, {"preFader", true}});
    CHECK(sent.data["send"]["send"] == 1);
    CHECK(sent.data["send"]["submix"] == "S2");
    CHECK(sent.data["send"]["preFader"] == true);
    ++undo;
    h.ok("track.setSend", {{"track", "A2"}, {"send", 1}, {"level", -3}, {"time", 0}});
    ++undo;
    h.ok("track.setSend", {{"track", "A2"}, {"send", 1}, {"level", -9}, {"time", 48}});
    ++undo;
    const TrackSend& s = seqOf(h).audioTracks[1]->sends[0];
    REQUIRE(s.level.isCurve());
    CHECK(s.level.curve()->keys.size() == 2);
    CHECK(h.errorOf("track.setSend", {{"track", "A2"}, {"send", 2}, {"level", -3}}) == "NotFound");
    CHECK(h.errorOf("track.addSend", {{"track", "S1"}, {"submix", "S2"}}) == "EditRejected");

    // The submix's own effects, fader, panner, mute: the existing commands with "S1".
    h.ok("audio.addEffect", {{"target", "S1"}, {"effect", "wabcut.audio.reverb"}});
    h.ok("track.setVolume", {{"track", "S1"}, {"db", -4.5}});
    h.ok("track.setPan", {{"track", "S1"}, {"pan", 0.25}});
    h.ok("track.setState", {{"track", "S1"}, {"muted", true}});
    undo += 4;
    const Track& bus = *seqOf(h).submixTracks[0];
    CHECK(bus.effects.size() == 1);
    CHECK(bus.volume == ParamValue(-4.5));
    CHECK(bus.pan == ParamValue(0.25));
    CHECK(bus.muted);
    CHECK(h.errorOf("track.setState", {{"track", "S1"}, {"targeted", true}}) == "EditRejected"); // no clips to target
    CHECK(h.session.undoCount() == undo);

    auto report = h.ok("audio.routing", Json::object());
    CHECK(report.data["tracks"][0]["output"] == "S1");
    CHECK(report.data["tracks"][1]["sends"][0]["submix"] == "S2");
    CHECK(report.data["submixes"][0]["name"] == "Voices");
    CHECK(report.data["submixes"][1]["format"] == "mono");
    CHECK(report.data["submixes"][1]["output"] == "S1");
    CHECK(h.session.undoCount() == undo); // a read

    // Dry run previews without committing; sequence.removeTrack deletes a submix and reassigns.
    const auto preview = h.run(Json{{"command", "sequence.removeTrack"}, {"params", Json{{"track", "S1"}}}, {"dryRun", true}});
    CHECK(preview.dryRun);
    CHECK(seqOf(h).submixTracks.size() == 2);
    h.ok("sequence.removeTrack", {{"track", "S1"}});
    CHECK(seqOf(h).submixTracks.size() == 1);
    CHECK_FALSE(seqOf(h).audioTracks[0]->output.isValid());
    CHECK_FALSE(seqOf(h).submixTracks[0]->output.isValid());
    h.ok("track.removeSend", {{"track", "A2"}, {"send", 1}});
    CHECK(seqOf(h).audioTracks[1]->sends.empty());
    // Undo brings the deleted submix and its routing back exactly.
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    CHECK(seqOf(h).submixTracks.size() == 2);
    CHECK(seqOf(h).audioTracks[0]->output == seqOf(h).submixTracks[0]->id);
}
