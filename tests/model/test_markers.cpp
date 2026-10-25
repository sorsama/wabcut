// Markers 2.0 (M5i): Premiere's marker types and web-link fields, updating and replacing sequence markers, clip
// markers on media items, the bounds for untrusted files and commands, and saving and loading.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GamingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/SequenceSettings.h>

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

// ---- 0.4: Markers > Ripple Sequence Markers -------------------------------------------------------------------------

namespace {

Marker span(std::int64_t frame, std::int64_t frames, std::string name) {
    Marker m;
    m.start = f(frame);
    m.duration = f(frames);
    m.name = std::move(name);
    return m;
}

ProjectState withMarkers(ProjectState state, SequenceId seq, const std::vector<Marker>& markers) {
    for (const Marker& m : markers) {
        state = ok(edit::addMarker(state, seq, m)).state;
    }
    return state;
}

ProjectState withMarks(const ProjectState& state, SequenceId seq, std::int64_t in, std::int64_t lastFrame) {
    auto marked = ok(edit::markIn(state, seq, f(in))).state;
    return ok(edit::markOut(marked, seq, f(lastFrame))).state;
}

/// "name@start" or "name@start+length" in frames, in time order: where the markers are after an edit.
std::string layout(const ProjectState& state, SequenceId seq) {
    std::string out;
    for (const Marker& m : state.findSequence(seq)->markers) {
        if (!out.empty()) {
            out += ' ';
        }
        out += m.name + "@" + std::to_string(ticksToFrames(m.start, kRate));
        if (m.duration.value > 0) {
            out += "+" + std::to_string(ticksToFrames(m.duration, kRate));
        }
    }
    return out;
}

ProjectState rippleMarkersOff(const ProjectState& state, SequenceId seq) {
    edit::SequenceSettingsChange off;
    off.rippleMarkers = false;
    return ok(edit::setSequenceSettings(state, seq, off)).state;
}

edit::SourceClip sourceOf(const Timeline& tl, char letter, std::int64_t frames) {
    return edit::SourceClip{tl.media(letter), {f(0), f(frames)},
                            {{{StreamKind::Video, 0}, tl.track("V1")}, {{StreamKind::Audio, 0}, tl.track("A1")}}};
}

} // namespace

TEST_CASE("Ripple Sequence Markers: extract moves later markers and shrinks a range marker", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaa|
    )");
    tl.linkMatching();
    CHECK(tl.sequence().rippleMarkers); // on by default, as in Premiere
    const ProjectState s = withMarkers(tl.state, tl.seq, {marker(2, "Intro"), marker(10, "Outro"), span(4, 2, "Chapter")});
    const auto r = ok(edit::extract(withMarks(s, tl.seq, 3, 4), tl.seq)); // removes [3, 5)
    CHECK(layout(r.state, tl.seq) == "Intro@2 Chapter@3+1 Outro@8");
    REQUIRE(r.changes.sequence(tl.seq));
    CHECK(r.changes.sequence(tl.seq)->markersChanged);
    requireValid(r.state);
}

TEST_CASE("Ripple Sequence Markers: insert and paste insert push later markers", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBb|
        A1 tgt |AaaaaaaaaaBb|
    )");
    tl.linkMatching();
    const ProjectState s = withMarkers(
        tl.state, tl.seq, {span(0, 4, "EndsAt"), marker(1, "Before"), span(2, 4, "Across"), marker(4, "At"), marker(8, "After")});
    SECTION("an insert edit") {
        const auto r = ok(edit::insertEdit(s, tl.seq, sourceOf(tl, 'S', 3), f(4)));
        // A marker at the insert point moves; a range across it grows; a range that ends there stays.
        CHECK(layout(r.state, tl.seq) == "EndsAt@0+4 Before@1 Across@2+7 At@7 After@11");
        requireValid(r.state);
    }
    SECTION("Paste Insert") {
        const ClipId b = tl.clip("V1@10");
        const auto copied = copyClips(s, tl.seq, std::span(&b, 1));
        REQUIRE(copied);
        PasteOptions insert;
        insert.mode = PasteMode::Insert;
        const auto pasted = pasteClips(s, tl.seq, *copied, f(4), insert);
        REQUIRE(pasted);
        CHECK(layout(pasted->edit.state, tl.seq) == "EndsAt@0+4 Before@1 Across@2+6 At@6 After@10");
        requireValid(pasted->edit.state);
    }
    SECTION("a Ctrl-drag insert") {
        const ClipId b = tl.clip("V1@10");
        const auto r = ok(edit::moveClips(s, tl.seq, std::span(&b, 1), f(-6), {}, edit::MoveMode::Insert));
        CHECK(layout(r.state, tl.seq) == "EndsAt@0+4 Before@1 Across@2+6 At@6 After@10");
    }
}

