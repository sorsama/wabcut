// AI-15: tool routing for small contexts and constrained tool calls (the envelope, its parser, the wrapper's retry).
#include "AgentTestUtil.h"

#include <wabcut/ai/ToolSchema.h>
#include <wabcut/ai/agent/ConstrainedTools.h>
#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/agent/ToolRouter.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/ToolExport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;

namespace {

std::vector<ToolSpec> catalogTools() {
    agent::AgentOptions options;
    options.allowed = command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence});
    return EditAgent(options).tools();
}

std::vector<std::string> routedNames(std::string_view prompt, std::size_t budget = 12) {
    RouteOptions options;
    options.maxTools = budget;
    std::vector<std::string> out;
    for (const RoutedTool& t : rankTools(catalogTools(), ai::edit::editingCommands(), prompt, options)) {
        out.push_back(t.name);
    }
    return out;
}

bool has(const std::vector<std::string>& names, std::string_view name) { return std::ranges::find(names, name) != names.end(); }

const ToolSpec& tool(const std::vector<ToolSpec>& tools, std::string_view name) {
    for (const ToolSpec& t : tools) {
        if (t.name == name) {
            return t;
        }
    }
    FAIL("no tool " << name);
    return tools.front();
}

} // namespace

TEST_CASE("The tool router offers the tools a request is about within the budget", "[ai][agent][router]") {
    CHECK(toolBudgetFor(6144) == 8);
    CHECK(toolBudgetFor(8192) == 12);
    CHECK(toolBudgetFor(16384) == 16);
    CHECK(toolBudgetFor(200000) == 0);
    CHECK(toolBudgetFor(0) == 0);

    const auto markers = routedNames("Add a chapter marker called Part 2 at 6 seconds.", 8);
    CHECK(markers.size() <= 8);
    CHECK(markers.front() == "sequence_summary"); // always offered, first
    CHECK(has(markers, "marker_add"));
    CHECK(has(routedNames("Shorten the second clip by one second", 8), "clip_trim"));
    CHECK(has(routedNames("Close the gap between the first and second clips", 8), "timeline_rippleDelete"));
    CHECK(has(routedNames("Fix the typo in the third subtitle", 8), "captions_updateCue"));
    CHECK(has(routedNames("Make the music on A2 quieter by 6 dB", 8), "track_setVolume"));
    CHECK(has(routedNames("Cut to camera 3 at 00:00:05:00", 8), "multicam_switchAngle"));
    CHECK(has(routedNames("Make the second clip black and white", 8), "clip_addEffect"));
    CHECK(has(routedNames("Split all tracks at 00:00:03:00", 8), "sequence_addEditToAllTracks"));
    // Deterministic and stable: the same request routes the same way.
    CHECK(routedNames("Fade out the music over the last 2 seconds") == routedNames("Fade out the music over the last 2 seconds"));
    // A request about nothing in the catalog gets the core read only.
    CHECK(routedNames("xyzzy plugh") == std::vector<std::string>{"sequence_summary"});
    // routeTools keeps the catalog's order (stable prompts for provider caches).
    RouteOptions options;
    options.maxTools = 6;
    const auto routed = routeTools(catalogTools(), ai::edit::editingCommands(), "Add a marker and trim the clip", options);
    CHECK(routed.size() <= 6);
    CHECK(std::ranges::is_sorted(routed, [](const ToolSpec& a, const ToolSpec& b) { return a.name < b.name; }));
}

