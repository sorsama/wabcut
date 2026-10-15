// docs/extensions.md: its commands-by-capability section is generated from the catalog and must stay in sync.

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/script/Docs.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

TEST_CASE("docs/extensions.md lists the commands each grant allows; up to date", "[script][docs]") {
    std::ifstream file(WABCUT_DOCS_DIR "/extensions.md", std::ios::binary);
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    std::string onDisk = text.str();
    onDisk.erase(std::remove(onDisk.begin(), onDisk.end(), '\r'), onDisk.end()); // checkouts may use CRLF
    const auto generated = wabcut::script::withExtensionCommands(onDisk, wabcut::ai::edit::editingCommands());
    REQUIRE(generated.has_value());
    INFO("regenerate it: build\\win-msvc\\bin\\wabcut_command_docgen.exe docs/commands.md docs/mcp.md docs/extensions.md");
    CHECK(*generated == onDisk);
    CHECK(onDisk.find("`captions.translate` *(AI)*") != std::string::npos);
}
