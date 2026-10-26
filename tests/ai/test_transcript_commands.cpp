// The transcript commands through the CommandEngine (AI-3): what agents, MCP and wabcut-cli use.

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/edit/MediaTranscript.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Schema.h>
#include <wabcut/command/ToolExport.h>

#include <wabcut/model/Nesting.h>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using wabcut::ai::Transcript;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

std::shared_ptr<const Transcript> interview() {
    return transcript("en", {{"So", 0, 3, 0},       // 0
                             {"um,", 4, 7, 0},      // 1  filler
                             {"welcome", 8, 13, 0}, // 2
                             {"to", 13, 15, 0},     // 3
                             {"the", 15, 17, 0},    // 4
                             {"show.", 17, 22, 0},  // 5  then a 1.6 s pause
                             {"Thanks", 62, 67, 1}, // 6
                             {"for", 67, 69, 1},    // 7
                             {"having", 69, 74, 1}, // 8
                             {"me.", 74, 80, 1}});  // 9
}

struct Fixture {
    Fixture() : Fixture(Timeline(kPicture)) {}
    explicit Fixture(Timeline timeline)
        : transcripts(provider(timeline, {{'A', interview()}})),
          h(withLinks(std::move(timeline)),
            command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscriptServices>(transcripts)},
            editingCommands()) {}

    static Timeline withLinks(Timeline tl) {
        tl.linkMatching();
        return tl;
    }

    std::shared_ptr<MapTranscriptProvider> transcripts;
    CommandHarness h;
};

} // namespace

TEST_CASE("The editing catalog is the built-in one plus the transcript and analysis commands", "[ai][edit][commands]") {
    const command::CommandRegistry& registry = editingCommands();
    for (const char* name : {"transcript.transcribe", "transcript.text", "transcript.words", "transcript.review",
                             "transcript.deleteRanges", "transcript.keepRanges", "transcript.moveRange",
                             "transcript.cleanup", "captions.generate", "captions.generateFromTranscript",
                             "captions.updateFromTranscript", "transcript.correctWord", "transcript.correctRange",
                             "transcript.speakers", "transcript.corrections", "transcript.mediaWords"}) {
        INFO(name);
        CHECK(registry.knows(name));
        CHECK_FALSE(command::builtinCommands().knows(name));
        CHECK(command::unsupportedSchemaKeywords(registry.find(name)->params).empty());
    }
    // The 21 transcript commands (AI-3, AI-7 transcribe, C.7 captions from the transcript, corrections, speakers, p02's
    // captions.hideProfanity and audio.bleepProfanity, 0.2 transcript.export / transcript.findReplace / transcript.find,
    // the P0 round's transcript.mediaWords),
    // + analysis.*, clip.splitAtShots (C.5), the 44 audio commands (AI-4, A.3 faders, A.4 enhance / stems, A.5 sync,
    // A.6's plugins.list / plugins.scan, A.7's audio.essentialSound / audio.remix and A.8's audio.remixRender /
    // track.setRecordArm / record.takes / record.useTake, A.9's ten channel commands, p02's six submix commands and audio.autoTag),
    // search.footage / search.indexMedia (AI-8)
    // the 16 colour commands (I.5: color.*, lut.*, cdl.*, scopes.read; I.9: color.management.*, color.inputSpace.*; 0.2 lut.export;
    // 0.4 color.grade.copy),
    // analysis.subjects / reframe.createSequence /
    // reframe.rerun / shorts.find / shorts.create (AI-9), 11 mask/tracking/redaction commands (I.6),
    // 8 local-runtime/generation/provenance commands (AI-10), roughcut.fromScript / roughcut.swapTake / takes.find /
    // takes.use / autozoom.apply (AI-11), captions.translate / dub.create / narration.create (AI-12),
    // analysis.speakers / multicam.autoSwitch / workflow.podcast (AI-13), mask.objectSelect / mask.refine /
    // mask.propagate (AI-14), highlights.find / importChat / toReel / toShort (G.2), and effect.analyzeStabilization /
    // effect.analyzeRollingShutter (P.2), and gen.sfx / redaction.detectFaces (0.2).
    CHECK(registry.all().size() == command::builtinCommands().all().size() + 21 + 3 + 44 + 2 + 16 + 5 + 11 + 8 + 5 + 3 + 3 + 3 + 4 + 2 + 2);
    for (const char* name : {"effect.analyzeStabilization", "effect.analyzeRollingShutter"}) {
        INFO(name);
        CHECK(registry.knows(name));
        CHECK_FALSE(command::builtinCommands().knows(name));
        CHECK(command::unsupportedSchemaKeywords(registry.find(name)->params).empty());
    }
    for (const char* name : {"roughcut.fromScript", "roughcut.swapTake", "takes.find", "takes.use", "autozoom.apply", "captions.translate",
                             "dub.create", "narration.create", "analysis.speakers", "multicam.autoSwitch", "workflow.podcast",
                             "mask.objectSelect", "mask.refine", "mask.propagate"}) {
        INFO(name);
        CHECK(registry.knows(name));
        CHECK(command::unsupportedSchemaKeywords(registry.find(name)->params).empty());
    }
    for (const char* name : {"mask.add", "mask.update", "mask.remove", "mask.list", "track.mask", "track.transform",
                             "redaction.add", "redaction.update", "redaction.remove", "redaction.override", "redaction.list",
                             "llm.local.status", "llm.local.start", "llm.local.stop", "llm.local.models",
                             "gen.extend", "gen.broll", "gen.image", "gen.sfx", "provenance.report", "redaction.detectFaces"}) {
        INFO(name);
        CHECK(registry.knows(name));
        CHECK(command::unsupportedSchemaKeywords(registry.find(name)->params).empty());
    }
    for (const char* name : {"color.grade.set", "color.grade.get", "color.auto", "color.match", "color.matchScene",
                             "lut.import", "lut.list", "lut.export", "cdl.export", "cdl.import", "scopes.read", "color.management.get",
                             "color.management.set", "color.inputSpace.set", "color.inputSpace.get"}) {
        INFO(name);
        CHECK(registry.knows(name));
    }
    CHECK(command::builtinCommands().knows("captions.get"));
    CHECK(command::builtinCommands().knows("captions.remove"));
    // Every example is valid against its own schema (they become the LLM tool examples).
    for (const command::CommandSpec& spec : registry.all()) {
        INFO(spec.name);
        CHECK(command::validateAgainstSchema(spec.params, spec.example).empty());
    }
    const Json tools = command::exportTools(registry, command::ToolFormat::Anthropic);
    bool exported = false;
    for (const Json& tool : tools) {
        exported = exported || tool["name"] == "transcript_deleteRanges";
    }
    CHECK(exported);
}

