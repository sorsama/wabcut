// 0.6 (P1 round, 3A, #62): the creator autopilot's commands through the editing catalog, on fixtures (transcripts laid
// out word by word, a colour-word embedder for the smart search, shots handed over in memory, drawn frames and a fake
// still writer; no model, no decoder, no network): auto B-roll lands on the library item that matches what is said
// (by its name, Description, clip marker or picture) and undoes in one step; the publishing kit's files are complete;
// pacing match gives the cuts a known reference rhythm implies, never inside a word; and the pure helpers.

#include "AutopilotTestUtil.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/autopilot/AutopilotCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/search/SearchCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/FrameServices.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using namespace wabcut::test::autopilotfx;
using command::Json;
namespace autopilot = wabcut::ai::autopilot;

namespace {

/// A chat model that answers with `reply` and keeps what it was asked.
class FakeChat final : public ai::IChatModel {
public:
    FakeChat(std::string reply, std::shared_ptr<std::string> asked) : m_reply(std::move(reply)), m_asked(std::move(asked)) {}
    const ai::ModelSpec& spec() const override {
        static const ai::ModelSpec model;
        return model;
    }
    std::expected<ai::ChatResponse, ai::AiError> chat(const ai::ChatRequest& request, const ai::ChatEventHandler&, std::stop_token) override {
        *m_asked = request.system + "\n" + (request.messages.empty() ? std::string() : request.messages.front().text());
        ai::ChatResponse response;
        response.message = ai::ChatMessage::assistant(m_reply);
        return response;
    }

private:
    std::string m_reply;
    std::shared_ptr<std::string> m_asked;
};

} // namespace

// --- Words and helpers ---------------------------------------------------------------------------------------------

TEST_CASE("Autopilot words: names split into stemmed keywords and safe file names", "[app][ai][autopilot]") {
    CHECK(autopilot::keywordTokens("DroneCity_night2.mp4") == std::vector<std::string>{"drone", "city", "night"});
    CHECK(autopilot::stem("flying") == "fly");
    CHECK(autopilot::stem("cities") == "city");
    CHECK(autopilot::stem("cooked") == "cook");
    CHECK(autopilot::stem("running") == "run");
    CHECK(autopilot::stem("glass") == "glass");
    CHECK(autopilot::youtubeTime(secs(0)) == "0:00");
    CHECK(autopilot::youtubeTime(secs(65.4)) == "1:05");
    CHECK(autopilot::youtubeTime(secs(3723)) == "1:02:03");
    CHECK(autopilot::safeFileName("CON") == "_CON");
    CHECK(autopilot::safeFileName("a/b:c?") == "a-b-c-");
    CHECK(autopilot::safeFileName("  .  ") == "Untitled");
    const std::vector<std::string> tags{"telescope", "first light", "mirror", "diy"};
    CHECK(autopilot::hashtags(tags) == "#Telescope #FirstLight #Mirror");
    const auto reply = autopilot::parseKitReply("Sure! {\"titles\": [\"A\", \"B\"], \"description\": \"D\", \"tags\": [\"#x\", \"y,z\"]} Done.");
    REQUIRE(reply);
    CHECK(reply->titles == std::vector<std::string>{"A", "B"});
    CHECK(reply->description == "D");
    CHECK(reply->tags == std::vector<std::string>{"x", "yz"});
    CHECK_FALSE(autopilot::parseKitReply("no json here"));
    CHECK_FALSE(autopilot::frameQuality(*drawnFrame(MediaItem{}, secs(1))).usable);  // black
    CHECK_FALSE(autopilot::frameQuality(*drawnFrame(MediaItem{}, secs(40))).usable); // flat
    const autopilot::FrameQuality good = autopilot::frameQuality(*drawnFrame(MediaItem{}, secs(70)));
    CHECK(good.usable);
    CHECK(good.score > 0.6);
    CHECK(good.sharpness > 0.6);
}

// --- Auto B-roll ---------------------------------------------------------------------------------------------------

