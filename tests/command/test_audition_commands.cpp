// 0.6 (card #74): the audition.* commands through the engine - create, describe, use a take, add clips and a source,
// rename, move, duplicate, remove and flatten, each one undo step named after the command, with clear refusals.

#include "CommandHarness.h"

#include <wabcut/model/Auditions.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

} // namespace

TEST_CASE("audition commands through the engine: one undo step each with clear refusals", "[command][auditions]") {
    Timeline tl(R"(
        V3 |C2cccc....|
        V2 |B5bbbb....|
        V1 |Aaaaaa..Ee|
    )");
    CommandHarness h(std::move(tl));
    const ProjectSnapshot original = h.state();

    const auto made = h.ok("audition.create", Json{{"clips", Json::array({at("V1", 0), at("V2", 0), at("V3", 0)})}});
    CHECK(made.data["count"] == 3);
    CHECK(made.data["active"] == 1);
    CHECK(h.session.undoLabel() == std::optional<std::string>("New Audition"));
    CHECK_SESSION(h, R"(
        V3 |..........|
        V2 |..........|
        V1 |Aaaaaa..Ee|
    )");
    const std::uint64_t audition = made.data["clip"];

    const auto described = h.ok("audition.describe", Json{{"clip", audition}});
    REQUIRE(described.data["takes"].size() == 3);
    CHECK(described.data["takes"][1]["name"] == "B");
    CHECK(described.data["takes"][1]["sourceIn"] == f(5).value);
    CHECK(described.data["takes"][0]["active"] == true);
    CHECK(described.data["duration"]["ticks"] == f(6).value);

    h.ok("audition.setActive", Json{{"clip", audition}, {"take", 3}});
    CHECK(h.session.undoLabel() == std::optional<std::string>("Use Take"));
    CHECK_SESSION(h, R"(
        V3 |..........|
        V2 |..........|
        V1 |C2cccc..Ee|
    )");
    h.ok("audition.renameTake", Json{{"clip", audition}, {"take", 3}, {"name", "Wide"}});
    h.ok("audition.moveTake", Json{{"clip", audition}, {"take", 3}, {"to", 1}});
    const auto moved = h.ok("audition.describe", Json{{"clip", audition}});
    CHECK(moved.data["takes"][0]["name"] == "Wide");
    CHECK(moved.data["active"] == 1);

    // A source as a take: E from frame 1.
    h.ok("audition.addSource", Json{{"clip", audition}, {"media", h.tl.media('E').value}, {"in", tc(1)}, {"name", "Insert"}});
    CHECK(h.ok("audition.describe", Json{{"clip", audition}}).data["count"] == 4);
    // Add to Audition: the E clip on V1 joins and leaves the timeline.
    h.ok("audition.addTakes", Json{{"clip", audition}, {"clips", Json::array({at("V1", 8)})}});
    CHECK_SESSION(h, R"(
        V3 |..........|
        V2 |..........|
        V1 |C2cccc....|
    )");
    CHECK(h.ok("audition.describe", Json{{"clip", audition}}).data["count"] == 5);
    // Duplicate the active take: the copy is active.
    const auto copied = h.ok("audition.duplicateTake", Json{{"clip", audition}});
    CHECK(copied.data["count"] == 6);
    CHECK(copied.data["active"] == 2);
    CHECK(h.ok("audition.describe", Json{{"clip", audition}}).data["takes"][1]["name"] == "Wide copy");
    h.ok("audition.removeTake", Json{{"clip", audition}, {"take", 6}});
    CHECK(h.ok("audition.describe", Json{{"clip", audition}}).data["count"] == 5);

    // Refusals.
    CHECK(h.errorOf("audition.setActive", Json{{"clip", audition}, {"take", 9}}) == "EditRejected");
    CHECK(h.errorOf("audition.setActive", Json{{"clip", audition}, {"take", 0}}) == "InvalidParams");
    CHECK(h.errorOf("audition.renameTake", Json{{"clip", audition}, {"take", 1}, {"name", ""}}) == "InvalidParams");
    CHECK(h.errorOf("audition.addSource", Json{{"clip", audition}}) == "InvalidParams");

    h.ok("audition.flatten", Json{{"clips", Json::array({audition})}});
    CHECK(h.session.undoLabel() == std::optional<std::string>("Flatten Audition"));
    CHECK(h.ok("audition.describe", Json{{"clip", audition}}).data["audition"] == false);
    CHECK(h.errorOf("audition.flatten", Json{{"clips", Json::array({audition})}}) == "EditRejected");

    while (h.session.canUndo()) {
        h.session.undo();
    }
    CHECK(*h.state() == *original);
}
