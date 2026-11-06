// 0.7 (card 029): where a project's ".bakN" copies and ".autosave" files are kept - beside it (before 0.7), in its
// "Wabcut Auto-Save" folder, or in a chosen root with one folder per project - and how recovery finds them in all three.

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace wabcut;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "model-backup-location" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

ProjectState withSequences(int n) {
    ProjectState state;
    for (int i = 0; i < n; ++i) {
        auto created = edit::addSequence(state, edit::SequenceSettings{});
        REQUIRE(created);
        state = std::move(created->state);
    }
    return state;
}

io::BackupLocation folderMode() {
    io::BackupLocation l;
    l.mode = io::BackupLocation::Mode::Folder;
    return l;
}

io::BackupLocation chosenMode(const fs::path& root) {
    io::BackupLocation l;
    l.mode = io::BackupLocation::Mode::Chosen;
    l.root = root;
    return l;
}

} // namespace

TEST_CASE("backupDirectory: beside, the Auto-Save folder or one folder per project in a chosen root", "[model][backup]") {
    const fs::path dir = workDir("directories");
    const fs::path film = dir / "Shows" / "Film.wabcut";
    const fs::path otherFilm = dir / "Ads" / "Film.wabcut";

    CHECK(io::backupDirectory(film, {}).empty()); // beside: how it always was
    CHECK(io::backupDirectory(fs::path{}, folderMode()).empty()); // an untitled project has no folder

    CHECK(io::backupDirectory(film, folderMode()) == dir / "Shows" / "Wabcut Auto-Save");

    const fs::path root = dir / "Backups";
    const fs::path chosen = io::backupDirectory(film, chosenMode(root));
    REQUIRE_FALSE(chosen.empty());
    CHECK(chosen.parent_path() == root);
    CHECK(chosen.filename().string().starts_with("Film-"));
    CHECK(chosen.filename().string().size() == std::string("Film-").size() + 8); // 8 hex digits of its folder
    // The same name in another folder is another project: its copies do not mix with these.
    CHECK(io::backupDirectory(otherFilm, chosenMode(root)) != chosen);
    CHECK(io::backupDirectory(film, chosenMode(root)) == chosen); // stable
    // A chosen location without a root falls back to beside.
    CHECK(io::backupDirectory(film, chosenMode({})).empty());

    // Where recovery looks: the setting's folder first, then beside and the other two.
    const auto dirs = io::backupDirectories(film, folderMode());
    REQUIRE(dirs.size() >= 2);
    CHECK(dirs.front() == dir / "Shows" / "Wabcut Auto-Save");
    CHECK(std::find(dirs.begin(), dirs.end(), fs::path{}) != dirs.end());
    const auto withRoot = io::backupDirectories(film, chosenMode(root));
    CHECK(withRoot.front() == chosen);
    CHECK(withRoot.size() == 3);
    CHECK(io::backupDirectories(film, {}).front().empty()); // beside is first when it is the setting
}

TEST_CASE("Names of the copies in a backup folder", "[model][backup]") {
    const fs::path file = fs::path("D:") / "Work" / "Film.wabcut";
    const fs::path dir = fs::path("D:") / "Backups" / "Film-12345678";
    CHECK(io::backupPath(file, 2, dir) == dir / "Film.wabcut.bak2");
    CHECK(io::autosavePath(file, dir) == dir / "Film.wabcut.autosave");
    CHECK(io::backupPath(file, 2, {}) == io::backupPath(file, 2));
    CHECK(io::autosavePath(file, {}) == io::autosavePath(file));
    CHECK(io::inBackupDirectory(file, {}) == file);
}