TEST_CASE("Auto B-roll places library shots that match what is said on a new track as one undo step", "[app][ai][autopilot][broll]") {
    Recording essay = brollEssay();
    Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
    const auto before = h.session.snapshot();

    // The preview (a dry run) changes nothing and lists the suggestions.
    const command::CommandResult preview = h.ok("autopilot.broll", Json::object(), true);
    INFO(preview.data.dump(2));
    CHECK(h.session.snapshot() == before);
    CHECK(preview.data["visual"] == false);
    CHECK(preview.data["lines"] == 7);
    REQUIRE(preview.data["suggestions"].size() == 3);
    const Json* drone = suggestionFor(preview.data, essay.media('B'));
    const Json* pasta = suggestionFor(preview.data, essay.media('C'));
    const Json* beach = suggestionFor(preview.data, essay.media('D'));
    REQUIRE(drone);
    REQUIRE(pasta);
    REQUIRE(beach);
    CHECK((*drone)["text"].get<std::string>().find("drone") != std::string::npos);
    CHECK((*drone)["evidence"] == "name");
    CHECK((*pasta)["text"].get<std::string>().find("pasta") != std::string::npos);
    CHECK((*pasta)["evidence"] == "description");
    CHECK((*beach)["text"].get<std::string>().find("surfing") != std::string::npos);
    CHECK((*beach)["evidence"] == "marker");
    // Around the clip marker at 12 s, as long as the line (within 2..6 s), where the line starts.
    const double length = (*beach)["duration"].get<double>();
    CHECK(length >= 2.0);
    CHECK(length <= 6.0);
    const double sourceIn = (*beach)["sourceIn"].get<double>();
    CHECK(sourceIn <= 12.0);
    CHECK(sourceIn + length >= 12.5);
    CHECK(std::abs((*beach)["at"].get<double>() - 46.0) < 0.05);
    // Nothing for the weather, the intro is left alone, and the unmatched lines carry a generation prompt.
    std::vector<std::string> unmatched;
    for (const Json& u : preview.data["unmatched"]) {
        unmatched.push_back(u["text"].get<std::string>());
        CHECK(u["prompt"].get<std::string>().find("B-roll") != std::string::npos);
    }
    CHECK(std::ranges::any_of(unmatched, [](const std::string& t) { return t.find("weather") != std::string::npos; }));
    CHECK(std::ranges::any_of(unmatched, [](const std::string& t) { return t.find("red sports car") != std::string::npos; }));
    CHECK(std::ranges::none_of(unmatched, [](const std::string& t) { return t.find("Welcome") != std::string::npos; }));
    CHECK(preview.data["track"]["created"] == true);
    CHECK(preview.data["track"]["label"] == "V2");

    // Apply: one edit, video only, on V2 named "B-roll"; the A-roll is untouched.
    const command::CommandResult applied = h.ok("autopilot.broll", Json::object());
    CHECK(applied.data["placed"] == 3);
    const auto after = h.session.snapshot();
    const Sequence& seq = *after->findSequence(essay.tl.seq);
    REQUIRE(seq.videoTracks.size() == 2);
    CHECK(seq.videoTracks[1]->name == "B-roll");
    CHECK(seq.videoTracks[1]->clips.size() == 3);
    CHECK(seq.audioTracks.size() == 1);
    CHECK(seq.audioTracks[0]->clips.size() == 1);
    CHECK(seq.videoTracks[0]->clips.size() == 1);
    for (const Clip& c : seq.videoTracks[1]->clips) {
        CHECK_FALSE(c.linkGroup.isValid());
        CHECK(c.start() >= secs(3.0));
    }
    REQUIRE(validate(*after));
    REQUIRE(h.session.undo());
    CHECK(*h.session.snapshot() == *before);
}

