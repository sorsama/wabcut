// Release workflows (RC.1, docs/internal/release-checklist.md): the import step of tools/release/rc_workflows.py. The CLI has no
// media import (an editor's import probes the files and fingerprints them), so the script writes one list of media
// files per workflow under test-output/release/projects/<name>.txt and runs this case, which imports each list into a
// new project through the app's import (editor::importMediaFiles) and saves it as <name>.wabcut next to the list. The
// script then edits, exports and checks the projects with wabcut-cli. Skipped when no list is there.

#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

TEST_CASE("Release workflows: import each workflow's media list into a saved project", "[editor][release]") {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "release" / "projects";
    std::vector<fs::path> lists;
    if (fs::is_directory(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".txt") {
                lists.push_back(entry.path());
            }
        }
    }
    if (lists.empty()) {
        SKIP("no release workflow lists (tools/release/rc_workflows.py writes them)");
    }
    for (const fs::path& list : lists) {
        INFO(list.string());
        std::vector<fs::path> files;
        std::ifstream in(list);
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                files.push_back(pathFromUtf8(line));
            }
        }
        REQUIRE_FALSE(files.empty());
        const fs::path project = fs::path(list).replace_extension(".wabcut");
        std::error_code ec;
        fs::remove(project, ec);
        EditSession session(ProjectState{});
        const MediaImportResult r = importMediaFiles(session, files, {.projectFile = project});
        for (const auto& failed : r.failed) {
            UNSCOPED_INFO(failed.file.string() << ": " << failed.error.message);
        }
        CHECK(r.failed.empty());
        CHECK(r.added.size() == files.size());
        ProjectDocument doc(session);
        REQUIRE(doc.saveAs(project));
        CHECK(fs::exists(project));
    }
}