TEST_CASE("The constrained envelope is a portable schema with one branch per tool and the answer", "[ai][agent][constrained]") {
    const auto tools = routeTools(catalogTools(), ai::edit::editingCommands(), "Add a marker named Intro", {});
    const Json schema = toolEnvelopeSchema(tools);
    CHECK(schema["type"] == "object");
    REQUIRE(schema["anyOf"].size() == tools.size() + 1);
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const Json& branch = schema["anyOf"][i];
        CHECK(branch["properties"]["name"]["const"] == tools[i].name);
        CHECK(branch["required"] == Json::array({"name", "parameters"}));
        CHECK(branch["additionalProperties"] == false);
        CHECK_FALSE(branch["properties"]["parameters"].value("properties", Json::object()).contains("sequence"));
    }
    CHECK(schema["anyOf"].back()["required"] == Json::array({"answer"}));
    // The request a local server gets is valid in the portable subset.
    ChatRequest request;
    request.system = "rules";
    request.tools = tools;
    request.messages.push_back(ChatMessage::user("Add a marker named Intro at 00:00:00:05."));
    wabcut::ai::test::ScriptedModel scripted("qwen3-1.7b");
    ConstrainedToolChat wrapper(scripted);
    const ChatRequest prepared = wrapper.prepare(request);
    CHECK(prepared.tools.empty());
    REQUIRE(prepared.responseFormat);
    CHECK(prepared.responseFormat->name == "tool_call");
    CHECK(validateChatRequest(prepared));
    CHECK(prepared.system.starts_with("rules\n\n# Tools\n"));
    CHECK(prepared.system.find("marker_add: Adds a sequence marker.") != std::string::npos);
    CHECK(prepared.system.find("  time* (int|string): Where the marker starts.") != std::string::npos);
    CHECK(prepared.system.find("{\"name\": \"<tool name>\", \"parameters\"") != std::string::npos);
    // Required choices drop the answer; None keeps only it; Named keeps one tool.
    ToolChoice required;
    required.mode = ToolChoiceMode::Required;
    CHECK(toolEnvelopeSchema(tools, required)["anyOf"].size() == tools.size());
    ToolChoice none;
    none.mode = ToolChoiceMode::None;
    CHECK(toolEnvelopeSchema(tools, none)["anyOf"].size() == 1);
    ToolChoice named;
    named.mode = ToolChoiceMode::Named;
    named.name = "marker_add";
    CHECK(toolEnvelopeSchema(tools, named)["anyOf"].size() == 1);
    // Signatures: required parameters first and starred, enums spelled out, no "sequence".
    const std::string signature = toolSignature(tool(catalogTools(), "marker_add"));
    CHECK(signature.find("type (comment|chapter|segmentation|webLink)") != std::string::npos);
    CHECK(signature.find("colorIndex (int 0..15)") != std::string::npos);
    CHECK(signature.find("\n  sequence") == std::string::npos); // no "sequence" parameter (descriptions may say the word)
    CHECK(signature.find("  time*") < signature.find("  name"));
}

TEST_CASE("Envelope replies parse and near misses are repaired and broken replies say why", "[ai][agent][constrained]") {
    const std::vector<ToolSpec> tools{{"marker_add", "", Json{{"type", "object"}}, true}, {"clip_trim", "", Json{{"type", "object"}}, true}};
    auto call = [&](std::string_view text) { return parseEnvelopeReply(text, tools); };

    auto clean = call(R"({"name": "marker_add", "parameters": {"time": "00:00:01:00"}})");
    REQUIRE(clean.call);
    CHECK(clean.call->name == "marker_add");
    CHECK(clean.call->arguments == Json{{"time", "00:00:01:00"}});
    CHECK_FALSE(clean.repaired);

    auto answer = call(R"({"answer": "Added the marker."})");
    REQUIRE(answer.answer);
    CHECK(*answer.answer == "Added the marker.");
    CHECK_FALSE(answer.repaired);

    for (const std::string_view nearMiss : {
             "```json\n{\"name\": \"marker_add\", \"parameters\": {\"time\": 5}}\n```",
             "Sure! {\"tool\": \"marker_add\", \"arguments\": {\"time\": 5}} Let me know.",
             R"({"name": "marker_add", "parameters": "{\"time\": 5}"})",
             R"({"name": "marker.add", "parameters": {"time": 5}})",
             R"({"name": "Marker_Add", "arguments": {"time": 5}})",
             R"({"function": {"name": "marker_add", "arguments": "{\"time\": 5}"}})",
             R"({"tool_calls": [{"type": "function", "function": {"name": "marker_add", "arguments": {"time": 5}}}]})",
             R"(<think>The user wants a marker.</think>{"name": "marker_add", "parameters": {"time": 5}})",
             R"({"name": "marker_add", "parameters": {"time": 5})", // cut off before the last brace
         }) {
        INFO(nearMiss);
        const auto parsed = call(nearMiss);
        REQUIRE(parsed.call);
        CHECK(parsed.call->name == "marker_add");
        CHECK(parsed.call->arguments == Json{{"time", 5}});
        CHECK(parsed.repaired);
    }
    // No parameters at all: an empty object (the agent's schema check reports the missing ones).
    auto bare = call(R"({"name": "clip_trim"})");
    REQUIRE(bare.call);
    CHECK(bare.call->arguments == Json::object());
    // Plain words are an answer.
    auto words = call("I added the marker at one second.");
    REQUIRE(words.answer);
    CHECK(words.repaired);

    for (const std::string_view broken : {"", "{\"name\": \"marker_add\", \"parameters\": {\"time\": \"00:0", "{\"nam: 1}",
                                           "{\"parameters\": {}}", "{\"name\": \"marker_add\", \"parameters\": [1, 2]}",
                                           "{\"name\": \"marker_add\", \"parameters\": \"not json\"}"}) {
        INFO(broken);
        const auto parsed = call(broken);
        CHECK_FALSE(parsed.call);
        CHECK_FALSE(parsed.answer);
        CHECK_FALSE(parsed.error.empty());
    }
}