TEST_CASE("Without transcripts the commands say so", "[ai][edit][commands]") {
    CommandHarness h(Timeline(kPicture), {}, editingCommands());
    CHECK(h.errorOf("transcript.text") == "InvalidRequest");
    CHECK(h.errorOf("transcript.deleteRanges", Json{{"words", Json::array({Json::array({0, 0})})}}) == "InvalidRequest");

    // Services without a transcript for this media: an empty answer that says why.
    CommandHarness empty(Timeline(kPicture),
                         command::EnginePolicy{command::CapabilitySet::all(), 1024,
                                               std::make_shared<TranscriptServices>(std::make_shared<MapTranscriptProvider>())},
                         editingCommands());
    const auto text = empty.ok("transcript.text");
    CHECK(text.data["words"] == 0);
    CHECK(text.warnings.size() == 1);
    CHECK(empty.errorOf("transcript.deleteRanges", Json{{"words", Json::array({Json::array({0, 0})})}}) == "EditRejected");
}

TEST_CASE("transcript.text and transcript.words read the sequence's words", "[ai][edit][commands]") {
    Fixture fx;
    const auto text = fx.h.ok("transcript.text");
    CHECK(text.data["words"] == 10);
    CHECK(text.data["language"] == "en");
    REQUIRE(text.data["paragraphs"].size() == 2); // the speaker changes after the pause
    CHECK(text.data["paragraphs"][0] == Json::array({0, 5, tc(0), tc(22), 0, "So um, welcome to the show."}));
    CHECK(text.data["paragraphs"][1] == Json::array({6, 9, tc(62), tc(80), 1, "Thanks for having me."}));

    const auto words = fx.h.ok("transcript.words", Json{{"first", 1}, {"limit", 2}});
    CHECK(words.data["total"] == 10);
    REQUIRE(words.data["words"].size() == 2);
    CHECK(words.data["truncated"] == true);
    CHECK(words.data["words"][0] == Json::array({1, "um,", tc(4), tc(7), fx.h.clip("A1@0").value, "A1", 0, 0.9}));
    const auto window = fx.h.ok("transcript.words", Json{{"from", tc(60)}, {"to", tc(70)}});
    CHECK(window.data["words"].size() == 3);
}