TEST_CASE("Auto B-roll matches by picture with the smart search and places only the chosen suggestions", "[app][ai][autopilot][broll]") {
    Recording essay = brollEssay();
    RedShots search(essay);
    Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts),
                                      std::make_shared<ai::search::SearchServices>(search.search)}));
    const command::CommandResult preview = h.ok("autopilot.broll", Json::object(), true);
    INFO(preview.data.dump(2));
    CHECK(preview.data["visual"] == true);
    REQUIRE(preview.data["suggestions"].size() == 4);
    const Json* car = suggestionFor(preview.data, essay.media('E'));
    REQUIRE(car);
    CHECK((*car)["how"] == "picture");
    CHECK((*car)["text"].get<std::string>().find("red sports car") != std::string::npos);

    // The chosen ones only (the sheet switched two off): placed exactly where the preview put them.
    Json placements = Json::array();
    for (const Json& s : preview.data["suggestions"]) {
        if (s["media"] == essay.media('E').value || s["media"] == essay.media('B').value) {
            placements.push_back(Json{{"media", s["media"]}, {"atTicks", s["atTicks"]}, {"sourceInTicks", s["sourceInTicks"]},
                                      {"durationTicks", s["durationTicks"]}, {"line", s["line"]}});
        }
    }
    REQUIRE(placements.size() == 2);
    const std::size_t undoBefore = h.session.undoCount();
    const command::CommandResult placed = h.ok("autopilot.broll", Json{{"placements", placements}});
    CHECK(placed.data["placed"] == 2);
    CHECK(h.session.undoCount() == undoBefore + 1);
    const Sequence& seq = *h.session.snapshot()->findSequence(essay.tl.seq);
    REQUIRE(seq.videoTracks.size() == 2);
    REQUIRE(seq.videoTracks[1]->clips.size() == 2);
    CHECK(seq.videoTracks[1]->clips[0].media == essay.media('B'));
    CHECK(seq.videoTracks[1]->clips[1].media == essay.media('E'));
    CHECK(seq.videoTracks[1]->clips[1].start() == Ticks{(*car)["atTicks"].get<std::int64_t>()});
    // A second run puts more B-roll on the same free track above the A-roll.
    const command::CommandResult again = h.ok("autopilot.broll", Json{{"placements", Json::array({Json{{"media", essay.media('C').value},
                                                                                                        {"at", 18.0}, {"sourceIn", 1.0}, {"duration", 3.0}}})}});
    CHECK(again.data["track"]["label"] == "V2");
    CHECK(again.data["track"]["created"] == false);
    // Placements over B-roll already there go on the next free track.
    const std::size_t undoNow = h.session.undoCount();
    const command::CommandResult clash = h.run("autopilot.broll", Json{{"placements", placements}});
    CHECK(clash.ok());
    CHECK(clash.data["track"]["label"] == "V3"); // the next free track
    CHECK(h.session.undoCount() == undoNow + 1);
}