TEST_CASE("The constrained wrapper asks once more after a malformed reply and counts what it saw", "[ai][agent][constrained]") {
    wabcut::ai::test::ScriptedModel scripted("qwen3-1.7b");
    const auto text = [](std::string body) {
        ChatResponse r;
        r.message = ChatMessage::assistant(std::move(body));
        r.usage.inputTokens = 900;
        r.usage.outputTokens = 20;
        return r;
    };
    scripted.turn(text("{\"name\": \"marker_add\", \"parameters\": {\"time\": \"00:0"));           // cut inside a string
    scripted.turn(text("{\"name\": \"marker_add\", \"parameters\": {\"time\": \"00:00:00:05\"}}")); // the retry
    scripted.turn(text("```json\n{\"answer\": \"Added.\"}\n```"));
    ConstrainedToolChat wrapper(scripted);

    ChatRequest request;
    request.tools = {{"marker_add", "Adds a marker.", Json{{"type", "object"}, {"properties", {{"time", {{"type", "string"}}}}}}, true}};
    request.messages.push_back(ChatMessage::user("Add a marker at 5 frames."));
    std::string streamed;
    const auto onEvent = [&](const ChatEvent& e) {
        if (const auto* d = std::get_if<TextDelta>(&e)) {
            streamed += d->text;
        }
    };
    auto first = wrapper.chat(request, onEvent, {});
    REQUIRE(first);
    REQUIRE(first->message.toolCalls().size() == 1);
    CHECK(first->stopReason == StopReason::ToolUse);
    CHECK(first->message.toolCalls()[0].arguments == Json{{"time", "00:00:00:05"}});
    CHECK(first->usage.inputTokens == 1800); // both calls
    CHECK(streamed.empty());                 // JSON never reaches the panel
    const auto requests = scripted.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].messages.back().text().find("Reply again with exactly one JSON object") != std::string::npos);

    request.messages.push_back(first->message);
    request.messages.push_back(ChatMessage::toolResults({{first->message.toolCalls()[0].id, "marker_add", "{\"ok\":true}", false}}));
    auto second = wrapper.chat(request, onEvent, {});
    REQUIRE(second);
    CHECK(second->message.toolCalls().empty());
    CHECK(second->message.text() == "Added.");
    CHECK(streamed == "Added.");
    // The history reached the model as envelope turns.
    const std::vector<ChatRequest> all = scripted.requests(); // a copy: keep it alive while `last` refers into it
    const ChatRequest& last = all.back();
    REQUIRE(last.messages.size() == 3);
    CHECK(last.messages[1].text() == R"({"name":"marker_add","parameters":{"time":"00:00:00:05"}})");
    CHECK(last.messages[2].text() == R"(Result of marker_add: {"ok":true})");

    const ConstrainedStats stats = wrapper.stats();
    CHECK(stats.replies == 3);
    CHECK(stats.calls == 1);
    CHECK(stats.answers == 1);
    CHECK(stats.malformed == 1);
    CHECK(stats.repaired == 1); // the fenced answer
}
