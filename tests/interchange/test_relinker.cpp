// MediaRelinker: exact path, project-relative, search roots; facts filter candidates; files are never
// touched.

#include "InterchangeTestSupport.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/interchange/MediaRelinker.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

MediaReference referenceTo(const std::string& path, const std::string& name = {}) {
    MediaReference r;
    r.original = path;
    r.path = path;
    r.name = name;
    return r;
}

} // namespace

TEST_CASE("interchange: relink: the exact path wins", "[interchange][relink]") {
    const auto dir = scratchDir("relink-exact");
    writeFile(dir / "a.mov", "x");
    MediaRelinker relinker({});
    const RelinkResult r = relinker.resolve(referenceTo(utf8(dir / "a.mov")));
    CHECK(r.method == RelinkMethod::ExactPath);
    CHECK(fs::equivalent(r.resolved, dir / "a.mov"));
    CHECK(r.found());
}

TEST_CASE("interchange: relink: project-relative references and moved folder structures",
          "[interchange][relink]") {
    const auto dir = scratchDir("relink-relative");
    const auto project = dir / "Project";
    writeFile(project / "Media" / "Cam A" / "c1.mov", "x");
    writeFile(dir / "Audio" / "vo.wav", "x");
    RelinkOptions options;
    options.projectDirectory = project;
    MediaRelinker relinker(options);

    SECTION("a relative reference resolves against the imported file's folder") {
        const RelinkResult r = relinker.resolve(referenceTo("Media/Cam A/c1.mov"));
        CHECK(r.method == RelinkMethod::ProjectRelative);
        CHECK(fs::equivalent(r.resolved, project / "Media" / "Cam A" / "c1.mov"));
    }
    SECTION("the tail of an absolute path from another machine") {
        const RelinkResult r = relinker.resolve(referenceTo("/Volumes/Old Drive/Show/Media/Cam A/c1.mov"));
        CHECK(r.method == RelinkMethod::ProjectRelative);
        CHECK(fs::equivalent(r.resolved, project / "Media" / "Cam A" / "c1.mov"));
        CHECK(r.tried.size() >= 2); // the longer tails were tried first
    }
    SECTION("the folder above the project") {
        const RelinkResult r = relinker.resolve(referenceTo("D:/Show/Audio/vo.wav"));
        CHECK(r.method == RelinkMethod::ProjectRelative);
        CHECK(fs::equivalent(r.resolved, dir / "Audio" / "vo.wav"));
    }
}

TEST_CASE("interchange: relink: file-name search under the roots", "[interchange][relink]") {
    const auto dir = scratchDir("relink-search");
    writeFile(dir / "Day 1" / "Card A" / "C0001.MP4", "aaaa");
    writeFile(dir / "Day 2" / "Card B" / "C0001.MP4", "bbbbbbbb");
    writeFile(dir / "Day 2" / "Sound" / "Scene 4.WAV", "x");
    RelinkOptions options;
    options.searchRoots = {dir};
    MediaRelinker relinker(options);
    CHECK(relinker.indexedFileCount() == 3);

    SECTION("case-insensitive names") {
        const RelinkResult r = relinker.resolve(referenceTo("/Volumes/Sound/scene 4.wav"));
        CHECK(r.method == RelinkMethod::SearchRoot);
        CHECK(fs::equivalent(r.resolved, dir / "Day 2" / "Sound" / "Scene 4.WAV"));
    }
    SECTION("several candidates: the one sharing the most folders with the original path") {
        const RelinkResult r = relinker.resolve(referenceTo("E:/Shoot/Day 2/Card B/C0001.MP4"));
        CHECK(r.method == RelinkMethod::SearchRoot);
        CHECK(fs::equivalent(r.resolved, dir / "Day 2" / "Card B" / "C0001.MP4"));
        CHECK(r.note.find("2 files named 'C0001.MP4'") != std::string::npos);
    }
    SECTION("a known size rejects same-name files of another size") {
        MediaReference reference = referenceTo("E:/Shoot/Day 2/Card B/C0001.MP4");
        reference.fileSize = 4;
        const RelinkResult r = relinker.resolve(reference);
        CHECK(fs::equivalent(r.resolved, dir / "Day 1" / "Card A" / "C0001.MP4"));
        reference.fileSize = 99;
        const RelinkResult none = relinker.resolve(reference);
        CHECK_FALSE(none.found());
        CHECK(none.note.find("size differs") != std::string::npos);
    }
    SECTION("only the clip name is known (EDL, OTIO MissingReference)") {
        MediaReference reference;
        reference.name = "Scene 4.wav";
        const RelinkResult r = relinker.resolve(reference);
        CHECK(r.method == RelinkMethod::SearchRoot);
    }
    SECTION("nothing found") {
        const RelinkResult r = relinker.resolve(referenceTo("/x/missing.mov"));
        CHECK(r.method == RelinkMethod::Unresolved);
        CHECK(r.resolved.empty());
        CHECK(r.note.find("no file named 'missing.mov'") != std::string::npos);
    }
}

