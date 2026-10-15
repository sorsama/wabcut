// Markers 2.0 (M5i): Premiere's marker types and web-link fields, updating and replacing sequence markers, clip
// markers on media items, the bounds for untrusted files and commands, and saving and loading.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Marker marker(std::int64_t frame, std::string name = {}) {
    Marker m;
    m.start = f(frame);
    m.name = std::move(name);
    return m;
}

std::vector<std::string> names(const std::vector<Marker>& markers) {
    std::vector<std::string> out;
    for (const Marker& m : markers) {
        out.push_back(m.name);
    }
    return out;
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

} // namespace

TEST_CASE("Marker types round-trip by name", "[model][markers]") {
    for (const MarkerType type : {MarkerType::Comment, MarkerType::Chapter, MarkerType::Segmentation, MarkerType::WebLink}) {
        CHECK(markerTypeFromName(markerTypeName(type)) == type);
    }
    CHECK(markerTypeName(MarkerType::WebLink) == "webLink");
    CHECK_FALSE(markerTypeFromName("flashCue"));
}

TEST_CASE("Markers outside the bounds are not allowed", "[model][markers]") {
    CHECK_FALSE(markerProblem(marker(3)));
    Marker negative = marker(0);
    negative.duration = f(-1);
    CHECK(markerProblem(negative));

    Marker longName = marker(0, std::string(kMaxMarkerTextBytes, 'n'));
    CHECK_FALSE(markerProblem(longName));
    longName.name.push_back('n');
    CHECK(markerProblem(longName));
    Marker longComment = marker(0);
    longComment.comment.assign(kMaxMarkerTextBytes + 1, 'c');
    CHECK(markerProblem(longComment));

    Marker link = marker(0);
    link.type = MarkerType::WebLink;
    for (const char* ok : {"https://wabcut.example/notes", "http://example.com", "HTTPS://EXAMPLE.COM"}) {
        link.url = ok;
        INFO(ok);
        CHECK_FALSE(markerProblem(link));
    }
    for (const char* bad : {"javascript:alert(1)", "file:///etc/passwd", "ftp://example.com", "example.com", "https:/x"}) {
        link.url = bad;
        INFO(bad);
        CHECK(markerProblem(link));
    }
    link.url = "https://example.com/" + std::string(kMaxMarkerUrlBytes, 'u');
    CHECK(markerProblem(link));
    link.url = "https://example.com";
    link.target.assign(kMaxMarkerUrlBytes + 1, 't');
    CHECK(markerProblem(link));
}

