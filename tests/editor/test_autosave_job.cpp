// P.1 P-1: the autosave in three parts, so its file work runs off the document's thread. prepareAutosave() takes the
// snapshot (immutable), AutosaveJob::write() writes and rotates on any thread, finishAutosave() records the revision.
// Before, autosave() did all of it on the GUI thread: ~0.25 s every minute on a 10,000-clip project.

#include <wabcut/editor/ProjectDocument.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "autosave-job" / name;
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

void rename(EditSession& session, const std::string& name) {
    REQUIRE(session.apply("rename", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto seq = std::make_shared<Sequence>(*s.sequences.front());
        seq->name = name;
        return s.withSequence(seq);
    }));
}

std::string sequenceNameIn(const fs::path& file) {
    auto loaded = io::loadProject(file);
    return loaded && !loaded->sequences.empty() ? loaded->sequences.front()->name : std::string("<unreadable>");
}

fs::path numbered(const fs::path& file, int n) {
    fs::path copy = io::autosavePath(file);
    copy += std::to_string(n);
    return copy;
}

} // namespace

TEST_CASE("An autosave job writes, on another thread, the revision it was prepared from; the document records it",
          "[editor][document][autosave]") {
    const fs::path dir = workDir("thread");
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.autosaveCopies = 3});
    const fs::path file = dir / "job.wabcut";
    REQUIRE(doc.saveAs(file));
    CHECK_FALSE(doc.prepareAutosave()); // nothing changed since the save

    rename(session, "A");
    std::optional<ProjectDocument::AutosaveJob> job = doc.prepareAutosave();
    REQUIRE(job);
    rename(session, "B"); // the user goes on editing while the worker writes
    std::expected<void, Error> written = std::unexpected(Error{ErrorCode::Unknown, "not run"});
    std::thread([&] { written = job->write(); }).join();
    REQUIRE(written);
    doc.finishAutosave(*job);
    CHECK(sequenceNameIn(io::autosavePath(file)) == "A");

    // "B" is newer than what was autosaved: the next job writes it, rotating "A" into the second copy as autosave()
    // always did.
    std::optional<ProjectDocument::AutosaveJob> next = doc.prepareAutosave();
    REQUIRE(next);
    REQUIRE(next->write());
    doc.finishAutosave(*next);
    CHECK(sequenceNameIn(io::autosavePath(file)) == "B");
    CHECK(sequenceNameIn(numbered(file, 2)) == "A");
    CHECK_FALSE(doc.prepareAutosave());
    const std::expected<bool, Error> plain = doc.autosave(); // the two paths agree on what is autosaved
    REQUIRE(plain);
    CHECK_FALSE(*plain);
}

TEST_CASE("A failed autosave job leaves every copy as it was and the revision still to be written",
          "[editor][document][autosave]") {
    const fs::path dir = workDir("failed");
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.autosaveCopies = 3});
    const fs::path file = dir / "job.wabcut";
    REQUIRE(doc.saveAs(file));
    rename(session, "A");
    REQUIRE(doc.autosave());
    rename(session, "B");
    std::optional<ProjectDocument::AutosaveJob> job = doc.prepareAutosave();
    REQUIRE(job);
    fs::path blocked = io::autosavePath(file);
    blocked += ".tmp";
    fs::create_directories(blocked); // the atomic write's temporary file cannot be created
    CHECK_FALSE(job->write());
    fs::remove_all(blocked);
    CHECK(sequenceNameIn(io::autosavePath(file)) == "A");
    CHECK_FALSE(fs::exists(numbered(file, 2)));
    // Not finished: the document still wants "B" written.
    std::optional<ProjectDocument::AutosaveJob> again = doc.prepareAutosave();
    REQUIRE(again);
    REQUIRE(again->write());
    doc.finishAutosave(*again);
    CHECK(sequenceNameIn(io::autosavePath(file)) == "B");
}

// Card 043: the io layer's "cannot create: <path>.autosave.tmp" named a file the user never made. A failed autosave names
// the project (or "the untitled project") and keeps the system's reason after it.
TEST_CASE("A failed autosave names the project, not the temporary file the atomic write used", "[editor][document][autosave]") {
    const fs::path dir = workDir("named");
    EditSession session(oneSequence());
    ProjectDocument doc(session, {.autosaveCopies = 3});
    const fs::path file = dir / "Film.wabcut";
    REQUIRE(doc.saveAs(file));
    rename(session, "A");
    std::optional<ProjectDocument::AutosaveJob> job = doc.prepareAutosave();
    REQUIRE(job);
    fs::path blocked = io::autosavePath(file);
    blocked += ".tmp";
    fs::create_directories(blocked); // the temporary file cannot be created
    const std::expected<void, Error> failed = job->write();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::IoError);
    CHECK(failed.error().message == "cannot create the autosave of Film.wabcut");
    CHECK(failed.error().message.find(".tmp") == std::string::npos);
    CHECK(failed.error().message.find(dir.string()) == std::string::npos);
    fs::remove_all(blocked);

    // An untitled project has no file name to give: it says so, and the path is not shown either.
    const fs::path folder = workDir("named-untitled") / "Autosave";
    EditSession untitledSession(oneSequence());
    ProjectDocument untitled(untitledSession, {.untitledAutosaveDir = folder});
    rename(untitledSession, "A");
    std::optional<ProjectDocument::AutosaveJob> untitledJob = untitled.prepareAutosave();
    REQUIRE(untitledJob);
    fs::path target = untitledJob->target;
    target += ".tmp";
    fs::create_directories(target);
    const std::expected<void, Error> untitledFailed = untitledJob->write();
    REQUIRE_FALSE(untitledFailed);
    CHECK(untitledFailed.error().message == "cannot create the autosave of the untitled project");
    CHECK(untitledFailed.error().message.find("Untitled-") == std::string::npos);
}