TEST_CASE("transcript.deleteRanges previews, cuts in one undo step, and undoes exactly", "[ai][edit][commands][undo]") {
    Fixture fx;
    const ProjectSnapshot original = fx.h.state();
    const Json params{{"words", Json::array({Json::array({1, 1}), Json::array({3, 4})})}, {"gaps", Json::array({5})}};

    const auto preview = fx.h.run(Json{{"command", "transcript.deleteRanges"}, {"params", params}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.dryRun);
    CHECK_FALSE(preview.committed);
    REQUIRE(preview.preview);
    CHECK(*fx.h.state() == *original);
    CHECK(preview.data["cuts"] == Json::array({Json::array({tc(3), tc(8)}), Json::array({tc(13), tc(17)}),
                                               Json::array({tc(26), tc(58)})}));
    CHECK(preview.data["words"] == 3);
    CHECK_FALSE(preview.changes.empty());

    const auto revision = fx.h.session.revision();
    const auto done = fx.h.ok("transcript.deleteRanges", params);
    CHECK(done.committed);
    CHECK(fx.h.session.revision() == revision + 1);
    CHECK(fx.h.session.undoCount() == 1);
    CHECK(fx.h.session.undoLabel() == "Delete Words");
    const auto after = SequenceTranscript::build(*fx.h.state(), fx.h.tl.seq, *fx.transcripts);
    CHECK(after.textOf(0, after.size() - 1) == "So welcome show. Thanks for having me.");
    CHECK(links(*fx.h.state(), fx.h.tl.seq).find("V1@0+A1@0") == 0);

    REQUIRE(fx.h.session.undo().has_value());
    CHECK(*fx.h.state() == *original);
    CHECK(render(*fx.h.state(), fx.h.tl.seq) == render(*original, fx.h.tl.seq));
}

TEST_CASE("transcript.deleteRanges moves later sequence markers", "[ai][edit][commands][markers]") {
    // 0.4, Ripple Sequence Markers (on by default): text cuts are extracts, so chapters stay on their words.
    Fixture fx;
    for (const auto& [frame, name] : {std::pair{1, "Intro"}, std::pair{10, "Mid"}, std::pair{40, "Pause"},
                                      std::pair{62, "Thanks"}}) {
        fx.h.ok("marker.add", Json{{"time", tc(frame)}, {"name", name}});
    }
    const ProjectSnapshot marked = fx.h.state();
    const std::size_t undo = fx.h.session.undoCount();
    // Cuts [3, 8), [13, 17) and [26, 58) (the test above): 41 frames in all.
    const Json params{{"words", Json::array({Json::array({1, 1}), Json::array({3, 4})})}, {"gaps", Json::array({5})}};
    fx.h.ok("transcript.deleteRanges", params);
    CHECK(fx.h.session.undoCount() == undo + 1); // the markers move in the same undo step
    const auto& markers = fx.h.state()->findSequence(fx.h.tl.seq)->markers;
    REQUIRE(markers.size() == 4);
    CHECK(markers[0].start == f(1));  // before every cut
    CHECK(markers[1].start == f(5));  // after the first cut
    CHECK(markers[2].start == f(17)); // inside the pause that went: at the cut
    CHECK(markers[3].start == f(21)); // on "Thanks", which moved 41 frames earlier
    REQUIRE(fx.h.session.undo().has_value());
    CHECK(*fx.h.state() == *marked);
}

TEST_CASE("transcript.keepRanges and transcript.moveRange", "[ai][edit][commands]") {
    Fixture fx;
    const ProjectSnapshot original = fx.h.state();
    SECTION("Rough cut") {
        fx.h.ok("transcript.keepRanges", Json{{"words", Json::array({Json::array({6, 9}), Json::array({2, 5})})}});
        const auto st = SequenceTranscript::build(*fx.h.state(), fx.h.tl.seq, *fx.transcripts);
        CHECK(st.textOf(0, st.size() - 1) == "welcome to the show. Thanks for having me.");
        CHECK(fx.h.session.undoCount() == 1);
    }
    SECTION("Move the answer before the question") {
        const auto moved = fx.h.ok("transcript.moveRange", Json{{"words", Json::array({6, 9})}, {"before", 0}});
        CHECK(moved.data["destination"] == tc(0));
        const auto st = SequenceTranscript::build(*fx.h.state(), fx.h.tl.seq, *fx.transcripts);
        CHECK(st.textOf(0, st.size() - 1) == "Thanks for having me. So um, welcome to the show.");
        CHECK(fx.h.session.undoCount() == 1);
        requireValid(*fx.h.state());
        INFO(render(*fx.h.state(), fx.h.tl.seq));
        CHECK(fx.h.state()->findSequence(fx.h.tl.seq)->contentEnd() == original->findSequence(fx.h.tl.seq)->contentEnd());
    }
    SECTION("Errors name the problem") {
        CHECK(fx.h.errorOf("transcript.moveRange", Json{{"words", Json::array({2, 4})}, {"before", 3}}) == "EditRejected");
        CHECK(fx.h.errorOf("transcript.keepRanges", Json{{"words", Json::array({Json::array({8, 12})})}}) ==
              "EditRejected");
    }
    fx.h.session.undo();
    CHECK(*fx.h.state() == *original);
}

TEST_CASE("transcript.review and transcript.cleanup", "[ai][edit][commands]") {
    Fixture fx;
    const auto items = fx.h.ok("transcript.review");
    REQUIRE(items.data["items"].size() == 2);
    CHECK(items.data["items"][0] == Json::array({"filler", 1, 1, tc(4), tc(7), 0.9, "um,", "So [um,] welcome to the show...."}));
    CHECK(items.data["items"][1][0] == "silence");
    CHECK(items.data["items"][1][6] == "1.6 s pause");

    const auto cleaned = fx.h.ok("transcript.cleanup", Json{{"fillers", true}, {"silences", 0.8}});
    CHECK(cleaned.data["fillers"] == 1);
    CHECK(cleaned.data["silences"] == 1);
    const auto st = SequenceTranscript::build(*fx.h.state(), fx.h.tl.seq, *fx.transcripts);
    CHECK(st.textOf(0, st.size() - 1) == "So welcome to the show. Thanks for having me.");
    CHECK(st[5].timeline.start - st[4].timeline.end() < Ticks::fromSeconds(1)); // the pause is gone, room tone stays
    CHECK(fx.h.session.undoCount() == 1);

    const auto nothing = fx.h.ok("transcript.cleanup", Json{{"fillers", true}, {"silences", 0.8}});
    CHECK_FALSE(nothing.committed);
    CHECK(nothing.warnings == std::vector<std::string>{"nothing to remove"});
}

TEST_CASE("captions.generate stores a caption track; captions.get and captions.remove", "[ai][edit][commands][captions]") {
    Fixture fx;
    const auto made = fx.h.ok("captions.generate", Json{{"maxCharsPerLine", 16}});
    REQUIRE(made.createdIds.size() == 1);
    const std::uint64_t id = made.createdIds[0];
    CHECK(made.data["captionTrack"] == id);
    CHECK(made.data["language"] == "en");
    const Sequence& seq = *fx.h.state()->findSequence(fx.h.tl.seq);
    REQUIRE(seq.captionTracks.size() == 1);
    CHECK(seq.captionTracks[0].id.value == id);
    CHECK(made.data["cues"] == seq.captionTracks[0].cues.size());
    CHECK(made.changes.sequence(fx.h.tl.seq)->captionsChanged);

    const auto cues = fx.h.ok("captions.get");
    CHECK(cues.data["captionTrack"]["id"] == id);
    CHECK(cues.data["cues"][0] == Json::array({tc(0), tc(22), "So um, welcome\nto the show."}));
    const auto summary = fx.h.ok("sequence.summary");
    CHECK(summary.data["captionTracks"] == Json::array({Json::array({id, "Captions", "en", true, seq.captionTracks[0].cues.size()})}));

    // Regenerating with "replace" keeps the id.
    const auto again = fx.h.ok("captions.generate", Json{{"replace", id}, {"maxCharsPerLine", 42}});
    CHECK(again.createdIds.empty());
    CHECK(fx.h.state()->findSequence(fx.h.tl.seq)->captionTracks.size() == 1);
    CHECK(fx.h.ok("captions.get").data["cues"][0][2] == "So um, welcome to the show.");

    fx.h.ok("captions.remove", Json{{"captionTrack", id}});
    CHECK(fx.h.state()->findSequence(fx.h.tl.seq)->captionTracks.empty());
    CHECK(fx.h.errorOf("captions.get") == "NotFound");
    CHECK(fx.h.errorOf("captions.remove", Json{{"captionTrack", id}}) == "NotFound");
    fx.h.session.undo();
    fx.h.session.undo();
    fx.h.session.undo();
    CHECK(fx.h.state()->findSequence(fx.h.tl.seq)->captionTracks.empty());
}

TEST_CASE("Transcript commands in a batch are one undo step", "[ai][edit][commands][undo]") {
    Fixture fx;
    const ProjectSnapshot original = fx.h.state();
    const auto result = fx.h.run(Json{{"label", "Tighten"},
                                      {"commands", Json::array({
                                                       Json{{"command", "transcript.deleteRanges"},
                                                            {"params", Json{{"words", Json::array({Json::array({1, 1})})}}}},
                                                       Json{{"command", "captions.generate"}},
                                                   })}});
    INFO(command::toJson(result).dump());
    REQUIRE(result.ok());
    CHECK(fx.h.session.undoCount() == 1);
    CHECK(fx.h.ok("captions.get").data["cues"][0][2] == "So welcome to the show.");
    fx.h.session.undo();
    CHECK(*fx.h.state() == *original);
}

TEST_CASE("Out-of-range word indices are refused without touching the project (Codex P1-3)", "[ai][edit][commands]") {
    Fixture fx;
    const ProjectSnapshot before = fx.h.state();
    const auto revision = fx.h.session.revision();
    for (const char* request : {
             R"({"command":"transcript.deleteRanges","params":{"gaps":[18446744073709551615]}})",
             R"({"command":"transcript.deleteRanges","params":{"words":[[0,18446744073709551615]]}})",
             R"({"command":"transcript.keepRanges","params":{"words":[[18446744073709551615,18446744073709551615]]}})",
             R"({"command":"transcript.moveRange","params":{"words":[0,1],"before":18446744073709551615}})",
             R"({"command":"transcript.words","params":{"last":18446744073709551615}})",
             R"({"command":"transcript.words","params":{"first":9223372036854775807}})"}) {
        INFO(request);
        const auto r = fx.h.run(Json::parse(request));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error->code == command::ErrorCode::InvalidParams);
    }
    // In range for the schema but past the transcript: refused by the planner, never read.
    CHECK(fx.h.errorOf("transcript.deleteRanges", Json{{"gaps", Json::array({9})}}) == "EditRejected"); // the last word
    CHECK(fx.h.errorOf("transcript.deleteRanges", Json{{"gaps", Json::array({999999999})}}) == "EditRejected");
    CHECK(fx.h.errorOf("transcript.moveRange", Json{{"words", Json::array({0, 1})}, {"before", 999999999}}) ==
          "EditRejected");
    CHECK(fx.h.ok("transcript.words", Json{{"first", 2}, {"last", 999999999}}).data["words"].size() == 8);
    CHECK(*fx.h.state() == *before);
    CHECK(fx.h.session.revision() == revision);
    CHECK(fx.h.session.undoCount() == 0);
}

