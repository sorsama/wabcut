// Review wave 5, S5-P1-1: the editing assistant never reads or writes files a model picks. A file command it calls
// (cdl.import, lut.import, ...: CommandSpec::touchesFiles) does not run in the staged preview; it is put aside as
// "needs your confirmation" for the user to run, and accepting the staged edit never runs it.

#include "AgentTestUtil.h"
#include "CommandHarness.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using ai::test::ScriptedModel;
using command::Json;
using wabcut::test::Timeline;
namespace fs = std::filesystem;

TEST_CASE("The assistant does not run file commands: they wait for the user's confirmation (S5-P1-1)", "[ai][agent][security]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "agent-files";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path cdl = dir / "grade.cc";
    {
        std::ofstream out(cdl, std::ios::binary);
        out << R"(<?xml version="1.0" encoding="UTF-8"?>
<ColorCorrection id="A"><SOPNode><Slope>1.2 1.0 0.9</Slope><Offset>0.01 0 0</Offset><Power>1 1 1</Power></SOPNode>
<SATNode><Saturation>1.1</Saturation></SATNode></ColorCorrection>
)";
    }
    EditSession session(tl.state);
    EditAgent agent{AgentOptions{}};
    ScriptedModel model;
    model.tool("cdl_import", Json{{"clip", tl.clip("V1@0").value}, {"path", pathToUtf8(cdl)}})
        .text("I applied the grade from grade.cc.");
    AgentRequest request;
    request.prompt = "Apply the grade in grade.cc to the first clip";
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(session.stateForSave());
    request.baseRevision = session.revision();
    const AgentOutcome out = agent.run(model, request, {}, {});
    INFO(out.error);
    // Nothing was read into the staged project: the preview holds no edit from the file.
    CHECK(out.batch.commands.empty());
    CHECK(out.changes.empty());
    CHECK(out.status == AgentStatus::NoChanges);
    // It waits for the user instead, with the path for them to see.
    REQUIRE(out.needsConfirmation.size() == 1);
    CHECK(out.needsConfirmation[0].command == "cdl.import");
    CHECK(out.needsConfirmation[0].params["path"] == pathToUtf8(cdl));
    CHECK(out.needsConfirmation[0].title.find("Import CDL") != std::string::npos);
    const auto noted = std::ranges::find_if(out.steps, [](const AgentStep& s) { return s.tool == "cdl.import"; });
    REQUIRE(noted != out.steps.end());
    CHECK(noted->ok);
    CHECK(noted->detail.find("Needs your confirmation") != std::string::npos);
    // The tool stays offered (the model may propose it); it just never runs here.
    const auto tools = agent.tools();
    CHECK(std::ranges::any_of(tools, [](const ToolSpec& t) { return t.name == "cdl_import"; }));
}
