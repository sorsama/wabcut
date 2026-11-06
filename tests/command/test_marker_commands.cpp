// Markers 2.0 commands (CatalogMarkers.cpp, marker.add): types and web links, editing and clearing sequence markers,
// clip markers on media items, exporting markers as CSV and YouTube chapters, and reading a marker list back in from
// a CSV, text or JSON file (marker.import, source.importMarkers; card 121).

#include "CommandHarness.h"

#include <wabcut/command/CsvText.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr const char* kBase = R"(
    V1 tgt |Aaaaaaaaaaaa|
)";

std::vector<std::string> names(const std::vector<Marker>& markers) {
    std::vector<std::string> out;
    for (const Marker& m : markers) {
        out.push_back(m.name);
    }
    return out;
}

} // namespace

TEST_CASE("marker.add takes Premiere's types and web links", "[command][markers]") {
    CommandHarness h(kBase);
    h.ok("marker.add", {{"time", tc(2)}, {"name", "Chapter 1"}, {"type", "chapter"}, {"colorIndex", 3}});
    h.ok("marker.add", {{"time", tc(5)}, {"name", "site"}, {"type", "webLink"}, {"url", "https://wabcut.example"},
                        {"target", "_blank"}});
    const auto& markers = h.sequence(h.state()).markers;
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].type == MarkerType::Chapter);
    CHECK(markers[0].colorIndex == 3);
    CHECK(markers[1].type == MarkerType::WebLink);
    CHECK(markers[1].url == "https://wabcut.example");
    CHECK(markers[1].target == "_blank");

    CHECK(h.errorOf("marker.add", {{"time", tc(1)}, {"url", "javascript:alert(1)"}}) == "InvalidParams");
    CHECK(h.errorOf("marker.add", {{"time", tc(1)}, {"type", "flashCue"}}) == "InvalidParams");
    CHECK(h.errorOf("marker.add", {{"time", tc(1)}, {"name", std::string(kMaxMarkerTextBytes + 1, 'n')}}) ==
          "InvalidParams");
}

TEST_CASE("marker.update changes only the keys given and keeps markers sorted", "[command][markers]") {
    CommandHarness h(kBase);
    for (const auto& [frame, name] : {std::pair{1, "a"}, std::pair{4, "b"}, std::pair{8, "c"}}) {
        h.ok("marker.add", {{"time", tc(frame)}, {"name", name}, {"comment", "note"}});
    }
    const auto r = h.ok("marker.update", {{"index", 1}, {"name", "Interview"}, {"colorIndex", 1}, {"duration", tc(2)}});
    CHECK(r.data["marker"]["name"] == "Interview");
    const Marker& b = h.sequence(h.state()).markers[1];
    CHECK(b.name == "Interview");
    CHECK(b.comment == "note"); // kept
    CHECK(b.colorIndex == 1);
    CHECK(b.duration == f(2));
    CHECK(b.start == f(4));

    h.ok("marker.update", {{"index", 1}, {"time", tc(10)}});
    CHECK(names(h.sequence(h.state()).markers) == std::vector<std::string>{"a", "c", "Interview"});

    CHECK(h.errorOf("marker.update", {{"index", 3}, {"name", "x"}}) == "NotFound");
    CHECK(h.errorOf("marker.update", {{"index", 0}, {"url", "file:///x"}}) == "InvalidParams");

    // One undo step each; clearAll removes every marker.
    h.ok("marker.clearAll");
    CHECK(h.sequence(h.state()).markers.empty());
}

