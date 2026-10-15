// Markers 2.0 commands (CatalogMarkers.cpp, marker.add): types and web links, editing and clearing sequence markers,
// clip markers on media items, and exporting markers as CSV and YouTube chapters.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
