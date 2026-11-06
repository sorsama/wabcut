// 0.7 (card 094): search across all projects of a production (editor/ProductionSearch.h) - the index of every project,
// the query (words, phrases, a field, a type), the ranking, a project open in memory, files that did not change, files that
// cannot be read, and the bounds.

#include <wabcut/editor/Production.h>
#include <wabcut/editor/ProductionSearch.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "production-search" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

struct MediaSpec {
    std::string name;
    std::string comment;
    std::string path;
    bool video = true;
    std::string bin;      // bin name (created at the top), or ""
    std::string tape;     // reel name
    std::string logNote;
    std::string marker;
    bool title = false;
};

/// A project holding the given sequences and media (the media files need not exist: only their names are searched).
ProjectState makeProject(const std::vector<std::string>& sequences, const std::vector<MediaSpec>& media) {
    ProjectState s;
    s.ids.next = 1;
    for (const std::string& name : sequences) {
        edit::SequenceSettings settings;
        settings.name = name;
        auto added = edit::addSequence(s, settings);
        REQUIRE(added);
        s = std::move(added->state);
    }
    for (const MediaSpec& spec : media) {
        BinId bin;
        if (!spec.bin.empty()) {
            auto added = edit::addBin(s, spec.bin);
            REQUIRE(added);
            s = std::move(added->state);
            bin = added->id;
        }
        MediaItem item;
        item.name = spec.name;
        item.comment = spec.comment;
        item.bin = bin;
        item.locator.path = spec.path;
        item.info.path = spec.path;
        item.metadata.logNote = spec.logNote;
        if (!spec.tape.empty()) {
            item.timecode = MediaTimecode{std::nullopt, spec.tape};
        }
        if (spec.video) {
            VideoStreamInfo video;
            video.index = 0;
            video.width = 1920;
            video.height = 1080;
            video.frameRate = kRate30;
            item.info.video.push_back(video);
        } else {
            AudioStreamInfo audio;
            audio.index = 0;
            item.info.audio.push_back(audio);
        }
        item.info.duration = Ticks::fromSeconds(90);
        if (!spec.marker.empty()) {
            Marker marker;
            marker.name = spec.marker;
            item.markers.push_back(marker);
        }
        if (spec.title) {
            item.generator = GeneratorSpec{std::string(kTitleGenerator), spec.name, "Arial", "Regular", {}};
            item.info.isStillImage = true; // a title is one still video stream
        }
        auto added = edit::addMedia(s, item);
        REQUIRE(added);
        s = std::move(added->state);
    }
    return s;
}

void write(const fs::path& file, const ProjectState& state) {
    fs::create_directories(file.parent_path());
    const auto saved = io::saveProject(state, file, io::SaveOptions{0});
    INFO((saved ? std::string() : saved.error().message));
    REQUIRE(saved);
}

/// "Documentary": Episodes/Episode 1, Episodes/Episode 2 and Selects at the top.
Production makeProduction(const fs::path& dir) {
    auto production = Production::create(dir / "Documentary", "Documentary");
    REQUIRE(production);
    REQUIRE(production->newFolder("", "Episodes"));
    write(dir / "Documentary" / "Episodes" / "Episode 1.wabcut",
          makeProject({"Rough Cut v1", "Fine Cut"},
                      {{"Interview Anna.mov", "great answer about the river", "D:/Shoot/Day1/A001_anna.mov", true, "Interviews", "A001",
                        "", "river story"},
                       {"B-roll river.mp4", "", "D:/Shoot/Day1/broll_0042.mp4", true, "Day 1"},
                       {"Room tone.wav", "", "D:/Shoot/audio/roomtone.wav", false}}));
    write(dir / "Documentary" / "Episodes" / "Episode 2.wabcut",
          makeProject({"Rough Cut v2"},
                      {{"Drone over the Riverbank.mp4", "wide establishing", "E:/Drone/DJI_0007.mp4", true, "Drone"},
                       {"Interview Ben.mov", "talks about the bridge", "D:/Shoot/Day2/B001_ben.mov", true, "Interviews", "B001",
                        "need a better take"},
                       {"Lower third", "", "", true, "", "", "", "", true}}));
    write(dir / "Documentary" / "Selects.wabcut", makeProject({"Selects"}, {{"ÉCOLE exterior.mov", "", "D:/Shoot/Day3/ecole.mov"}}));
    return *production;
}