TEST_CASE("Clip markers are added, edited and removed on the media in media time", "[command][markers]") {
    CommandHarness h(Timeline(kBase, {{'S', 20}}));
    const MediaId s = h.tl.media('S');
    h.ok("source.addMarker", {{"media", s.value}, {"time", tc(12)}, {"name", "logo"}, {"colorIndex", 6}});
    h.ok("source.addMarker", {{"media", s.value}, {"time", tc(3)}, {"name", "slate"}});
    CHECK(names(h.state()->findMedia(s)->markers) == std::vector<std::string>{"slate", "logo"});
    CHECK(h.state()->findMedia(s)->markers[1].start == f(12));

    h.ok("source.updateMarker", {{"media", s.value}, {"index", 0}, {"time", tc(15)}, {"comment", "end card"}});
    const auto& markers = h.state()->findMedia(s)->markers;
    CHECK(names(markers) == std::vector<std::string>{"logo", "slate"});
    CHECK(markers[1].comment == "end card");

    h.ok("source.removeMarker", {{"media", s.value}, {"index", 0}});
    CHECK(names(h.state()->findMedia(s)->markers) == std::vector<std::string>{"slate"});
    CHECK(h.errorOf("source.removeMarker", {{"media", s.value}, {"index", 4}}) == "NotFound");
    CHECK(h.errorOf("source.addMarker", {{"media", s.value}, {"name", "no time"}}) == "InvalidParams");
    CHECK(h.errorOf("source.addMarker", {{"media", s.value}, {"time", tc(1)}, {"url", "ftp://x"}}) == "InvalidParams");
    CHECK(h.sequence(h.state()).markers.empty()); // the sequence is untouched
}

TEST_CASE("sequence.exportMarkers writes Premiere's CSV columns", "[command][markers]") {
    CommandHarness h(kBase);
    h.ok("marker.add", {{"time", tc(2)}, {"duration", tc(3)}, {"name", "Intro, part 1"}, {"comment", "say \"hi\""},
                        {"colorIndex", 1}, {"type", "chapter"}});
    h.ok("marker.add", {{"time", tc(7)}, {"name", "Cut"}});
    const auto r = h.ok("sequence.exportMarkers", {{"format", "csv"}});
    CHECK(r.data["count"] == 2);
    CHECK(r.data["text"] == "Marker Name,Description,In,Out,Duration,Marker Type,Color\n"
                            "\"Intro, part 1\",\"say \"\"hi\"\"\"," +
                                tc(2) + "," + tc(5) + "," + tc(3) + ",Chapter,Red\n" + "Cut,," + tc(7) + "," + tc(7) +
                                "," + tc(0) + ",Comment,Green\n");
    CHECK(r.revision == h.ok("sequence.summary").revision); // nothing changed
}

TEST_CASE("sequence.exportMarkers writes YouTube chapters and warns about YouTube's rules", "[command][markers]") {
    CommandHarness h(kBase);
    // 25 fps: chapter markers at 0:00, 0:12 and 1:05; a comment marker in between is left out.
    h.ok("marker.add", {{"time", tc(0)}, {"name", "Intro"}, {"type", "chapter"}});
    h.ok("marker.add", {{"time", tc(300)}, {"name", "Setup\nmore"}, {"type", "chapter"}});
    h.ok("marker.add", {{"time", tc(500)}, {"name", "a comment"}});
    h.ok("marker.add", {{"time", tc(1625)}, {"type", "chapter"}});
    auto r = h.ok("sequence.exportMarkers", {{"format", "youtube"}});
    CHECK(r.data["count"] == 3);
    CHECK(r.data["text"] == "0:00 Intro\n0:12 Setup\n1:05 Chapter 3\n");
    // The last chapter runs to the end of the 12-frame sequence: shorter than 10 s.
    CHECK(r.data["warnings"] == Json::array({"chapter 3 (1:05) is shorter than 10 seconds"}));

    // No chapter markers: every marker is a chapter. Two markers, the first at 0:04.
    CommandHarness plain(kBase);
    plain.ok("marker.add", {{"time", tc(100)}, {"name", "One"}});
    plain.ok("marker.add", {{"time", Ticks::fromSeconds(3604).value}, {"comment", "Two\nlines"}});
    r = plain.ok("sequence.exportMarkers", {{"format", "youtube"}});
    CHECK(r.data["text"] == "0:04 One\n1:00:04 Two\n");
    const Json& warnings = r.data["warnings"];
    CHECK(std::find(warnings.begin(), warnings.end(), Json("the first chapter must start at 0:00")) != warnings.end());
    CHECK(std::find(warnings.begin(), warnings.end(), Json("YouTube needs at least 3 chapters")) != warnings.end());
}