// ---- C.17: deleting words through a multicam clip ----------------------------------------------------------------------

TEST_CASE("Deleting words inside a multicam clip cuts it in place in one undo step (C.17b)", "[ai][edit][commands][multicam][undo]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2     |..........................................................................................|
    )");
    edit::MulticamSpec spec;
    spec.angles = {tl.media('A')};
    spec.sync = MulticamSync::Manual;
    const auto mc = edit::createMulticam(tl.state, spec);
    REQUIRE(mc.has_value());
    edit::SourceClip src;
    src.sequence = mc->id;
    src.sourceRange = TimeRange::fromStartEnd(f(0), f(90));
    src.patches = edit::defaultSourcePatches(*mc->state.findSequence(tl.seq), *mc->state.findSequence(mc->id));
    src.videoAngle = AngleSet{1};
    src.audioAngles = AngleSet{1};
    tl.state = ok(edit::overwriteEdit(mc->state, tl.seq, src, f(0))).state;
    const auto transcripts = provider(tl, {{'A', interview()}});
    CommandHarness h(std::move(tl),
                     command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscriptServices>(transcripts)},
                     editingCommands());
    const ProjectSnapshot original = h.state();
    const SequenceId multicam = mc->id;
    // Every clip left on the timeline is still a piece of the multicam clip; how many there are.
    const auto multicamPieces = [&](const ProjectState& s) {
        int pieces = 0;
        bool allMulticam = true;
        s.findSequence(h.tl.seq)->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                allMulticam = allMulticam && c.isNested() && c.sequence == multicam;
                ++pieces;
            }
        });
        return allMulticam ? pieces : -1;
    };
    REQUIRE(multicamPieces(*original) == 2);
    // Read through: the multicam clip's words are the sequence's.
    const auto words = SequenceTranscript::build(*original, h.tl.seq, *transcripts);
    REQUIRE(words.size() == 10);
    CHECK(words.textOf(0, 9) == "So um, welcome to the show. Thanks for having me.");

    // The preview changes nothing.
    const Json params{{"sequence", h.tl.seq.value}, {"words", Json::array({Json::array({1, 1})})}};
    const auto preview = h.run(Json{{"command", "transcript.deleteRanges"}, {"params", params}, {"dryRun", true}});
    INFO(command::toJson(preview).dump());
    REQUIRE(preview.ok());
    CHECK(*h.state() == *original);

    // Done: the multicam clip is razored and rippled in place (no flatten), in one undo step.
    h.ok("transcript.deleteRanges", params);
    CHECK(h.session.undoCount() == 1);
    CHECK(multicamPieces(*h.state()) == 4); // video and audio, each in two pieces around the cut
    const auto after = SequenceTranscript::build(*h.state(), h.tl.seq, *transcripts);
    CHECK(after.textOf(0, after.size() - 1) == "So welcome to the show. Thanks for having me.");
    // Angle switching still works on the pieces.
    const ProjectSnapshot cut = h.state();
    const Clip& last = cut->findSequence(h.tl.seq)->findTrack(h.tl.track("V1"))->clips.back();
    CHECK(edit::switchMulticamAngle(*cut, h.tl.seq, last.id, std::nullopt, 0).has_value());
    // Undo brings the uncut multicam clip back exactly.
    REQUIRE(h.session.undo().has_value());
    CHECK(*h.state() == *original);
    CHECK(multicamPieces(*h.state()) == 2);
}