const SearchHit* hitNamed(const SearchResult& r, const std::string& name) {
    const auto found = std::find_if(r.hits.begin(), r.hits.end(), [&](const SearchHit& h) { return h.name == name; });
    return found == r.hits.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("Searching a production finds footage and sequences in any of its projects (094)", "[editor][production][search]") {
    const fs::path dir = scratch("basic");
    const Production production = makeProduction(dir);
    const ProductionIndex index = scanProduction(production, {});
    REQUIRE(index.projects.size() == 3);
    CHECK(index.projects[0].name == "Episode 1");
    CHECK(index.projects[0].relative == "Episodes/Episode 1.wabcut");
    CHECK(index.projects[2].name == "Selects");
    for (const ProjectIndex& p : index.projects) {
        INFO(p.name << ": " << p.error);
        CHECK(p.error.empty());
        CHECK_FALSE(p.open);
        CHECK(p.size > 0);
    }

    // "river": the name of a clip in Episode 1, the name of one in Episode 2 (a word inside a longer one, found anyway),
    // a comment, a marker. A name that has the word ranks before one that only mentions it elsewhere.
    SearchQuery q;
    q.text = "river";
    const SearchResult r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 3);
    CHECK(r.total == 3);
    CHECK(r.projects == 3);
    CHECK(r.hits[0].name == "B-roll river.mp4");
    CHECK(r.hits[0].projectName == "Episode 1");
    CHECK(r.hits[0].kind == "video");
    CHECK(r.hits[0].matched == std::vector<std::string>{"name"});
    CHECK(r.hits[0].location == "Day 1");
    CHECK(r.hits[0].width == 1920);
    CHECK(r.hits[0].duration == Ticks::fromSeconds(90));
    CHECK_FALSE(r.hits[0].sequence);
    CHECK(r.hits[1].name == "Drone over the Riverbank.mp4"); // "river" inside "Riverbank"
    CHECK(r.hits[1].projectName == "Episode 2");
    CHECK(r.hits[2].name == "Interview Anna.mov"); // its comment and marker say "river"
    CHECK(r.hits[2].matched == std::vector<std::string>{"comment"});
    CHECK(r.hits[0].score > r.hits[2].score);

    // Sequences are found too, with the project they live in; the type narrows a search to them.
    q.text = "cut";
    q.type = "sequence";
    const SearchResult cuts = searchProduction(index, q);
    REQUIRE(cuts.hits.size() == 3);
    for (const SearchHit& h : cuts.hits) {
        CHECK(h.sequence);
        CHECK(h.kind == "sequence");
        CHECK(h.width == 1920);
    }
    CHECK(hitNamed(cuts, "Rough Cut v1")->projectName == "Episode 1");
    CHECK(hitNamed(cuts, "Fine Cut")->projectName == "Episode 1");
    CHECK(hitNamed(cuts, "Rough Cut v2")->projectName == "Episode 2");
    CHECK(hitNamed(cuts, "Rough Cut v2")->projectRelative == "Episodes/Episode 2.wabcut");
    CHECK(hitNamed(cuts, "Rough Cut v2")->project == dir / "Documentary" / "Episodes" / "Episode 2.wabcut");

    // One project only.
    q.project = "Episodes/Episode 2.wabcut";
    const SearchResult only = searchProduction(index, q);
    REQUIRE(only.hits.size() == 1);
    CHECK(only.projects == 1);
    CHECK(only.hits[0].name == "Rough Cut v2");
}

