// Fuzz harness: command requests and batches (command/Engine.h) - what an agent, a script or the CLI sends, untrusted.
// Each input is a JSON request document run by a fresh engine with every edit capability over a small project with
// every shape (ScaleProject.h) and the catalog without its file commands (FuzzRegistry.h). A rejected request leaves the
// project as it was; an accepted one leaves a project that passes validate(Strict) and saves; every result serialises.

#include "FuzzSupport.h"

#include "FuzzRegistry.h"
#include "ScaleProject.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const ProjectState base = test::makeScaleProject(test::smallScaleSpec()).state;
    const command::Json document = command::Json::parse(fuzz::text(data, size), nullptr, false);
    if (document.is_discarded()) {
        return 0;
    }
    EditSession session(base);
    command::CommandEngine engine(
        session, fuzz::safeCommands(),
        command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence,
                                                          command::Capability::EditProject}),
                              1024, nullptr});
    const ProjectSnapshot before = session.snapshot();
    const command::CommandResult result = engine.executeJson(document);
    const ProjectSnapshot after = session.snapshot();
    if (!result.ok()) {
        WABCUT_FUZZ_CHECK(after == before, "a rejected request changed the project: " + document.dump());
    } else {
        const auto valid = validate(*after, Validation::Strict);
        WABCUT_FUZZ_CHECK(valid, (valid ? std::string() : valid.error().message) + "\n" + document.dump());
        // The edited project still saves (no invalid UTF-8 in a name cut to a length, no value JSON cannot hold).
        const auto saved = io::toJson(*after);
        WABCUT_FUZZ_CHECK(saved, (saved ? std::string() : saved.error().message) + "\n" + document.dump());
    }
    // Results go out as JSON text (MCP, the CLI); dump() throws on invalid UTF-8.
    const std::string serialised = command::toJson(result).dump();
    WABCUT_FUZZ_CHECK(!serialised.empty(), document.dump());
    return 0;
}
