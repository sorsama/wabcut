// C.9 commands through the engine: version history (history.*), review notes and packages (review.*) and the
// three-way project merge (project.merge).

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/ReviewPackage.h>
#include <wabcut/model/History.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

fs::path freshDir(const std::string& tag) {
    const fs::path p = fs::temp_directory_path() / ("wabcut-c9-commands-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool hasLine(const Json& lines, const std::string& text) {
    for (const Json& l : lines) {
        if (l.get<std::string>() == text) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaBbbb....|
    A1 tgt |AaaaBbbb....|
)";

/// A frame source like the renderer's: "PNG" bytes naming the time.
class FakeFrames final : public command::ReviewFrameSource {
public:
    std::expected<command::ReviewFrame, Error> frame(const ProjectState&, SequenceId, Ticks time, int) const override {
        return command::ReviewFrame{"image/png", "png@" + std::to_string(time.value)};
    }
};

} // namespace

TEST_CASE("history: versions saved, named, compared, restored and exported through the engine", "[command][history]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const fs::path dir = freshDir("history");
    auto opened = ProjectHistory::open(dir / "film.wabcut.history");
    REQUIRE(opened);
    auto history = std::make_shared<ProjectHistory>(std::move(*opened));
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024,
                                                          std::make_shared<command::HistoryServices>(history)});
    const ProjectSnapshot original = h.state();
    const std::uint64_t v1 = h.ok("history.checkpoint", Json{{"label", "First cut"}}).data["revision"][0];
    CHECK(h.session.undoCount() == 0); // the project did not change
    h.ok("clip.trim", Json{{"clip", at("V1", 4)}, {"edge", "tail"}, {"delta", "-00:00:00:02"}});
    h.ok("history.checkpoint");
    const auto list = h.ok("history.list");
    REQUIRE(list.data["revisions"].size() == 2);
    CHECK(list.data["revisions"][0][3] == "First cut");
    CHECK(list.data["revisions"][1][2] == "manual");
    CHECK(list.data["diskBytes"] > 0);
    const std::uint64_t v2 = list.data["revisions"][1][0];
    h.ok("history.name", Json{{"revision", v2}, {"label", "Client cut v2"}});
    CHECK(history->find(v2)->label == "Client cut v2");
    CHECK(h.session.undoCount() == 1); // naming is not an edit
    CHECK(h.errorOf("history.name", Json{{"revision", 99}, {"label", "x"}}) == "NotFound");

    const auto diff = h.ok("history.diff", Json{{"from", v1}});
    const std::string name = "'" + h.tl.sequence().name + "'";
    CHECK(hasLine(diff.data["summary"], name + ": V1 'B' trimmed 00:00:00:04-00:00:00:08 -> 00:00:00:04-00:00:00:06"));
    CHECK(diff.data["sequences"][0]["clipsChanged"] == 2);
    CHECK(h.ok("history.diff", Json{{"from", v1}, {"to", v1}}).data["summary"].empty());

    // The whole project, one undo step; the project as it was (edited since the last version) is saved first.
    h.ok("clip.trim", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"delta", "-00:00:00:01"}});
    h.ok("history.restore", Json{{"revision", v1}});
    CHECK_TIMELINE(*h.state(), h.tl.seq, kPicture);
    CHECK(history->revisions().size() == 3);
    CHECK(history->revisions().back().kind == HistoryKind::Restore);
    h.session.undo();
    CHECK(h.sequence(h.state()).findClip(h.clip("V1@4"))->clip().duration() == f(2));

    // One sequence of the version, as a new sequence; the media is the project's own.
    const std::size_t media = h.state()->media.size();
    const auto copy = h.ok("history.restore", Json{{"revision", v1}, {"from", h.tl.seq.value}, {"name", "Old cut"}});
    const SequenceId copied{copy.data["sequence"].get<std::uint64_t>()};
    CHECK(h.state()->findSequence(copied)->name == "Old cut");
    CHECK_TIMELINE(*h.state(), copied, kPicture);
    CHECK(h.state()->media.size() == media);

    // As a project of its own.
    const std::string file = pathToUtf8(dir / "first cut.wabcut");
    h.ok("history.export", Json{{"sequence", h.tl.seq.value}, {"revision", v1}, {"path", file}});
    const auto saved = io::loadProject(pathFromUtf8(file));
    REQUIRE(saved);
    CHECK(*saved == *original);
    CHECK(h.errorOf("history.export", Json{{"sequence", h.tl.seq.value}, {"revision", v1}, {"path", file}}) == "EditRejected");
    CHECK(h.errorOf("history.restore", Json{{"sequence", h.tl.seq.value}, {"revision", 99}}) == "NotFound");

    CommandHarness none(kPicture);
    CHECK(none.errorOf("history.list") == "InvalidRequest");
}