TEST_CASE("A production search takes every word, a quoted phrase, one field and a type (094)", "[editor][production][search]") {
    const fs::path dir = scratch("query");
    const ProductionIndex index = scanProduction(makeProduction(dir), {});

    SearchQuery q;
    // Every word must be found, each in any field: "interview" is in two names, "bridge" only in Ben's comment.
    q.text = "interview bridge";
    SearchResult r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "Interview Ben.mov");
    CHECK(r.hits[0].matched.size() == 2); // name and comment

    // A quoted phrase is one piece of text.
    q.text = "\"great answer\"";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "Interview Anna.mov");
    q.text = "\"answer great\"";
    CHECK(searchProduction(index, q).hits.empty());

    // A field: "river" in a comment only; the file path, the log note, the tape name, the bin.
    q.text = "river";
    q.field = "comment";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "Interview Anna.mov");
    q = {};
    q.text = "DJI_0007";
    q.field = "path";
    CHECK(searchProduction(index, q).hits.size() == 1);
    q.field = "name";
    CHECK(searchProduction(index, q).hits.empty());
    q = {};
    q.text = "better take";
    q.field = "logNote";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "Interview Ben.mov");
    q = {};
    q.text = "B001";
    q.field = "tapeName";
    REQUIRE(searchProduction(index, q).hits.size() == 1);
    q = {};
    q.text = "interviews"; // the bin both interviews are filed in
    q.field = "bin";
    CHECK(searchProduction(index, q).hits.size() == 2);
    q = {};
    q.text = "river story"; // a marker of Anna's clip
    q.field = "marker";
    REQUIRE(searchProduction(index, q).hits.size() == 1);
    // A sequence has a name, markers and a bin; other fields find nothing in it.
    q = {};
    q.text = "Fine";
    q.field = "comment";
    CHECK(searchProduction(index, q).hits.empty());

    // Case is ignored the way Find and Replace ignores it: "école" finds "ÉCOLE".
    q = {};
    q.text = "école";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "ÉCOLE exterior.mov");

    // Types: audio, a title, a sequence; the Library's kinds. A type alone lists everything of it, grouped by project.
    q = {};
    q.type = "audio";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "Room tone.wav");
    CHECK(r.hits[0].kind == "audio");
    q.type = "title";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].name == "Lower third");
    q.type = "sequence";
    r = searchProduction(index, q);
    REQUIRE(r.hits.size() == 4);
    CHECK(r.hits[0].projectName == "Episode 1");
    CHECK(r.hits[2].projectName == "Episode 2");
    CHECK(r.hits[3].projectName == "Selects");

    // Nothing asked, nothing found.
    CHECK(searchProduction(index, {}).hits.empty());
    q = {};
    q.text = "   ";
    CHECK(searchProduction(index, q).hits.empty());
}

TEST_CASE("A production search refuses a query it cannot run and keeps its bounds (094)", "[editor][production][search]") {
    SearchQuery q;
    CHECK_FALSE(searchProblem(q).has_value());
    q.field = "nope";
    CHECK(searchProblem(q).value_or("").find("unknown field") == 0);
    q.field = "path";
    q.type = "film";
    CHECK(searchProblem(q).value_or("").find("unknown type") == 0);
    q.type = "still";
    CHECK_FALSE(searchProblem(q).has_value());
    q.text = std::string(2000, 'a');
    CHECK(searchProblem(q).has_value());
    q.text = "bad \xFF\xFE bytes";
    CHECK(searchProblem(q).value_or("").find("UTF-8") != std::string::npos);

    // Many hits: `total` counts them all, the list is cut to the limit, best first.
    std::vector<MediaSpec> media;
    for (int i = 0; i < 40; ++i) {
        media.push_back({"Take " + std::to_string(i + 100), "", "D:/take" + std::to_string(i) + ".mov"});
    }
    const fs::path dir = scratch("limit");
    auto production = Production::create(dir / "P", "P");
    REQUIRE(production);
    write(dir / "P" / "Big.wabcut", makeProject({}, media));
    const ProductionIndex index = scanProduction(*production, {});
    SearchQuery take;
    take.text = "take";
    take.limit = 10;
    const SearchResult r = searchProduction(index, take);
    CHECK(r.total == 40);
    CHECK(r.hits.size() == 10);
    take.limit = 100000;
    CHECK(searchProduction(index, take).hits.size() == 40);
    take.limit = 0; // the default
    CHECK(searchProduction(index, take).hits.size() == 40);
    take.text = "take 107"; // two words: both found
    const SearchResult exact = searchProduction(index, take);
    REQUIRE(exact.hits.size() == 1);
    CHECK(exact.hits[0].name == "Take 107");
}

