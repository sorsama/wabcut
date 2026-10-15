// `wabcut-cli ext` (C.12): an extension command run headless on a project file, for automation.

#include "ScriptTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/script/Extension.h>
#include <wabcut/script/Manager.h>
#include <wabcut/script/Permissions.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <sstream>

using namespace wabcut;
using namespace wabcut::script::test;
using wabcut::script::Json;

namespace {

/// Runs wabcut-cli with `args` and WABCUT_DATA_DIR=`data`; stdout parsed as JSON (the exit code in `code`).
Json cli(const fs::path& dir, const fs::path& data, const std::string& args, int& code) {
    const auto out = dir / "out.json";
#if defined(_WIN32)
    const std::string line = "set \"WABCUT_DATA_DIR=" + data.string() + "\" && \"" + std::string(WABCUT_CLI_PATH) +
                             "\" " + args + " > \"" + out.string() + "\"";
    code = std::system(("\"" + line + "\"").c_str()); // cmd.exe strips one pair of outer quotes
#else
    const std::string line = "WABCUT_DATA_DIR='" + data.string() + "' '" + std::string(WABCUT_CLI_PATH) + "' " + args +
                             " > '" + out.string() + "'";
    code = std::system(line.c_str());
#endif
    std::ifstream in(out, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const Json j = Json::parse(text.str(), nullptr, false);
    INFO(text.str());
    REQUIRE_FALSE(j.is_discarded());
    return j;
}

std::string quoted(const fs::path& p) { return "\"" + p.string() + "\""; }

} // namespace

TEST_CASE("wabcut-cli ext run runs an extension command on a project and saves it", "[script][cli]") {
    if (!fs::exists(fs::path(WABCUT_CLI_PATH))) {
        SKIP("wabcut-cli is not built (" << WABCUT_CLI_PATH << ")");
    }
    const fs::path dir = scratch("cli");
    const fs::path data = dir / "appdata";
    const fs::path project = dir / "film.wabcut";
    wabcut::test::Timeline tl("    V1 tgt |A" + std::string(99, 'a') + "|\n    A1 tgt |A" + std::string(99, 'a') + "|\n");
    REQUIRE(io::saveProject(tl.state, project, io::SaveOptions{0}).has_value());
    const fs::path sample = fs::path(WABCUT_SCRIPT_SAMPLES_DIR) / "beat-markers";

    // Without grants (none given in Wabcut, no --grant) it refuses and changes nothing.
    int code = 0;
    Json refused = cli(dir, data, "ext run " + quoted(sample) + " addBeatMarkers " + quoted(project), code);
    CHECK(code == 3);
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["message"].get<std::string>().find("--grant") != std::string::npos);

    Json ran = cli(dir, data,
                   "ext run " + quoted(sample) + " addBeatMarkers " + quoted(project) +
                       " --grant-requested --args \"{\\\"every\\\":\\\"beat\\\",\\\"bpm\\\":120}\" --save",
                   code);
    INFO(ran.dump(2));
    CHECK(code == 0);
    CHECK(ran["ok"] == true);
    CHECK(ran["result"]["markers"] == 8);
    CHECK(ran["saved"] == true);
    CHECK(ran["extension"]["granted"] == Json::array({"Read", "EditSequence"}));
    CHECK(ran["undo"] == Json::array({"Beat Markers: Set Beat Grid", "Beat Markers: Add Markers at Every Beat"}));
    auto saved = io::loadProject(project);
    REQUIRE(saved.has_value());
    CHECK(saved->findSequence(tl.seq)->markers.size() == 8);

    // Only what the manifest asks for can be granted.
    Json wider = cli(dir, data,
                     "ext run " + quoted(sample) + " addBeatMarkers " + quoted(project) + " --grant Read,EditProject", code);
    CHECK(code == 2);
    CHECK(wider["error"]["message"].get<std::string>().find("EditProject") != std::string::npos);

    // validate: the manifest, files and content hash, or why not.
    Json valid = cli(dir, data, "ext validate " + quoted(sample), code);
    CHECK(code == 0);
    CHECK(valid["manifest"]["id"] == "com.wabcut.samples.beat-markers");
    CHECK(valid["contentHash"].get<std::string>().size() == 64);
    writeFile(dir / "broken" / "wabcut-extension.json", "{\"id\": \"x\"}");
    Json broken = cli(dir, data, "ext validate " + quoted(dir / "broken"), code);
    CHECK(code == 1);
    CHECK(broken["ok"] == false);
}

TEST_CASE("wabcut-cli ext run of a folder never borrows an installed extension's grants (S5-P2-4)", "[script][cli][security]") {
    if (!fs::exists(fs::path(WABCUT_CLI_PATH))) {
        SKIP("wabcut-cli is not built (" << WABCUT_CLI_PATH << ")");
    }
    const fs::path dir = scratch("cli-borrow");
    const fs::path data = dir / "appdata";
    const fs::path project = dir / "film.wabcut";
    wabcut::test::Timeline tl("    V1 tgt |Aaaa|\n    A1 tgt |Aaaa|\n");
    REQUIRE(io::saveProject(tl.state, project, io::SaveOptions{0}).has_value());
    // An installed extension the user granted Read, as the app records it.
    const wabcut::script::ExtensionPaths paths = wabcut::script::extensionPathsUnder(data);
    const Json m = manifest("com.test.borrow", "Borrow", {"Read"});
    const fs::path installed = writeExtension(paths.userExtensions / "com.test.borrow", m,
                                              {{"main.js", "wabcut.ui.registerCommand('go', 'Go', () => 'installed');"}});
    {
        auto bundle = wabcut::script::loadExtension(installed, wabcut::script::ExtensionScope::User);
        REQUIRE(bundle.has_value());
        wabcut::script::PermissionStore store = wabcut::script::PermissionStore::load(paths.permissions);
        REQUIRE(store.grant(*bundle, wabcut::command::CapabilitySet::of({wabcut::command::Capability::Read})));
        REQUIRE(store.save());
    }
    int code = 0;
    Json ok = cli(dir, data, "ext run com.test.borrow go " + quoted(project), code);
    INFO(ok.dump(2));
    CHECK(code == 0);
    CHECK(ok["result"] == "installed");

    // A folder elsewhere claims the same id with other code: it gets no grants without --grant.
    const fs::path copy = writeExtension(dir / "elsewhere" / "borrow", m,
                                         {{"main.js", "wabcut.ui.registerCommand('go', 'Go', () => 'impostor');"}});
    Json refused = cli(dir, data, "ext run " + quoted(copy) + " go " + quoted(project), code);
    INFO(refused.dump(2));
    CHECK(code == 3);
    CHECK(refused["ok"] == false);
}
