// X.1 through the engine: interchange.export writes a timeline file other editors read and returns its report; a dry
// run writes nothing; the project does not change.

#include "CommandHarness.h"

#include <wabcut/interchange/Import.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaBbbbCccc........|
    A1 tgt |AaaaBbbbCccc........|
)";

fs::path freshDir(const std::string& tag) {
    const fs::path p = fs::temp_directory_path() / ("wabcut-x1-commands-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

} // namespace

TEST_CASE("interchange.export writes a timeline file and returns its report (X.1)", "[command][interchange]") {
    CommandHarness h(kPicture);
    h.tl.linkMatching();
    h.session.reset(h.tl.state);
    const fs::path dir = freshDir("export");
    for (const auto& [format, name] : {std::pair{"fcp7-xml", "cut.xml"}, std::pair{"fcpxml", "cut.fcpxml"},
                                       std::pair{"otio", "cut.otio"}, std::pair{"edl", "cut.edl"}}) {
        INFO(format);
        const std::string file = pathToUtf8(dir / name);
        // A dry run reports what would be written and writes nothing.
        const auto preview = h.run(Json{{"command", "interchange.export"}, {"params", {{"format", format}, {"path", file}}}, {"dryRun", true}});
        REQUIRE(preview.ok());
        CHECK(preview.data["written"] == false);
        CHECK(preview.data["bytes"].get<std::size_t>() > 0);
        CHECK_FALSE(fs::exists(pathFromUtf8(file)));

        const auto written = h.ok("interchange.export", Json{{"format", format}, {"path", file}});
        CHECK(written.data["written"] == true);
        CHECK(written.data["report"]["dropped"] == 0);
        CHECK(written.data["report"]["carried"].get<std::size_t>() > 0);
        REQUIRE(fs::exists(pathFromUtf8(file)));
        interchange::ImportOptions options;
        options.edlFrameRate = kRate;
        const auto back = interchange::ImportRouter::importFile(pathFromUtf8(file), options);
        INFO((back ? std::string() : back.error().message));
        REQUIRE(back.has_value());
        CHECK(back->project.findSequence(back->sequences.front())->videoTracks.front()->clips.size() == 3);
    }
    CHECK(h.session.undoCount() == 0); // the project did not change
    CHECK(h.errorOf("interchange.export", Json{{"format", "xml"}, {"path", pathToUtf8(dir / "x.xml")}}) == "InvalidParams");
    CHECK(h.errorOf("interchange.export", Json{{"format", "fcpxml"}, {"fcpxmlVersion", "1.9"}, {"path", pathToUtf8(dir / "x.fcpxml")}}) ==
          "InvalidParams");
}
