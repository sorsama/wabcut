// The text and JSON views `wabcut-cli import` prints, and importing into an existing project.

#include "InterchangeTestSupport.h"

#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using Json = nlohmann::json;

TEST_CASE("interchange: summary: timeline and report as text", "[interchange][summary]") {
    const ImportResult r = importFixture("fcp7_23976_mac.xml");
    const std::string text = formatSummary(r);
    CHECK(
        text.find("Imported Final Cut Pro 7 XML into bin 'fcp7_23976_mac': 1 sequence(s), 4 media item(s)") !=
        std::string::npos);
    CHECK(text.find("Sequence 'Reel 2 Assembly': 23.976 fps, 1920x1080, 48000 Hz, duration 00:00:10:00") !=
          std::string::npos);
    CHECK(text.find("00:00:04:00 - 00:00:07:00  'Broll_01.mov'") != std::string::npos);
    CHECK(text.find("OFFLINE 'Room_Tone.aif'") != std::string::npos);
    CHECK(text.find("APPROXIMATED audio-mapping") != std::string::npos);
    CHECK(text.find("at fcp7_23976_mac.xml:") != std::string::npos);
    // Carried entries are counted, and listed only on request.
    CHECK(text.find("CARRIED") == std::string::npos);
    SummaryOptions options;
    options.listCarried = true;
    options.listClips = false;
    const std::string full = formatSummary(r, options);
    CHECK(full.find("CARRIED") != std::string::npos);
    CHECK(full.find("00:00:04:00 - 00:00:07:00") == std::string::npos);
}

TEST_CASE("interchange: summary: JSON has every clip, media item and report entry",
          "[interchange][summary]") {
    const ImportResult r = importFixture("premiere_2997df.xml");
    const Json j = Json::parse(toJson(r));
    CHECK(j["format"] == "fcp7-xml");
    REQUIRE(j["sequences"].size() == 2);
    const Json& seq = j["sequences"][0];
    CHECK(seq["name"] == "Main Edit");
    CHECK(seq["frameRate"]["num"] == 30000);
    CHECK(seq["frameRate"]["den"] == 1001);
    CHECK(seq["dropFrame"] == true);
    REQUIRE(seq["tracks"].size() == 7);
    const Json& v1 = seq["tracks"][0];
    CHECK(v1["label"] == "V1");
    REQUIRE(v1["clips"].size() == 4);
    CHECK(v1["clips"][1]["name"] == "A002_C010.mov");
    CHECK(v1["clips"][1]["startTicks"] == 150 * 8'475'667'200LL);
    CHECK(v1["clips"][1]["start"] == "00:00:05;00");
    CHECK(v1["clips"][2]["enabled"] == false);
    CHECK(j["media"].size() == 6);
    CHECK(j["unresolvedMedia"].size() == 6);
    CHECK(j["report"]["entries"].size() == r.report.size());
    CHECK(j["report"]["counts"]["dropped"] == 1); // C.8: the nested sequence is carried; X.1: the Gaussian Blur too
    const Json& first = j["report"]["entries"][0];
    CHECK(first.contains("outcome"));
    CHECK(first.contains("category"));
    CHECK(first.contains("element"));
    CHECK(first.contains("line"));
}

TEST_CASE("interchange: import into an existing project adds a bin, media and sequences",
          "[interchange][summary]") {
    auto seeded = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(seeded);
    ImportOptions options;
    options.targetProject = seeded->state;
    const ImportResult r = importFixture("resolve_timeline.otio", options);
    CHECK(r.project.sequences.size() == 3); // the seeded one, the timeline and its nested stack (C.8)
    CHECK(r.project.findSequence(seeded->id) != nullptr);
    CHECK(r.project.findSequence(r.sequences[0])->bin == r.bin);
    CHECK(r.project.findBin(r.bin)->name == "resolve_timeline");
    for (const MediaId id : r.media) {
        CHECK(r.project.findMedia(id)->bin == r.bin);
    }
    // Ids keep growing from the target's generator: nothing is reused.
    CHECK(r.sequences[0].value > seeded->id.value);

    ProjectState broken = seeded->state;
    broken.ids.next = 0;
    options.targetProject = broken;
    const auto refused = ImportRouter::importFile(fixturePath("resolve_timeline.otio"), options);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
}