TEST_CASE("Enhance Speech and stems refuse a multicam clip with where to go instead (0.1.2)", "[ai][audio][commands][multicam]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    edit::MulticamSpec spec;
    spec.angles = {tl.media('A')};
    spec.sync = MulticamSync::Manual;
    const auto mc = edit::createMulticam(tl.state, spec);
    REQUIRE(mc.has_value());
    edit::SourceClip src;
    src.sequence = mc->id;
    src.sourceRange = TimeRange::fromStartEnd(f(0), f(20));
    src.patches = edit::defaultSourcePatches(*mc->state.findSequence(tl.seq), *mc->state.findSequence(mc->id));
    src.videoAngle = AngleSet{1};
    src.audioAngles = AngleSet{1};
    tl.state = ok(edit::overwriteEdit(mc->state, tl.seq, src, f(0))).state;
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024, nullptr}, editingCommands());
    const Clip& audio = h.state()->findSequence(h.tl.seq)->findTrack(h.tl.track("A1"))->clips.front();
    REQUIRE(audio.isNested());
    const auto enhanced = h.run(Json{{"command", "audio.enhanceSpeech"},
                                     {"params", {{"sequence", h.tl.seq.value}, {"clips", {audio.id.value}}, {"render", false}}}});
    REQUIRE_FALSE(enhanced.ok());
    CHECK(command::toJson(enhanced).dump().find("multicam or nested sequence") != std::string::npos);
    CHECK(h.session.undoCount() == 0);
}

