// 0.7 (card 029): a ProjectDocument that keeps its ".bakN" copies and autosave files in the project's "Wabcut Auto-Save"
// folder or in a chosen root, how many it keeps, and that recovery, the version list and the clean-up find and remove
// them wherever an earlier setting put them.

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/Recovery.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <string>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "backup-policy" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

ProjectState oneSequence() {
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    return std::move(created->state);
}

/// A new revision: the sequence gets this name.
void rename(EditSession& session, const std::string& name) {
    REQUIRE(session.apply("rename", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto seq = std::make_shared<Sequence>(*s.sequences.front());
        seq->name = name;
        return s.withSequence(seq);
    }));
}

std::string nameIn(const fs::path& file) {
    auto loaded = io::loadProject(file);
    return loaded && !loaded->sequences.empty() ? loaded->sequences.front()->name : std::string("<unreadable>");
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

TEST_CASE("Saves and autosaves go to the Auto-Save folder, the project stays where it is", "[editor][backup]") {
    const fs::path dir = workDir("folder");
    const fs::path file = dir / "film.wabcut";
    const fs::path copies = dir / "Wabcut Auto-Save";
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.saveBackups = 2, .autosaveCopies = 3, .backupLocation = folderMode()});
    REQUIRE(doc.saveAs(file));
    for (const char* name : {"A", "B", "C", "D"}) {
        rename(session, name);
        REQUIRE(doc.save());
    }
    CHECK(nameIn(file) == "D");
    CHECK(nameIn(io::backupPath(file, 1, copies)) == "C");
    CHECK(nameIn(io::backupPath(file, 2, copies)) == "B");
    CHECK_FALSE(fs::exists(io::backupPath(file, 3, copies))); // two backups kept
    CHECK_FALSE(fs::exists(io::backupPath(file, 1)));         // none beside the project
    CHECK(doc.backupDirectory(file) == copies);

    // Autosaves: the newest, then the rotated older ones, all in the folder.
    for (const char* name : {"E", "F", "G", "H"}) {
        rename(session, name);
        REQUIRE(doc.autosave().value());
    }
    CHECK(doc.autosaveFile() == io::autosavePath(file, copies));
    CHECK(nameIn(io::autosavePath(file, copies)) == "H");
    CHECK(nameIn(io::autosavePath(file, copies).string() + "2") == "G");
    CHECK(nameIn(io::autosavePath(file, copies).string() + "3") == "F");
    CHECK_FALSE(fs::exists(io::autosavePath(file, copies).string() + "4")); // three kept
    CHECK_FALSE(fs::exists(io::autosavePath(file)));                        // none beside the project

    // The version list and recovery see them.
    const std::vector<RecoveryFile> found = doc.recoveryFiles(file);
    int inFolder = 0;
    for (const RecoveryFile& r : found)
        inFolder += r.file.parent_path() == copies ? 1 : 0;
    CHECK(inFolder == 5); // 2 backups + 3 autosaves
    const auto newest = ProjectDocument::newestUnsavedAutosave(file, 3, folderMode());
    REQUIRE(newest);
    CHECK(newest->file == io::autosavePath(file, copies));
    // Every place is searched, whatever the setting says: the answer is the same with the default location.
    const auto sameAnswer = ProjectDocument::newestUnsavedAutosave(file, 3, io::BackupLocation{});
    REQUIRE(sameAnswer);
    CHECK(sameAnswer->file == newest->file);
    const std::vector<RecoveryEntry> versions = recoveryEntries(file, {}, 3, folderMode());
    int entriesInFolder = 0;
    for (const RecoveryEntry& e : versions)
        entriesInFolder += e.file.parent_path() == copies ? 1 : 0;
    CHECK(entriesInFolder == 5);

    // A clean exit removes the autosaves, and only those.
    doc.removeAutosaves();
    CHECK_FALSE(fs::exists(io::autosavePath(file, copies)));
    CHECK_FALSE(fs::exists(io::autosavePath(file, copies).string() + "2"));
    CHECK(fs::exists(io::backupPath(file, 1, copies)));
}

