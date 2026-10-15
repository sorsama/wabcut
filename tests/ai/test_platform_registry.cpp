// AI platform: the shipped model registry (models.json), prices and costs, the portable tool-schema subset and its
// per-provider rewrites (checked against the real command catalog).

#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/ToolSchema.h>
#include <wabcut/command/Registry.h>
#include <wabcut/command/ToolExport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut::ai;
using namespace std::chrono;

namespace {

year_month_day ymd(int y, unsigned m, unsigned d) { return year_month_day{year(y), month(m), day(d)}; }

} // namespace

TEST_CASE("The shipped registry parses and covers the seeded providers and models", "[ai][platform][registry]") {
    REQUIRE(ModelRegistry::fromJson(ModelRegistry::builtinJson()));
    const ModelRegistry& r = ModelRegistry::builtin();
    CHECK(r.updated() == "2026-10-12");

    for (const char* id : {"claude-sonnet-5-5", "claude-opus-5-5", "claude-haiku-5-5", "claude-fable-5-1", "gpt-6.1-sol",
                           "gpt-6-astra", "gpt-6-luna", "gemini-3.8-flash", "nova-3", "scribe_v2", "universal-3-5-pro",
                           "voxtral-mini-latest"}) {
        INFO(id);
        const ModelSpec* m = r.model(id);
        REQUIRE(m != nullptr);
        CHECK(r.provider(m->provider) != nullptr);
        CHECK_FALSE(m->prices.empty());
        CHECK_FALSE(m->source.empty());
    }
    for (const char* id : {"anthropic", "openai", "gemini", "deepgram", "elevenlabs", "assemblyai", "mistral",
                           "openrouter", "groq", "ollama", "lmstudio", "llama-server"}) {
        INFO(id);
        const ProviderSpec* p = r.provider(id);
        REQUIRE(p != nullptr);
        CHECK_FALSE(p->keyCheck.path.empty());
        CHECK((p->local || !p->termsUrl.empty()));
        CHECK(p->envVar.starts_with("WABCUT_"));
        CHECK(p->baseUrl.starts_with(p->local ? "http://" : "https://"));
    }

    // Capabilities the adapters rely on.
    const ModelSpec& sonnet = *r.model("claude-sonnet-5-5");
    CHECK(sonnet.api == "anthropic-messages");
    CHECK(sonnet.caps.strictSchema);
    CHECK_FALSE(sonnet.caps.forcedToolChoice);
    CHECK(sonnet.caps.videoInput == VideoInput::Frames);
    const ModelSpec& sol = *r.model("gpt-6.1-sol");
    CHECK(sol.api == "openai-responses");
    CHECK(sol.caps.toolsOnResponses);
    CHECK_FALSE(sol.caps.toolsOnChat);
    CHECK(r.model("gemini-3.8-flash")->caps.videoInput == VideoInput::Native);
    const ModelSpec& nova = *r.model("nova-3");
    CHECK(nova.caps.has(Capability::Transcribe));
    CHECK(nova.caps.wordTimestamps);
    CHECK(nova.caps.diarization);
    CHECK(r.provider("llama-server")->baseUrl == "http://127.0.0.1:9931/v1");
    CHECK(findHeader(r.provider("anthropic")->extraHeaders, "anthropic-version") == "2023-06-01");

    CHECK(r.modelsWith(Capability::Transcribe).size() == 4);
    CHECK(r.modelsOf("anthropic").size() == 4);
    // AI-12: the voice models (ElevenLabs v4 / v3 / Flash v2.5, OpenAI gpt-4o-mini-tts, Gemini 3.8 Flash TTS).
    CHECK(r.modelsWith(Capability::Speak).size() == 5);
    CHECK(r.model("gpt-4o-mini-tts")->retirement == ymd(2027, 1, 6));
}

TEST_CASE("Unverified registry entries are listed", "[ai][platform][registry]") {
    const auto unverified = ModelRegistry::builtin().unverifiedEntries();
    CHECK(unverified == std::vector<std::string>{"model:voxtral-mini-latest"});
}