TEST_CASE("sequence.exportMarkers is a read command a read-only client may run", "[command][markers]") {
    command::EnginePolicy readOnly;
    readOnly.allowed = command::CapabilitySet::of({command::Capability::Read});
    CommandHarness h(Timeline(kBase), readOnly);
    CHECK(h.ok("sequence.exportMarkers").data["format"] == "csv");
    CHECK(h.errorOf("marker.clearAll") == "PermissionDenied");
    CHECK(h.errorOf("marker.update", {{"index", 0}}) == "PermissionDenied");
}

// P1 round (stream 1C, #24): YouTube chapters from In to Out (times counted from the In mark) and chapter markers only.
TEST_CASE("sequence.exportMarkers counts YouTube chapters from the In mark and keeps chapter markers only when asked",
          "[command][markers][p1]") {
    Timeline tl(kBase);
    Sequence s = tl.sequence();
    s.duration = Ticks::fromSeconds(120);
    s.markIn = Ticks::fromSeconds(10);
    s.markOut = Ticks::fromSeconds(70);
    tl.state = tl.state.withSequence(std::make_shared<const Sequence>(std::move(s)));
    CommandHarness h(std::move(tl));
    const auto at = [](int seconds) { return Ticks::fromSeconds(seconds).value; };
    h.ok("marker.add", {{"time", at(5)}, {"name", "Pre-roll"}, {"type", "chapter"}});
    h.ok("marker.add", {{"time", at(10)}, {"name", "Intro"}, {"type", "chapter"}});
    h.ok("marker.add", {{"time", at(15)}, {"name", "todo: fix the mix"}});
    h.ok("marker.add", {{"time", at(40)}, {"name", "Part 2"}, {"type", "chapter"}});
    h.ok("marker.add", {{"time", at(100)}, {"name", "Outro"}, {"type", "chapter"}});

    // In to Out: the chapters inside, from 0:00 at the In mark; the last runs to the Out mark.
    auto r = h.ok("sequence.exportMarkers", {{"format", "youtube"}, {"range", "inOut"}, {"chaptersOnly", true}});
    CHECK(r.data["text"] == "0:00 Intro\n0:30 Part 2\n");
    CHECK(r.data["count"] == 2);
    CHECK(r.data["warnings"] == Json::array({"YouTube needs at least 3 chapters"}));
    CHECK(r.data["rangeStart"]["ticks"] == at(10));
    // The whole sequence, as before (times from its start).
    r = h.ok("sequence.exportMarkers", {{"format", "youtube"}});
    CHECK(r.data["text"] == "0:05 Pre-roll\n0:10 Intro\n0:40 Part 2\n1:40 Outro\n");
    // CSV from In to Out: the markers inside, in sequence time; chapters only drops the to-do.
    r = h.ok("sequence.exportMarkers", {{"format", "csv"}, {"range", "inOut"}});
    CHECK(r.data["count"] == 3);
    r = h.ok("sequence.exportMarkers", {{"format", "csv"}, {"range", "inOut"}, {"chaptersOnly", true}});
    CHECK(r.data["count"] == 2);

    // Chapters only without a chapter marker: nothing, and why (never every marker, so to-dos stay out).
    CommandHarness plain(kBase);
    plain.ok("marker.add", {{"time", tc(1)}, {"name", "todo"}});
    r = plain.ok("sequence.exportMarkers", {{"format", "youtube"}, {"chaptersOnly", true}});
    CHECK(r.data["text"] == "");
    CHECK(r.data["count"] == 0);
    CHECK(r.data["warnings"] == Json::array({"no chapter markers"}));
    // From In to Out without marks: refused.
    CHECK(plain.errorOf("sequence.exportMarkers", {{"format", "youtube"}, {"range", "inOut"}}) == "InvalidParams");
}