// P0 round: the source clip transcript for the CLI and MCP (transcript.mediaWords, MediaTranscript.h).
TEST_CASE("transcript.mediaWords reads a media item's words in source time whatever the sequence keeps",
          "[ai][edit][commands][source]") {
    Fixture fx;
    // The sequence loses its first two words; the media keeps all ten.
    fx.h.ok("transcript.deleteRanges", Json{{"words", Json::array({Json::array({0, 1})})}});
    CHECK(fx.h.ok("transcript.words").data["total"] == 8);

    const auto all = fx.h.ok("transcript.mediaWords", Json{{"media", "A"}});
    CHECK(all.warnings.empty());
    CHECK(all.data["total"] == 10);
    CHECK(all.data["audioStream"] == 0);
    CHECK(all.data["language"] == "en");
    CHECK(all.data["name"] == "A");
    CHECK(all.data["wordFields"] == Json::array({"index", "text", "start", "end", "startTicks", "endTicks", "speaker", "confidence"}));
    REQUIRE(all.data["words"].size() == 10);
    CHECK(all.data["words"][1] == Json::array({1, "um,", tc(4), tc(7), frames(4).value, frames(7).value, 0, 0.9}));
    CHECK(all.data["words"][6] == Json::array({6, "Thanks", tc(62), tc(67), frames(62).value, frames(67).value, 1, 0.9}));

    // A source-time window (timecodes in the media's rate, or ticks), an index window and a limit.
    const auto window = fx.h.ok("transcript.mediaWords", Json{{"media", "A"}, {"from", tc(60)}, {"to", tc(70)}});
    CHECK(window.data["words"].size() == 3);
    const auto ticks = fx.h.ok("transcript.mediaWords", Json{{"media", "A"}, {"from", frames(60).value}, {"to", frames(70).value}});
    CHECK(ticks.data["words"] == window.data["words"]);
    const auto some = fx.h.ok("transcript.mediaWords", Json{{"media", "A"}, {"first", 2}, {"limit", 3}});
    REQUIRE(some.data["words"].size() == 3);
    CHECK(some.data["words"][0][1] == "welcome");
    CHECK(some.data["truncated"] == true);
    // A read: no undo step.
    CHECK(fx.h.session.undoCount() == 1);
}

