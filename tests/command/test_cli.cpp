// The wabcut-cli `cmd` and `tools` subcommands (as library functions).

#include "TimelineFixture.h"

#include <wabcut/command/Cli.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace wabcut;
using namespace wabcut::command;
using namespace wabcut::test;

namespace {

std::filesystem::path freshProject(const char* name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    for (int i = 0; i <= 5; ++i) {
        std::filesystem::remove(i == 0 ? file : io::backupPath(file, i));
    }
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    REQUIRE(io::saveProject(tl.linkMatching().state, file));
    return file;
}

const Clip* firstClip(const ProjectState& state) { return &state.sequences[0]->videoTracks[0]->clips[0]; }

} // namespace

TEST_CASE("cmd --dry-run reports without touching the file; --save commits it", "[command][cli]") {
    const auto file = freshProject("cli.wabcut");
    const std::string request =
        R"({"command": "timeline.rippleDelete", "params": {"clips": [{"track": "V1", "time": "00:00:00:01"}]}})";

    const CliOutcome dry = runCommandCli(file, request, true, true);
    INFO(dry.out << dry.err);
    REQUIRE(dry.exitCode == 0);
    const Json dryJson = Json::parse(dry.out);
    CHECK(dryJson["ok"] == true);
    CHECK(dryJson["dryRun"] == true);
    CHECK(dryJson["committed"] == false);
    CHECK(dryJson["saved"] == false);
    CHECK(dryJson["changes"]["sequences"][0]["removedClips"].size() == 2);
    CHECK(firstClip(*io::loadProject(file))->start() == Ticks{});

    const CliOutcome commit = runCommandCli(file, request, false, true);
    INFO(commit.out << commit.err);
    REQUIRE(commit.exitCode == 0);
    const Json commitJson = Json::parse(commit.out);
    CHECK(commitJson["committed"] == true);
    CHECK(commitJson["revision"] == 1);
    CHECK(commitJson["saved"] == true);
    const auto saved = io::loadProject(file);
    REQUIRE(saved);
    CHECK(firstClip(*saved)->name() == "B");
    CHECK(std::filesystem::exists(io::backupPath(file, 1)));

    // Without --save nothing is written.
    const CliOutcome unsaved =
        runCommandCli(file, R"({"command": "sequence.addEdit", "params": {"time": "00:00:00:03"}})", false, false);
    INFO(unsaved.out << unsaved.err);
    REQUIRE(unsaved.exitCode == 0);
    CHECK(Json::parse(unsaved.out)["committed"] == true);
    CHECK_FALSE(Json::parse(unsaved.out).contains("saved"));
    CHECK(firstClip(*io::loadProject(file))->duration() == f(4));
}

TEST_CASE("cmd runs batches and reports failures with exit codes", "[command][cli]") {
    const auto file = freshProject("cli-batch.wabcut");
    const CliOutcome batch = runCommandCli(
        file,
        R"({"commands": [{"command": "sequence.addEdit", "params": {"time": "00:00:00:02"}},
                         {"command": "sequence.summary"}], "dryRun": true})",
        false, false);
    INFO(batch.out << batch.err);
    REQUIRE(batch.exitCode == 0);
    CHECK(Json::parse(batch.out)["steps"].size() == 2);

    const CliOutcome failed = runCommandCli(file, R"({"command": "clip.rename", "params": {"clips": [999], "name": "x"}})",
                                            false, true);
    CHECK(failed.exitCode == 1);
    CHECK(Json::parse(failed.out)["error"]["code"] == "NotFound");

    CHECK(runCommandCli(file, "{not json", false, false).exitCode == 2);
    CHECK(runCommandCli(file.parent_path() / "missing.wabcut", R"({"command": "project.summary"})", false, false)
              .exitCode == 2);
}

TEST_CASE("tools prints definitions in the requested format", "[command][cli]") {
    const CliOutcome anthropic = runToolsCli("anthropic");
    REQUIRE(anthropic.exitCode == 0);
    CHECK(Json::parse(anthropic.out)[0].contains("input_schema"));
    const CliOutcome openai = runToolsCli("openai");
    REQUIRE(openai.exitCode == 0);
    CHECK(Json::parse(openai.out)[0]["type"] == "function");
    CHECK(runToolsCli("jsonschema").exitCode == 0);
    CHECK(runToolsCli("xml").exitCode == 2);
}
