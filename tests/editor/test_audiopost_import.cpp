// AAF / OMF import into an open project (0.6, card #68): File > Import of an AAF or OMF is one undo step, its embedded
// audio is written next to the project and probed like any media, and undo takes all of it back.

#include <wabcut/editor/MediaImport.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path scratch(std::string_view name) {
    const fs::path dir = fs::path(WABCUT_TEST_MEDIA_DIR).parent_path() / "editor" / std::string(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("AAF and OMF import into a project are one undo step with their audio written beside it (0.6)",
          "[editor][import][aaf-import]") {
    const fs::path fixtures(WABCUT_INTERCHANGE_FIXTURES);
    for (const char* name : {"audiopost.aaf", "audiopost.omf"}) {
        INFO(name);
        const fs::path dir = scratch(std::string("audiopost-import-") + name);
        CHECK(classifyImportFile(fixtures / "export" / name) == ImportFileKind::Interchange);
        ProjectState start;
        auto seq = edit::addSequence(start, edit::SequenceSettings{});
        REQUIRE(seq.has_value());
        EditSession session(seq->state);
        const std::size_t before = session.snapshot()->sequences.size();
        TimelineImportOptions options;
        options.projectFile = dir / "Mix project.wabcut";
        auto imported = importTimelineFile(session, fixtures / "export" / name, options);
        INFO((imported ? std::string() : imported.error().message));
        REQUIRE(imported.has_value());
        CHECK(session.undoCount() == 1);
        CHECK(session.snapshot()->sequences.size() == before + 1);
        // The embedded audio is in "<file> Media" beside the project, online, with its probed streams.
        const fs::path media = dir / "audiopost Media";
        REQUIRE(fs::is_directory(media));
        std::size_t audio = 0;
        for (const MediaId id : imported->media) {
            const MediaItem* item = session.snapshot()->findMedia(id);
            REQUIRE(item != nullptr);
            if (item->info.video.empty()) {
                ++audio;
                CHECK_FALSE(item->locator.offline);
                CHECK(fs::path(pathFromUtf8(item->locator.path)).parent_path() == fs::absolute(media));
                REQUIRE_FALSE(item->info.audio.empty());
                CHECK(item->info.audio.front().channels == 1);
            }
        }
        CHECK(audio == 6);
        // One undo takes the whole import back.
        REQUIRE(session.undo().has_value());
        CHECK(session.snapshot()->sequences.size() == before);
        CHECK(session.snapshot()->media.empty());
    }
}

// Fix round 1 (ARGON-08): the import runs on a worker in the app (prepare, then commit), reports its progress and can be
// cancelled; a cancelled import changes nothing and leaves none of the audio it wrote.
TEST_CASE("AAF import off the GUI thread reports progress and cancels without leaving files (0.6)",
          "[editor][import][aaf-import]") {
    const fs::path fixtures(WABCUT_INTERCHANGE_FIXTURES);
    const fs::path dir = scratch("audiopost-import-progress");
    ProjectState start;
    auto seq = edit::addSequence(start, edit::SequenceSettings{});
    REQUIRE(seq.has_value());
    EditSession session(seq->state);

    // Prepared on the side: the session is not touched until the commit.
    std::vector<double> seen;
    TimelineImportOptions options;
    options.projectFile = dir / "Mix project.wabcut";
    options.progress = [&](double share) {
        seen.push_back(share);
        return true;
    };
    auto prepared = prepareTimelineImport(session.stateForSave(), fixtures / "export" / "audiopost.aaf", options);
    INFO((prepared ? std::string() : prepared.error().message));
    REQUIRE(prepared.has_value());
    CHECK(session.undoCount() == 0);
    REQUIRE(seen.size() >= 3);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(seen.front() >= 0.0);
    CHECK(seen.back() == 1.0);
    REQUIRE(commitTimelineImport(session, fixtures / "export" / "audiopost.aaf", *prepared).has_value());
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Import audiopost.aaf");

    // Cancel once the first embedded file was written: an error, nothing committed, no audio left behind.
    const fs::path other = scratch("audiopost-import-cancel");
    TimelineImportOptions cancelled;
    cancelled.projectFile = other / "Mix project.wabcut";
    cancelled.progress = [](double share) { return share <= 0.0; };
    auto stopped = prepareTimelineImport(session.stateForSave(), fixtures / "export" / "audiopost.aaf", cancelled);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(fs::exists(other / "audiopost Media"));
    CHECK(session.undoCount() == 1);
}

TEST_CASE("AAF embedded audio goes to a folder that can be written (0.6)", "[editor][import][aaf-import]") {
    const fs::path dir = scratch("audiopost-media-folder");
    fs::create_directories(dir / "Project");
    fs::create_directories(dir / "Delivery");
    const fs::path aaf = dir / "Delivery" / "Final mix.aaf";
    const fs::path fallback = dir / "Documents" / "Imported Media";
    // A saved project: beside the project.
    CHECK(embeddedMediaFolderFor(aaf, dir / "Project" / "Edit.wabcut", fallback) == dir / "Project" / "Final mix Media");
    // An unsaved project: beside the file.
    CHECK(embeddedMediaFolderFor(aaf, {}, fallback) == dir / "Delivery" / "Final mix Media");
    // Neither can be written (a project folder that is gone, the file on a share or disc that cannot be written): the
    // fallback (the app: Documents/Wabcut/Imported Media), never an import with all its audio offline.
    const fs::path unreachable = dir / "Gone" / "Final mix.aaf";
    CHECK(embeddedMediaFolderFor(unreachable, dir / "Gone too" / "Edit.wabcut", fallback) == fallback / "Final mix Media");
    // No probe file is left behind.
    CHECK_FALSE(fs::exists(dir / "Project" / ".wabcut-write-test"));
    CHECK_FALSE(fs::exists(dir / "Delivery" / ".wabcut-write-test"));
}