TEST_CASE("A production search reads an open project from memory and a file again only when it changed (094)",
          "[editor][production][search]") {
    const fs::path dir = scratch("cache");
    const Production production = makeProduction(dir);
    ProductionIndex first = scanProduction(production, {});
    REQUIRE(first.projects.size() == 3);

    // A scan with the earlier one at hand takes an unchanged file from it (marked here by renaming an item in the
    // earlier index: the file still says "Room tone.wav"), reads a file that changed, and a project that is open comes
    // from memory.
    for (SearchItem& item : first.projects[0].items) {
        if (item.name == "Room tone.wav") {
            item.name = "Room tone (cached)";
        }
    }
    write(dir / "Documentary" / "Selects.wabcut", makeProject({"Selects", "Selects alt"}, {{"ÉCOLE exterior.mov", "", "D:/Shoot/Day3/ecole.mov"}}));
    // The 'open' project: Episode 2 in memory with a clip added since it was saved.
    auto memory = std::make_shared<ProjectState>(makeProject({"Rough Cut v2"}, {{"Unsaved interview", "", "D:/x.mov"}}));
    ScanOptions options;
    options.previous = &first;
    options.openState = [&](const fs::path& file) -> std::shared_ptr<const ProjectState> {
        return file.filename() == "Episode 2.wabcut" ? memory : nullptr;
    };
    std::vector<std::string> seen;
    options.progress = [&](int done, int total, const std::string& current) {
        CHECK(total == 3);
        CHECK(done >= 0);
        if (!current.empty()) {
            seen.push_back(current);
        }
    };
    const ProductionIndex second = scanProduction(production, options);
    REQUIRE(second.projects.size() == 3);
    CHECK(seen == std::vector<std::string>{"Episode 1.wabcut", "Episode 2.wabcut", "Selects.wabcut"});
    const auto namesOf = [](const ProjectIndex& p) {
        std::vector<std::string> out;
        for (const SearchItem& i : p.items) {
            out.push_back(i.name);
        }
        return out;
    };
    const auto has = [&](const ProjectIndex& p, const std::string& name) {
        const auto names = namesOf(p);
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    CHECK(has(second.projects[0], "Room tone (cached)")); // unchanged file: not read again
    CHECK_FALSE(second.projects[0].open);
    CHECK(second.projects[1].open);                        // in memory
    CHECK(has(second.projects[1], "Unsaved interview"));
    CHECK_FALSE(has(second.projects[1], "Interview Ben.mov"));
    CHECK(has(second.projects[2], "Selects alt"));         // changed on disk: read again
    CHECK_FALSE(second.projects[2].open);

    SearchQuery q;
    q.text = "unsaved";
    const SearchResult r = searchProduction(second, q);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].projectOpen);
    CHECK(r.hits[0].projectName == "Episode 2");

    // The project is closed again: its file is read, the unsaved clip is gone.
    ScanOptions closed;
    closed.previous = &second;
    const ProductionIndex third = scanProduction(production, closed);
    CHECK_FALSE(third.projects[1].open);
    CHECK(has(third.projects[1], "Interview Ben.mov"));
    CHECK_FALSE(has(third.projects[1], "Unsaved interview"));
}