TEST_CASE("Auto B-roll skips video tracks that are switched off or locked (card 167)", "[app][ai][autopilot][broll]") {
    const std::string clip = "A" + std::string(60 * 25 - 1, 'a');
    const std::string base = "V1 tgt |" + clip + "|\nA1 tgt |" + clip + "|\n";
    SECTION("an empty hidden V2 is passed over: the B-roll gets a track of its own that shows") {
        Recording essay = brollEssay("V2 off |.|\n" + base);
        Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
        const command::CommandResult preview = h.ok("autopilot.broll", Json::object(), true);
        CHECK(preview.data["track"]["created"] == true);
        CHECK(preview.data["track"]["label"] == "V3");
        const command::CommandResult applied = h.ok("autopilot.broll", Json::object());
        CHECK(applied.data["placed"] == 3);
        const Sequence& seq = *h.session.snapshot()->findSequence(essay.tl.seq);
        REQUIRE(seq.videoTracks.size() == 3);
        CHECK_FALSE(seq.videoTracks[1]->enabled);
        CHECK(seq.videoTracks[1]->clips.empty()); // the hidden track stays as it was
        CHECK(seq.videoTracks[2]->name == "B-roll");
        CHECK(seq.videoTracks[2]->enabled);
        CHECK(seq.videoTracks[2]->clips.size() == 3);
    }
    SECTION("a free visible track above the hidden one is used") {
        Recording essay = brollEssay("V3 |.|\nV2 off |.|\n" + base);
        Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
        const command::CommandResult applied = h.ok("autopilot.broll", Json::object());
        CHECK(applied.data["track"]["label"] == "V3");
        CHECK(applied.data["track"]["created"] == false);
        const Sequence& seq = *h.session.snapshot()->findSequence(essay.tl.seq);
        REQUIRE(seq.videoTracks.size() == 3);
        CHECK(seq.videoTracks[1]->clips.empty());
        CHECK(seq.videoTracks[2]->clips.size() == 3);
    }
    SECTION("a locked V2 is passed over too") {
        Recording essay = brollEssay("V2 lock |.|\n" + base);
        Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
        const command::CommandResult applied = h.ok("autopilot.broll", Json::object());
        CHECK(applied.data["track"]["label"] == "V3");
        CHECK(applied.data["track"]["created"] == true);
        const Sequence& seq = *h.session.snapshot()->findSequence(essay.tl.seq);
        REQUIRE(seq.videoTracks.size() == 3);
        CHECK(seq.videoTracks[1]->clips.empty());
        CHECK(seq.videoTracks[2]->clips.size() == 3);
    }
    SECTION("a visible free V2 is still the first choice") {
        Recording essay = brollEssay("V2 |.|\n" + base);
        Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
        const command::CommandResult applied = h.ok("autopilot.broll", Json::object());
        CHECK(applied.data["track"]["label"] == "V2");
        CHECK(applied.data["track"]["created"] == false);
    }
}

TEST_CASE("Auto B-roll needs transcripts and says why nothing was placed", "[app][ai][autopilot][broll]") {
    Recording essay = brollEssay();
    Harness none(essay.tl.state, bundle({}));
    const command::CommandResult refused = none.run("autopilot.broll", Json::object());
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error->code == command::ErrorCode::InvalidRequest);
    // A library with nothing to match: no edit, a warning.
    Recording bare(60, brollLines());
    Harness h(bare.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(bare.transcripts)}));
    const std::size_t undoBefore = h.session.undoCount();
    const command::CommandResult result = h.ok("autopilot.broll", Json::object());
    CHECK(result.data["placed"] == 0);
    CHECK(h.session.undoCount() == undoBefore);
    CHECK(std::ranges::any_of(result.warnings, [](const std::string& w) { return w.find("no B-roll was placed") != std::string::npos; }));
}

// --- Publishing kit -------------------------------------------------------------------------------------------------