// D7: a marker start that falls between two frames is snapped onto the frame that contains it, so the command never
// reports success while placing a marker where nothing can show it (nor quietly at 0). A range marker's length is a
// measured span (the loudness QC overs are sub-frame) and is kept exactly as asked.
TEST_CASE("marker.add and marker.update snap a start between frames onto the frame grid", "[command][markers]") {
    CommandHarness h(kBase); // 25 fps, 10,160,640,000 ticks a frame
    const Ticks halfFrame{f(1).value / 2};
    // Five and a half frames: the marker starts on frame 5; its two-and-a-half-frame length is kept.
    h.ok("marker.add", {{"time", (f(5) + halfFrame).value}, {"duration", (f(2) + halfFrame).value}, {"name", "between"}});
    const auto& markers = h.sequence(h.state()).markers;
    REQUIRE(markers.size() == 1);
    CHECK(markers[0].start == f(5));
    CHECK(markers[0].duration == f(2) + halfFrame);
    // What the command reports afterwards is what it stored.
    const auto listed = h.ok("marker.list");
    CHECK(listed.data["markers"][0]["start"]["ticks"] == f(5).value);
    CHECK(listed.data["markers"][0]["duration"]["ticks"] == (f(2) + halfFrame).value);
    CHECK(listed.data["markers"][0]["start"]["timecode"] == tc(5));
    // A start already on a frame is left alone.
    h.ok("marker.add", {{"time", tc(9)}, {"name", "aligned"}});
    CHECK(h.sequence(h.state()).markers[1].start == f(9));
    // marker.update snaps its own start the same way.
    h.ok("marker.update", {{"index", 0}, {"time", (f(7) + halfFrame).value}, {"duration", (f(3) + halfFrame).value}});
    const auto& moved = h.sequence(h.state()).markers;
    REQUIRE(moved.size() == 2);
    CHECK(moved[0].name == "between");
    CHECK(moved[0].start == f(7));
    CHECK(moved[0].duration == f(3) + halfFrame);
}

// ---- Import (card 121) ---------------------------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

