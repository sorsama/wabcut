// The built-in catalog: complete, consistent, and named like the keymap where they overlap.

#include <wabcut/command/Registry.h>
#include <wabcut/command/Schema.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace wabcut;
using namespace wabcut::command;

TEST_CASE("The catalog has the commands an agent needs", "[command][registry]") {
    const CommandRegistry& registry = builtinCommands();
    CHECK(registry.all().size() >= 25);
    for (const char* name :
         {"clip.insert", "clip.overwrite", "sequence.lift", "sequence.extract", "sequence.addEdit",
          "sequence.addEditToAllTracks", "edit.clear", "timeline.rippleDelete", "sequence.rippleTrimPreviousToPlayhead",
          "sequence.rippleTrimNextToPlayhead", "clip.trim", "clip.rippleTrim", "clip.roll", "clip.slip", "clip.slide",
          "clip.move", "clip.setParam", "clip.enable", "clip.link", "clip.unlink", "clip.rename", "clip.setColor",
          "marker.add", "marker.remove", "marker.markIn", "marker.markOut", "marker.clearInAndOut",
          "timeline.removeSilences", "track.setState", "sequence.addTrack", "sequence.removeTrack", "project.summary",
          "sequence.summary", "clip.details", "media.list", "clip.find"}) {
        INFO(name);
        const CommandSpec* spec = registry.find(name);
        REQUIRE(spec);
        CHECK(spec->name == name);
    }
}

TEST_CASE("Aliases and tool names resolve to the command", "[command][registry]") {
    const CommandRegistry& registry = builtinCommands();
    CHECK(registry.find("sequence.insert")->name == "clip.insert");
    CHECK(registry.find("markers.add")->name == "marker.add");
    CHECK(registry.find("timeline.razor")->name == "sequence.addEdit");
    CHECK(registry.find("timeline_razor")->name == "sequence.addEdit");
    CHECK(registry.find("timeline_rippleDelete")->name == "timeline.rippleDelete");
    CHECK(registry.find("clip.setParam", 1));
    CHECK_FALSE(registry.find("clip.setParam", 2));
    CHECK_FALSE(registry.find("clip"));
    CHECK_FALSE(registry.knows("timeline.explode"));
}

TEST_CASE("Every command is well formed", "[command][registry]") {
    std::set<std::string> titles;
    for (const CommandSpec& spec : builtinCommands().all()) {
        INFO(spec.name);
        CHECK(spec.name.find('_') == std::string::npos);
        CHECK(spec.version >= 1);
        CHECK_FALSE(spec.title.empty());
        CHECK(spec.description.size() > 20);
        CHECK(spec.params["type"] == "object");
        CHECK(spec.params["additionalProperties"] == false);
        CHECK(unsupportedSchemaKeywords(spec.params).empty());
        // The example is valid input (docs and tool definitions show it).
        const auto issues = validateAgainstSchema(spec.params, spec.example);
        CHECK(issues.empty());
        // Everything that works on a sequence takes the same optional "sequence" parameter. 0.2: the export presets, queue
        // and watch folders are the host's, not a sequence's.
        const bool hostExport = spec.name.starts_with("export.preset") || spec.name.starts_with("export.queue.") ||
                                spec.name.starts_with("export.watchFolder.");
        if (spec.name != "media.list" && spec.name != "project.summary" && spec.name != "effect.list" && !hostExport) {
            CHECK(spec.params["properties"].contains("sequence"));
        }
        CHECK(toolNameFor(spec.name).size() <= 64);
        titles.insert(spec.title);
    }
    CHECK(titles.size() == builtinCommands().all().size()); // undo labels are distinct
}

TEST_CASE("Command names match the keymap's command ids where both exist", "[command][registry]") {
    std::ifstream file(WABCUT_KEYMAP_DIR "/premiere-windows.json");
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    const std::string keymap = text.str();
    for (const char* id :
         {"clip.insert", "clip.overwrite", "sequence.lift", "sequence.extract", "sequence.addEdit",
          "sequence.addEditToAllTracks", "edit.clear", "timeline.rippleDelete", "edit.rippleDelete",
          "sequence.rippleTrimPreviousToPlayhead", "sequence.rippleTrimNextToPlayhead", "clip.enable", "clip.link",
          "marker.add", "marker.markIn", "marker.markOut", "marker.clearInAndOut", "clip.speedDuration",
          "sequence.applyVideoTransition", "sequence.applyAudioTransition", "sequence.applyDefaultTransitions"}) {
        INFO(id);
        CHECK(keymap.find('"' + std::string(id) + '"') != std::string::npos);
        CHECK(builtinCommands().knows(id));
    }
}

TEST_CASE("The registry refuses inconsistent commands", "[command][registry]") {
    CommandRegistry registry;
    const auto spec = [](std::string name, std::vector<std::string> aliases = {}) {
        return CommandSpec{std::move(name), 1, "T", "A test command.", Capability::Read, schema::object({}),
                           Json::object(), std::move(aliases), [](ExecutionContext&) { return CommandOutput{}; }};
    };
    REQUIRE(registry.add(spec("test.one", {"test.uno"})));
    CHECK_FALSE(registry.add(spec("test.one")));            // name + version taken
    CHECK_FALSE(registry.add(spec("test_two")));            // '_' is reserved for tool names
    CHECK_FALSE(registry.add(spec("undotted")));
    CHECK_FALSE(registry.add(spec("test.two", {"test.uno"}))); // alias taken
    CHECK_FALSE(registry.add(spec("test.three", {"test.one"})));
    auto noExecutor = spec("test.four");
    noExecutor.execute = nullptr;
    CHECK_FALSE(registry.add(noExecutor));
    auto badSchema = spec("test.five");
    badSchema.params = Json{{"type", "object"}, {"oneOf", Json::array()}};
    CHECK_FALSE(registry.add(badSchema));
    auto v2 = spec("test.one");
    v2.version = 2;
    REQUIRE(registry.add(v2));
    CHECK(registry.find("test.one")->version == 2);
    CHECK(registry.find("test.uno", 1)->version == 1);
    CHECK(registry.all().size() == 2);
}
