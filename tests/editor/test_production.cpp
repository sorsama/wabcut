// 0.2 (P.3): Productions and project locks (editor/Production.h) - a folder of projects, Add Project to Production,
// project locking for shared folders, and reading another project for import with its media found where it is.

#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/PreviewRender.h>
#include <wabcut/editor/Production.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "production" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

/// A project with one media item whose file sits next to it in "Media/clip.mov" (found by its relative path only).
ProjectState projectWithMedia(const fs::path& projectFile, bool absolute) {
    const fs::path media = projectFile.parent_path() / "Media" / "clip.mov";
    writeText(media, std::string(1000, 'x'));
    ProjectState s;
    s.ids.next = 1;
    MediaItem item;
    item.name = "clip";
    item.locator.path = absolute ? pathToUtf8(media) : "Z:/Nowhere/clip.mov";
    item.locator.relativePath = "Media/clip.mov";
    item.info.path = item.locator.path;
    auto added = edit::addMedia(s, item);
    REQUIRE(added);
    return added->state;
}

} // namespace

TEST_CASE("A production is a folder of projects with a production file", "[editor][production]") {
    const fs::path dir = scratch("basic");
    auto made = Production::create(dir / "Documentary", "Doc");
    REQUIRE(made);
    CHECK(made->name() == "Doc");
    CHECK(fs::is_regular_file(dir / "Documentary" / std::string(kProductionFileName)));
    CHECK(Production::create(dir / "Documentary").error().code == ErrorCode::InvalidArgument); // already one
    CHECK(Production::create(dir / "Documentary" / "Inner").error().code == ErrorCode::InvalidArgument); // nested

    auto opened = Production::open(dir / "Documentary");
    REQUIRE(opened);
    CHECK(opened->name() == "Doc");
    CHECK(Production::open(dir).error().code == ErrorCode::NotFound);
    REQUIRE(opened->rename("Documentary 2026"));
    CHECK(Production::open(dir / "Documentary")->name() == "Documentary 2026");

    // Folders and projects, depth first, sorted; autosaves, backups, locks and hidden files are left out.
    ProjectState empty;
    REQUIRE(opened->newFolder("", "Episodes"));
    REQUIRE(opened->newFolder("Episodes", "Ep 1"));
    CHECK(opened->newFolder("", "Episodes").error().code == ErrorCode::InvalidArgument);
    CHECK(opened->newFolder("", "a/b").error().code == ErrorCode::InvalidArgument);
    CHECK(opened->newFolder("../outside", "x").error().code == ErrorCode::InvalidArgument);
    const auto assembly = opened->newProject("Episodes/Ep 1", "Assembly", empty);
    REQUIRE(assembly);
    REQUIRE(opened->newProject("", "Selects", empty));
    CHECK(opened->newProject("", "Selects", empty).error().code == ErrorCode::InvalidArgument);
    writeText(dir / "Documentary" / "Selects.wabcut.autosave", "{}");
    writeText(dir / "Documentary" / "Selects.wabcut.bak1", "{}");
    writeText(dir / "Documentary" / ".hidden.wabcut", "{}");
    const std::vector<ProductionEntry> entries = opened->entries();
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].folder);
    CHECK(entries[0].relative == "Episodes");
    CHECK(entries[1].relative == "Episodes/Ep 1");
    CHECK(entries[2].relative == "Episodes/Ep 1/Assembly.wabcut");
    CHECK(entries[2].name == "Assembly");
    CHECK(entries[2].depth == 2);
    CHECK(entries[3].relative == "Selects.wabcut");
    CHECK(Production::folderOf(*assembly) == opened->folder());
    CHECK_FALSE(Production::folderOf(dir / "elsewhere.wabcut").has_value());
    CHECK(opened->contains(*assembly));
    CHECK_FALSE(opened->contains(dir / "elsewhere.wabcut"));
}

