#pragma once

// Recorded-fixture helpers for the wire-adapter contract tests (owner: AI platform engineer).
//
// Fixtures live in tests/ai/fixtures/<provider>/:
//   *.sse   a streamed response body, byte for byte as the provider sends it
//   *.json  a buffered response body, or an expected request body ("request snapshot")
// The responses follow the vendors' documented examples (field names verified against their API references on
// 2026-10-11); no fixture contains a real key. Request snapshots are compared as parsed JSON. Set
// WABCUT_UPDATE_FIXTURES=1 to rewrite snapshots after an intended change, then review the diff.

#include "FakeTransport.h"

#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Secrets.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

namespace wabcut::ai::test {

inline std::string fixturePath(const std::string& name) { return std::string(WABCUT_AI_FIXTURES) + "/" + name; }

inline std::string fixture(const std::string& name) {
    const std::string content = readFile(fixturePath(name));
    INFO("fixture " << name);
    REQUIRE_FALSE(content.empty());
    return content;
}

inline Json fixtureJson(const std::string& name) {
    auto j = Json::parse(fixture(name), nullptr, false);
    INFO("fixture " << name);
    REQUIRE_FALSE(j.is_discarded());
    return j;
}

/// Compares `actual` with the snapshot `name` (or rewrites it when WABCUT_UPDATE_FIXTURES is set).
inline void checkSnapshot(const std::string& name, const Json& actual) {
    if (systemEnv("WABCUT_UPDATE_FIXTURES")) {
        std::ofstream(fixturePath(name), std::ios::binary | std::ios::trunc) << actual.dump(2) << '\n';
    }
    const Json expected = fixtureJson(name);
    INFO("snapshot " << name << "\nexpected: " << expected.dump(2) << "\nactual:   " << actual.dump(2));
    CHECK(actual == expected);
}

inline ScriptedResponse sse(const std::string& name, std::size_t chunkSize = 7) {
    ScriptedResponse r;
    r.status = 200;
    r.headers = {{"content-type", "text/event-stream"}};
    r.body = fixture(name);
    r.chunkSize = chunkSize;
    return r;
}

inline ScriptedResponse json(int status, const std::string& name, HttpHeaders headers = {}) {
    ScriptedResponse r;
    r.status = status;
    r.headers = std::move(headers);
    r.headers.push_back({"content-type", "application/json"});
    r.body = fixture(name);
    return r;
}

/// The built-in registry's entries for `modelId`, with a fake key.
inline ProviderContext context(const std::string& modelId, IHttpTransport& transport, std::string key = "test-key-0123456789") {
    const ModelRegistry& registry = ModelRegistry::builtin();
    const ModelSpec* model = registry.model(modelId);
    REQUIRE(model != nullptr);
    const ProviderSpec* provider = registry.provider(model->provider);
    REQUIRE(provider != nullptr);
    return ProviderContext{*provider, *model, provider->baseUrl, Secret(std::move(key)), transport};
}

inline Json requestBody(const HttpRequest& request) {
    auto j = Json::parse(request.body, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    return j;
}

/// Events seen by a ChatEventHandler, flattened for assertions.
struct EventLog {
    std::string text;
    std::string arguments;
    std::string reasoning;
    std::vector<std::string> toolsStarted;
    int usageUpdates = 0;
    ChatEventHandler handler() {
        return [this](const ChatEvent& e) {
            if (const auto* t = std::get_if<TextDelta>(&e)) {
                text += t->text;
            } else if (const auto* s = std::get_if<ToolCallStarted>(&e)) {
                toolsStarted.push_back(s->name);
            } else if (const auto* a = std::get_if<ToolCallArgumentsDelta>(&e)) {
                arguments += a->text;
            } else if (const auto* r = std::get_if<ReasoningDelta>(&e)) {
                reasoning += r->text;
            } else if (std::holds_alternative<UsageUpdate>(e)) {
                ++usageUpdates;
            }
        };
    }
};

/// A request with the usual agent shape: system prompt, a user turn with an image, an assistant tool call, its result
/// and a follow-up question, plus one strict tool with an optional bounded parameter.
inline ChatRequest agentRequest() {
    ChatRequest r;
    r.system = "You edit video timelines. Use the tools.";
    ChatMessage user = ChatMessage::user("Remove the silence on A1 after 00:00:12.");
    user.parts.push_back(ImagePart{"image/png", {0x89, 0x50, 0x4E, 0x47}, ""});
    r.messages.push_back(user);
    ChatMessage assistant;
    assistant.role = Role::Assistant;
    assistant.parts.push_back(TextPart{"Let me look at the sequence."});
    ToolCall call;
    call.id = "call_1";
    call.name = "sequence_summary";
    call.arguments = Json{{"track", "A1"}};
    call.argumentsText = R"({"track":"A1"})";
    assistant.parts.push_back(call);
    r.messages.push_back(assistant);
    r.messages.push_back(ChatMessage::toolResults({ToolResult{"call_1", "sequence_summary", R"({"clips":3})", false}}));
    ToolSpec tool;
    tool.name = "timeline_rippleDelete";
    tool.description = "Ripple-delete a range on a track.";
    tool.parameters = Json::parse(R"({"type":"object","properties":{
        "track":{"type":"string","description":"Track id"},
        "frames":{"type":"integer","minimum":1,"description":"Length"}},
      "required":["track"],"additionalProperties":false})");
    r.tools = {tool};
    r.toolChoice.mode = ToolChoiceMode::Required;
    r.maxOutputTokens = 1024;
    return r;
}

} // namespace wabcut::ai::test