TEST_CASE("transcript.mediaWords applies the project's corrections and redactions", "[ai][edit][commands][source][redaction]") {
    Fixture fx;
    fx.h.ok("transcript.correctWord", Json{{"word", 2}, {"text", "Welcome"}});
    fx.h.ok("redaction.add", Json{{"media", "A"},
                                  {"source", Json{{"start", tc(0)}, {"end", tc(90)}}},
                                  {"audio", "bleep"},
                                  {"words", Json::array({Json{{"start", tc(69)}, {"end", tc(74)}}})}});
    const auto words = fx.h.ok("transcript.mediaWords", Json{{"media", "A"}}).data["words"];
    REQUIRE(words.size() == 10);
    CHECK(words[2][1] == "Welcome");
    CHECK(words[8][1] == "[bleep]"); // "having"
    CHECK(words[9][1] == "me.");
}

TEST_CASE("transcript.mediaWords says when there is nothing to read and refuses a wrong stream",
          "[ai][edit][commands][source]") {
    Fixture fx;
    CHECK(fx.h.errorOf("transcript.mediaWords", Json{{"media", "A"}, {"audioStream", 3}}) == "InvalidParams");
    CHECK(fx.h.errorOf("transcript.mediaWords", Json{{"media", 999999}}) != ""); // no such item
    CHECK(fx.h.errorOf("transcript.mediaWords") == "InvalidParams"); // media is required

    // Services without a transcript of the media: an empty answer that says why.
    CommandHarness empty(Timeline(kPicture),
                         command::EnginePolicy{command::CapabilitySet::all(), 1024,
                                               std::make_shared<TranscriptServices>(std::make_shared<MapTranscriptProvider>())},
                         editingCommands());
    const auto none = empty.ok("transcript.mediaWords", Json{{"media", "A"}});
    CHECK(none.data["total"] == 0);
    CHECK(none.warnings.size() == 1);
    // Without transcript services at all.
    CommandHarness bare(Timeline(kPicture), {}, editingCommands());
    CHECK(bare.errorOf("transcript.mediaWords", Json{{"media", "A"}}) == "InvalidRequest");
}