TEST_CASE("Publishing kit writes complete files with chapters from markers and usable thumbnails", "[app][ai][autopilot][kit]") {
    Recording essay(120, telescopeLines());
    essay.addMarker(0.0, "Intro");
    essay.addMarker(40.0, "Building the tube");
    essay.addMarker(60.0, "First light");
    auto asked = std::make_shared<std::vector<Ticks>>();
    Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts),
                                      std::make_shared<autopilot::AutopilotServices>(drawnFrame), fakeStills(asked)}));
    const auto before = h.session.snapshot();

    const command::CommandResult preview = h.ok("autopilot.publishKit", Json{{"thumbnails", 3}}, true);
    const Json& kit = preview.data;
    INFO(kit.dump(2));
    REQUIRE(kit["titles"].size() >= 1);
    for (const Json& t : kit["titles"]) {
        CHECK(t.get<std::string>().size() <= 70);
        CHECK_FALSE(t.get<std::string>().empty());
    }
    CHECK(kit["title"] == kit["titles"][0]);
    CHECK(kit["chaptersFrom"] == "markers");
    CHECK(kit["chaptersText"] == "0:00 Intro\n0:40 Building the tube\n1:00 First light\n");
    const std::string description = kit["description"].get<std::string>();
    CHECK(description.find("Chapters\n0:00 Intro\n0:40 Building the tube\n1:00 First light") != std::string::npos);
    CHECK(description.find("#Telescope") != std::string::npos);
    const std::vector<std::string> tags = kit["tags"].get<std::vector<std::string>>();
    CHECK(std::ranges::find(tags, "telescope") != tags.end());
    CHECK(std::ranges::find(tags, "mirror") != tags.end());
    CHECK(tags.size() <= 15);
    // Thumbnails: only usable frames (not the black or flat ones), at least 5 s apart.
    REQUIRE(kit["thumbnails"].size() >= 2);
    std::vector<double> times;
    for (const Json& t : kit["thumbnails"]) {
        times.push_back(t["seconds"].get<double>());
        CHECK(t["seconds"].get<double>() >= 50.0);
    }
    for (std::size_t i = 0; i < times.size(); ++i) {
        for (std::size_t j = i + 1; j < times.size(); ++j) {
            CHECK(std::abs(times[i] - times[j]) >= 5.0);
        }
    }
    CHECK(asked->empty()); // a preview writes nothing

    // Written into a folder named after the sequence; the project does not change.
    ai::search::test::TempDir dir("kit");
    const command::CommandResult written =
        h.ok("autopilot.publishKit", Json{{"thumbnails", 3}, {"folder", pathToUtf8(dir.path)}, {"title", "I built a telescope"}, {"tags", Json::array({"telescope", "diy"})}});
    CHECK(written.data["written"] == true);
    const std::filesystem::path folder = dir.path / "Telescope build - publishing kit";
    CHECK(written.data["folder"] == pathToUtf8(folder));
    for (const char* name : {"title.txt", "description.txt", "chapters.txt", "tags.txt", "publishing-kit.md", "publishing-kit.json"}) {
        INFO(name);
        CHECK(std::filesystem::is_regular_file(folder / name));
    }
    CHECK(readFile(folder / "title.txt") == "I built a telescope\n");
    CHECK(readFile(folder / "chapters.txt") == "0:00 Intro\n0:40 Building the tube\n1:00 First light\n");
    CHECK(readFile(folder / "tags.txt") == "telescope, diy\n");
    CHECK(readFile(folder / "description.txt").find("0:40 Building the tube") != std::string::npos);
    CHECK(asked->size() == kit["thumbnails"].size());
    for (std::size_t i = 1; i <= kit["thumbnails"].size(); ++i) {
        CHECK(std::filesystem::is_regular_file(folder / ("thumbnail-" + std::to_string(i) + ".jpg")));
    }
    const std::string md = readFile(folder / "publishing-kit.md");
    CHECK(md.find("# Publishing kit: Telescope build") != std::string::npos);
    CHECK(md.find("thumbnail-1.jpg") != std::string::npos);
    CHECK(md.find("Nothing was uploaded") != std::string::npos);
    const Json saved = Json::parse(readFile(folder / "publishing-kit.json"));
    CHECK(saved["title"] == "I built a telescope");
    CHECK(saved["thumbnails"][0]["file"] == "thumbnail-1.jpg");
    CHECK(*h.session.snapshot() == *before);
    CHECK(h.session.undoCount() == 0);
}

