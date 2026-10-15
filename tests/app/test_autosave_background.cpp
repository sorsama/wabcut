// P.1 P-1: the ProjectFile bridge's autosave timer writes on a worker. The GUI thread prepares the job (an immutable
// snapshot) and records it when written; saving, opening and closing the project wait for a write still running, so
// no autosave lands after them. Before, the whole write ran on the GUI thread (~0.25 s every minute on 10,000 clips).

#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QUrl>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace wabcut;
using namespace wabcut::app;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "autosave-background" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QUrl url(const fs::path& p) { return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string())); }

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

void addSequence(ProjectController& controller) {
    REQUIRE(controller.session().apply("add sequence", [](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto created = edit::addSequence(s, edit::SequenceSettings{});
        if (!created) {
            return std::unexpected(created.error());
        }
        return std::move(created->state);
    }));
}

/// Runs the event loop until `done` or 10 s.
template <class Done>
bool processUntil(Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < until) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

} // namespace

TEST_CASE("The autosave timer writes on a worker; saving and closing wait for it", "[app][autosave]") {
    const fs::path dir = workDir("timer");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(1), project, io::SaveOptions{0}));
    const fs::path autosave = io::autosavePath(project);
    {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        REQUIRE(lifecycle.open(url(project)));

        // A tick: the job is written off the GUI thread and recorded from the event loop.
        addSequence(controller);
        const QString before = lifecycle.autosaveText();
        lifecycle.startBackgroundAutosave();
        addSequence(controller); // edits go on while it writes: the job keeps the revision it was prepared from
        REQUIRE(processUntil([&] { return lifecycle.autosaveText() != before; }));
        CHECK(lifecycle.autosaveText().startsWith(QStringLiteral("Autosaved")));
        CHECK(sequencesIn(autosave) == 2);

        // A tick, then Save at once: the save waits for the write, and the project holds the newest revision.
        lifecycle.startBackgroundAutosave();
        REQUIRE(lifecycle.save());
        CHECK_FALSE(lifecycle.dirty());
        CHECK(sequencesIn(project) == 3);

        // A tick, then the project closes (here the bridge goes away): the write finishes first, then the clean exit
        // removes the autosaves, so none is left behind to be offered as a crash recovery.
        addSequence(controller);
        lifecycle.startBackgroundAutosave();
    }
    CHECK_FALSE(fs::exists(autosave));
}