TEST_CASE("addMarker checks the bounds and the count", "[model][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    Marker bad = marker(2);
    bad.url = "file:///x";
    CHECK(edit::addMarker(tl.state, tl.seq, bad).error().code == ErrorCode::InvalidArgument);

    std::vector<Marker> full(kMaxMarkers, marker(1));
    const ProjectState state = ok(edit::setSequenceMarkers(tl.state, tl.seq, full)).state;
    CHECK(edit::addMarker(state, tl.seq, marker(2)).error().code == ErrorCode::InvalidArgument);
    full.push_back(marker(1));
    CHECK(edit::setSequenceMarkers(tl.state, tl.seq, full).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("updateMarker renames, recolours, retypes, lengthens and moves a marker", "[model][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaa|
    )");
    ProjectState state = tl.state;
    for (const auto& [frame, name] : {std::pair{2, "a"}, std::pair{5, "b"}, std::pair{9, "c"}}) {
        state = ok(edit::addMarker(state, tl.seq, marker(frame, name))).state;
    }

    Marker b = state.findSequence(tl.seq)->markers[1];
    b.name = "Interview";
    b.comment = "pick up the second take";
    b.colorIndex = 3;
    b.type = MarkerType::Chapter;
    b.duration = f(2);
    const auto renamed = ok(edit::updateMarker(state, tl.seq, 1, b));
    const Sequence& seq = *renamed.state.findSequence(tl.seq);
    CHECK(seq.markers[1] == b);
    CHECK(seq.markers[1].end() == f(7));
    REQUIRE(renamed.changes.sequences.size() == 1);
    CHECK(renamed.changes.sequences[0].markersChanged);
    requireValid(renamed.state);

    // Moving re-sorts: "Interview" goes after "c".
    b.start = f(11);
    const auto moved = ok(edit::updateMarker(renamed.state, tl.seq, 1, b));
    CHECK(names(moved.state.findSequence(tl.seq)->markers) == std::vector<std::string>{"a", "c", "Interview"});
    requireValid(moved.state);

    // The same marker changes nothing; a bad index or a bad marker is refused.
    CHECK(ok(edit::updateMarker(moved.state, tl.seq, 2, b)).changes.empty());
    CHECK(edit::updateMarker(moved.state, tl.seq, 3, b).error().code == ErrorCode::NotFound);
    b.url = "mailto:x@example.com";
    CHECK(edit::updateMarker(moved.state, tl.seq, 2, b).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("setSequenceMarkers sorts, keeps equal starts in order, and clears", "[model][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    const auto set = ok(edit::setSequenceMarkers(tl.state, tl.seq, {marker(6, "x"), marker(1, "y"), marker(6, "z")}));
    CHECK(names(set.state.findSequence(tl.seq)->markers) == std::vector<std::string>{"y", "x", "z"});
    requireValid(set.state);
    const auto cleared = ok(edit::setSequenceMarkers(set.state, tl.seq, {}));
    CHECK(cleared.state.findSequence(tl.seq)->markers.empty());
    REQUIRE(cleared.changes.sequences.size() == 1);
    CHECK(cleared.changes.sequences[0].markersChanged);
}

TEST_CASE("Clip markers live on the media item in media time", "[model][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');
    Marker link = marker(12, "logo");
    link.type = MarkerType::WebLink;
    link.url = "https://wabcut.example";
    link.target = "_blank";
    const auto set = ok(edit::setMediaMarkers(tl.state, s, {link, marker(3, "slate")}));
    const MediaItem* item = set.state.findMedia(s);
    REQUIRE(item);
    CHECK(names(item->markers) == std::vector<std::string>{"slate", "logo"});
    CHECK(set.changes.modifiedMedia == std::vector<MediaId>{s});
    CHECK(set.state.sequences == tl.state.sequences);
    requireValid(set.state);

    CHECK(ok(edit::setMediaMarkers(set.state, s, item->markers)).changes.empty());
    CHECK(edit::setMediaMarkers(tl.state, MediaId{999999}, {}).error().code == ErrorCode::NotFound);
    Marker bad = marker(1);
    bad.url = "javascript:void(0)";
    CHECK(edit::setMediaMarkers(tl.state, s, {bad}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Marker types, web links and clip markers survive saving; new keys only when set",
          "[model][io][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');
    ProjectState state = ok(edit::addMarker(tl.state, tl.seq, marker(1, "plain"))).state;
    const auto plainJson = io::toJson(state);
    REQUIRE(plainJson);
    CHECK(plainJson->find("\"type\"") == std::string::npos);
    CHECK(plainJson->find("\"url\"") == std::string::npos);
    CHECK(plainJson->find("\"target\"") == std::string::npos);

    Marker chapter = marker(4, "Chapter 2");
    chapter.type = MarkerType::Chapter;
    chapter.colorIndex = 5;
    chapter.duration = f(3);
    Marker link = marker(2, "site");
    link.type = MarkerType::WebLink;
    link.url = "https://wabcut.example/a?b=c";
    link.target = "frame1";
    state = ok(edit::addMarker(state, tl.seq, chapter)).state;
    state = ok(edit::setMediaMarkers(state, s, {link})).state;

    const auto json = io::toJson(state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == state);
    CHECK(loaded->findSequence(tl.seq)->markers[1] == chapter);
    CHECK(loaded->findMedia(s)->markers == std::vector<Marker>{link});
    requireValid(*loaded);
}

TEST_CASE("Project files with markers outside the bounds do not load", "[model][io][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )",
                {{'S', 20}});
    Marker link = marker(2, std::string(kMaxMarkerTextBytes, 'n'));
    link.type = MarkerType::WebLink;
    link.url = "https://wabcut.example";
    const ProjectState state = ok(edit::addMarker(tl.state, tl.seq, link)).state;
    const ProjectState onMedia = ok(edit::setMediaMarkers(tl.state, tl.media('S'), {link})).state;
    for (const ProjectState* s : {&state, &onMedia}) {
        const auto json = io::toJson(*s);
        REQUIRE(json);
        REQUIRE(io::fromJson(*json));
        CHECK_FALSE(io::fromJson(replaced(*json, "https://wabcut.example", "javascript:alert(1)")));
        CHECK_FALSE(io::fromJson(replaced(*json, std::string(kMaxMarkerTextBytes, 'n'),
                                          std::string(kMaxMarkerTextBytes + 1, 'n'))));
        CHECK_FALSE(io::fromJson(replaced(*json, "\"webLink\"", "\"flashCue\"")));
    }
}
