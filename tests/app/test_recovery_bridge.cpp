// Codex wave-3 regressions in the ProjectFile bridge's recovery (docs/internal/reviews-codex-wave3.md):
//   P1-3  Recover (the startup offer) must not delete the only copy of the recovered revision: the revision is autosaved
//         again first, and a failed autosave (disk full, no permission) never discards the recovery file.
//   P2-11 Open Recovery Copy resolves media relative to the copy's project folder, so a relocated project's
//         autosave/backup copies find their media; the copy stays untitled and dirty.
// Both failed before their fixes (the recovery file was removed unconditionally; relative media came up offline).

#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::app;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "wave3" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QUrl url(const fs::path& p) {
    return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string()));
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

int sequencesIn(const fs::path& file) {
    auto loaded = io::loadProject(file);
    return loaded ? static_cast<int>(loaded->sequences.size()) : -1;
}

/// The newest loadable autosave of `project` holds `sequences` sequences (what a restart after a crash would offer).
int newestAutosaveSequences(const fs::path& project) {
    EditSession session;
    editor::ProjectDocument doc(session);
    for (const editor::RecoveryFile& r : doc.recoveryFiles(project)) {
        if (r.kind == editor::RecoveryFile::Kind::Autosave && r.loadable) {
            return sequencesIn(r.file);
        }
    }
    return -1;
}

/// The bridge's untitled autosave folder, replaced by a file for the scope (writes there fail).
struct BlockedUntitledAutosaves {
    fs::path folder;
    fs::path aside;
    BlockedUntitledAutosaves() {
        folder = fs::path(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toStdU16String()) / "Autosave";
        aside = folder;
        aside += ".wave3-aside";
        std::error_code ec;
        fs::remove_all(aside, ec);
        if (fs::exists(folder)) {
            fs::rename(folder, aside, ec);
        }
        fs::create_directories(folder.parent_path(), ec);
        std::ofstream(folder) << "not a folder";
    }
    ~BlockedUntitledAutosaves() {
        std::error_code ec;
        fs::remove(folder, ec);
        if (fs::exists(aside)) {
            fs::rename(aside, folder, ec);
        }
    }
};

} // namespace

TEST_CASE("Recover (wave-3 P1-3): the recovered revision stays on disk until it is autosaved again",
          "[app][recovery][wave3]") {
    const fs::path dir = workDir("recover-titled");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(1), project, io::SaveOptions{0}));
    const fs::path autosave = io::autosavePath(project);
    REQUIRE(io::saveProject(withSequences(2), autosave, io::SaveOptions{0})); // what the crash left
    QSettings().setValue(QStringLiteral("project/lastFile"), QString::fromStdU16String(project.u16string()));

    SECTION("recovered, then the process dies at once: the revision is still there") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        REQUIRE(lifecycle.recover(url(autosave)));
        CHECK(lifecycle.dirty());
        CHECK(newestAutosaveSequences(project) == 2);
    }
    SECTION("the autosave cannot be written: the recovery file is never discarded") {
        fs::path blocked = autosave;
        blocked += ".tmp";
        fs::create_directories(blocked); // the atomic write's temporary file cannot be created
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        REQUIRE(lifecycle.recover(url(autosave)));
        CHECK(fs::exists(autosave));
        CHECK(sequencesIn(autosave) == 2);
        fs::remove_all(blocked);
    }
}

TEST_CASE("Recover (wave-3 P1-3): an untitled recovery whose new autosave fails keeps its file", "[app][recovery][wave3]") {
    const fs::path dir = workDir("recover-untitled");
    const fs::path recovery = dir / "Untitled-5-1.wabcut.autosave";
    REQUIRE(io::saveProject(withSequences(2), recovery, io::SaveOptions{0}));
    BlockedUntitledAutosaves blocked;
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.recover(url(recovery)));
    CHECK(fs::exists(recovery));
    CHECK(sequencesIn(recovery) == 2);
}

TEST_CASE("Open Recovery Copy (wave-3 P2-11): a relocated project's copies find their relative media",
          "[app][recovery][wave3]") {
    const fs::path dir = workDir("recovery-copy");
    const fs::path moved = dir / "moved";
    fs::create_directories(moved / "Media");
    std::ofstream(moved / "Media" / "clip.bin") << "media next to the copies";
    MediaItem media;
    media.name = "clip";
    media.locator.path = "Z:/no-such-drive/old-place/Media/clip.bin";
    media.locator.relativePath = "Media/clip.bin";
    auto added = edit::addMedia(withSequences(1), media);
    REQUIRE(added);
    const fs::path project = moved / "film.wabcut";
    for (const fs::path& copy : {io::autosavePath(project), io::backupPath(project, 2)}) {
        INFO(copy.filename().string());
        REQUIRE(io::saveProject(added->state, copy, io::SaveOptions{0}));
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        REQUIRE(lifecycle.openRecoveryCopy(url(copy)));
        CHECK(lifecycle.offlineCount() == 0);
        CHECK(lifecycle.document().isUntitled());
        CHECK(lifecycle.dirty());
    }
}

// I.7: the installer's opt-in .wabcut association starts `wabcut.exe "<file>"`; the first argument naming an existing
// .wabcut file is the project to open, anything else (options, --rhi's value, other files, missing files) is ignored.
TEST_CASE("A .wabcut on the command line is the project to open; other arguments are ignored", "[app][launch]") {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "launch";
    std::filesystem::create_directories(dir);
    const std::filesystem::path project = dir / "My Film.WABCUT";
    std::ofstream(project) << "{}";
    std::ofstream(dir / "notes.txt") << "x";
    const QString file = QString::fromStdU16String(project.u16string());
    const QString notes = QString::fromStdU16String((dir / "notes.txt").u16string());
    const QString missing = QString::fromStdU16String((dir / "gone.wabcut").u16string());
    CHECK(ProjectLifecycle::launchProjectIn({QStringLiteral("wabcut.exe"), file}) == QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()));
    CHECK(ProjectLifecycle::launchProjectIn({QStringLiteral("wabcut.exe"), QStringLiteral("--rhi"), QStringLiteral("d3d11"),
                                             notes, missing, file})
              .toLocalFile() == QFileInfo(file).absoluteFilePath());
    CHECK(ProjectLifecycle::launchProjectIn({QStringLiteral("wabcut.exe"), notes, missing}).isEmpty());
    CHECK(ProjectLifecycle::launchProjectIn({QStringLiteral("wabcut.exe"), QStringLiteral("--rhi"), file}).isEmpty()); // --rhi's value
    CHECK(ProjectLifecycle::launchProjectIn({QStringLiteral("wabcut.exe")}).isEmpty());
}
