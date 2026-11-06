// 0.7 (batch A9, card 124): track groups through the command engine - trackGroup.create / set / setTracksState / addTracks /
// removeTracks / route / ungroup: one undo step each, refusals by message, and sequence.summary listing the groups.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V2 |..Bbb...|
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
    A2 |.Bbbbbb.|
    A3 |..Cccc..|
)";

std::string message(CommandHarness& h, std::string_view name, Json params) {
    const auto result = h.run(Json{{"command", name}, {"params", std::move(params)}});
    return result.ok() ? std::string() : result.error->message;
}

const Track& trackOf(CommandHarness& h, const char* label) {
    return *h.sequence(h.state()).findTrack(h.tl.track(label));
}

} // namespace

TEST_CASE("trackGroup.* groups, changes and dissolves track groups, one undo step each", "[command][trackgroup]") {
    CommandHarness h(kPicture);
    const auto made = h.ok("trackGroup.create", Json{{"tracks", Json::array({"A1", "A3"})}, {"name", "Dialogue"}, {"colorIndex", 5}});
    REQUIRE(made.data["trackGroup"].is_number());
    const std::uint64_t group = made.data["trackGroup"].get<std::uint64_t>();
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Group Tracks");
    const Sequence& seq = h.sequence(h.state());
    REQUIRE(seq.trackGroups.size() == 1);
    CHECK(seq.trackGroups.front().id.value == group);
    CHECK(seq.trackGroups.front().name == "Dialogue");
    CHECK(seq.trackGroups.front().colorIndex == 5);
    // A3 came up next to A1 (the group's tracks sit together).
    CHECK(seq.audioTracks[1]->id == h.tl.track("A3"));

    // sequence.summary lists the group with its tracks' labels in stack order (the labels follow the new order: A2 is now A3).
    const auto summary = h.ok("sequence.summary", Json::object());
    REQUIRE(summary.data.contains("trackGroups"));
    const Json& listed = summary.data["trackGroups"].at(0);
    CHECK(listed["name"] == "Dialogue");
    CHECK(listed["kind"] == "audio");
    CHECK(listed["collapsed"] == false);
    CHECK(listed["colorIndex"] == 5);
    CHECK(listed["tracks"] == Json::array({"A1", "A2"}));

    // Collapse and rename: one step each; the same value again is a no-op that adds no step.
    h.ok("trackGroup.set", Json{{"trackGroup", group}, {"collapsed", true}, {"name", "Voices"}});
    CHECK(h.session.undoCount() == 2);
    CHECK(h.sequence(h.state()).trackGroups.front().collapsed);
    CHECK(h.sequence(h.state()).trackGroups.front().name == "Voices");
    const std::size_t steps = h.session.undoCount();
    h.ok("trackGroup.set", Json{{"trackGroup", group}, {"collapsed", true}});
    CHECK(h.session.undoCount() == steps);

    // One change reaches every track of the group.
    h.ok("trackGroup.setTracksState", Json{{"trackGroup", group}, {"muted", true}, {"locked", true}});
    CHECK(trackOf(h, "A1").muted);
    CHECK(trackOf(h, "A3").muted);
    CHECK(trackOf(h, "A3").locked);
    CHECK_FALSE(trackOf(h, "A2").muted);

    // A track joins and leaves (the labels follow the group's new order: the track that was A2 is A3 now).
    h.ok("trackGroup.addTracks", Json{{"trackGroup", group}, {"tracks", Json::array({"A3"})}});
    CHECK(h.sequence(h.state()).trackGroups.front().members.size() == 3);
    h.ok("trackGroup.removeTracks", Json{{"tracks", Json::array({"A3"})}});
    CHECK(h.sequence(h.state()).trackGroups.front().members.size() == 2);

    // An audio group goes to a new submix, or back to the Mix (a locked track keeps its output: unlock the group first).
    CHECK(message(h, "trackGroup.route", Json{{"trackGroup", group}, {"output", "mix"}}).find("locked") != std::string::npos);
    h.ok("trackGroup.setTracksState", Json{{"trackGroup", group}, {"locked", false}});
    CHECK_FALSE(trackOf(h, "A3").locked);
    const auto routed = h.ok("trackGroup.route", Json{{"trackGroup", group}, {"output", "newSubmix"}});
    CHECK(routed.data["submix"].get<std::uint64_t>() == h.sequence(h.state()).submixTracks.front()->id.value);
    CHECK(trackOf(h, "A1").output == h.sequence(h.state()).submixTracks.front()->id);
    h.ok("trackGroup.route", Json{{"trackGroup", group}, {"output", "mix"}});
    CHECK(trackOf(h, "A1").output == TrackId{});

    // Ungroup: the tracks stay; undo brings the group back.
    h.ok("trackGroup.ungroup", Json{{"trackGroup", group}});
    CHECK(h.sequence(h.state()).trackGroups.empty());
    REQUIRE(h.session.undo());
    CHECK(h.sequence(h.state()).trackGroups.size() == 1);
}

TEST_CASE("trackGroup.* refuse what cannot be done, naming why", "[command][trackgroup]") {
    CommandHarness h(kPicture);
    CHECK(message(h, "trackGroup.create", Json{{"tracks", Json::array({"V1", "A1"})}}).find("not both") != std::string::npos);
    CHECK(h.errorOf("trackGroup.create", Json{{"tracks", Json::array()}}) != "");
    CHECK(h.errorOf("trackGroup.create", Json{{"tracks", Json::array({"A9"})}}) != "");
    CHECK(h.errorOf("trackGroup.create", Json{{"tracks", Json::array({"A1"})}, {"colorIndex", 40}}) != "");
    const auto made = h.ok("trackGroup.create", Json{{"tracks", Json::array({"A1", "A2"})}, {"name", "Dialogue"}});
    const std::uint64_t group = made.data["trackGroup"].get<std::uint64_t>();
    CHECK(message(h, "trackGroup.create", Json{{"tracks", Json::array({"A2", "A3"})}}).find("Dialogue") != std::string::npos);
    CHECK(h.errorOf("trackGroup.set", Json{{"trackGroup", 999999}, {"collapsed", true}}) == "NotFound");
    CHECK(h.errorOf("trackGroup.set", Json{{"trackGroup", group}, {"name", "   "}}) != "");
    CHECK(h.errorOf("trackGroup.addTracks", Json{{"trackGroup", group}, {"tracks", Json::array({"V1"})}}) != "");
    CHECK(h.errorOf("trackGroup.route", Json{{"trackGroup", group}, {"submix", 424242}}) == "NotFound");
    const auto video = h.ok("trackGroup.create", Json{{"tracks", Json::array({"V1", "V2"})}, {"name", "Picture"}});
    CHECK(message(h, "trackGroup.route", Json{{"trackGroup", video.data["trackGroup"]}, {"output", "newSubmix"}}).find("audio group") !=
          std::string::npos);
    // Nothing above changed the project but the two groups made.
    CHECK(h.session.undoCount() == 2);
}