TEST_CASE("MediaTranscript: words in source time; Thai joins without spaces; the nearest word; sameAs",
          "[ai][edit][source]") {
    Timeline tl(kPicture);
    const auto thai = transcript("th", {{"\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5", 2, 6, 0},
                                        {"\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A", 6, 9, 0},
                                        {"OK", 20, 24, 1}});
    const auto p = provider(tl, {{'A', thai}});
    const MediaTranscript mt = MediaTranscript::build(tl.state, tl.media('A'), 0, *p);
    REQUIRE(mt.size() == 3);
    CHECK(mt.language() == "th");
    CHECK(mt[0].source == TimeRange::fromStartEnd(frames(2), frames(6)));
    CHECK(mt.textOf(0, 1) == "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A");
    CHECK(mt.wordAt(frames(7)) == std::optional<std::size_t>(1));  // inside
    CHECK(mt.wordAt(frames(18)) == std::optional<std::size_t>(2)); // nearer the third
    CHECK(mt.wordAt(frames(11)) == std::optional<std::size_t>(1)); // nearer the second
    const auto [b, e] = mt.wordsIn(TimeRange::fromStartEnd(frames(5), frames(21)));
    CHECK(b == 0);
    CHECK(e == 3);
    CHECK(mt.sameAs(MediaTranscript::build(tl.state, tl.media('A'), 0, *p)));
    // Another stream (none here), an unknown item: no words.
    CHECK(MediaTranscript::build(tl.state, tl.media('A'), 1, *p).empty());
    CHECK(MediaTranscript::build(tl.state, MediaId{999}, 0, *p).empty());
    CHECK_FALSE(mt.sameAs(MediaTranscript{}));
}

TEST_CASE("Fix round 1: a zero-length word is in its range and mediaWords lists only the words in the window",
          "[ai][edit][commands][source]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    // A long word over two short ones (overlapping speech), and a zero-length word at 40.
    const auto p = provider(tl, {{'A', transcript("en", {{"Sooo", 0, 60, 0},
                                                         {"yes", 10, 20, 1},
                                                         {"no", 22, 24, 1},
                                                         {"[click]", 40, 40, 0},
                                                         {"done.", 70, 80, 0}})}});
    const MediaTranscript mt = MediaTranscript::build(tl.state, tl.media('A'), 0, *p);
    REQUIRE(mt.size() == 5);
    // The zero-length word is inside a range that starts at its instant.
    const auto [b, e] = mt.wordsIn(TimeRange::fromStartEnd(frames(40), frames(50)));
    CHECK(b <= 3);
    CHECK(e >= 4);
    // Without a long word before it, the zero-length word starts the span of a range that starts at its instant.
    const auto plain = provider(tl, {{'A', transcript("en", {{"yes", 10, 20, 1}, {"[click]", 40, 40, 0}, {"done.", 70, 80, 0}})}});
    const MediaTranscript three = MediaTranscript::build(tl.state, tl.media('A'), 0, *plain);
    REQUIRE(three.size() == 3);
    CHECK(three.wordsIn(TimeRange::fromStartEnd(frames(40), frames(50))) == std::pair<std::size_t, std::size_t>{1, 2});
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscriptServices>(p)},
                     editingCommands());
    // [30, 45): the long word and the zero-length one; not the short words the long one spans.
    const auto window = h.ok("transcript.mediaWords", Json{{"media", "A"}, {"from", frames(30).value}, {"to", frames(45).value}});
    REQUIRE(window.data["words"].size() == 2);
    CHECK(window.data["words"][0][1] == "Sooo");
    CHECK(window.data["words"][1][1] == "[click]");
    // [40, 41): the zero-length word at its start.
    const auto instant = h.ok("transcript.mediaWords", Json{{"media", "A"}, {"from", frames(40).value}, {"to", frames(41).value}});
    REQUIRE(instant.data["words"].size() == 2);
    CHECK(instant.data["words"][1][1] == "[click]");
}