/// A folder of marker files that goes with the test.
struct MarkerFiles {
    fs::path dir;
    explicit MarkerFiles(const std::string& tag) : dir(fs::temp_directory_path() / ("wabcut-marker-import-" + tag)) {
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~MarkerFiles() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string write(const std::string& name, const std::string& bytes) const {
        const fs::path file = dir / name;
        std::ofstream out(file, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return file.string();
    }
};

const Marker& markerAt(const Sequence& seq, std::size_t i) { return seq.markers.at(i); }

} // namespace

TEST_CASE("marker.import reads back what sequence.exportMarkers wrote (card 121)", "[command][markers][import]") {
    MarkerFiles files("roundtrip");
    CommandHarness source(kBase);
    source.ok("marker.add", {{"time", tc(2)}, {"duration", tc(3)}, {"name", "Intro, part 1"}, {"comment", "say \"hi\"\nsecond line"},
                             {"colorIndex", 1}, {"type", "chapter"}});
    source.ok("marker.add", {{"time", tc(7)}, {"name", "Cut"}, {"colorIndex", 6}, {"type", "segmentation"}});
    const std::string csv = source.ok("sequence.exportMarkers", {{"format", "csv"}}).data["text"];
    const std::string path = files.write("markers.csv", csv);

    CommandHarness h(kBase);
    const std::size_t steps = h.session.undoCount();
    const auto r = h.ok("marker.import", {{"path", path}});
    CHECK(r.data["format"] == "csv");
    CHECK(r.data["imported"] == 2);
    CHECK(r.data["skipped"] == 0);
    const Sequence& seq = h.sequence(h.state());
    REQUIRE(seq.markers.size() == 2);
    CHECK(markerAt(seq, 0).name == "Intro, part 1");
    CHECK(markerAt(seq, 0).comment == "say \"hi\"\nsecond line");
    CHECK(markerAt(seq, 0).start == f(2));
    CHECK(markerAt(seq, 0).duration == f(3));
    CHECK(markerAt(seq, 0).colorIndex == 1);
    CHECK(markerAt(seq, 0).type == MarkerType::Chapter);
    CHECK(markerAt(seq, 1).name == "Cut");
    CHECK(markerAt(seq, 1).start == f(7));
    CHECK(markerAt(seq, 1).duration == Ticks{});
    CHECK(markerAt(seq, 1).colorIndex == 6);
    CHECK(markerAt(seq, 1).type == MarkerType::Segmentation);

    // One undo step brings the sequence back.
    CHECK(h.session.undoCount() == steps + 1);
    CHECK(h.session.undoLabel() == "Import Markers");
    REQUIRE(h.session.undo());
    CHECK(h.sequence(h.state()).markers.empty());
}

TEST_CASE("marker.import reads tables with other delimiters, names and no header row (card 121)", "[command][markers][import]") {
    MarkerFiles files("tables");
    // A spreadsheet's export: semicolons, a UTF-8 byte order mark and other column names.
    {
        CommandHarness h(kBase);
        const std::string path = files.write("sheet.csv", "\xEF\xBB\xBF" "Title;Notes;Start;Length\r\n"
                                                           "Door;\"open; slowly\";00:00:01:00;00:00:00:10\r\n"
                                                           "Window;;00:00:04:00;\r\n");
        const auto r = h.ok("marker.import", {{"path", path}});
        CHECK(r.data["imported"] == 2);
        const Sequence& seq = h.sequence(h.state());
        CHECK(markerAt(seq, 0).name == "Door");
        CHECK(markerAt(seq, 0).comment == "open; slowly");
        CHECK(markerAt(seq, 0).start == f(25));
        CHECK(markerAt(seq, 0).duration == f(10));
        CHECK(markerAt(seq, 1).start == f(100));
    }
    // Tab separated (Premiere's own export), with Out instead of Duration.
    {
        CommandHarness h(kBase);
        const std::string path = files.write("tabs.csv", "Marker Name\tDescription\tIn\tOut\tDuration\tMarker Type\n"
                                                         "A\tfirst\t00:00:02:00\t00:00:03:00\t\tComment\n");
        h.ok("marker.import", {{"path", path}});
        const Sequence& seq = h.sequence(h.state());
        REQUIRE(seq.markers.size() == 1);
        CHECK(markerAt(seq, 0).duration == f(25));
        CHECK(markerAt(seq, 0).comment == "first");
    }
    // No header row: time, name, comment.
    {
        CommandHarness h(kBase);
        const std::string path = files.write("plain.csv", "00:00:02:00,Opening,the scene\n00:00:05:12,Closing\n");
        const auto r = h.ok("marker.import", {{"path", path}});
        CHECK(r.data["format"] == "csv");
        const Sequence& seq = h.sequence(h.state());
        REQUIRE(seq.markers.size() == 2);
        CHECK(markerAt(seq, 0).name == "Opening");
        CHECK(markerAt(seq, 0).comment == "the scene");
        CHECK(markerAt(seq, 1).start == f(137));
    }
    // A Windows-1252 file (a spreadsheet's "CSV"): the accents come out right.
    {
        CommandHarness h(kBase);
        const std::string path = files.write("cp1252.csv", "Name,In\nCaf\xE9 \x93scene\x94,00:00:01:00\n");
        h.ok("marker.import", {{"path", path}});
        CHECK(markerAt(h.sequence(h.state()), 0).name == "Caf\xC3\xA9 \xE2\x80\x9Cscene\xE2\x80\x9D");
    }
}

TEST_CASE("marker.import reads YouTube chapters, Audacity labels and clock times from text files (card 121)",
          "[command][markers][import]") {
    MarkerFiles files("text");
    {
        CommandHarness h(kBase);
        const std::string path = files.write("chapters.txt", "0:00 Intro\n1:02 - Setup\n\n# a comment\n1:02:03 Outro and thanks\n");
        const auto r = h.ok("marker.import", {{"path", path}});
        CHECK(r.data["format"] == "txt");
        CHECK(r.data["imported"] == 3);
        const Sequence& seq = h.sequence(h.state());
        CHECK(markerAt(seq, 0).name == "Intro");
        CHECK(markerAt(seq, 0).start == Ticks{});
        CHECK(markerAt(seq, 1).name == "Setup");
        CHECK(markerAt(seq, 1).start == Ticks::fromSeconds(62));
        CHECK(markerAt(seq, 2).name == "Outro and thanks");
        CHECK(markerAt(seq, 2).start == Ticks::fromSeconds(3723));
    }
    {
        // Audacity labels: start, end, label; a line with a bare number is not a time ("10 things").
        CommandHarness h(kBase);
        const std::string path = files.write("labels.txt", "2.000000\t3.000000\tFirst\n10 things to know\n4.000000\t4.000000\tSecond\n");
        const auto r = h.ok("marker.import", {{"path", path}});
        CHECK(r.data["imported"] == 2);
        CHECK(r.data["skipped"] == 1);
        CHECK(r.data["warnings"][0].get<std::string>().find("line 2") != std::string::npos);
        const Sequence& seq = h.sequence(h.state());
        CHECK(markerAt(seq, 0).name == "First");
        CHECK(markerAt(seq, 0).start == f(50));
        CHECK(markerAt(seq, 0).duration == f(25));
        CHECK(markerAt(seq, 1).duration == Ticks{});
    }
    {
        // Seconds with a unit, milliseconds and bracketed timecodes.
        CommandHarness h(kBase);
        const std::string path = files.write("units.txt", "12s one\n1500ms two\n[00:00:03:00] three\n");
        h.ok("marker.import", {{"path", path}});
        const Sequence& seq = h.sequence(h.state());
        REQUIRE(seq.markers.size() == 3);
        CHECK(markerAt(seq, 0).name == "two");
        CHECK(markerAt(seq, 0).start == f(37)); // 1.5 s is frame 37.5: onto the frame grid
        CHECK(markerAt(seq, 1).name == "three");
        CHECK(markerAt(seq, 1).start == f(75));
        CHECK(markerAt(seq, 2).name == "one");
        CHECK(markerAt(seq, 2).start == f(300));
    }
}

TEST_CASE("marker.import reads JSON, including marker.list's own output (card 121)", "[command][markers][import]") {
    MarkerFiles files("json");
    CommandHarness source(kBase);
    source.ok("marker.add", {{"time", tc(10)}, {"duration", tc(5)}, {"name", "kept"}, {"comment", "c"}, {"colorIndex", 3},
                             {"type", "chapter"}});
    const std::string listed = source.ok("marker.list").data.dump();
    {
        CommandHarness h(kBase);
        const auto r = h.ok("marker.import", {{"path", files.write("list.json", listed)}});
        CHECK(r.data["format"] == "json");
        const Sequence& seq = h.sequence(h.state());
        REQUIRE(seq.markers.size() == 1);
        CHECK(markerAt(seq, 0).name == "kept");
        CHECK(markerAt(seq, 0).start == f(10));
        CHECK(markerAt(seq, 0).duration == f(5));
        CHECK(markerAt(seq, 0).colorIndex == 3);
        CHECK(markerAt(seq, 0).type == MarkerType::Chapter);
    }
    {
        // A plain array: seconds as numbers or text, a colour name, an entry with no time and a non-object entry.
        CommandHarness h(kBase);
        const std::string json =
            R"([{"name": "a", "time": 2}, {"title": "b", "start": "00:00:04:00", "color": "Blue", "duration": 1.0},
                {"name": "no time"}, 7])";
        const auto r = h.ok("marker.import", {{"path", files.write("array.json", json)}});
        CHECK(r.data["imported"] == 2);
        CHECK(r.data["skipped"] == 2);
        const Sequence& seq = h.sequence(h.state());
        CHECK(markerAt(seq, 0).start == f(50));
        CHECK(markerAt(seq, 1).name == "b");
        CHECK(markerAt(seq, 1).colorIndex == 6);
        CHECK(markerAt(seq, 1).duration == f(25));
    }
}