TEST_CASE("Costs follow the registry's price rules", "[ai][platform][registry][cost]") {
    const ModelRegistry& r = ModelRegistry::builtin();
    const auto today = ymd(2026, 10, 11);

    Usage oneMillionEach;
    oneMillionEach.inputTokens = 1'000'000;
    oneMillionEach.outputTokens = 1'000'000;
    CHECK(costOf(*r.model("claude-sonnet-5-5"), oneMillionEach, today) == 12 * kMicroUsdPerUsd);

    // Haiku 5.5: the long-prompt tier applies above 100K prompt tokens.
    Usage shortPrompt;
    shortPrompt.inputTokens = 100'000;
    shortPrompt.outputTokens = 1000;
    CHECK(costOf(*r.model("claude-haiku-5-5"), shortPrompt, today) == 10'000 + 500);
    Usage longPrompt = shortPrompt;
    longPrompt.inputTokens = 100'001;
    CHECK(costOf(*r.model("claude-haiku-5-5"), longPrompt, today) == 50'001 + 2'500);

    // Gemini 3.8 Flash doubles on 2027-01-01.
    const ModelSpec& flash = *r.model("gemini-3.8-flash");
    CHECK(costOf(flash, oneMillionEach, ymd(2026, 12, 31)) == 4'500'000);
    CHECK(costOf(flash, oneMillionEach, ymd(2027, 1, 1)) == 9'000'000);

    // Per-minute transcription, rounded up to whole micro-USD.
    Usage hour;
    hour.audioMilliseconds = 3'600'000;
    CHECK(costOf(*r.model("nova-3"), hour, today) == 258'000); // $0.0043 x 60
    Usage second;
    second.audioMilliseconds = 1000;
    CHECK(costOf(*r.model("nova-3"), second, today) == 72); // ceil(4300 / 60)

    // Cached tokens without a cache price bill at the input rate (never under-estimated).
    Usage cached = oneMillionEach;
    cached.cacheReadTokens = 500'000;
    CHECK(costOf(*r.model("claude-sonnet-5-5"), cached, today) == 12 * kMicroUsdPerUsd);

    CHECK(formatUsd(2'000'000) == "$2.00");
    CHECK(formatUsd(4300) == "$0.0043");
    CHECK(formatUsd(1) == "$0.000001");
    CHECK(formatUsd(-1'500'000) == "-$1.50");
}

TEST_CASE("Dates, deprecation and user models", "[ai][platform][registry]") {
    CHECK(parseDate("2027-01-01") == ymd(2027, 1, 1));
    CHECK_FALSE(parseDate("2027-02-30"));
    CHECK_FALSE(parseDate("2027-1-01"));
    CHECK(formatDate(ymd(2026, 3, 9)) == "2026-03-09");

    auto parsed = ModelRegistry::fromJson(R"({"schemaVersion":1,"providers":[
        {"id":"p","api":"chat-completions","baseUrl":"https://p.test/v1","configurableBaseUrl":true}],
      "models":[{"id":"old","provider":"p","capabilities":{"chat":true},"deprecation":"2026-01-01"},
                {"id":"new","provider":"p","aliases":["latest"],"capabilities":{"chat":true}}]})");
    REQUIRE(parsed);
    ModelRegistry r = std::move(*parsed);
    CHECK(r.usableModel("old", ymd(2025, 12, 31)));
    const auto old = r.usableModel("old", ymd(2026, 1, 1));
    REQUIRE_FALSE(old);
    CHECK(old.error().code == AiErrorCode::Deprecated);
    CHECK(r.usableModel("latest", ymd(2026, 1, 1)).value()->id == "new");
    CHECK(r.usableModel("nope", ymd(2026, 1, 1)).error().code == AiErrorCode::ModelMissing);

    ModelSpec mine;
    mine.id = "qwen3.5:9b";
    mine.provider = "p";
    REQUIRE(r.addModel(mine));
    CHECK(r.model("qwen3.5:9b")->api == "chat-completions");
    CHECK(r.model("qwen3.5:9b")->caps.has(Capability::Chat));
    CHECK(r.isUserModel("qwen3.5:9b"));
    CHECK_FALSE(r.addModel(mine)); // taken
    CHECK(r.removeUserModel("qwen3.5:9b"));
    CHECK_FALSE(r.removeUserModel("new")); // built-in

    ProviderSpec custom;
    custom.id = "office-gpu";
    custom.api = "chat-completions";
    custom.baseUrl = "http://10.0.0.5:8080/v1";
    ModelSpec office;
    office.id = "office-model";
    office.caps.capabilities = {Capability::Chat};
    REQUIRE(r.addCustomProvider(custom, {office}));
    CHECK(r.model("office-model")->provider == "office-gpu");
    custom.id = "p";
    CHECK_FALSE(r.addCustomProvider(custom, {})); // built-in id

    // Round trip through JSON.
    auto again = ModelRegistry::fromJson(ModelRegistry::builtin().toJson().dump());
    REQUIRE(again);
    CHECK(again->models() == ModelRegistry::builtin().models());
    CHECK(again->providers() == ModelRegistry::builtin().providers());
}