TEST_CASE("interchange: relink: an inspector's duration picks between candidates", "[interchange][relink]") {
    const auto dir = scratchDir("relink-inspector");
    writeFile(dir / "a" / "take.mov", "1");
    writeFile(dir / "b" / "take.mov", "2");
    RelinkOptions options;
    options.searchRoots = {dir};
    options.inspector = [&](const fs::path& p) -> std::optional<MediaFacts> {
        MediaFacts facts;
        facts.duration = p.parent_path().filename() == "a" ? Ticks::fromSeconds(10) : Ticks::fromSeconds(42);
        return facts;
    };
    MediaRelinker relinker(options);
    MediaReference reference = referenceTo("/old/take.mov");
    reference.duration = Ticks::fromSeconds(42) + Ticks{kTicksPerSecond / 10}; // within tolerance
    const RelinkResult r = relinker.resolve(reference);
    CHECK(fs::equivalent(r.resolved, dir / "b" / "take.mov"));
}

TEST_CASE("interchange: relink: the index stops at its limit and never touches files",
          "[interchange][relink]") {
    const auto dir = scratchDir("relink-limit");
    for (int i = 0; i < 20; ++i) {
        writeFile(dir / ("f" + std::to_string(i) + ".mov"), "data");
    }
    const auto before = fs::last_write_time(dir / "f3.mov");
    RelinkOptions options;
    options.searchRoots = {dir, dir / "does-not-exist"};
    options.maxIndexedFiles = 5;
    MediaRelinker relinker(options);
    CHECK(relinker.indexedFileCount() == 5);
    CHECK(relinker.indexTruncated());
    (void)relinker.resolve(referenceTo("/x/f3.mov"));
    CHECK(fs::file_size(dir / "f3.mov") == 4);
    CHECK(fs::last_write_time(dir / "f3.mov") == before);
}

TEST_CASE("interchange: relink: a network share is looked up like a local folder, by the existence check only (C.10)",
          "[interchange][relink][unc]") {
    // The policy (docs/interchange.md, "Network paths"): a share path takes the same plain existence check, in the
    // same order, as a local one; an unreachable share leaves the media offline, never an error; nothing is mounted.
    std::vector<std::string> asked;
    bool present = true;
    RelinkOptions options;
    options.projectDirectory = "//fileserver/share/Edit";
    options.fileExists = [&](const fs::path& p) {
        asked.push_back(utf8(p));
        return present;
    };
    const auto decoded = mediaReferenceToPath("file://fileserver/share/Footage/B001.mov");
    REQUIRE(decoded);
    const MediaReference share = referenceTo(*decoded, "B001.mov");
    MediaRelinker relinker(options);
    const RelinkResult found = relinker.resolve(share);
    CHECK(found.method == RelinkMethod::ExactPath);
    CHECK(utf8(found.resolved) == "//fileserver/share/Footage/B001.mov");
    REQUIRE(asked.size() == 1);
    CHECK(asked.front() == "//fileserver/share/Footage/B001.mov");

    // Missing or unreachable: offline with every path tried listed, like a missing local file; only paths a local
    // reference would get were asked about (folders of that host, nothing else on the network).
    present = false;
    asked.clear();
    MediaRelinker again(options);
    const RelinkResult missing = again.resolve(share);
    CHECK_FALSE(missing.found());
    CHECK_FALSE(missing.tried.empty());
    CHECK(utf8(fs::path(missing.tried.front())) == "//fileserver/share/Footage/B001.mov"); // listed in native form
    for (const std::string& path : asked) {
        INFO(path);
        CHECK(path.starts_with("//fileserver/"));
    }
}