TEST_CASE("review: notes, threads, packages and markers through the engine", "[command][review]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )",
                     command::EnginePolicy{command::CapabilitySet::all(), 1024,
                                           std::make_shared<command::ReviewServices>(std::make_shared<FakeFrames>())});
    const std::uint64_t logo =
        h.ok("review.add", Json{{"time", tc(4)},
                                {"text", "Logo too small"},
                                {"author", "Client"},
                                {"mark", Json{{"x", 0.7}, {"y", 0.05}, {"width", 0.2}, {"height", 0.1}}}})
            .data["note"];
    const std::uint64_t pause =
        h.ok("review.add", Json{{"start", tc(10)}, {"end", tc(14)}, {"text", "Tighten this pause"}, {"author", "Client"}})
            .data["note"];
    h.ok("review.reply", Json{{"note", logo}, {"text", "Bigger in v3"}, {"author", "Editor"}});
    h.ok("review.resolve", Json{{"note", logo}});
    const auto list = h.ok("review.list");
    REQUIRE(list.data["notes"].size() == 2);
    CHECK(list.data["notes"][0]["start"] == tc(4));
    CHECK(list.data["notes"][0]["status"] == "resolved");
    CHECK(list.data["notes"][0]["replies"][0][1] == "Bigger in v3");
    CHECK(list.data["notes"][1]["end"] == tc(14));
    CHECK(h.ok("review.list", Json{{"status", "open"}}).data["notes"].size() == 1);
    CHECK(h.errorOf("review.add", Json{{"time", tc(4)}, {"text", "x"}, {"mark", Json{{"x", 0.9}, {"y", 0.9}, {"width", 0.5}}}}) ==
          "EditRejected");
    CHECK(h.errorOf("review.reply", Json{{"note", 999}, {"text", "x"}}) == "NotFound");
    const std::uint64_t typo = h.ok("review.add", Json{{"time", tc(2)}, {"text", "typo"}}).data["note"];
    h.ok("review.remove", Json{{"note", typo}});
    CHECK(h.state()->findReviewNote(ReviewNoteId{typo}) == nullptr);
    CHECK(h.errorOf("review.remove", Json{{"note", typo}}) == "NotFound");

    // A package: notes.json, one self-contained page, the thumbnails.
    const fs::path dir = freshDir("package") / "client cut.wabreview";
    const auto exported = h.ok("review.exportPackage", Json{{"path", pathToUtf8(dir)}, {"title", "Client cut v2"}});
    CHECK(exported.data["notes"] == 2);
    CHECK(exported.data["frames"] == 2);
    const std::string page = readText(dir / "index.html");
    CHECK(page.find("<title>Client cut v2</title>") != std::string::npos);
    CHECK(page.find("Logo too small") != std::string::npos);
    CHECK(page.find("data:image/png;base64,") != std::string::npos);
    CHECK(page.find("class=\"mark\"") != std::string::npos);
    CHECK(fs::exists(dir / "frames" / ("note-" + std::to_string(logo) + ".png")));

    // The reviewer replies, adds a note, changes one and adds one on a sequence this project does not have.
    Json pkg = Json::parse(readText(dir / "notes.json"));
    REQUIRE(pkg["notes"].size() == 2);
    pkg["notes"][1]["replies"].push_back(Json{{"author", "Client"}, {"text", "Better now"}, {"createdAt", ""}});
    pkg["notes"][1]["status"] = "resolved";
    Json added = pkg["notes"][1];
    added["id"] = 0;
    added["start"] = f(16).value;
    added["text"] = "Music too loud here";
    added["replies"] = Json::array();
    pkg["notes"].push_back(added);
    Json changed = pkg["notes"][0];
    changed["text"] = "Logo still too small";
    pkg["notes"][0] = changed;
    Json stray = added;
    stray["sequence"] = 424242;
    pkg["notes"].push_back(stray);
    std::ofstream(dir / "notes.json", std::ios::binary) << pkg.dump(2);
    const std::size_t before = h.session.undoCount();
    const auto imported = h.ok("review.importPackage", Json{{"path", pathToUtf8(dir)}});
    CHECK(imported.data["added"] == 1);
    CHECK(imported.data["updated"] == 1);
    CHECK(imported.data["conflicts"].size() == 2);
    CHECK(h.session.undoCount() == before + 1);
    const auto after = h.ok("review.list").data["notes"];
    REQUIRE(after.size() == 3);
    CHECK(after[1]["replies"].size() == 1);
    CHECK(after[1]["status"] == "resolved");
    CHECK(after[2]["text"] == "Music too loud here");
    CHECK(after[0]["text"] == "Logo too small"); // the project's text stayed (a conflict)
    // Importing again changes nothing.
    std::ofstream(dir / "notes.json", std::ios::binary) << pkg.dump(2);
    CHECK(h.ok("review.importPackage", Json{{"path", pathToUtf8(dir)}}).data["unchanged"] == 2);

    // Notes to markers.
    CHECK(h.ok("review.toMarkers", Json{{"remove", true}}).data["markers"] == 3);
    CHECK(h.ok("review.list").data["notes"].empty());
    CHECK(h.sequence(h.state()).markers[0].colorIndex == 0);
    (void)pause;

    // Without a renderer the package shows placeholder cards; a damaged package is refused.
    CommandHarness plain(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    plain.ok("review.add", Json{{"time", tc(2)}, {"text", "Check"}});
    const fs::path cards = freshDir("cards") / "p.wabreview";
    plain.ok("review.exportPackage", Json{{"path", pathToUtf8(cards)}});
    CHECK(readText(cards / "index.html").find("data:image/svg+xml;base64,") != std::string::npos);
    std::ofstream(cards / "notes.json") << "{ not json";
    CHECK(plain.errorOf("review.importPackage", Json{{"path", pathToUtf8(cards)}}) == "EditRejected");
}