TEST_CASE("Ripple Sequence Markers: ripple delete and close gap pull later markers back", "[model][markers][ripple]") {
    SECTION("Ripple Delete of a clip") {
        Timeline tl(R"(
            V1 tgt |AaaaBbbbCccc|
            A1 tgt |AaaaBbbbCccc|
        )");
        tl.linkMatching();
        const ProjectState s =
            withMarkers(tl.state, tl.seq, {marker(2, "InA"), marker(5, "InB"), span(6, 4, "AcrossBC"), marker(9, "InC")});
        const ClipId b = tl.clip("V1@4");
        const auto r = ok(edit::rippleDeleteClips(s, tl.seq, std::span(&b, 1)));
        CHECK(layout(r.state, tl.seq) == "InA@2 InB@4 AcrossBC@4+2 InC@5");
        requireValid(r.state);
    }
    SECTION("Ripple Delete of a gap and Close Gap") {
        Timeline tl(R"(
            V1 tgt |Aaaa....Bbbb|
            A1 tgt |Aaaa....Bbbb|
        )");
        tl.linkMatching();
        const ProjectState s = withMarkers(tl.state, tl.seq, {marker(2, "InA"), marker(6, "InGap"), marker(9, "InB")});
        const auto gap = ok(edit::rippleDeleteGap(s, tl.seq, tl.track("V1"), f(5)));
        CHECK(layout(gap.state, tl.seq) == "InA@2 InGap@4 InB@5");
        const auto closed = ok(edit::closeGaps(s, tl.seq, {}));
        CHECK(layout(closed.state, tl.seq) == "InA@2 InGap@4 InB@5");
    }
}

TEST_CASE("Ripple Sequence Markers: ripple trims and Q and W move later markers", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaBbbbbb|
        A1 tgt |AaaaaaBbbbbb|
    )");
    tl.linkMatching();
    const ProjectState s = withMarkers(tl.state, tl.seq, {marker(3, "InA"), marker(8, "InB")});
    const ClipId a = tl.clip("V1@0");
    SECTION("a ripple trim that shortens the clip") {
        const auto r = ok(edit::rippleTrim(s, tl.seq, a, edit::Edge::Tail, f(-2)));
        CHECK(layout(r.state, tl.seq) == "InA@3 InB@6");
    }
    SECTION("a ripple trim that lengthens the clip") {
        const auto r = ok(edit::rippleTrim(s, tl.seq, a, edit::Edge::Tail, f(2)));
        CHECK(layout(r.state, tl.seq) == "InA@3 InB@10");
    }
    SECTION("Q: Ripple Trim Previous Edit to Playhead") {
        const auto r = ok(edit::rippleTrimPreviousEditToPlayhead(s, tl.seq, f(8))); // removes [6, 8)
        CHECK(layout(r.state, tl.seq) == "InA@3 InB@6");
    }
    SECTION("W: Ripple Trim Next Edit to Playhead") {
        const auto r = ok(edit::rippleTrimNextEditToPlayhead(s, tl.seq, f(2))); // removes [2, 6)
        CHECK(layout(r.state, tl.seq) == "InA@2 InB@4");
    }
}

TEST_CASE("Ripple Sequence Markers: a marker inside a removed range moves to the cut", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaa|
    )");
    tl.linkMatching();
    Marker whole = span(4, 2, "Whole");
    whole.highlight = MarkerHighlight{0.9, HighlightConfidence::High, {"ace"}, f(5), "highlights"};
    const ProjectState s = withMarkers(tl.state, tl.seq, {whole, marker(5, "Inside"), span(7, 3, "Tail")});
    const auto r = ok(edit::extract(withMarks(s, tl.seq, 3, 7), tl.seq)); // removes [3, 8)
    // Nothing is deleted (Premiere deletes them; a chapter is never lost silently here): a marker fully inside becomes a
    // point at the cut, a range keeps what is left, and a highlight's peak stays inside its marker.
    CHECK(layout(r.state, tl.seq) == "Whole@3 Inside@3 Tail@3+2");
    const auto& markers = r.state.findSequence(tl.seq)->markers;
    REQUIRE(markers.size() == 3);
    REQUIRE(markers[0].highlight);
    CHECK(markers[0].highlight->peak == f(3));
    requireValid(r.state);
}

TEST_CASE("Ripple Sequence Markers: speed changes with ripple move later markers", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBb..........|
        A1 tgt |AaaaaaaaaaBb..........|
    )");
    tl.linkMatching();
    const ProjectState s = withMarkers(tl.state, tl.seq, {marker(3, "OverA"), marker(11, "OnB")});
    const std::vector<ClipId> a{tl.clip("V1@0")};
    edit::SpeedChange change;
    change.ripple = true;
    SECTION("twice as fast: the clip is 5 frames shorter") {
        change.speed = Speed{2, 1};
        CHECK(layout(ok(edit::setSpeed(s, tl.seq, a, change)).state, tl.seq) == "OverA@3 OnB@6");
    }
    SECTION("half speed: the clip is 10 frames longer") {
        change.speed = Speed{1, 2};
        CHECK(layout(ok(edit::setSpeed(s, tl.seq, a, change)).state, tl.seq) == "OverA@3 OnB@21");
    }
    SECTION("without ripple nothing moves") {
        change.speed = Speed{2, 1};
        change.ripple = false;
        CHECK(layout(ok(edit::setSpeed(s, tl.seq, a, change)).state, tl.seq) == "OverA@3 OnB@11");
    }
}

