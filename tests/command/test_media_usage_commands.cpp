// P1 round (#25, stream 2C): sequence.mediaUsage, the media usage report (CatalogMediaUsage.cpp): one row per use with
// record and source timecodes, linked video and audio as one use, speed, nested sequences flattened into the top
// sequence's record time (or listed as themselves), In to Out, the totals per source, CSV quoting and JSON.

#include "CommandHarness.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return out;
}

/// Media A gets a clearance-style name (with a comma), a path, scene / take; B is AI-generated.
Timeline usageTimeline() {
    Timeline tl(R"(
        V1 tgt |Aaaa..B10bbb|
        A1 tgt |Aaaa........|
    )");
    tl.linkMatching();
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    for (MediaPtr& item : tl.state.media) {
        auto copy = std::make_shared<MediaItem>(*item);
        if (item->id == a) {
            copy->name = "Archive, Reel 7";
            copy->locator.path = "/archive/reel7.mov";
            copy->metadata.scene = "12";
            copy->metadata.take = "3";
        } else if (item->id == b) {
            copy->name = "Generated sky";
            GeneratedProvenance g;
            g.provider = "fal";
            g.model = "sky-v1";
            g.promptHash = std::string(64, 'a');
            g.createdAt = "2026-10-11T09:30:00Z";
            g.kind = "video";
            g.operation = "text-to-video";
            copy->generated = g;
        }
        item = std::move(copy);
    }
    return tl;
}

std::uint64_t mediaNamed(const CommandHarness& h, const std::string& name) {
    for (const MediaPtr& item : h.state()->media) {
        if (item->name == name)
            return item->id.value;
    }
    FAIL("no media " << name);
    return 0;
}

} // namespace