TEST_CASE("A production search reports a project it cannot read and goes on with the others (094)", "[editor][production][search]") {
    const fs::path dir = scratch("broken");
    const Production production = makeProduction(dir);
    {
        std::ofstream broken(dir / "Documentary" / "Broken.wabcut", std::ios::binary);
        broken << "{ this is not a project";
    }
    const ProductionIndex index = scanProduction(production, {});
    REQUIRE(index.projects.size() == 4);
    const auto broken = std::find_if(index.projects.begin(), index.projects.end(), [](const ProjectIndex& p) { return p.name == "Broken"; });
    REQUIRE(broken != index.projects.end());
    CHECK_FALSE(broken->error.empty());
    CHECK(broken->items.empty());
    SearchQuery q;
    q.text = "river";
    const SearchResult r = searchProduction(index, q);
    CHECK(r.hits.size() == 3);
    REQUIRE(r.unreadable.size() == 1);
    CHECK(r.unreadable[0].find("Broken.wabcut") == 0);

    // Scanning again with the earlier index does not read the broken file again, and a saved fix is read.
    ScanOptions again;
    again.previous = &index;
    CHECK(scanProduction(production, again).projects.size() == 4);
    write(dir / "Documentary" / "Broken.wabcut", makeProject({}, {{"River test", "", "D:/r.mov"}}));
    const ProductionIndex fixed = scanProduction(production, again);
    SearchQuery river;
    river.text = "river";
    CHECK(searchProduction(fixed, river).hits.size() == 4);
    CHECK(searchProduction(fixed, river).unreadable.empty());
}

TEST_CASE("A production scan stops when asked and leaves what it read (094)", "[editor][production][search]") {
    const fs::path dir = scratch("stop");
    const Production production = makeProduction(dir);
    std::stop_source source;
    source.request_stop();
    CHECK(scanProduction(production, {}, source.get_token()).projects.empty());

    std::stop_source later;
    ScanOptions options;
    options.progress = [&](int done, int, const std::string&) {
        if (done == 1) {
            later.request_stop(); // after the first project
        }
    };
    const ProductionIndex partial = scanProduction(production, options, later.get_token());
    CHECK(partial.projects.size() == 1);
    CHECK(partial.projects[0].name == "Episode 1");
}

TEST_CASE("indexProject lists each bin path and survives a damaged bin tree (094)", "[editor][production][search]") {
    ProjectState s;
    s.ids.next = 1;
    auto footage = edit::addBin(s, "Footage");
    REQUIRE(footage);
    s = footage->state;
    auto day = edit::addBin(s, "Day 1", footage->id);
    REQUIRE(day);
    s = day->state;
    MediaItem item;
    item.name = "Deep clip";
    item.bin = day->id;
    item.locator.path = "D:/deep.mov";
    item.info.path = item.locator.path;
    auto added = edit::addMedia(s, item);
    REQUIRE(added);
    s = added->state;
    // A damaged project: two bins that are each other's parent.
    Bin a;
    a.id = BinId{900};
    a.parent = BinId{901};
    a.name = "Loop A";
    Bin b;
    b.id = BinId{901};
    b.parent = BinId{900};
    b.name = "Loop B";
    s.bins.push_back(a);
    s.bins.push_back(b);
    MediaItem looped = item;
    looped.id = MediaId{777};
    looped.name = "In the loop";
    looped.bin = BinId{900};
    s.media.push_back(std::make_shared<MediaItem>(looped));

    const ProjectIndex index = indexProject(s);
    const auto find = [&](const std::string& name) -> const SearchItem* {
        for (const SearchItem& i : index.items) {
            if (i.name == name) {
                return &i;
            }
        }
        return nullptr;
    };
    REQUIRE(find("Deep clip"));
    CHECK(find("Deep clip")->location == "Footage / Day 1");
    REQUIRE(find("In the loop"));
    CHECK_FALSE(find("In the loop")->location.empty()); // some path, and it ended
}
