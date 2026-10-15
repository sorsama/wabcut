// Tool definitions for LLM APIs.

#include <wabcut/command/Registry.h>
#include <wabcut/command/Schema.h>
#include <wabcut/command/ToolExport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <regex>
#include <set>

using namespace wabcut::command;

TEST_CASE("Anthropic tools: name, description, input_schema", "[command][tools]") {
    const Json tools = exportTools(builtinCommands(), ToolFormat::Anthropic);
    REQUIRE(tools.size() == builtinCommands().all().size());
    const std::regex namePattern("^[a-zA-Z0-9_-]{1,64}$");
    std::set<std::string> names;
    for (const Json& tool : tools) {
        INFO(tool.dump());
        CHECK(tool.size() == 3);
        CHECK(std::regex_match(tool["name"].get<std::string>(), namePattern));
        CHECK(tool["input_schema"]["type"] == "object");
        CHECK_FALSE(tool["description"].get<std::string>().empty());
        names.insert(tool["name"].get<std::string>());
        // A tool call comes back under the tool name; the registry resolves it.
        CHECK(builtinCommands().knows(tool["name"].get<std::string>()));
    }
    CHECK(names.size() == tools.size());
    CHECK(names.contains("timeline_rippleDelete"));
}

TEST_CASE("OpenAI tools: type function with name, description, parameters", "[command][tools]") {
    const Json tools = exportTools(builtinCommands(), ToolFormat::OpenAI);
    REQUIRE(tools.size() == builtinCommands().all().size());
    for (const Json& tool : tools) {
        INFO(tool.dump());
        CHECK(tool["type"] == "function");
        CHECK(tool["function"]["name"].get<std::string>().size() <= 64);
        CHECK(tool["function"]["parameters"]["type"] == "object");
        CHECK(tool["function"].contains("description"));
    }
}

TEST_CASE("JSON Schema export and capability filtering", "[command][tools]") {
    const Json all = exportTools(builtinCommands(), ToolFormat::JsonSchema);
    const Json& first = all[0];
    CHECK(first.contains("name"));
    CHECK(first.contains("toolName"));
    CHECK(first["parameters"]["$schema"] == kJsonSchemaDialect);
    CHECK(first.contains("example"));

    const Json readOnly = exportTools(builtinCommands(), ToolFormat::Anthropic, CapabilitySet::of({Capability::Read}));
    CHECK(readOnly.size() == static_cast<std::size_t>(std::count_if(
                                 builtinCommands().all().begin(), builtinCommands().all().end(),
                                 [](const CommandSpec& s) { return s.capability == Capability::Read; })));
    for (const Json& tool : readOnly) {
        const CommandSpec* spec = builtinCommands().find(tool["name"].get<std::string>());
        REQUIRE(spec);
        CHECK(spec->capability == Capability::Read);
    }
    CHECK(toolFormatFromString("anthropic") == ToolFormat::Anthropic);
    CHECK(toolFormatFromString("openai") == ToolFormat::OpenAI);
    CHECK(toolFormatFromString("jsonschema") == ToolFormat::JsonSchema);
    CHECK_FALSE(toolFormatFromString("yaml"));
}

TEST_CASE("Only the newest version of a command is exported", "[command][tools]") {
    CommandRegistry registry;
    for (int version : {1, 2}) {
        REQUIRE(registry.add(CommandSpec{"test.cmd", version, "T", "v" + std::to_string(version), Capability::Read,
                                         schema::object({}), Json::object(), {},
                                         [](ExecutionContext&) { return CommandOutput{}; }}));
    }
    const Json tools = exportTools(registry, ToolFormat::JsonSchema);
    REQUIRE(tools.size() == 1);
    CHECK(tools[0]["version"] == 2);
    CHECK(tools[0]["description"] == "v2");
}