TEST_CASE("marker.import joins, shifts, replaces and never adds a file twice (card 121)", "[command][markers][import]") {
    MarkerFiles files("options");
    const std::string path = files.write("m.csv", "Name,In\nOne,00:00:01:00\nTwo,00:00:02:00\n");
    CommandHarness h(kBase);
    h.ok("marker.add", {{"time", tc(60)}, {"name", "mine"}});
    auto r = h.ok("marker.import", {{"path", path}});
    CHECK(r.data["imported"] == 2);
    CHECK(names(h.sequence(h.state()).markers) == std::vector<std::string>{"One", "Two", "mine"}); // sorted by time

    // The same file again adds nothing: its markers are already there.
    r = h.ok("marker.import", {{"path", path}});
    CHECK(r.data["imported"] == 0);
    CHECK(r.data["duplicates"] == 2);
    CHECK(h.sequence(h.state()).markers.size() == 3);

    // An offset shifts every time; one that goes before 0 skips the marker.
    r = h.ok("marker.import", {{"path", path}, {"offset", tc(10)}});
    CHECK(r.data["imported"] == 2);
    CHECK(markerAt(h.sequence(h.state()), 1).start == f(35));
    r = h.ok("marker.import", {{"path", path}, {"offset", -f(30).value}});
    CHECK(r.data["imported"] == 1);
    CHECK(r.data["skipped"] == 1);
    CHECK(markerAt(h.sequence(h.state()), 0).start == f(20)); // Two, 30 frames earlier; One would start before 0

    // Replace: only the file's markers remain.
    r = h.ok("marker.import", {{"path", path}, {"replace", true}});
    CHECK(r.data["replaced"] == true);
    CHECK(names(h.sequence(h.state()).markers) == std::vector<std::string>{"One", "Two"});
}

