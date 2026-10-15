// Read commands: compact summaries for agents.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("project.summary lists sequences and media counts", "[command][query]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    const Json data = h.ok("project.summary").data;
    REQUIRE(data["sequences"].size() == 1);
    const Json& seq = data["sequences"][0];
    CHECK(seq["id"] == h.tl.seq.value);
    CHECK(seq["name"] == "Fixture");
    CHECK(seq["frameRate"] == "25/1");
    CHECK(seq["videoTracks"] == 1);
    CHECK(seq["audioTracks"] == 1);
    CHECK(seq["clips"] == 4);
    CHECK(data["media"] == 26); // the fixture makes media A-Z
    CHECK(data["ticksPerSecond"] == kTicksPerSecond);
}

TEST_CASE("sequence.summary is compact: tracks with clip arrays in clipFields order", "[command][query]") {
    CommandHarness h(Timeline(R"(
        V2 lock |..Cc......|
        V1 tgt  |Aaaa..Bbbb|
        A1 tgt  |Aaaa..Bbbb|
    )")
                         .linkMatching());
    h.ok("clip.enable", Json{{"clips", Json::array({h.clip("V1@6").value})}, {"enabled", false}});
    h.ok("marker.add", Json{{"time", tc(5)}, {"name", "Beat"}});
    h.ok("marker.markIn", Json{{"time", tc(2)}});
    const Json data = h.ok("sequence.summary").data;

    CHECK(data["clipFields"] == Json::array({"id", "name", "start", "end", "media", "sourceIn", "link"}));
    CHECK(data["sequence"]["in"] == tc(2));
    CHECK(data["sequence"]["out"].is_null());
    REQUIRE(data["tracks"].size() == 3);
    const Json& v1 = data["tracks"][0];
    CHECK(v1["track"] == "V1");
    CHECK(v1["flags"] == Json::array({"targeted", "syncLocked"}));
    REQUIRE(v1["clips"].size() == 2);
    const Json& a = v1["clips"][0];
    CHECK(a[0] == h.clip("V1@0").value);
    CHECK(a[1] == "A");
    CHECK(a[2] == tc(0));
    CHECK(a[3] == tc(4));
    CHECK(a[4] == h.tl.media('A').value);
    CHECK(a[5] == tc(0));
    CHECK(a[6].is_number()); // linked to A1's clip
    CHECK(data["tracks"][2]["clips"][0][6] == a[6]);
    const Json& v2 = data["tracks"][1];
    CHECK(v2["track"] == "V2");
    CHECK(v2["flags"] == Json::array({"locked", "syncLocked"}));
    CHECK(v1["disabled"] == Json::array({h.clip("V1@6").value}));
    CHECK_FALSE(v2.contains("disabled"));
    CHECK(v2["clips"][0][6].is_null());
    REQUIRE(data["markers"].size() == 1);
    CHECK(data["markers"][0] == Json::array({0, tc(5), tc(0), "Beat"}));

    SECTION("a time window and a clip limit") {
        const Json window = h.ok("sequence.summary", Json{{"from", tc(5)}, {"to", tc(7)}}).data;
        CHECK(window["tracks"][0]["clips"].size() == 1); // only B
        CHECK(window["tracks"][1]["clips"].empty());     // C ends at 4
        const Json limited = h.ok("sequence.summary", Json{{"maxClips", 2}}).data;
        CHECK(limited["truncated"] == true);
        CHECK_FALSE(window.contains("truncated"));
    }
}

TEST_CASE("clip.details tells everything about one clip", "[command][query]") {
    CommandHarness h(Timeline(R"(
        V1 tgt |Aaaa..B5bbb|
        A1 tgt |Aaaa..B5bbb|
    )")
                         .linkMatching());
    const Json d = h.ok("clip.details", Json{{"clip", Json{{"track", "V1"}, {"time", tc(7)}}}}).data;
    CHECK(d["id"] == h.clip("V1@6").value);
    CHECK(d["track"] == "V1");
    CHECK(d["start"]["timecode"] == tc(6));
    CHECK(d["end"]["ticks"] == f(11).value);
    CHECK(d["sourceIn"]["timecode"] == tc(5));
    CHECK(d["sourceOut"]["timecode"] == tc(10));
    CHECK(d["media"]["name"] == "B");
    CHECK(d["stream"] == Json{{"kind", "video"}, {"index", 0}});
    CHECK(d["speed"] == "1/1");
    CHECK(d["enabled"] == true);
    CHECK(d["linkedClips"] == Json::array({h.clip("A1@6").value}));
    CHECK(d["params"]["opacity.opacity"] == 1.0);
    CHECK(d["params"].size() == 9);
}

TEST_CASE("media.list and clip.find", "[command][query]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..BbbbAa|
        A1 tgt |Aaaa..Bbbb..|
    )");
    h.ok("clip.rename", Json{{"clips", Json::array({h.clip("V1@6").value})}, {"name", "Interview take 2"}});

    const Json media = h.ok("media.list", Json{{"name", "b"}}).data;
    CHECK(media["mediaFields"][0] == "id");
    REQUIRE(media["media"].size() == 1);
    CHECK(media["media"][0][1] == "B");
    CHECK(media["media"][0][2] == 40000); // 1000 frames at 25 fps
    CHECK(media["media"][0][3] == true);
    CHECK(media["media"][0][4] == 2);
    CHECK(h.ok("media.list").data["media"].size() == 26);

    const Json byName = h.ok("clip.find", Json{{"name", "INTERVIEW"}}).data;
    REQUIRE(byName["clips"].size() == 1);
    CHECK(byName["clips"][0] == Json::array({h.clip("V1@6").value, "V1", "Interview take 2", tc(6), tc(10), nullptr}));

    CHECK(h.ok("clip.find", Json{{"time", tc(2)}}).data["clips"].size() == 2);
    CHECK(h.ok("clip.find", Json{{"time", tc(2)}, {"tracks", Json::array({"A1"})}}).data["clips"].size() == 1);
    CHECK(h.ok("clip.find", Json{{"media", "A"}}).data["clips"].size() == 3);
    const Json limited = h.ok("clip.find", Json{{"limit", 2}}).data;
    CHECK(limited["clips"].size() == 2);
    CHECK(limited["truncated"] == true);
    CHECK(h.session.undoCount() == 1); // queries never add undo steps
}