TEST_CASE("Malformed registry files are rejected with the entry named", "[ai][platform][registry]") {
    auto expectError = [](const char* json, const char* fragment) {
        auto r = ModelRegistry::fromJson(json);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::InvalidRequest);
        INFO(r.error().message);
        CHECK(r.error().message.find(fragment) != std::string::npos);
    };
    expectError("[]", "object");
    expectError(R"({"schemaVersion":2,"providers":[],"models":[]})", "schemaVersion");
    expectError(R"({"schemaVersion":1,"providers":[{"id":"p"}],"models":[]})", "baseUrl");
    expectError(R"({"schemaVersion":1,"providers":[{"id":"p","baseUrl":"https://x","auth":"magic"}],"models":[]})",
                "auth");
    expectError(R"({"schemaVersion":1,"providers":[{"id":"p","api":"a","baseUrl":"https://x"}],
                    "models":[{"id":"m","provider":"q","capabilities":{"chat":true}}]})",
                "unknown provider");
    expectError(R"({"schemaVersion":1,"providers":[{"id":"p","api":"a","baseUrl":"https://x"}],
                    "models":[{"id":"m","provider":"p","capabilities":{"telepathy":true}}]})",
                "telepathy");
    expectError(R"({"schemaVersion":1,"providers":[{"id":"p","api":"a","baseUrl":"https://x"}],
                    "models":[{"id":"m","provider":"p","capabilities":{"chat":true}},
                              {"id":"m","provider":"p","capabilities":{"chat":true}}]})",
                "duplicate model");
    expectError(R"({"schemaVersion":1,"providers":[{"id":"p","api":"a","baseUrl":"https://x"}],
                    "models":[{"id":"m","provider":"p","capabilities":{"chat":true},"deprecation":"soon"}]})",
                "deprecation");
}

TEST_CASE("Portable schema subset", "[ai][platform][schema]") {
    const Json ok = Json::parse(R"({"type":"object","properties":{
        "clip":{"type":"string","description":"clip id","minLength":1},
        "frames":{"type":"integer","minimum":0,"maximum":100},
        "mode":{"enum":["ripple","lift"]},
        "tags":{"type":"array","items":{"type":"string"},"maxItems":5},
        "note":{"type":["string","null"]},
        "where":{"anyOf":[{"type":"string"},{"type":"integer"}]}},
      "required":["clip"],"additionalProperties":false})");
    CHECK(checkPortableSchema(ok));

    auto rejected = [](const char* json, const char* fragment) {
        auto r = checkPortableSchema(Json::parse(json));
        REQUIRE_FALSE(r);
        INFO(r.error().message);
        CHECK(r.error().message.find(fragment) != std::string::npos);
    };
    rejected(R"({"type":"object","properties":{"x":{"oneOf":[{"type":"string"}]}}})", "/properties/x: 'oneOf'");
    rejected(R"({"type":"object","additionalProperties":true})", "additionalProperties");
    rejected(R"({"type":"object","properties":{},"required":["missing"]})", "missing");
    rejected(R"({"type":"object","properties":{"x":{"$ref":"#/$defs/x"}}})", "$ref");
    rejected(R"({"type":"date"})", "type");
}