TEST_CASE("marker.import refuses what is not a marker file and says which lines it skipped (card 121)", "[command][markers][import]") {
    MarkerFiles files("refusals");
    CommandHarness h(kBase);
    CHECK(h.errorOf("marker.import", {{"path", (files.dir / "missing.csv").string()}}) == "NotFound");
    const std::string junk = files.write("junk.txt", "nothing here\nnor here\n");
    const auto bad = h.run(Json{{"command", "marker.import"}, {"params", {{"path", junk}}}});
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error->message.find("no marker found") != std::string::npos);
    CHECK(h.errorOf("marker.import", {{"path", files.write("empty.csv", "")}}) == "InvalidParams");
    CHECK(h.errorOf("marker.import", {{"path", junk}, {"format", "yaml"}}) == "InvalidParams");
    CHECK(h.sequence(h.state()).markers.empty());

    // Bad rows are skipped one by one, with their line numbers; a time past a month is not a time.
    const std::string mixed =
        files.write("mixed.csv", "Name,In\nOk,00:00:01:00\nBad,soon\nHuge,99999999:00:00:00\nAlso ok,00:00:03:00\n");
    const auto r = h.ok("marker.import", {{"path", mixed}});
    CHECK(r.data["imported"] == 2);
    CHECK(r.data["skipped"] == 2);
    const Json& warnings = r.data["warnings"];
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].get<std::string>().find("line 3") != std::string::npos);
    CHECK(warnings[1].get<std::string>().find("line 4") != std::string::npos);
}