TEST_CASE("sequence.mediaUsage lists each use with record and source timecodes and totals per source",
          "[command][export][p1]") {
    CommandHarness h(usageTimeline());
    // Modify > Timecode: A's source timecode starts at 01:00:00:00 with reel A007.
    h.ok("media.setTimecode", {{"media", Json::array({mediaNamed(h, "Archive, Reel 7")})}, {"timecode", "01:00:00:00"},
                               {"reelName", "A007"}});
    const auto r = h.ok("sequence.mediaUsage");
    CHECK(r.data["format"] == "csv");
    CHECK(r.data["uses"] == 2); // A's linked video and audio are one use
    CHECK(r.data["media"] == 2);
    const auto& uses = r.data["report"]["uses"];
    REQUIRE(uses.size() == 2);
    CHECK(uses[0]["media"] == "Archive, Reel 7");
    CHECK(uses[0]["path"] == "/archive/reel7.mov");
    CHECK(uses[0]["tapeName"] == "A007");
    CHECK(uses[0]["scene"] == "12");
    CHECK(uses[0]["take"] == "3");
    CHECK(uses[0]["track"] == "V1 A1");
    CHECK(uses[0]["recordIn"] == "00:00:00:00");
    CHECK(uses[0]["recordOut"] == "00:00:00:04");
    CHECK(uses[0]["sourceIn"] == "01:00:00:00");
    CHECK(uses[0]["sourceOut"] == "01:00:00:04");
    CHECK(uses[0]["speed"] == "100%");
    CHECK(uses[0]["aiGenerated"] == false);
    CHECK(uses[1]["media"] == "Generated sky");
    CHECK(uses[1]["aiGenerated"] == true);
    CHECK(uses[1]["track"] == "V1");
    CHECK(uses[1]["recordIn"] == "00:00:00:06");
    CHECK(uses[1]["sourceIn"] == "00:00:00:10");
    CHECK(uses[1]["recordOut"] == "00:00:00:12"); // "B10bbb": 6 frames from source frame 10
    CHECK(uses[1]["sourceOut"] == "00:00:00:16");
    CHECK(uses[1]["seconds"].get<double>() == Catch::Approx(0.24));

    const auto csv = lines(r.data["text"].get<std::string>());
    REQUIRE(csv.size() >= 7);
    CHECK(csv[0].starts_with("Media Name,File Path,Tape Name,Scene,Shot,Take,AI Generated,Track,Nested In,Record In"));
    CHECK(csv[1].starts_with("\"Archive, Reel 7\",/archive/reel7.mov,A007,12,,3,,V1 A1,,00:00:00:00,00:00:00:04,"
                             "01:00:00:00,01:00:00:04,00:00:00:04,0.160,100%,"));
    CHECK(csv[2].starts_with("Generated sky,"));
    CHECK(csv[3].empty());
    // (0.7: the rights and clearance columns follow the last of the old ones; a source without a record leaves them blank.)
    CHECK(csv[4] == "Media Name,File Path,Tape Name,AI Generated,Uses,Used Seconds,Source Seconds,"
                    "Clearance,Source,Licence,Territory,Expires,Cost,Credit Line,Rights Flags");
    CHECK(csv[5] == "\"Archive, Reel 7\",/archive/reel7.mov,A007,,1,0.160,0.160,,,,,,,,");
    CHECK(csv[6].starts_with("Generated sky,"));
    CHECK(csv[6].find(",yes,1,0.240,0.240") != std::string::npos);

    // JSON: the same report.
    const auto json = h.ok("sequence.mediaUsage", {{"format", "json"}});
    const Json parsed = Json::parse(json.data["text"].get<std::string>());
    CHECK(parsed["uses"].size() == 2);
    CHECK(parsed["media"][0]["uses"] == 1);

    // In to Out: without marks it says so; with marks only the uses inside, clipped to them.
    CHECK(h.errorOf("sequence.mediaUsage", {{"range", "inOut"}}) == "InvalidParams");
    h.ok("marker.markIn", {{"time", tc(2)}});
    h.ok("marker.markOut", {{"time", tc(7)}});
    const auto marked = h.ok("sequence.mediaUsage", {{"range", "inOut"}});
    const auto& inside = marked.data["report"]["uses"];
    REQUIRE(inside.size() == 2);
    CHECK(inside[0]["recordIn"] == "00:00:00:02");
    CHECK(inside[0]["recordOut"] == "00:00:00:04");
    CHECK(inside[0]["sourceIn"] == "01:00:00:02");
    CHECK(inside[1]["recordIn"] == "00:00:00:06");
    CHECK(inside[1]["sourceIn"] == "00:00:00:10");
    CHECK(marked.data["report"]["range"]["name"] == "inOut");
}