TEST_CASE("Schemas are rewritten for each provider's strict mode", "[ai][platform][schema]") {
    const Json portable = Json::parse(R"({"type":"object","properties":{
        "clip":{"type":"string","minLength":1,"description":"Clip id"},
        "frames":{"type":"integer","minimum":0,"default":5},
        "kind":{"const":"ripple"}},
      "required":["clip"],"additionalProperties":false,"$comment":"internal"})");

    const Json anthropic = adaptSchema(portable, SchemaDialect::Anthropic, true);
    CHECK_FALSE(anthropic.contains("$comment"));
    CHECK_FALSE(anthropic["properties"]["clip"].contains("minLength"));
    CHECK(anthropic["properties"]["clip"]["description"] == "Clip id. Minimum length: 1.");
    CHECK(anthropic["properties"]["frames"]["description"] == "Minimum: 0. Default: 5.");
    CHECK(anthropic["required"] == Json::array({"clip"}));
    CHECK(anthropic["properties"]["kind"]["const"] == "ripple");
    CHECK(anthropic["additionalProperties"] == false);

    const Json openai = adaptSchema(portable, SchemaDialect::OpenAiStrict, true);
    CHECK(openai["required"] == Json::array({"clip", "frames", "kind"}));
    CHECK(openai["properties"]["clip"]["type"] == "string");                        // was required
    CHECK(openai["properties"]["frames"]["type"] == Json::array({"integer", "null"})); // optional -> nullable
    CHECK(openai["properties"]["frames"]["minimum"] == 0);                          // supported in strict mode
    CHECK(openai["properties"]["kind"]["enum"] == Json::array({"ripple", nullptr}));
    CHECK_FALSE(openai["properties"]["clip"].contains("minLength"));

    const Json gemini = adaptSchema(portable, SchemaDialect::Gemini, true);
    CHECK_FALSE(gemini.contains("additionalProperties"));
    CHECK(gemini["required"] == Json::array({"clip"}));
    CHECK(gemini["properties"]["kind"]["enum"] == Json::array({"ripple"}));

    const Json loose = adaptSchema(portable, SchemaDialect::OpenAiCompatible, false);
    CHECK(loose["properties"]["clip"]["minLength"] == 1);
    CHECK_FALSE(loose.contains("$comment"));

    const Json empty = adaptSchema(Json{{"type", "object"}}, SchemaDialect::OpenAiStrict, true);
    CHECK(empty["properties"] == Json::object());
    CHECK(empty["required"] == Json::array());
    CHECK(empty["additionalProperties"] == false);
}

TEST_CASE("Every command of the catalog is a portable tool", "[ai][platform][schema]") {
    const auto& registry = wabcut::command::builtinCommands();
    const Json exported = wabcut::command::exportTools(registry, wabcut::command::ToolFormat::JsonSchema);
    const auto tools = toolSpecsFromCommandExport(exported);
    REQUIRE(tools.size() == exported.size());
    REQUIRE(tools.size() > 10);
    ChatRequest request;
    request.messages.push_back(ChatMessage::user("hi"));
    request.tools = tools;
    const auto valid = validateChatRequest(request);
    INFO((valid ? std::string() : valid.error().message));
    CHECK(valid);
    for (const auto& tool : tools) {
        INFO(tool.name);
        const Json strict = adaptSchema(tool.parameters, SchemaDialect::OpenAiStrict, true);
        CHECK(strict["additionalProperties"] == false);
        CHECK(strict["required"].size() == strict["properties"].size()); // OpenAI strict: every property required
        CHECK(adaptSchema(tool.parameters, SchemaDialect::Anthropic, true)["additionalProperties"] == false);
    }
}

TEST_CASE("Chat requests are validated before sending", "[ai][platform][schema]") {
    ChatRequest request;
    CHECK(validateChatRequest(request).error().message.find("no messages") != std::string::npos);
    request.messages.push_back(ChatMessage::user("cut the silences"));
    CHECK(validateChatRequest(request));

    ToolSpec tool;
    tool.name = "timeline.rippleDelete"; // dots are not allowed by the APIs
    tool.parameters = Json{{"type", "object"}};
    request.tools = {tool};
    CHECK_FALSE(validateChatRequest(request));
    request.tools[0].name = "timeline_rippleDelete";
    CHECK(validateChatRequest(request));
    request.tools.push_back(request.tools[0]);
    CHECK(validateChatRequest(request).error().message.find("duplicate") != std::string::npos);
    request.tools.pop_back();

    request.toolChoice = {ToolChoiceMode::Named, "other"};
    CHECK_FALSE(validateChatRequest(request));
    request.toolChoice = {ToolChoiceMode::Named, "timeline_rippleDelete"};
    CHECK(validateChatRequest(request));

    request.responseFormat = ResponseFormat{"plan", Json{{"type", "string"}}};
    CHECK_FALSE(validateChatRequest(request));

    CHECK(isValidToolName("a-b_C9"));
    CHECK_FALSE(isValidToolName(std::string(65, 'a')));
    CHECK(parseToolArguments("").value() == Json::object());
    CHECK(parseToolArguments(R"({"clip":"c1"})").value()["clip"] == "c1");
    CHECK(parseToolArguments("[1]").error().code == AiErrorCode::InvalidRequest);
    CHECK(parseToolArguments("{\"clip\":").error().code == AiErrorCode::InvalidRequest);
}