TEST_CASE("Projects are locked for one session and open read-only for the others", "[editor][production][lock]") {
    const fs::path dir = scratch("locks");
    const fs::path project = dir / "Film.wabcut";
    REQUIRE(io::saveProject(ProjectState{}, project));

    auto mine = acquireProjectLock(project);
    REQUIRE(mine);
    CHECK_FALSE(mine->has_value()); // ours now
    const auto lock = readProjectLock(project);
    REQUIRE(lock);
    CHECK(lock->pid == currentLockHolder().pid);
    CHECK_FALSE(heldByOther(*lock));
    REQUIRE(acquireProjectLock(project)); // taking our own lock again is fine

    // Another computer's lock: the project is theirs.
    const nlohmann::json theirs{{"format", "wabcut-project-lock"}, {"user", "alex"}, {"host", "EDIT-BAY-2"},
                                {"pid", 4242}, {"since", "2026-10-16T09:00:00Z"}};
    writeText(projectLockPath(project), theirs.dump());
    auto other = acquireProjectLock(project);
    REQUIRE(other);
    REQUIRE(other->has_value());
    CHECK((*other)->owner() == "alex on EDIT-BAY-2");
    releaseProjectLock(project); // not ours: stays
    CHECK(fs::exists(projectLockPath(project)));

    // A lock of this computer whose process is gone is stale: taken over.
    nlohmann::json stale = theirs;
    stale["host"] = currentLockHolder().host;
    stale["pid"] = 0x7ffffff0;
    writeText(projectLockPath(project), stale.dump());
    auto taken = acquireProjectLock(project);
    REQUIRE(taken);
    CHECK_FALSE(taken->has_value());
    releaseProjectLock(project);
    CHECK_FALSE(fs::exists(projectLockPath(project)));

    // Garbage is not a lock.
    writeText(projectLockPath(project), "not json");
    CHECK_FALSE(readProjectLock(project).has_value());
}

TEST_CASE("Production Settings put every project's previews and media cache in the production's folders", "[editor][production][settings]") {
    const fs::path dir = scratch("settings");
    auto production = Production::create(dir / "Feature", "Feature");
    REQUIRE(production);
    CHECK(production->settings() == ProductionSettings{});
    const fs::path project = production->folder() / "Reel 1.wabcut";
    // The default: previews next to the project.
    CHECK(production->previewFolderFor(project).empty());
    CHECK(previewFolderFor(project) == production->folder() / "Reel 1 Preview Files");

    // Relative to the production folder, or absolute; kept in its file.
    const fs::path cache = dir / "Fast SSD" / "Cache";
    REQUIRE(production->setSettings({.previews = "Previews", .mediaCache = pathToUtf8(cache)}));
    CHECK(production->resolve("Previews") == (production->folder() / "Previews").lexically_normal());
    CHECK(production->resolve(pathToUtf8(cache)) == cache.lexically_normal());
    CHECK(production->resolve("").empty());
    CHECK(previewFolderFor(project) == (production->folder() / "Previews" / "Reel 1 Preview Files").lexically_normal());
    REQUIRE(production->rename("Feature (cut)"));
    auto reopened = Production::open(production->folder());
    REQUIRE(reopened);
    CHECK(reopened->name() == "Feature (cut)");
    CHECK(reopened->settings().previews == "Previews");
    CHECK(reopened->settings().mediaCache == pathToUtf8(cache));
    const auto found = productionOf(project);
    REQUIRE(found);
    CHECK(found->settings() == reopened->settings());
    CHECK_FALSE(productionOf(dir / "loose.wabcut").has_value());
    // A project outside a production keeps the default.
    CHECK(previewFolderFor(dir / "loose.wabcut") == dir / "loose Preview Files");

    // Too long: refused, nothing changes.
    CHECK(production->setSettings({.previews = std::string(5000, 'p')}).error().code == ErrorCode::InvalidArgument);
    CHECK(Production::open(production->folder())->settings().previews == "Previews");
    // Back to the defaults: the file has no settings left.
    REQUIRE(production->setSettings({}));
    std::ifstream in(production->folder() / std::string(kProductionFileName), std::ios::binary);
    const nlohmann::json doc = nlohmann::json::parse(in);
    CHECK_FALSE(doc.contains("settings"));
}

