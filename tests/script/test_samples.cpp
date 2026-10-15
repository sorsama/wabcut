// The sample extensions end to end on fixture projects (C.12): src/script/samples, run headless (runHeadless, what
// `wabcut-cli ext run` uses) in the real wabcut-scripthost.

#include "EditTestUtil.h"
#include "ScriptTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/script/Headless.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;
using wabcut::ai::edit::test::W;

namespace {

ExtensionBundle sample(const std::string& name) {
    auto bundle = loadExtension(fs::path(WABCUT_SCRIPT_SAMPLES_DIR) / name, ExtensionScope::User, parseVersion("0.1.0"));
    INFO(name << ": " << (bundle ? std::string() : bundle.error().message));
    REQUIRE(bundle.has_value());
    return *bundle;
}

HeadlessReport runSample(const std::string& name, const std::string& command, EditSession& session, Json args,
                         Json answers = Json::array(), std::shared_ptr<command::CommandServices> services = nullptr) {
    HeadlessRun run;
    run.bundle = sample(name);
    run.granted = run.bundle.manifest.requested();
    run.command = command;
    run.args = std::move(args);
    run.answers = std::move(answers);
    run.session = &session;
    run.registry = &ai::edit::editingCommands();
    run.services = std::move(services);
    run.wabcutVersion = "0.1.0";
    auto report = runHeadless(std::move(run));
    REQUIRE(report.has_value());
    INFO(toJson(*report).dump(2));
    REQUIRE(report->ok());
    return *report;
}

/// A sequence of `seconds` at 25 fps: V1 and A1 with one clip of media A.
std::string picture(int seconds) {
    const std::string clip = "A" + std::string(static_cast<std::size_t>(seconds * 25 - 1), 'a');
    return "    V1 tgt |" + clip + "|\n    A1 tgt |" + clip + "|\n";
}

} // namespace

TEST_CASE("Every sample has a valid manifest and declares the commands it registers", "[script][samples]") {
    for (const char* name : {"beat-markers", "transcript-names", "youtube-chapters"}) {
        const ExtensionBundle b = sample(name);
        CHECK(b.manifest.warnings.empty());
        CHECK_FALSE(b.manifest.commands.empty());
        CHECK_FALSE(b.manifest.requested().has(command::Capability::MediaAnalysis)); // none needs AI
    }
}

TEST_CASE("Sample: Beat Markers adds a marker at every beat; in one undo step", "[script][samples]") {
    wabcut::test::Timeline tl(picture(4));
    EditSession session(tl.state);
    // No beat grid yet: the tempo comes from the arguments (headless; in the app the prompt asks).
    const HeadlessReport report = runSample("beat-markers", "addBeatMarkers", session, {{"every", "beat"}, {"bpm", 120}});
    const ProjectSnapshot after = session.snapshot();
    const Sequence& seq = *after->findSequence(tl.seq);
    REQUIRE(seq.beatGrid.has_value());
    CHECK(report.call.result["markers"] == seq.markers.size());
    CHECK(seq.markers.size() == 8); // 4 s at 120 BPM
    CHECK(seq.markers[0].name == "Beat 1");
    CHECK(seq.markers[1].start > seq.markers[0].start);
    CHECK(report.undoLabels ==
          std::vector<std::string>{"Beat Markers: Set Beat Grid", "Beat Markers: Add Markers at Every Beat"});
    CHECK(report.notifications.size() == 1); // the toast
    CHECK(report.notifications[0].level == "toast");

    // Again, answering the prompt: downbeats only, in red.
    const HeadlessReport bars = runSample("beat-markers", "addBeatMarkers", session, Json::object(),
                                          Json::array({{{"every", "downbeat"}, {"color", "1"}}}));
    REQUIRE(bars.prompts.size() == 1);
    CHECK(bars.prompts[0]["form"]["title"] == "Add Markers at Every Beat");
    CHECK(bars.call.result["markers"] == 2); // 2 bars of 4/4
    const Sequence& seq2 = *session.snapshot()->findSequence(tl.seq);
    CHECK(seq2.markers.size() == 10);
    CHECK(std::count_if(seq2.markers.begin(), seq2.markers.end(), [](const Marker& m) { return m.name.starts_with("Bar "); }) == 2);
    CHECK(bars.undoLabels == std::vector<std::string>{"Beat Markers: Add Markers at Every Beat"});

    // Cancelling the prompt changes nothing.
    const auto revision = session.revision();
    const HeadlessReport cancelled = runSample("beat-markers", "addBeatMarkers", session, Json::object(), Json::array({nullptr}));
    CHECK(cancelled.call.result["cancelled"] == true);
    CHECK(session.revision() == revision);
}