TEST_CASE("Ripple Sequence Markers: lift overwrite roll and time remapping leave markers alone", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaBbbbbbbbbbbbbb|
        A1 tgt |AaaaaaBbbbbbbbbbbbbb|
    )");
    tl.linkMatching();
    const ProjectState s = withMarkers(tl.state, tl.seq, {marker(2, "One"), marker(8, "Two"), span(12, 3, "Three")});
    const std::string before = layout(s, tl.seq);
    CHECK(layout(ok(edit::lift(withMarks(s, tl.seq, 3, 7), tl.seq)).state, tl.seq) == before);
    CHECK(layout(ok(edit::overwriteEdit(s, tl.seq, sourceOf(tl, 'S', 3), f(4))).state, tl.seq) == before);
    CHECK(layout(ok(edit::rollEdit(s, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, f(2))).state, tl.seq) == before);
    const ClipId b = tl.clip("V1@6");
    CHECK(layout(ok(edit::moveClips(s, tl.seq, std::span(&b, 1), f(2))).state, tl.seq) == before);
    // A hold inside a remapped audio clip keeps the clip's length: the captions after it move, the markers stay.
    const ClipId audio = tl.clip("A1@6");
    const ProjectState remapped = ok(edit::setTimeRemapEnabled(s, tl.seq, audio, true)).state;
    CHECK(layout(ok(edit::addTimeRemapHold(remapped, tl.seq, audio, f(9), f(2))).state, tl.seq) == before);
}

TEST_CASE("Ripple Sequence Markers off: ripple edits leave the markers where they are", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCccc|
        A1 tgt |AaaaBbbbCccc|
    )");
    tl.linkMatching();
    const ProjectState s =
        rippleMarkersOff(withMarkers(tl.state, tl.seq, {marker(2, "InA"), marker(5, "InB"), marker(9, "InC")}), tl.seq);
    CHECK_FALSE(s.findSequence(tl.seq)->rippleMarkers);
    const std::string before = layout(s, tl.seq);
    CHECK(layout(ok(edit::extract(withMarks(s, tl.seq, 4, 7), tl.seq)).state, tl.seq) == before);
    CHECK(layout(ok(edit::insertEdit(s, tl.seq, sourceOf(tl, 'S', 3), f(4))).state, tl.seq) == before);
    const ClipId b = tl.clip("V1@4");
    CHECK(layout(ok(edit::rippleDeleteClips(s, tl.seq, std::span(&b, 1))).state, tl.seq) == before);
    CHECK(layout(ok(edit::rippleTrimNextEditToPlayhead(s, tl.seq, f(2))).state, tl.seq) == before);
    // Dead-time removal keeps its own "move markers" option, whatever the sequence says.
    const TimeRange range = TimeRange::fromStartEnd(f(4), f(8));
    CHECK(layout(ok(edit::removeTimeRanges(s, tl.seq, std::span(&range, 1), true)).state, tl.seq) == "InA@2 InB@4 InC@5");
}

TEST_CASE("Ripple Sequence Markers is a sequence setting with one change and undo", "[model][markers][ripple]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    edit::SequenceSettingsChange off;
    off.rippleMarkers = false;
    // The switch is a sequence setting (the views refresh the sequence's header); no track or marker changes.
    const auto result = ok(edit::setSequenceSettings(tl.state, tl.seq, off));
    CHECK_FALSE(result.state.findSequence(tl.seq)->rippleMarkers);
    REQUIRE(result.changes.sequence(tl.seq));
    CHECK(result.changes.sequence(tl.seq)->settingsChanged);
    CHECK_FALSE(result.changes.sequence(tl.seq)->markersChanged);
    CHECK(result.changes.sequence(tl.seq)->tracks.empty());
    // The same value again is no change.
    edit::SequenceSettingsChange on;
    on.rippleMarkers = true;
    CHECK(ok(edit::setSequenceSettings(tl.state, tl.seq, on)).changes.empty());
    // One undo step, undone.
    EditSession session(tl.state);
    REQUIRE(session.apply("Ripple Sequence Markers",
                          [&](const ProjectState& state) { return edit::setSequenceSettings(state, tl.seq, off); }));
    CHECK_FALSE(session.snapshot()->findSequence(tl.seq)->rippleMarkers);
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Ripple Sequence Markers");
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findSequence(tl.seq)->rippleMarkers);
}