TEST_CASE("Publishing kit markdown pairs each thumbnail file with its own time when an earlier one failed to write",
          "[app][ai][autopilot][kit]") {
    Recording essay(120, telescopeLines());
    // A still writer that cannot write thumbnail-1.jpg (disk full, antivirus lock) and writes the others.
    auto stills = std::make_shared<command::FrameExportServices>(
        [](const ProjectState&, SequenceId, Ticks, const std::filesystem::path& file, const std::string&,
           const command::FrameExportServices::Options& o) -> std::expected<command::FrameExportServices::Written, Error> {
            if (file.filename() == "thumbnail-1.jpg") {
                return std::unexpected(Error{ErrorCode::IoError, "disk full"});
            }
            std::ofstream(file, std::ios::binary) << "JPEG";
            return command::FrameExportServices::Written{file, o.width, o.height, 4, 92};
        },
        command::FrameExportServices::Describer{}, std::vector<std::string>{"jpeg", "png"});
    Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts),
                                      std::make_shared<autopilot::AutopilotServices>(drawnFrame), stills}));
    ai::search::test::TempDir dir("kit-fail");
    const command::CommandResult written =
        h.ok("autopilot.publishKit", Json{{"thumbnails", 4}, {"folder", pathToUtf8(dir.path)}});
    const Json& thumbs = written.data["thumbnails"];
    INFO(written.data.dump(2));
    REQUIRE(thumbs.size() >= 2); // at least one candidate after the one that failed
    CHECK(std::ranges::any_of(written.warnings, [](const std::string& w) { return w.find("thumbnail-1.jpg") != std::string::npos; }));
    const std::filesystem::path folder = dir.path / "Telescope build - publishing kit";
    const std::string md = readFile(folder / "publishing-kit.md");
    CHECK(md.find("thumbnail-1.jpg") == std::string::npos);
    // Every file that was written is listed with the time of ITS candidate (not of the one at its position among the
    // files written), and the JSON agrees.
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        const std::string file = "thumbnail-" + std::to_string(i + 1) + ".jpg";
        INFO(file);
        if (i == 0) {
            CHECK_FALSE(thumbs[i].contains("file"));
            continue;
        }
        CHECK(thumbs[i]["file"] == file);
        CHECK(md.find("- " + file + " at " + thumbs[i]["time"].get<std::string>() + " (") != std::string::npos);
    }
}

TEST_CASE("Publishing kit finds chapters in the topics without markers and warns about YouTube rules", "[app][ai][autopilot][kit]") {
    Recording essay(120, telescopeLines());
    Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
    const command::CommandResult result = h.ok("autopilot.publishKit", Json{{"chapters", "auto"}}, true);
    INFO(result.data.dump(2));
    const std::string from = result.data["chaptersFrom"].get<std::string>();
    CHECK((from == "topics" || from == "none"));
    if (from == "topics") {
        REQUIRE(result.data["chapters"].size() >= 2);
        CHECK(result.data["chapters"][0]["time"] == "0:00");
    }
    // Without frames the thumbnails are ranked by what happens there; no writer: no thumbnail files, a warning.
    CHECK(result.data["thumbnails"].size() >= 1);
    ai::search::test::TempDir dir("kit-nowriter");
    const command::CommandResult written = h.ok("autopilot.publishKit", Json{{"folder", pathToUtf8(dir.path)}});
    CHECK(std::ranges::any_of(written.warnings, [](const std::string& w) { return w.find("cannot render frames") != std::string::npos; }));
    CHECK(std::filesystem::is_regular_file(dir.path / "Telescope build - publishing kit" / "title.txt"));
    // "none": no chapters at all.
    CHECK(h.ok("autopilot.publishKit", Json{{"chapters", "none"}}, true).data["chapters"].empty());
}