TEST_CASE("Sample: Transcript Clip Names renames clips after their first words", "[script][samples]") {
    const std::string clip = std::string("A") + std::string(59, 'a');
    const std::string clipB = std::string("B") + std::string(59, 'b');
    wabcut::test::Timeline tl("    V1 tgt |" + clip + clipB + "|\n    A1 tgt |" + clip + clipB + "|\n");
    tl.linkMatching();
    auto transcripts = ai::edit::test::provider(
        tl, {{'A', ai::edit::test::transcript("en", {W{"So,", 0, 3}, W{"um...", 4, 7}, W{"welcome", 8, 13}, W{"to", 13, 15},
                                                     W{"the", 15, 17}, W{"show!", 17, 22}})},
             {'B', ai::edit::test::transcript("en", {W{"Thanks", 2, 7}, W{"for", 7, 9}, W{"having", 9, 14}, W{"me.", 14, 20}})}});
    EditSession session(tl.state);
    const HeadlessReport report = runSample("transcript-names", "renameFromTranscript", session, Json::object(),
                                            Json::array({{{"words", 3}}}),
                                            std::make_shared<ai::edit::TranscriptServices>(transcripts));
    CHECK(report.call.result["renamed"] == 4); // the video clips too, through their links
    const ProjectSnapshot after = session.snapshot();
    const auto nameAt = [&](std::string_view ref) {
        return after->findSequence(tl.seq)->findClip(wabcut::test::clipAt(*after, tl.seq, ref))->clip().name();
    };
    CHECK(nameAt("V1@0") == "So um welcome");
    CHECK(nameAt("A1@0") == "So um welcome");
    CHECK(nameAt("V1@60") == "Thanks for having");
    CHECK(nameAt("A1@60") == "Thanks for having");
    CHECK(report.undoLabels == std::vector<std::string>{"Transcript Clip Names: Rename Clips from Transcript"});

    // Without a transcript it says so and changes nothing.
    wabcut::test::Timeline silent(picture(2));
    EditSession quiet(silent.state);
    const HeadlessReport none = runSample("transcript-names", "renameFromTranscript", quiet, {{"words", 2}},
                                          Json::array(), std::make_shared<ai::edit::TranscriptServices>(
                                                             std::make_shared<ai::edit::MapTranscriptProvider>()));
    CHECK(none.call.result["renamed"] == 0);
    REQUIRE(none.notifications.size() == 1);
    CHECK(none.notifications[0].level == "warning");
    CHECK(quiet.revision() == 0);
}

TEST_CASE("Sample: YouTube Chapters turns chapter markers into description text", "[script][samples]") {
    wabcut::test::Timeline tl(picture(40));
    EditSession session(tl.state);
    {
        command::CommandEngine engine(session);
        for (const auto& [time, name] : {std::pair{"00:00:00:00", "Intro"}, std::pair{"00:00:12:00", "The gear"},
                                         std::pair{"00:00:25:00", "Wrap-up"}}) {
            const auto r = engine.executeJson(
                Json{{"command", "marker.add"}, {"params", {{"time", time}, {"name", name}, {"type", "chapter"}}}});
            REQUIRE(r.ok());
        }
    }
    const auto revision = session.revision();
    const HeadlessReport report = runSample("youtube-chapters", "chaptersToDescription", session, Json::object());
    const std::string text = report.call.result["text"].get<std::string>();
    CHECK(text.find("0:00 Intro") != std::string::npos);
    CHECK(text.find("0:12 The gear") != std::string::npos);
    CHECK(text.find("0:25 Wrap-up") != std::string::npos);
    REQUIRE(report.prompts.size() == 1); // shown read-only, to copy
    const Json& field = report.prompts[0]["form"]["fields"][0];
    CHECK(field["readOnly"] == true);
    CHECK(field["value"] == text);
    CHECK(session.revision() == revision); // read only
    CHECK(report.undoLabels.empty());

    // Quiet: just the text.
    const HeadlessReport quiet = runSample("youtube-chapters", "chaptersToDescription", session, {{"quiet", true}});
    CHECK(quiet.prompts.empty());
    CHECK(quiet.call.result["text"] == text);
}