TEST_CASE("project.merge stages conflicts, applies choices and merges disjoint edits", "[command][merge]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const fs::path dir = freshDir("merge");
    REQUIRE(io::saveProject(tl.state, dir / "base.wabcut"));
    // Theirs: B trimmed by 1 frame and a marker; mine (the harness): B trimmed by 2.
    ProjectState theirs = ok(edit::trimEdge(tl.state, tl.seq, tl.clip("V1@4"), edit::Edge::Tail, -f(1))).state;
    theirs = ok(edit::addMarker(theirs, tl.seq, Marker{f(9), Ticks{}, "Their marker"})).state;
    REQUIRE(io::saveProject(theirs, dir / "theirs.wabcut"));
    CommandHarness h(std::move(tl));
    h.ok("clip.trim", Json{{"clip", at("V1", 4)}, {"edge", "tail"}, {"delta", "-00:00:00:02"}});
    const ProjectSnapshot mine = h.state();
    const Json files{{"theirs", pathToUtf8(dir / "theirs.wabcut")}, {"base", pathToUtf8(dir / "base.wabcut")}};

    const auto staged = h.ok("project.merge", files);
    CHECK(staged.data["staged"] == true);
    REQUIRE(staged.data["conflicts"].size() == 2); // B on V1 and on A1
    CHECK(*h.state() == *mine);
    Json choices = Json::array();
    for (const Json& k : staged.data["conflicts"]) {
        choices.push_back(Json{{"conflict", k["id"]}, {"take", "theirs"}});
    }
    Json params = files;
    params["choices"] = choices;
    const auto applied = h.ok("project.merge", params);
    CHECK(applied.data["staged"] == false);
    CHECK(h.sequence(h.state()).findClip(h.clip("V1@4"))->clip().duration() == f(3));
    CHECK(h.sequence(h.state()).markers.size() == 1);
    h.session.undo();
    CHECK(*h.state() == *mine);
    // "apply" takes mine for the unresolved ones; their marker comes along.
    params = files;
    params["apply"] = true;
    h.ok("project.merge", params);
    CHECK(h.sequence(h.state()).findClip(h.clip("V1@4"))->clip().duration() == f(2));
    CHECK(h.sequence(h.state()).markers.size() == 1);
    CHECK(h.errorOf("project.merge", Json{{"theirs", "nope.wabcut"}, {"base", "nope.wabcut"}}) != "");
    Json wrong = files;
    wrong["choices"] = Json{{"clip:1", "theirs"}}; // a map is not a list of choices
    CHECK(h.errorOf("project.merge", wrong) == "InvalidParams");
}