TEST_CASE("source.importMarkers puts a list on a clip's media in media time (card 121)", "[command][markers][import]") {
    MarkerFiles files("clip");
    CommandHarness h(Timeline(kBase, {{'S', 20}}));
    const MediaId s = h.tl.media('S');
    const std::string path = files.write("takes.csv", "Name,In,Marker Type\nGood take,00:00:00:10,Chapter\nSlate,00:00:00:02,\n");
    const auto r = h.ok("source.importMarkers", {{"media", s.value}, {"path", path}});
    CHECK(r.data["imported"] == 2);
    const auto& markers = h.state()->findMedia(s)->markers;
    REQUIRE(markers.size() == 2);
    CHECK(names(markers) == std::vector<std::string>{"Slate", "Good take"});
    CHECK(markers[1].start == f(10));
    CHECK(markers[1].type == MarkerType::Chapter);
    CHECK(h.sequence(h.state()).markers.empty()); // the sequence is untouched
    // A second import adds nothing; replace swaps.
    CHECK(h.ok("source.importMarkers", {{"media", s.value}, {"path", path}}).data["imported"] == 0);
    h.ok("source.importMarkers", {{"media", s.value}, {"path", files.write("one.txt", "00:00:01:00 only\n")}, {"replace", true}});
    CHECK(names(h.state()->findMedia(s)->markers) == std::vector<std::string>{"only"});
    CHECK(h.errorOf("source.importMarkers", {{"media", 99999}, {"path", path}}) == "NotFound");
}
// Card #84 follow-up: marker names and comments come from clips, files and projects other people made; a spreadsheet
// runs a cell that starts with = + - @ as a formula. They get a leading quote (CsvText.h), the others are as they were.
TEST_CASE("sequence.exportMarkers neutralises spreadsheet formulas in names and comments", "[command][markers][csv]") {
    CommandHarness h(kBase);
    h.ok("marker.add", {{"time", tc(2)}, {"name", "=HYPERLINK(\"http://evil\",\"x\")"}, {"comment", "+1 great"}});
    h.ok("marker.add", {{"time", tc(4)}, {"name", "@SUM(A1)"}, {"comment", "-2 dB, then \"quiet\""}});
    h.ok("marker.add", {{"time", tc(6)}, {"name", "-- intro"}, {"comment", "plain"}});
    h.ok("marker.add", {{"time", tc(8)}, {"name", "Fine"}, {"comment", "a = b"}});
    h.ok("marker.add", {{"time", tc(10)}, {"name", "'=already quoted"}, {"comment", ""}});
    const std::string text = h.ok("sequence.exportMarkers", {{"format", "csv"}}).data["text"];
    CHECK(text.find("\"'=HYPERLINK(\"\"http://evil\"\",\"\"x\"\")\",'+1 great,") != std::string::npos);
    CHECK(text.find("'@SUM(A1),\"'-2 dB, then \"\"quiet\"\"\",") != std::string::npos);
    CHECK(text.find("'-- intro,plain,") != std::string::npos);
    CHECK(text.find("Fine,a = b,") != std::string::npos); // only a LEADING character matters
    CHECK(text.find("''=already quoted,") != std::string::npos);
    // No line of the file starts a cell with a formula character.
    std::size_t at = 0;
    while ((at = text.find('\n', at)) != std::string::npos && at + 1 < text.size()) {
        ++at;
        CHECK_FALSE(wabcut::command::isCsvFormulaStart(text[at]));
    }
}

TEST_CASE("CSV text cells: the guard is exactly undone by csvTextUnguarded", "[command][csv]") {
    using wabcut::command::csvText;
    using wabcut::command::csvTextUnguarded;
    using wabcut::command::needsCsvGuard;
    for (const char* formula : {"=1+1", "+44", "-3", "@user", "\tx", "\rx"}) {
        INFO(formula);
        CHECK(needsCsvGuard(formula));
        CHECK(csvText(formula).find('\'') <= 1); // first, or just inside the quotes a carriage return needs
    }
    for (const char* plain : {"", "x", "1+1", "a=b", "'quoted", "'", " =not first", "naïve"}) {
        INFO(plain);
        CHECK_FALSE(needsCsvGuard(plain));
    }
    // Round trip: what a reader gets after taking off the CSV quoting is the text that was written.
    for (const std::string original : {"=cmd|' /C calc'!A0", "'=x", "''+y", "@a", "-1", "plain", "", "x,y", "line\nbreak", "q\"uote"}) {
        INFO(original);
        std::string cell = csvText(original);
        if (cell.size() >= 2 && cell.front() == '"' && cell.back() == '"') { // RFC 4180 unquote
            std::string inner = cell.substr(1, cell.size() - 2);
            std::string out;
            for (std::size_t i = 0; i < inner.size(); ++i) {
                out += inner[i];
                if (inner[i] == '"' && i + 1 < inner.size() && inner[i + 1] == '"') {
                    ++i;
                }
            }
            cell = out;
        }
        CHECK(csvTextUnguarded(cell) == original);
    }
}