TEST_CASE("sequence.mediaUsage CSV neutralises spreadsheet formulas in names, tape, scene, shot and take (084)",
          "[command][export][p1]") {
    Timeline tl = usageTimeline(); // A is "Archive, Reel 7" and B "Generated sky" now
    for (MediaPtr& item : tl.state.media) {
        auto copy = std::make_shared<MediaItem>(*item);
        if (item->name == "Archive, Reel 7") {
            copy->name = "=HYPERLINK(\"http://x\",\"Click\")"; // quotes and a comma as well
            copy->locator.path = "+cmd|' /C calc'!A0";
            copy->metadata.scene = "@SUM(1+1)";
            copy->metadata.shot = "-2+3";
            copy->metadata.take = "\t=1";
        } else if (item->name == "Generated sky") {
            copy->name = "-1+1";
        }
        item = std::move(copy);
    }
    CommandHarness h(std::move(tl));
    h.ok("media.setTimecode", {{"media", Json::array({mediaNamed(h, "=HYPERLINK(\"http://x\",\"Click\")")})},
                               {"timecode", "01:00:00:00"}, {"reelName", "=A007"}});
    const auto r = h.ok("sequence.mediaUsage");
    const auto csv = lines(r.data["text"].get<std::string>());
    REQUIRE(csv.size() >= 7);
    // The quote goes in front of the formula character, then the cell is quoted as before.
    CHECK(csv[1].starts_with("\"'=HYPERLINK(\"\"http://x\"\",\"\"Click\"\")\","));
    CHECK(csv[1].find(",'+cmd|' /C calc'!A0,'=A007,'@SUM(1+1),'-2+3,'\t=1,") != std::string::npos);
    CHECK(csv[2].starts_with("'-1+1,"));
    // The totals block (sorted by name): the neutralised cells are there too.
    CHECK(std::any_of(csv.begin() + 5, csv.end(), [](const std::string& row) {
        return row.starts_with("\"'=HYPERLINK(\"\"http://x\"\",\"\"Click\"\")\",'+cmd|' /C calc'!A0,'=A007,");
    }));
    CHECK(std::any_of(csv.begin() + 5, csv.end(), [](const std::string& row) { return row.starts_with("'-1+1,"); }));
    // No cell of the report starts with a formula character: split the plain rows on commas outside quotes.
    for (std::size_t row : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{6}}) {
        bool quoted = false;
        bool cellStart = true;
        for (const char ch : csv[row]) {
            if (cellStart && !quoted) {
                CHECK(std::string("=+-@\t\r").find(ch) == std::string::npos);
            }
            if (ch == '"')
                quoted = !quoted;
            cellStart = ch == ',' && !quoted;
        }
    }
    // The JSON report keeps the real names (it is data, not a sheet).
    CHECK(r.data["report"]["uses"][0]["media"] == "=HYPERLINK(\"http://x\",\"Click\")");
}

TEST_CASE("sequence.mediaUsage reports speed and flattens nested sequences into record time", "[command][export][p1]") {
    CommandHarness h(usageTimeline());
    // B at 50 %: 12 frames on the timeline show its 6 frames of the source.
    h.ok("clip.speed", {{"clips", Json::array({h.clip("V1@6").value})}, {"speed", 50}});
    {
        const auto r = h.ok("sequence.mediaUsage");
        const auto& b = r.data["report"]["uses"][1];
        CHECK(b["speed"] == "50%");
        CHECK(b["recordIn"] == "00:00:00:06");
        CHECK(b["recordOut"] == "00:00:00:18");
        CHECK(b["sourceIn"] == "00:00:00:10");
        CHECK(b["sourceOut"] == "00:00:00:16");
        CHECK(r.data["report"]["media"][1]["usedSeconds"].get<double>() == Catch::Approx(0.48));
        CHECK(r.data["report"]["media"][1]["sourceSeconds"].get<double>() == Catch::Approx(0.24));
    }
    // A nested: flattened, its source still reads in the top sequence's record time, through "Nest".
    h.ok("sequence.nest", {{"clips", Json::array({h.clip("V1@0").value, h.clip("A1@0").value})}, {"name", "Nest"}});
    {
        const auto r = h.ok("sequence.mediaUsage", {{"sequence", h.tl.seq.value}}); // two sequences now
        const auto& uses = r.data["report"]["uses"];
        REQUIRE(uses.size() == 2);
        CHECK(uses[0]["media"] == "Archive, Reel 7");
        CHECK(uses[0]["nestedIn"] == "Nest");
        CHECK(uses[0]["track"] == "V1 > V1 A1 > A1");
        CHECK(uses[0]["recordIn"] == "00:00:00:00");
        CHECK(uses[0]["recordOut"] == "00:00:00:04");
        CHECK(uses[0]["sourceIn"] == "00:00:00:00");
    }
    // flatten off: the nest is one row (its video and audio clip together), named after it.
    {
        const auto r = h.ok("sequence.mediaUsage", {{"flatten", false}, {"sequence", h.tl.seq.value}});
        const auto& uses = r.data["report"]["uses"];
        REQUIRE(uses.size() == 2);
        CHECK(uses[0]["media"] == "nested sequence: Nest");
        CHECK(uses[0]["track"] == "V1 A1");
        CHECK(r.data["media"] == 1); // only B is a source of this sequence itself
    }
}