TEST_CASE("Publishing kit writes the copy with the user's chat model from the transcript text only", "[app][ai][autopilot][kit][cloud]") {
    Recording essay(120, telescopeLines());
    essay.addMarker(0.0, "Intro");
    essay.addMarker(60.0, "First light");
    const auto asked = std::make_shared<std::string>();
    const std::string reply =
        "Here you go: {\"titles\": [\"I Built a Telescope From Scratch\"], \"description\": \"A homemade telescope, start to first light.\", "
        "\"tags\": [\"telescope\", \"diy telescope\"]}";
    autopilot::AutopilotServices::ChatOpener opener = [reply, asked]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> {
        return std::make_unique<FakeChat>(reply, asked);
    };
    Harness h(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts),
                                      std::make_shared<autopilot::AutopilotServices>(autopilot::FramePicture{}, opener)}));
    const command::CommandResult result = h.ok("autopilot.publishKit", Json{{"cloud", true}}, true);
    INFO(result.data.dump(2));
    CHECK(result.data["cloud"] == true);
    CHECK(result.data["titles"][0] == "I Built a Telescope From Scratch");
    CHECK(result.data["titles"].size() >= 2); // the offline ideas follow
    CHECK(result.data["description"].get<std::string>().starts_with("A homemade telescope, start to first light."));
    CHECK(result.data["description"].get<std::string>().find("1:00 First light") != std::string::npos);
    CHECK(result.data["tags"] == Json::array({"telescope", "diy telescope"}));
    // What was sent: the transcript's words and the chapter titles, as text.
    CHECK(asked->find("Saturn and its rings") != std::string::npos);
    CHECK(asked->find("1:00 First light") != std::string::npos);

    // An answer that cannot be read keeps the offline copy, with a warning.
    autopilot::AutopilotServices::ChatOpener garbled = [asked]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> {
        return std::make_unique<FakeChat>("I cannot help with that.", asked);
    };
    Harness g(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts),
                                      std::make_shared<autopilot::AutopilotServices>(autopilot::FramePicture{}, garbled)}));
    const command::CommandResult kept = g.ok("autopilot.publishKit", Json{{"cloud", true}}, true);
    CHECK(kept.data["cloud"] == false);
    CHECK(std::ranges::any_of(kept.warnings, [](const std::string& w) { return w.find("could not be read") != std::string::npos; }));
    // No chat model here: refused, nothing is sent.
    Harness offline(essay.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
    const command::CommandResult refused = offline.run("autopilot.publishKit", Json{{"cloud", true}}, true);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error->code == command::ErrorCode::InvalidRequest);
}

// --- Pacing match ---------------------------------------------------------------------------------------------------

namespace {

/// A minute of talk in sentences of seven words with a 0.6 s pause after each.
std::vector<std::pair<double, std::string>> steadyTalk() {
    std::vector<std::pair<double, std::string>> lines;
    const char* sentences[] = {"We start the build with the frame.", "Then the motor goes in the middle.",
                               "The wires run along the left side.", "Next we check every screw by hand.",
                               "After that the lid snaps on top."};
    double t = 0.5;
    for (int i = 0; t < 58.0; ++i) {
        lines.emplace_back(t, sentences[i % 5]);
        t += 7 * 0.40 + 0.6;
    }
    return lines;
}

bool insideAWord(const ai::Transcript& t, Ticks time) {
    return std::ranges::any_of(t.words, [&](const ai::TranscriptWord& w) { return w.t0 < time && time < w.t1; });
}

} // namespace