TEST_CASE("A project open in memory is prepared for import like one read from its file", "[editor][production][import]") {
    const fs::path dir = scratch("import-open");
    const fs::path source = dir / "Other" / "Other.wabcut";
    fs::create_directories(source.parent_path());
    const ProjectState state = projectWithMedia(source, false);
    const ProjectState ready = preparedForImport(state, source, dir / "Mine" / "Mine.wabcut");
    REQUIRE(ready.media.size() == 1);
    CHECK_FALSE(ready.media.front()->locator.offline);
    CHECK(ready.media.front()->locator.relativePath == "../Other/Media/clip.mov");
}

TEST_CASE("Another project is read for import with its media found where it is", "[editor][production][import]") {
    const fs::path dir = scratch("import");
    const fs::path source = dir / "Other" / "Other.wabcut";
    fs::create_directories(source.parent_path());
    REQUIRE(io::saveProject(projectWithMedia(source, false), source));
    const fs::path target = dir / "Mine" / "Mine.wabcut";

    auto read = loadProjectForImport(source, target);
    REQUIRE(read);
    REQUIRE(read->media.size() == 1);
    const MediaLocator& loc = read->media.front()->locator;
    CHECK_FALSE(loc.offline); // found by its relative path next to the other project
    CHECK(fs::path(pathFromUtf8(loc.path)) == (source.parent_path() / "Media" / "clip.mov").lexically_normal());
    CHECK(loc.relativePath == "../Other/Media/clip.mov"); // made for the importing project
    // Untitled: no relative paths.
    auto untitled = loadProjectForImport(source, {});
    REQUIRE(untitled);
    CHECK(untitled->media.front()->locator.relativePath.empty());

    // Add Project to Production copies the project, its media still playing the same file.
    auto production = Production::create(dir / "Prod");
    REQUIRE(production);
    const auto copy = production->addProject(source);
    REQUIRE(copy);
    CHECK(*copy == production->folder() / "Other.wabcut");
    auto loaded = io::loadProject(*copy);
    REQUIRE(loaded);
    CHECK(fs::path(pathFromUtf8(loaded->media.front()->locator.path)) ==
          (source.parent_path() / "Media" / "clip.mov").lexically_normal());
    const auto second = production->addProject(source);
    REQUIRE(second);
    CHECK(*second == production->folder() / "Other 2.wabcut");
}

TEST_CASE("Importing media fills its clip metadata from the file's XMP sidecar", "[editor][import][xmp]") {
    const fs::path source = fs::path(WABCUT_TEST_MEDIA_DIR) / "sine_48k.wav";
    if (!fs::exists(source)) {
        SKIP("test media not generated");
    }
    const fs::path dir = scratch("import-xmp");
    const fs::path media = dir / "take.wav";
    fs::copy_file(source, media);
    writeText(dir / "take.xmp", R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
<rdf:Description rdf:about="" xmlns:xmpDM="http://ns.adobe.com/xmp/1.0/DynamicMedia/" xmpDM:scene="4" xmpDM:good="True">
<xmpDM:logComment>Room tone</xmpDM:logComment><xmpDM:tapeName>R2</xmpDM:tapeName></rdf:Description></rdf:RDF></x:xmpmeta>)");
    EditSession session{ProjectState{}};
    const std::vector<fs::path> files{media};
    const MediaImportResult result = importMediaFiles(session, files);
    REQUIRE(result.added.size() == 1);
    const MediaItem& item = *session.snapshot()->findMedia(result.added.front());
    CHECK(item.metadata.scene == "4");
    CHECK(item.metadata.good);
    CHECK(item.metadata.logNote == "Room tone");
    REQUIRE(item.timecode);
    CHECK(item.timecode->reelName == "R2");

    // Off: nothing is read.
    EditSession plain{ProjectState{}};
    const MediaImportResult off = importMediaFiles(plain, files, {.readXmp = false});
    REQUIRE(off.added.size() == 1);
    CHECK(plain.snapshot()->findMedia(off.added.front())->metadata.isEmpty());
}
