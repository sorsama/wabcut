// P0 round: OBS chapter markers on import (editor/MediaImport.h): a recording's chapters become clip markers of its new
// media item in the same "Import" undo step - Cyan point markers named by their titles or "OBS marker n", a lone
// chapter at 00:00 and chapters past the end left out, at most 1,000 per file - and MediaImportOptions::readChapters
// (Preferences > Media) switches it off. The recordings come from tools/gen_test_media.py.

#include <wabcut/editor/MediaFormats.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/media/Chapters.h>

#include <wabcut/command/Engine.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) { return fs::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveChapterMedia() {
    return fs::exists(testMedia("obs_chapters.mkv")) && fs::exists(testMedia("obs_chapter_zero.mkv"));
}

Ticks ms(std::int64_t milliseconds) { return Ticks{milliseconds * (kTicksPerSecond / 1000)}; }

/// media.importCameraClips (the Media Browser's import) over the host's media format services.
command::CommandResult importClips(EditSession& session, bool readChapters, command::Json params) {
    command::EnginePolicy policy;
    policy.services = mediaFormatServices({}, readChapters);
    command::CommandEngine engine(session, command::builtinCommands(), std::move(policy));
    command::CommandRequest request;
    request.command = "media.importCameraClips";
    request.params = std::move(params);
    return engine.execute(request);
}

media::MediaChapter chapter(std::int64_t startMs, std::string title = {}) {
    return media::MediaChapter{ms(startMs), ms(startMs), std::move(title)};
}

} // namespace

TEST_CASE("Chapter import: an OBS recording's chapters become Cyan clip markers in the one Import undo step",
          "[editor][import][chapters]") {
    if (!haveChapterMedia()) {
        SKIP("test media missing");
    }
    EditSession session(ProjectState{});
    const std::vector<fs::path> files{testMedia("obs_chapters.mkv")};
    const MediaImportResult r = importMediaFiles(session, files);
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 1);
    // Five chapters in the file; the one at 5 s lies past its 4 s end.
    CHECK(r.chapterMarkers == 4);
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Import");

    const MediaItem* item = session.snapshot()->findMedia(r.added.front());
    REQUIRE(item != nullptr);
    REQUIRE(item->markers.size() == 4);
    const std::vector<Ticks> starts{ms(1000), ms(2000), ms(2500), ms(3000)};
    for (std::size_t i = 0; i < item->markers.size(); ++i) {
        const Marker& m = item->markers[i];
        INFO("marker " << i);
        CHECK(m.start == starts[i]);
        CHECK(m.duration == Ticks{});
        CHECK(m.type == MarkerType::Comment);
        CHECK(m.colorIndex == kChapterMarkerColor);
        CHECK(m.comment.empty());
    }
    CHECK(item->markers[0].name == "Boss fight");
    CHECK(item->markers[1].name == "OBS marker 2"); // untitled: numbered in time order
    CHECK(item->markers[2].name == "Clutch \xEF\xBF\xBD win");
    CHECK(item->markers[3].name.size() <= media::kMaxChapterTitleBytes);

    // The markers are part of the item: they save and load with the project.
    const auto json = io::toJson(*session.snapshot());
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    const MediaItem* reloaded = loaded->findMedia(r.added.front());
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->markers == item->markers);

    // One undo takes the item away with its markers.
    REQUIRE(session.undo());
    CHECK(session.snapshot()->media.empty());
    REQUIRE(session.redo());
    CHECK(session.snapshot()->findMedia(r.added.front())->markers.size() == 4);
}

TEST_CASE("Chapter import: a lone chapter at 00:00 and the preference off make no markers", "[editor][import][chapters]") {
    if (!haveChapterMedia()) {
        SKIP("test media missing");
    }
    {
        EditSession session(ProjectState{});
        const std::vector<fs::path> files{testMedia("obs_chapter_zero.mkv")};
        const MediaImportResult r = importMediaFiles(session, files);
        REQUIRE(r.added.size() == 1);
        CHECK(r.chapterMarkers == 0);
        CHECK(session.snapshot()->findMedia(r.added.front())->markers.empty());
    }
    {
        // Preferences > Media "Chapter markers in recordings become clip markers" off.
        EditSession session(ProjectState{});
        const std::vector<fs::path> files{testMedia("obs_chapters.mkv")};
        const MediaImportResult r = importMediaFiles(session, files, {.readChapters = false});
        REQUIRE(r.added.size() == 1);
        CHECK(r.chapterMarkers == 0);
        CHECK(session.snapshot()->findMedia(r.added.front())->markers.empty());
        CHECK(session.undoCount() == 1);
    }
    {
        // A file already in the project is not marked again.
        EditSession session(ProjectState{});
        const std::vector<fs::path> files{testMedia("obs_chapters.mkv")};
        REQUIRE(importMediaFiles(session, files).chapterMarkers == 4);
        const MediaImportResult again = importMediaFiles(session, files);
        CHECK(again.added.empty());
        CHECK(again.chapterMarkers == 0);
        CHECK(session.undoCount() == 1);
    }
}