TEST_CASE("Pacing match suggests cuts at the reference rhythm between words and applies them as one undo step", "[app][ai][autopilot][pacing]") {
    Recording talk(60, steadyTalk());
    // The reference: media R, a 40 s video cut every 4 s.
    auto shots = std::make_shared<ai::analysis::MapAnalysisProvider>();
    shots->addShots(talk.media('R'), ai::search::test::shotsAt(40, {4, 8, 12, 16, 20, 24, 28, 32, 36}));
    Harness h(talk.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(talk.transcripts),
                                     std::make_shared<ai::analysis::AnalysisServices>(shots)}));
    const auto before = h.session.snapshot();
    const command::CommandResult result = h.ok("autopilot.pacing", Json{{"reference", talk.media('R').value}}, true);
    const Json& d = result.data;
    INFO(d.dump(2));
    CHECK(d["reference"]["cuts"] == 9);
    CHECK(std::abs(d["reference"]["medianShot"].get<double>() - 4.0) < 0.01);
    CHECK(std::abs(d["reference"]["cutsPerMinute"].get<double>() - 13.5) < 0.1);
    CHECK(d["edit"]["cuts"] == 0);
    const std::size_t cuts = d["suggestions"].size();
    CHECK(cuts >= 12);
    CHECK(cuts <= 16);
    const ai::Transcript& words = *talk.transcripts->transcript(*talk.tl.state.findMedia(talk.media('A')), 0);
    double previous = 0.0;
    for (const Json& s : d["suggestions"]) {
        const double at = s["seconds"].get<double>();
        INFO(s.dump());
        CHECK_FALSE(insideAWord(words, Ticks{s["ticks"].get<std::int64_t>()}));
        CHECK(at - previous >= 0.6 * 4.0 * 0.6);
        CHECK(at - previous <= 4.0 * 1.6);
        CHECK((s["kind"] == "sentence" || s["kind"] == "pause" || s["kind"] == "between words"));
        previous = at;
    }
    // The edit with the cuts has about the reference's pace.
    CHECK(std::abs(d["after"]["cutsPerMinute"].get<double>() - 13.5) < 3.5);
    CHECK(*h.session.snapshot() == *before);

    // Markers for the chosen suggestions, one undo step.
    const command::CommandResult markers = h.ok("autopilot.pacing", Json{{"reference", talk.media('R').value}, {"apply", "markers"}, {"only", Json::array({1, 2, 3})}});
    CHECK(markers.data["applied"] == 3);
    const Sequence& marked = *h.session.snapshot()->findSequence(talk.tl.seq);
    CHECK(std::ranges::count_if(marked.markers, [](const Marker& m) { return m.name == "Cut (pace)"; }) == 3);
    REQUIRE(h.session.undo());
    CHECK(*h.session.snapshot() == *before);

    // Cuts on all tracks, from the cuts the analysis listed (as the sheet passes them back): one undo step.
    const Json cutTimes = d["reference"]["cutTimes"];
    REQUIRE(cutTimes.size() == 9);
    const command::CommandResult cut =
        h.ok("autopilot.pacing", Json{{"referenceCuts", cutTimes}, {"referenceLength", d["reference"]["length"]}, {"apply", "cuts"}});
    CHECK(cut.data["applied"] == cuts);
    CHECK(cut.data["suggestions"] == d["suggestions"]);
    const Sequence& split = *h.session.snapshot()->findSequence(talk.tl.seq);
    CHECK(split.videoTracks[0]->clips.size() == cuts + 1);
    CHECK(split.audioTracks[0]->clips.size() == cuts + 1);
    REQUIRE(validate(*h.session.snapshot()));
    REQUIRE(h.session.undo());
    CHECK(*h.session.snapshot() == *before);
}

TEST_CASE("Pacing match leaves an edit that already cuts faster than the reference alone", "[app][ai][autopilot][pacing]") {
    // Two recordings alternating every 2 s, against a reference that cuts every 4 s.
    std::string v;
    for (int i = 0; i < 30; ++i) {
        const char letter = i % 2 == 0 ? 'A' : 'B';
        v += std::string(1, letter) + std::string(49, static_cast<char>(letter - 'A' + 'a'));
    }
    Recording talk(60, steadyTalk(), "V1 |" + v + "|\nA1 |" + v + "|\n");
    const command::CommandResult result =
        Harness(talk.tl.state, bundle({std::make_shared<ai::edit::TranscriptServices>(talk.transcripts)}))
            .ok("autopilot.pacing", Json{{"referenceCuts", Json::array({4, 8, 12, 16, 20, 24, 28, 32, 36})}, {"referenceLength", 40}}, true);
    INFO(result.data.dump(2));
    CHECK(result.data["edit"]["cuts"] == 29);
    CHECK(result.data["suggestions"].empty());
    CHECK(result.data["fasterShots"].get<int>() == 30); // 2 s shots: well under the reference's 4 s
    CHECK(std::ranges::any_of(result.warnings, [](const std::string& w) { return w.find("already cuts") != std::string::npos; }));
    // A reference is needed.
    Harness h(talk.tl.state, bundle({}));
    CHECK(h.run("autopilot.pacing", Json::object(), true).error->code == command::ErrorCode::InvalidParams);
    CHECK(h.run("autopilot.pacing", Json{{"reference", talk.media('R').value}}, true).error->code == command::ErrorCode::InvalidRequest);
}
