// P.1: ProjectDocument::newestUnsavedAutosave() (what the app's recovery scan offers for the last project, now read on
// a worker and parsing only what it must) gives the same file as the first loadable autosave newer than the project
// in recoveryFiles() (which parses every autosave and backup), in every situation a start can find.

#include <wabcut/editor/ProjectDocument.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path scanDir(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "recovery-scan" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/// A project with `sequences` sequences (to tell the files apart).
ProjectState withSequences(int sequences) {
    ProjectState state;
    for (int i = 0; i < sequences; ++i) {
        auto created = edit::addSequence(state, edit::SequenceSettings{});
        REQUIRE(created);
        state = std::move(created->state);
    }
    return state;
}

fs::path rotated(const fs::path& project, int n) {
    fs::path p = io::autosavePath(project);
    if (n > 1) {
        p += std::to_string(n);
    }
    return p;
}

void stamp(const fs::path& file, int secondsAfterBase) {
    static const fs::file_time_type base = fs::file_time_type::clock::now() - std::chrono::hours(1);
    fs::last_write_time(file, base + std::chrono::seconds(secondsAfterBase));
}

void writeProject(const fs::path& file, int sequences, int at) {
    REQUIRE(io::saveProject(withSequences(sequences), file, io::SaveOptions{0}));
    stamp(file, at);
}

void writeDamaged(const fs::path& file, int at) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "{ \"format\": \"wabcut-project\", half a file";
    stamp(file, at);
}

/// What the scan offered before P.1: the first loadable autosave newer than the project in recoveryFiles().
std::optional<fs::path> previousChoice(const fs::path& project) {
    EditSession session;
    const ProjectDocument doc(session, {.autosaveCopies = 3});
    for (const RecoveryFile& r : doc.recoveryFiles(project)) {
        if (r.kind == RecoveryFile::Kind::Autosave && r.newerThanProject && r.loadable) {
            return r.file;
        }
    }
    return std::nullopt;
}

std::optional<fs::path> lazyChoice(const fs::path& project) {
    const auto r = ProjectDocument::newestUnsavedAutosave(project, 3);
    if (r) {
        CHECK(r->loadable);
        CHECK(r->newerThanProject);
        CHECK(r->kind == RecoveryFile::Kind::Autosave);
    }
    return r ? std::optional<fs::path>(r->file) : std::nullopt;
}

} // namespace

TEST_CASE("The lazy recovery scan offers what the full scan offered, in every situation", "[editor][document][recovery]") {
    SECTION("a clean exit: backups only") {
        const fs::path project = scanDir("clean") / "p.wabcut";
        writeProject(project, 1, 100);
        for (int n = 1; n <= 5; ++n) {
            writeProject(io::backupPath(project, n), 1, 100 - n);
        }
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK_FALSE(lazyChoice(project));
    }
    SECTION("an unclean exit: the newest autosave, newer than the project") {
        const fs::path project = scanDir("crash") / "p.wabcut";
        writeProject(project, 1, 100);
        writeProject(io::backupPath(project, 1), 1, 90);
        writeProject(rotated(project, 3), 2, 101);
        writeProject(rotated(project, 2), 3, 102);
        writeProject(rotated(project, 1), 4, 103);
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK(lazyChoice(project) == rotated(project, 1));
    }
    SECTION("the newest autosave is damaged: the next one that loads and is newer than the project") {
        const fs::path project = scanDir("damaged") / "p.wabcut";
        writeProject(project, 1, 100);
        writeProject(rotated(project, 3), 2, 101);
        writeProject(rotated(project, 2), 3, 102);
        writeDamaged(rotated(project, 1), 103);
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK(lazyChoice(project) == rotated(project, 2));
    }
    SECTION("autosaves older than the project (saved after them): nothing to offer") {
        const fs::path project = scanDir("saved-after") / "p.wabcut";
        writeProject(project, 1, 200);
        writeProject(rotated(project, 1), 2, 150);
        writeProject(rotated(project, 2), 3, 140);
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK_FALSE(lazyChoice(project));
    }
    SECTION("only an older autosave is newer than the project and it is the one that loads") {
        const fs::path project = scanDir("mixed") / "p.wabcut";
        writeProject(project, 1, 100);
        writeDamaged(rotated(project, 1), 120);
        writeProject(rotated(project, 2), 2, 110);
        writeProject(rotated(project, 3), 3, 90); // older than the project: never offered
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK(lazyChoice(project) == rotated(project, 2));
    }
    SECTION("the project file is gone: the newest autosave that loads") {
        const fs::path project = scanDir("missing") / "p.wabcut";
        writeDamaged(rotated(project, 1), 103);
        writeProject(rotated(project, 2), 2, 102);
        writeProject(rotated(project, 3), 3, 101);
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK(lazyChoice(project) == rotated(project, 2));
    }
    SECTION("every autosave is damaged") {
        const fs::path project = scanDir("all-damaged") / "p.wabcut";
        writeProject(project, 1, 100);
        writeDamaged(rotated(project, 1), 103);
        writeDamaged(rotated(project, 2), 102);
        CHECK(lazyChoice(project) == previousChoice(project));
        CHECK_FALSE(lazyChoice(project));
    }
}