TEST_CASE("Chapter markers: sorted by time with one marker per moment and a title winning", "[editor][import][chapters]") {
    // Out of order, the same moment twice (untitled first), one past the end.
    const std::vector<media::MediaChapter> chapters{chapter(3000, "Third"), chapter(1000), chapter(2000),
                                                    chapter(2000, "Two"), chapter(9000, "Late")};
    const std::vector<Marker> marks = chapterMarkers(chapters, ms(5000));
    REQUIRE(marks.size() == 3);
    CHECK(marks[0].start == ms(1000));
    CHECK(marks[0].name == "OBS marker 1");
    CHECK(marks[1].start == ms(2000));
    CHECK(marks[1].name == "Two");
    CHECK(marks[2].start == ms(3000));
    CHECK(marks[2].name == "Third");
    // Without a known length nothing is past the end.
    CHECK(chapterMarkers(chapters).size() == 4);
}

TEST_CASE("Chapter markers: a lone chapter at the start is skipped and a file gives at most 1000", "[editor][import][chapters]") {
    CHECK(chapterMarkers({chapter(0, "Chapter 1")}).empty());
    CHECK(chapterMarkers({chapter(50)}).empty()); // within the first tenth of a second
    REQUIRE(chapterMarkers({chapter(1000)}).size() == 1);
    // Two chapters, the first at 00:00: both are the user's.
    CHECK(chapterMarkers({chapter(0), chapter(1000)}).size() == 2);

    std::vector<media::MediaChapter> many;
    for (int i = 0; i < 1500; ++i)
        many.push_back(chapter(1000 + i * 10));
    const std::vector<Marker> capped = chapterMarkers(many);
    REQUIRE(capped.size() == kMaxChapterMarkers);
    CHECK(capped.back().name == "OBS marker 1000");
}

TEST_CASE("Chapter import: the Media Browser import and its Ingest copy bring the chapter markers unless switched off",
          "[editor][import][chapters][camera-cards]") {
    if (!haveChapterMedia()) {
        SKIP("test media missing");
    }
    const std::string file = pathToUtf8(testMedia("obs_chapters.mkv"));
    {
        EditSession session(ProjectState{});
        const command::CommandResult r = importClips(session, true, {{"files", command::Json::array({file})}});
        INFO((r.error ? r.error->message : std::string()));
        REQUIRE(r.ok());
        CHECK(r.data["markers"] == 4);
        REQUIRE(r.data["imported"].size() == 1);
        CHECK(r.data["imported"][0]["markers"] == 4);
        CHECK(session.undoCount() == 1);
        const ProjectSnapshot snap = session.snapshot();
        REQUIRE(snap->media.size() == 1);
        const MediaItem& item = *snap->media.front();
        REQUIRE(item.markers.size() == 4);
        CHECK(item.markers[0].name == "Boss fight");
        CHECK(item.markers[1].name == "OBS marker 2");
        CHECK(item.markers[0].colorIndex == kChapterMarkerColor);
        // One undo takes the item and its markers away.
        REQUIRE(session.undo());
        CHECK(session.snapshot()->media.empty());
    }
    {
        // Ingest (Copy): the copy is read, and it has the same chapters.
        const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "chapter-ingest";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        EditSession session(ProjectState{});
        const command::CommandResult r =
            importClips(session, true, {{"files", command::Json::array({file})}, {"copyTo", pathToUtf8(dir)}});
        INFO((r.error ? r.error->message : std::string()));
        REQUIRE(r.ok());
        CHECK(r.data["markers"] == 4);
        const MediaItem& item = *session.snapshot()->media.front();
        CHECK(item.locator.path != file); // the copy
        CHECK(item.markers.size() == 4);
    }
    {
        // Preferences > Media off: no markers, and none counted.
        EditSession session(ProjectState{});
        const command::CommandResult r = importClips(session, false, {{"files", command::Json::array({file})}});
        REQUIRE(r.ok());
        CHECK(r.data["markers"] == 0);
        CHECK_FALSE(r.data["imported"][0].contains("markers"));
        CHECK(session.snapshot()->media.front()->markers.empty());
    }
    {
        // A lone chapter at 00:00 and a file without chapters: nothing.
        EditSession session(ProjectState{});
        const command::CommandResult r = importClips(
            session, true,
            {{"files", command::Json::array({pathToUtf8(testMedia("obs_chapter_zero.mkv")), pathToUtf8(testMedia("sine_48k.wav"))})}});
        REQUIRE(r.ok());
        CHECK(r.data["markers"] == 0);
        for (const MediaPtr& m : session.snapshot()->media)
            CHECK(m->markers.empty());
    }
}

TEST_CASE("Chapter markers: addChapterMarkers keeps the item's markers in time order and skips stills and generators",
          "[editor][import][chapters]") {
    if (!haveChapterMedia()) {
        SKIP("test media missing");
    }
    auto described = describeMediaFile(testMedia("obs_chapters.mkv"));
    REQUIRE(described);
    MediaItem item = *described;
    Marker mine;
    mine.start = ms(1500);
    mine.name = "Mine";
    item.markers.push_back(mine);
    CHECK(addChapterMarkers(item) == 4);
    REQUIRE(item.markers.size() == 5);
    CHECK(item.markers[1].name == "Mine"); // between 1 s and 2 s
    for (std::size_t i = 1; i < item.markers.size(); ++i)
        CHECK(item.markers[i - 1].start <= item.markers[i].start);
    MediaItem still = *described;
    still.markers.clear();
    still.info.isStillImage = true;
    CHECK(addChapterMarkers(still) == 0);
    CHECK(still.markers.empty());
}
