// C.13: the editing assistant adds motion graphics from templates as staged edits - "add a lower third for each speaker
// with their name" is template.list, then template.instantiate per speaker with the name field, then (optionally)
// template.setField. Saving templates, the brand kit and Lottie import stay out of its reach.

#include "AgentTestUtil.h"
#include "CommandHarness.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/GraphicOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using ai::test::ScriptedModel;
using command::Json;
using wabcut::test::f;
using wabcut::test::Timeline;

namespace {

/// A lower-third template: a box behind a name, with a "name" field.
GraphicTemplate lowerThird() {
    Graphic g;
    GraphicLayer box;
    box.id = 1;
    box.kind = LayerKind::Shape;
    box.shape.width = ParamValue(500.0);
    box.shape.height = ParamValue(100.0);
    box.transform.x = ParamValue(400.0);
    box.transform.y = ParamValue(900.0);
    GraphicLayer name;
    name.id = 2;
    name.kind = LayerKind::Text;
    name.text.runs = {TextRun{"Name", TextStyle{}}};
    name.transform.x = ParamValue(180.0);
    name.transform.y = ParamValue(860.0);
    g.layers = {box, name};
    auto t = edit::makeTemplate(g, "lower-third", "Lower third", {TemplateField{"name", "Name", FieldKind::Text, 2, "text", {}}}, f(8));
    REQUIRE(t);
    return *t;
}

} // namespace

TEST_CASE("The assistant adds a lower third for each speaker from a template as staged edits (C.13)", "[ai][agent][graphics]") {
    Timeline tl(R"(
        V2     |....................|
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    auto saved = edit::saveProjectTemplate(tl.state, lowerThird());
    REQUIRE(saved);
    EditSession session(saved->state);
    EditAgent agent{AgentOptions{}};
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& toolName) {
        return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == toolName; });
    };
    for (const char* name : {"template_list", "template_instantiate", "template_setField", "graphic_add", "graphic_get",
                             "graphic_updateLayer", "graphic_applyPreset"}) {
        INFO(name);
        CHECK(offered(name));
    }
    for (const char* name : {"template_save", "brand_set", "lottie_import"}) { // project-level, or files
        INFO(name);
        CHECK_FALSE(offered(name));
    }

    ScriptedModel model;
    model.tool("template_list", Json::object())
        .tool("template_instantiate", Json{{"template", "lower-third"}, {"at", "00:00:00:00"}, {"track", "V2"},
                                           {"values", Json{{"name", "Somchai"}}}})
        .tool("template_instantiate", Json{{"template", "lower-third"}, {"at", "00:00:00:10"}, {"track", "V2"},
                                           {"values", Json{{"name", "Malee"}}}})
        .text("Each speaker has a lower third with their name when they first speak.");
    AgentRequest request;
    request.prompt = "Add a lower third for each speaker with their name";
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(session.stateForSave());
    request.baseRevision = session.revision();
    const auto before = session.snapshot();
    const AgentOutcome out = agent.run(model, request, {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed()); // the gap between the two lower thirds is fine: overlays come and go
    REQUIRE(out.batch.commands.size() == 2);
    CHECK(out.batch.commands[0].command == "template.instantiate");
    CHECK(session.snapshot() == before); // staged only

    const ApplyResult applied = applyStaged(session, out);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    const Sequence& seq = *session.snapshot()->findSequence(tl.seq);
    const Track& v2 = *seq.videoTracks.at(1);
    REQUIRE(v2.clips.size() == 2);
    std::vector<std::string> names;
    for (const Clip& c : v2.clips) {
        REQUIRE(c.graphic());
        REQUIRE(c.graphic()->instance);
        CHECK(c.graphic()->instance->templateId == "lower-third");
        names.push_back(c.graphic()->findLayer(2)->text.text());
        CHECK(c.duration() == f(8));
    }
    CHECK(names == std::vector<std::string>{"Somchai", "Malee"});
    REQUIRE(session.undo()); // one undo step
    CHECK(*session.snapshot() == *before);
}