TEST_CASE("saveProject rotates its copies into the backup folder and leaves the project where it is", "[model][backup]") {
    const fs::path dir = workDir("rotate");
    const fs::path file = dir / "film.wabcut";
    const fs::path copies = dir / "Wabcut Auto-Save";

    REQUIRE(io::saveProject(withSequences(1), file, io::SaveOptions{3, copies}));
    CHECK_FALSE(fs::exists(copies)); // the first save has no earlier version to keep
    for (int i = 2; i <= 6; ++i)
        REQUIRE(io::saveProject(withSequences(i), file, io::SaveOptions{3, copies}));

    CHECK(fs::exists(file));
    CHECK(fs::exists(io::backupPath(file, 1, copies)));
    CHECK(fs::exists(io::backupPath(file, 2, copies)));
    CHECK(fs::exists(io::backupPath(file, 3, copies)));
    CHECK_FALSE(fs::exists(io::backupPath(file, 4, copies))); // only as many as asked for
    CHECK_FALSE(fs::exists(io::backupPath(file, 1)));         // nothing beside the project
    // Newest first: .bak1 is the save before the last one.
    CHECK(io::loadProject(io::backupPath(file, 1, copies))->sequences.size() == 5);
    CHECK(io::loadProject(io::backupPath(file, 3, copies))->sequences.size() == 3);
    CHECK(io::loadProject(file)->sequences.size() == 6);
    // No temporary file is left behind.
    for (const auto& entry : fs::directory_iterator(dir))
        CHECK(entry.path().extension() != ".tmp");

    // 0 copies: none are written.
    const fs::path none = dir / "none.wabcut";
    REQUIRE(io::saveProject(withSequences(1), none, io::SaveOptions{0, copies}));
    REQUIRE(io::saveProject(withSequences(2), none, io::SaveOptions{0, copies}));
    CHECK_FALSE(fs::exists(io::backupPath(none, 1, copies)));
}

TEST_CASE("writeAutosave writes into the backup folder, creating it", "[model][backup]") {
    const fs::path dir = workDir("autosave");
    const fs::path file = dir / "film.wabcut";
    const fs::path copies = dir / "Sub" / "Wabcut Auto-Save";
    REQUIRE(io::writeAutosave(withSequences(2), file, copies));
    CHECK(fs::exists(io::autosavePath(file, copies)));
    CHECK_FALSE(fs::exists(io::autosavePath(file)));
    CHECK(io::loadProject(io::autosavePath(file, copies))->sequences.size() == 2);
}

TEST_CASE("recoveryCandidates finds copies beside the project, in its Auto-Save folder and in the chosen root", "[model][backup]") {
    const fs::path dir = workDir("recovery");
    const fs::path file = dir / "film.wabcut";
    const fs::path root = dir / "Elsewhere";
    const io::BackupLocation location = chosenMode(root);
    REQUIRE(io::saveProject(withSequences(1), file, io::SaveOptions{0}));

    // A copy of every place a setting of today or of an earlier time put one.
    fs::create_directories(dir / "Wabcut Auto-Save");
    REQUIRE(io::saveProject(withSequences(2), io::backupPath(file, 1), io::SaveOptions{0}));                       // beside
    REQUIRE(io::saveProject(withSequences(3), io::backupPath(file, 2, dir / "Wabcut Auto-Save"), io::SaveOptions{0})); // Auto-Save
    REQUIRE(io::writeAutosave(withSequences(4), file, io::backupDirectory(file, location)));                      // chosen root
    REQUIRE(io::saveProject(withSequences(5), io::backupPath(file, 10, dir / "Wabcut Auto-Save"), io::SaveOptions{0})); // a larger earlier setting

    const auto candidates = io::recoveryCandidates(file, location);
    CHECK(candidates.size() == 4);
    for (const auto& c : candidates)
        CHECK(c.loadable);
    int autosaves = 0;
    int backups = 0;
    for (const auto& c : candidates) {
        (c.kind == io::RecoveryCandidate::Kind::Autosave ? autosaves : backups) += 1;
    }
    CHECK(autosaves == 1);
    CHECK(backups == 3);
    // Without the root in the setting the root's copy is not found (the root is only known from the setting).
    CHECK(io::recoveryCandidates(file, {}).size() == 3);
}