TEST_CASE("A chosen root keeps one folder per project", "[editor][backup]") {
    const fs::path dir = workDir("chosen");
    const fs::path root = dir / "Elsewhere";
    fs::create_directories(dir / "one");
    fs::create_directories(dir / "two");
    const fs::path first = dir / "one" / "film.wabcut";
    const fs::path second = dir / "two" / "film.wabcut"; // the same name
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.saveBackups = 2, .autosaveCopies = 2, .backupLocation = chosenMode(root)});
    REQUIRE(doc.saveAs(first));
    rename(session, "A");
    REQUIRE(doc.save());
    REQUIRE(doc.saveAs(second));
    rename(session, "B");
    REQUIRE(doc.save());

    const fs::path firstDir = io::backupDirectory(first, chosenMode(root));
    const fs::path secondDir = io::backupDirectory(second, chosenMode(root));
    REQUIRE(firstDir != secondDir);
    CHECK(fs::exists(io::backupPath(first, 1, firstDir)));
    CHECK(fs::exists(io::backupPath(second, 1, secondDir)));
    CHECK(nameIn(io::backupPath(second, 1, secondDir)) != nameIn(io::backupPath(first, 1, firstDir)));
    CHECK_FALSE(fs::exists(io::backupPath(first, 1)));
}

TEST_CASE("Changing the setting keeps the earlier copies findable and the count obeyed", "[editor][backup]") {
    const fs::path dir = workDir("change");
    const fs::path file = dir / "film.wabcut";
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.saveBackups = 3, .autosaveCopies = 3}); // beside, like before 0.7
    REQUIRE(doc.saveAs(file));
    rename(session, "A");
    REQUIRE(doc.save());
    rename(session, "B");
    REQUIRE(doc.autosave().value());
    CHECK(fs::exists(io::backupPath(file, 1)));
    CHECK(fs::exists(io::autosavePath(file)));

    // The user moves them into the Auto-Save folder and asks for one backup only.
    doc.setBackupPolicy(1, 2, folderMode());
    CHECK(doc.saveBackups() == 1);
    CHECK(doc.autosaveCopies() == 2);
    rename(session, "C");
    REQUIRE(doc.save());
    rename(session, "D");
    REQUIRE(doc.save());
    const fs::path copies = dir / "Wabcut Auto-Save";
    CHECK(fs::exists(io::backupPath(file, 1, copies)));
    CHECK_FALSE(fs::exists(io::backupPath(file, 2, copies)));
    // The copies from before are still found (and a recovery after a crash would offer them).
    bool beside = false;
    for (const RecoveryFile& r : doc.recoveryFiles(file))
        beside = beside || r.file == io::backupPath(file, 1);
    CHECK(beside);
    CHECK(ProjectDocument::newestUnsavedAutosave(file, 2, folderMode()).has_value() == false); // the project is newer than them

    // The numbers are bounded: 0-10 backups, 1-10 autosaves.
    doc.setBackupPolicy(99, 0, io::BackupLocation{});
    CHECK(doc.saveBackups() == io::kMaxBackupCount);
    CHECK(doc.autosaveCopies() == 1);
    doc.setBackupPolicy(-3, 99, io::BackupLocation{});
    CHECK(doc.saveBackups() == 0);
    CHECK(doc.autosaveCopies() == io::kMaxAutosaveCopies);

    // The clean-up finds the autosaves wherever they went: the old place and the new one.
    doc.setBackupPolicy(3, 3, folderMode());
    rename(session, "E");
    REQUIRE(doc.autosave().value());
    REQUIRE(fs::exists(io::autosavePath(file)));  // the one from before the change, beside the project
    REQUIRE(fs::exists(io::autosavePath(file, copies)));
    doc.removeAutosaves();
    CHECK_FALSE(fs::exists(io::autosavePath(file)));
    CHECK_FALSE(fs::exists(io::autosavePath(file, copies)));
}

TEST_CASE("A copy in the Auto-Save folder belongs to the project next to that folder", "[editor][backup]") {
    const fs::path dir = workDir("project-of");
    CHECK(ProjectDocument::projectOfRecoveryFile(dir / "Wabcut Auto-Save" / "film.wabcut.bak2") == dir / "film.wabcut");
    CHECK(ProjectDocument::projectOfRecoveryFile(dir / "Wabcut Auto-Save" / "film.wabcut.autosave3") == dir / "film.wabcut");
    CHECK(ProjectDocument::projectOfRecoveryFile(dir / "film.wabcut.bak1") == dir / "film.wabcut"); // as before
    // A recovery file opened as an untitled copy resolves media against the project the app names.
    const fs::path project = dir / "film.wabcut";
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.backupLocation = folderMode()});
    REQUIRE(doc.saveAs(project));
    rename(session, "A");
    REQUIRE(doc.save());
    ProjectDocument other(session);
    REQUIRE(other.recover(io::backupPath(project, 1, dir / "Wabcut Auto-Save"), {}, project));
    CHECK(other.isUntitled());
    CHECK(other.isDirty());
}