TEST_CASE("review packages: a hostile notes.json and a read-only client (C.10)", "[command][review]") {
    Timeline tl(kPicture);
    // notes.json nested 100,000 deep is refused before it is parsed.
    const std::string deep = R"({"format": "wabcut-review", "version": 1, "notes": [], "x": )" + std::string(100'000, '[') +
                             std::string(100'000, ']') + "}";
    const auto refused = command::importReviewPackageData(tl.state, deep);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == wabcut::ErrorCode::ParseError);
    // Writing a package writes files: a client that may only read cannot.
    CommandHarness readOnly(kPicture, command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read}), 1024, nullptr});
    const fs::path folder = freshDir("read-only") / "package";
    CHECK(readOnly.errorOf("review.exportPackage", Json{{"path", pathToUtf8(folder)}}) == "PermissionDenied");
    CHECK_FALSE(fs::exists(folder));
}

TEST_CASE("history: a dry-run or failed restore saves no version (E5-P2-7)", "[command][history]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const fs::path dir = freshDir("history-dry");
    auto opened = ProjectHistory::open(dir / "film.wabcut.history");
    REQUIRE(opened);
    auto history = std::make_shared<ProjectHistory>(std::move(*opened));
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024,
                                                          std::make_shared<command::HistoryServices>(history)});
    const std::uint64_t v1 = h.ok("history.checkpoint").data["revision"][0];
    h.ok("clip.trim", Json{{"clip", at("V1", 4)}, {"edge", "tail"}, {"delta", "-00:00:00:02"}});
    REQUIRE(history->revisions().size() == 1);

    const auto preview = h.run(Json{{"command", "history.restore"}, {"params", {{"revision", v1}}}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(history->revisions().size() == 1); // a preview writes nothing to disk

    const auto failed = h.run(Json{{"commands", Json::array({
                                       Json{{"command", "history.restore"}, {"params", {{"revision", v1}}}},
                                       Json{{"command", "clip.trim"},
                                            {"params", {{"clip", 999'999}, {"edge", "tail"}, {"delta", "-00:00:00:01"}}}},
                                   })}});
    REQUIRE_FALSE(failed.ok());
    CHECK(history->revisions().size() == 1);

    // A real restore saves the project as it was first.
    h.ok("history.restore", Json{{"revision", v1}});
    REQUIRE(history->revisions().size() == 2);
    CHECK(history->revisions().back().kind == HistoryKind::Restore);
}
