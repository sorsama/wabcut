// docs/commands.md is generated from the registry wabcut-cli serves (the built-in catalog plus the transcript
// commands) and must stay in sync with it.

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Cli.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace wabcut::command;

TEST_CASE("docs/commands.md is up to date", "[command][docs]") {
    std::ifstream file(WABCUT_DOCS_DIR "/commands.md", std::ios::binary);
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    std::string onDisk = text.str();
    onDisk.erase(std::remove(onDisk.begin(), onDisk.end(), '\r'), onDisk.end()); // checkouts may use CRLF
    const std::string generated = catalogMarkdown(wabcut::ai::edit::editingCommands());
    INFO("regenerate it: build\\win-msvc\\bin\\wabcut_command_docgen.exe docs/commands.md docs/mcp.md");
    CHECK(onDisk == generated);
}

TEST_CASE("The catalog reference documents every command", "[command][docs]") {
    const CommandRegistry& registry = wabcut::ai::edit::editingCommands();
    const std::string md = catalogMarkdown(registry);
    for (const CommandSpec& spec : registry.all()) {
        INFO(spec.name);
        CHECK(md.find("### `" + spec.name + "` v" + std::to_string(spec.version)) != std::string::npos);
    }
    CHECK(md.find("| `ranges[].start` |") != std::string::npos); // nested parameters are listed
    CHECK(md.find("| `gap.track` |") != std::string::npos);
}
