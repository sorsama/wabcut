// C.8 commands through the engine: sequence.nest / unnest, clip.openNested, editing a sequence in as a source, and the
// multicam commands (create with each kind of sync, angles, switchAngle as one undo step, flatten, setSync).

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Nesting.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

/// The params with the fixture's sequence named (the project has several once a command made one).
Json in(const CommandHarness& h, Json params) {
    params["sequence"] = h.tl.seq.value;
    return params;
}

std::string anglesOf(CommandHarness& h, const char* track, int frame) {
    const auto d = h.ok("clip.details", in(h, Json{{"clip", at(track, frame)}}));
    return d.data["nested"]["angles"].dump();
}

} // namespace

TEST_CASE("sequence.nest and sequence.unnest through the engine, one undo step each", "[command][nesting]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    tl.linkMatching();
    CommandHarness h(std::move(tl));
    const ProjectSnapshot original = h.state();
    const auto dry = h.run(Json{{"command", "sequence.nest"},
                                {"params", Json{{"clips", Json::array({at("V1", 0), at("V1", 4)})}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *original);

    const auto nested = h.ok("sequence.nest", Json{{"clips", Json::array({at("V1", 0), at("V1", 4)})}, {"name", "Talk"}});
    CHECK(nested.data["name"] == "Talk");
    CHECK(nested.data["clips"].size() == 2);
    CHECK(h.session.undoCount() == 1);
    const std::uint64_t nestedId = nested.data["nested"];
    CHECK_TIMELINE(*h.state(), h.tl.seq, R"(
        V1 |Tttttttt|
        A1 |Tttttttt|
    )");
    const auto opened = h.ok("clip.openNested", in(h, Json{{"clip", at("V1", 0)}}));
    CHECK(opened.data["sequence"] == nestedId);
    CHECK(opened.data["multicam"] == false);
    CHECK(opened.data["to"]["ticks"] == f(8).value);
    const auto details = h.ok("clip.details", in(h, Json{{"clip", at("V1", 0)}}));
    CHECK(details.data["media"].is_null());
    CHECK(details.data["nested"]["id"] == nestedId);

    h.ok("sequence.unnest", in(h, Json{{"clip", at("V1", 0)}}));
    CHECK_TIMELINE(*h.state(), h.tl.seq, R"(
        V1 |AaaaBbbb|
        A1 |AaaaBbbb|
    )");
    CHECK(h.session.undoCount() == 2);
    // Refusals carry the reason; a clip that is not nested cannot be unnested or opened.
    CHECK(h.errorOf("sequence.unnest", in(h, Json{{"clip", at("V1", 0)}})) == "EditRejected");
    CHECK(h.errorOf("clip.openNested", in(h, Json{{"clip", at("V1", 0)}})) == "EditRejected");
    while (h.session.canUndo()) {
        h.session.undo();
    }
    CHECK(*h.state() == *original);
}

TEST_CASE("A sequence edited into another one is a nested clip; never into itself", "[command][nesting]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa........|
        A1 tgt |Aaaa........|
    )");
    const auto nested = h.ok("sequence.nest", Json{{"clips", Json::array({at("V1", 0), at("A1", 0)})}, {"name", "Nest"}});
    const std::uint64_t nestedId = nested.data["nested"];
    h.ok("sequence.overwrite", in(h, Json{{"sourceSequence", nestedId}, {"at", tc(6)}, {"sourceIn", tc(1)}}));
    CHECK_TIMELINE(*h.state(), h.tl.seq, R"(
        V1 |Nnnn..N1n...|
        A1 |Nnnn..N1n...|
    )");
    CHECK(h.errorOf("sequence.overwrite", Json{{"sequence", nestedId}, {"sourceSequence", h.tl.seq.value}, {"at", 0}}) ==
          "EditRejected");
    CHECK(h.errorOf("sequence.insert", in(h, Json{{"at", 0}})) == "InvalidParams"); // neither media nor a sequence
}

TEST_CASE("multicam.create syncs the angles; switchAngle records a live edit as one undo step", "[command][multicam]") {
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |....................|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    auto sync = std::make_shared<command::MulticamServices>(
        std::make_shared<const OffsetTableAudioSync>(std::vector<std::pair<MediaId, Ticks>>{{a, Ticks{}}, {b, f(2)}}));
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024, sync});
    CHECK(h.errorOf("multicam.create", Json{{"media", Json::array({"A", "B"})}}) == "EditRejected"); // no timecode
    const auto made = h.ok("multicam.create", Json{{"media", Json::array({"A", "B"})}, {"sync", "audio"}, {"name", "Cam"}});
    const std::uint64_t source = made.data["multicam"];
    CHECK(made.data["angles"][1]["offset"]["ticks"] == f(2).value);
    CHECK(made.data["angles"][0]["video"] == "V1");
    CHECK(h.ok("multicam.angles", in(h, Json{{"multicam", source}})).data["sync"] == "audio");

    h.ok("sequence.overwrite", in(h, Json{{"sourceSequence", source}, {"at", 0}, {"sourceIn", tc(2)}, {"sourceOut", tc(22)}}));
    CHECK(anglesOf(h, "V1", 0) == "[1]");
    const std::size_t before = h.session.undoCount();
    const auto live = h.ok("multicam.switchAngle",
                           in(h, Json{{"clip", at("V1", 0)},
                                      {"target", "video"},
                                      {"switches", Json::array({Json{{"at", tc(10)}, {"angle", 1}},
                                                                Json{{"at", tc(4)}, {"angle", 2}}})}}));
    CHECK(live.data["switches"] == 2);
    CHECK(h.session.undoCount() == before + 1);
    CHECK(anglesOf(h, "V1", 0) == "[1]");
    CHECK(anglesOf(h, "V1", 4) == "[2]");
    CHECK(anglesOf(h, "V1", 10) == "[1]");
    CHECK(anglesOf(h, "A1", 12) == "[1]"); // the sound did not switch
    CHECK(h.errorOf("multicam.switchAngle", in(h, Json{{"clip", at("V1", 0)}, {"angle", 3}})) == "EditRejected");
    CHECK(h.errorOf("multicam.switchAngle", in(h, Json{{"clip", at("V1", 0)}})) == "InvalidParams");

    // Sync, then flatten the angle 2 piece into B's own clip.
    h.ok("multicam.setSync", in(h, Json{{"multicam", source}, {"angle", 2}, {"offset", tc(3)}}));
    CHECK(h.ok("multicam.angles", in(h, Json{{"multicam", source}})).data["sync"] == "manual");
    h.ok("multicam.flatten", in(h, Json{{"clip", at("V1", 4)}}));
    const auto flat = h.ok("clip.details", in(h, Json{{"clip", at("V1", 4)}}));
    CHECK(flat.data["media"]["name"] == "B");
    CHECK(flat.data["sourceIn"]["ticks"] == f(3).value); // multicam 6 is B's frame 3 after the sync change
    CHECK(h.errorOf("multicam.setSync", in(h, Json{{"angle", 1}, {"offset", 0}})) == "EditRejected"); // not a source
}
