// review.importComments (0.7, card 035): the comments a reviewer exported from a review link's page become review notes through the
// command engine - one undo step, nothing twice, nothing for another link - and the generic dry run changes nothing.

#include "CommandHarness.h"

#include <wabcut/model/ReviewNotes.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

std::string row300() { return "A" + std::string(299, 'a'); }

std::string commentsText(const std::string& link) {
    return Json{{"format", "wabcut-review-comments"},
                {"version", 1},
                {"link", link},
                {"reviewer", "Maria"},
                {"comments", Json::array({Json{{"frame", 10}, {"text", "The logo is too small"},
                                               {"mark", Json{{"x", 0.7}, {"y", 0.05}, {"width", 0.2}, {"height", 0.1}}}},
                                          Json{{"frame", 50}, {"text", "Cut this shorter"}}})}}
        .dump();
}

} // namespace

TEST_CASE("review.importComments adds the comments as review notes in one undo step", "[command][review][cloud]") {
    CommandHarness h("V1 tgt |" + row300() + "|\nA1 tgt |" + row300() + "|");
    const std::size_t undoBefore = h.session.undoCount();
    const auto result = h.ok("review.importComments", Json{{"text", commentsText("lid")}, {"link", "lid"}, {"start", "00:00:04:00"}});
    CHECK(result.data["added"] == 2);
    CHECK(result.data["reviewer"] == "Maria");
    REQUIRE(h.state()->reviewNotes.size() == 2);
    CHECK(h.state()->reviewNotes[0].author == "Maria (review link)");
    CHECK(h.state()->reviewNotes[0].start == framesToTicks(100 + 10, kRate25)); // 4 s at 25 fps, plus frame 10
    CHECK(h.state()->reviewNotes[0].mark.has_value());
    CHECK(h.session.undoCount() == undoBefore + 1);

    // the same file again changes nothing
    const auto again = h.ok("review.importComments", Json{{"text", commentsText("lid")}, {"link", "lid"}, {"start", "00:00:04:00"}});
    CHECK(again.data["added"] == 0);
    CHECK(again.data["unchanged"] == 2);
    CHECK(h.state()->reviewNotes.size() == 2);

    // undo takes both away
    REQUIRE(h.session.undo().has_value());
    CHECK(h.state()->reviewNotes.size() == 0);
}

TEST_CASE("review.importComments can also make markers limit the range and refuses another link's file", "[command][review][cloud]") {
    CommandHarness h("V1 tgt |" + row300() + "|\nA1 tgt |" + row300() + "|");
    const auto withMarkers = h.ok("review.importComments", Json{{"text", commentsText("lid")}, {"link", "lid"}, {"markers", true}});
    CHECK(withMarkers.data["markers"] == 2);
    CHECK(h.sequence(h.state()).markers.size() == 2);
    CHECK(h.errorOf("review.importComments", Json{{"text", commentsText("another")}, {"link", "lid"}}) != "");

    CommandHarness limited("V1 tgt |" + row300() + "|\nA1 tgt |" + row300() + "|");
    const auto cut = limited.ok("review.importComments", Json{{"text", commentsText("lid")}, {"link", "lid"}, {"duration", "00:00:01:00"}});
    CHECK(cut.data["added"] == 1); // frame 50 is past a one-second copy
    CHECK(cut.data["conflicts"].size() == 1);
}

TEST_CASE("review.importComments reads a file refuses what is not a comments file and a dry run changes nothing", "[command][review][cloud]") {
    CommandHarness h("V1 tgt |" + row300() + "|\nA1 tgt |" + row300() + "|");
    const fs::path file = fs::temp_directory_path() / "wabcut-review-comments-command.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << commentsText("lid");
    }
    const std::string path = file.string();
    const auto dry = h.run(Json{{"command", "review.importComments"}, {"params", Json{{"path", path}, {"link", "lid"}}}, {"dryRun", true}});
    CHECK(dry.ok());
    CHECK(h.state()->reviewNotes.size() == 0);
    h.ok("review.importComments", Json{{"path", path}, {"link", "lid"}});
    CHECK(h.state()->reviewNotes.size() == 2);

    CHECK(h.errorOf("review.importComments", Json{{"link", "lid"}}) == "InvalidParams"); // neither path nor text
    CHECK(h.errorOf("review.importComments", Json{{"path", "Z:/no/such/file.json"}, {"link", "lid"}}) != "");
    CHECK(h.errorOf("review.importComments", Json{{"text", "{}"}, {"link", "lid"}}) != "");
    CHECK(h.errorOf("review.importComments", Json{{"text", commentsText("lid")}}) == "InvalidParams"); // the link id is required
    std::error_code ec;
    fs::remove(file, ec);
}
