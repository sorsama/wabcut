// Stream A, batch A7 (0.7), model: several markers at once (card 119), keyframes that follow edits (card 120: pins to a
// clip's start or end, track keyframes that ripple), and free-text notes on clips (card 125).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/KeyPins.h>
#include <wabcut/model/MarkerGroupOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Marker markerAt(std::int64_t frame, const char* name, std::int64_t length = 0) {
    Marker m;
    m.start = f(frame);
    m.duration = f(length);
    m.name = name;
    return m;
}

/// "name@frame" in list order.
std::string layout(const std::vector<Marker>& markers) {
    std::string out;
    for (const Marker& m : markers) {
        if (!out.empty()) {
            out += ' ';
        }
        out += m.name + "@" + std::to_string(ticksToFrames(m.start, kRate));
    }
    return out;
}

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) { return state.findSequence(seq)->findClip(id)->clip(); }

const Curve& opacityCurve(const ProjectState& state, SequenceId seq, ClipId id) {
    const Curve* curve = clipOf(state, seq, id).properties()[ParamId::Opacity].curve();
    REQUIRE(curve != nullptr);
    return *curve;
}

/// "frame:value" of every key, in order (frames relative to the media's start).
std::string keys(const Curve& curve) {
    std::string out;
    for (const Keyframe& k : curve.keys) {
        if (!out.empty()) {
            out += ' ';
        }
        out += std::to_string(ticksToFrames(k.time, kRate)) + ":" + std::to_string(static_cast<int>(k.value * 100));
    }
    return out;
}

const edit::ParamRef kOpacity = edit::ParamRef::of(ParamId::Opacity);

} // namespace

// ---- 119: several markers at once ----------------------------------------------------------------------------

TEST_CASE("removeMarkersAt removes the named markers and keeps the others in order (card 119)", "[model][markers][group]") {
    const std::vector<Marker> markers{markerAt(1, "a"), markerAt(3, "b"), markerAt(5, "c"), markerAt(7, "d")};
    const std::size_t drop[] = {2, 0, 2}; // any order, one repeated
    const auto r = edit::removeMarkersAt(markers, drop);
    REQUIRE(r);
    CHECK(layout(r->markers) == "b@3 d@7");
    CHECK(r->touched.empty());
    const std::size_t outside[] = {4};
    CHECK_FALSE(edit::removeMarkersAt(markers, outside));
    CHECK_FALSE(edit::removeMarkersAt(markers, {}));
    const std::size_t everything[] = {0, 1, 2, 3};
    CHECK(edit::removeMarkersAt(markers, everything)->markers.empty());
}

TEST_CASE("moveMarkersBy moves a group together, keeps the list sorted and stops at 0 (card 119)", "[model][markers][group]") {
    std::vector<Marker> markers{markerAt(2, "a"), markerAt(4, "b", 3), markerAt(9, "c"), markerAt(12, "d")};
    markers[1].highlight = MarkerHighlight{};
    markers[1].highlight->peak = f(5);
    const std::size_t picked[] = {1, 2};

    SECTION("later: across an unselected marker the list re-sorts and `touched` follows the moved ones") {
        const auto r = edit::moveMarkersBy(markers, picked, f(6));
        REQUIRE(r);
        CHECK(layout(r->markers) == "a@2 b@10 d@12 c@15");
        CHECK(r->touched == std::vector<std::size_t>{1, 3});
        CHECK(r->appliedDelta == f(6));
        CHECK(r->markers[1].duration == f(3));          // its length stays
        CHECK(r->markers[1].highlight->peak == f(11));  // and its highlight's peak moves with it
    }
    SECTION("earlier: cut short so the earliest picked marker stops at 0 and the others keep their distance") {
        const auto r = edit::moveMarkersBy(markers, picked, f(-10));
        REQUIRE(r);
        CHECK(r->appliedDelta == f(-4));
        CHECK(layout(r->markers) == "b@0 a@2 c@5 d@12");
        CHECK(r->touched == std::vector<std::size_t>{0, 2});
    }
    SECTION("nothing named") {
        CHECK_FALSE(edit::moveMarkersBy(markers, {}, f(1)));
    }
}

TEST_CASE("duplicateMarkersBy copies a group at an offset and pasteMarkersAt puts a copied group at a time (card 119)",
          "[model][markers][group]") {
    std::vector<Marker> markers{markerAt(2, "a"), markerAt(4, "b", 2), markerAt(20, "z")};
    markers[1].colorIndex = 3;
    markers[1].type = MarkerType::Chapter;
    const std::size_t picked[] = {0, 1};

    SECTION("duplicate") {
        const auto r = edit::duplicateMarkersBy(markers, picked, f(10));
        REQUIRE(r);
        CHECK(layout(r->markers) == "a@2 b@4 a@12 b@14 z@20");
        CHECK(r->touched == std::vector<std::size_t>{2, 3});
        CHECK(r->markers[3].colorIndex == 3);
        CHECK(r->markers[3].type == MarkerType::Chapter);
        CHECK(r->markers[3].duration == f(2));
        CHECK_FALSE(edit::duplicateMarkersBy(markers, picked, Ticks{}));  // on top of the originals: refused
        CHECK_FALSE(edit::duplicateMarkersBy(markers, picked, f(-3)));    // "a" would start before 0
        CHECK(edit::duplicateMarkersBy(markers, picked, f(-2))->markers.front().start == Ticks{});
    }
    SECTION("paste keeps the distances; the earliest copied marker lands on `at`") {
        const std::vector<Marker> copied{markers[1], markers[0]}; // any order
        const auto r = edit::pasteMarkersAt(markers, copied, f(30));
        REQUIRE(r);
        CHECK(layout(r->markers) == "a@2 b@4 z@20 a@30 b@32");
        CHECK(r->touched == std::vector<std::size_t>{3, 4});
        CHECK_FALSE(edit::pasteMarkersAt(markers, {}, f(1)));
        CHECK_FALSE(edit::pasteMarkersAt(markers, copied, f(-1)));
    }
    SECTION("the limit of markers per list is kept") {
        std::vector<Marker> many(kMaxMarkers, markerAt(1, "x"));
        const std::size_t one[] = {0};
        CHECK_FALSE(edit::duplicateMarkersBy(many, one, f(1)));
        CHECK_FALSE(edit::pasteMarkersAt(many, std::span<const Marker>(many.data(), 1), f(1)));
    }
}

// ---- 125: notes on clips -------------------------------------------------------------------------------------

TEST_CASE("A clip's note is saved, carried by razor halves and copies, one undo step, and bounded (card 125)",
          "[model][note]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        V2     |.....Bbbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V2@5");
    edit::ClipAttributeChange write;
    write.note = "Cut the cough at 0:04";

    EditSession session(tl.state);
    REQUIRE(session.apply("Set Clip Note", [&](const ProjectState& st) {
        return edit::setClipAttributes(st, tl.seq, std::span(&a, 1), write);
    }));
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Set Clip Note");
    const ProjectState& s = *session.snapshot();
    CHECK(clipOf(s, tl.seq, a).note() == "Cut the cough at 0:04");
    CHECK(clipOf(s, tl.seq, b).note().empty()); // another clip: not touched
    REQUIRE(session.undo());
    CHECK(clipOf(*session.snapshot(), tl.seq, a).note().empty());

    SECTION("a note is a change the views hear about (the clip's label role)") {
        const auto r = ok(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), write));
        CHECK_FALSE(r.changes.empty());
    }
    SECTION("writing the same note again changes nothing; an empty note clears it") {
        const auto once = ok(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), write)).state;
        CHECK(ok(edit::setClipAttributes(once, tl.seq, std::span(&a, 1), write)).changes.empty());
        edit::ClipAttributeChange clear;
        clear.note = "";
        CHECK(clipOf(ok(edit::setClipAttributes(once, tl.seq, std::span(&a, 1), clear)).state, tl.seq, a).note().empty());
    }
    SECTION("saved with the clip, and written only when set") {
        const ProjectState noted = ok(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), write)).state;
        const auto text = io::toJson(noted);
        REQUIRE(text);
        CHECK(text->find("\"note\"") != std::string::npos);
        const auto loaded = io::fromJson(*text);
        REQUIRE(loaded);
        CHECK(clipOf(*loaded, tl.seq, a).note() == "Cut the cough at 0:04");
        const auto plain = io::toJson(tl.state);
        REQUIRE(plain);
        CHECK(plain->find("\"note\"") == std::string::npos);
    }
    SECTION("both halves of a razor cut carry it") {
        const ProjectState noted = ok(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), write)).state;
        const TrackId v1 = tl.track("V1");
        const auto cut = ok(edit::addEdit(noted, tl.seq, f(4), std::span(&v1, 1)));
        REQUIRE(cut.created.size() == 1);
        CHECK(clipOf(cut.state, tl.seq, cut.created.front()).note() == "Cut the cough at 0:04");
        CHECK(clipOf(cut.state, tl.seq, a).note() == "Cut the cough at 0:04");
    }
    SECTION("too long a note is refused; the limit itself is fine") {
        edit::ClipAttributeChange tooLong;
        tooLong.note = std::string(kMaxClipNoteBytes + 1, 'n');
        CHECK_FALSE(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), tooLong));
        edit::ClipAttributeChange limit;
        limit.note = std::string(kMaxClipNoteBytes, 'n');
        CHECK(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), limit));
    }
    SECTION("a project file with a note over the limit does not load") {
        const ProjectState noted = ok(edit::setClipAttributes(tl.state, tl.seq, std::span(&a, 1), write)).state;
        auto text = io::toJson(noted);
        REQUIRE(text);
        const std::size_t at = text->find("Cut the cough at 0:04");
        REQUIRE(at != std::string::npos);
        text->replace(at, std::string("Cut the cough at 0:04").size(), std::string(kMaxClipNoteBytes + 1, 'n'));
        CHECK_FALSE(io::fromJson(*text));
    }
}

// ---- 120: pinned keyframes -----------------------------------------------------------------------------------

TEST_CASE("A keyframe pinned to a clip's start follows a head trim; unpinned and inert ones stay (card 120)",
          "[model][keyframes][pin]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    ProjectState s = ok(edit::addKeyframe(tl.state, tl.seq, a, kOpacity, Keyframe{f(0), 0.0})).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, kOpacity, Keyframe{f(4), 1.0})).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, kOpacity, Keyframe{f(6), 0.5})).state;
    CHECK(keys(opacityCurve(s, tl.seq, a)) == "0:0 4:100 6:50");

    SECTION("a plain edit leaves media-time keys on the picture (the Premiere behaviour)") {
        EditSession session(s);
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, a, edit::Edge::Head, f(3)); }));
        CHECK(keys(opacityCurve(*session.snapshot(), tl.seq, a)) == "0:0 4:100 6:50");
    }
    SECTION("pinned to the start, the key moves to the new first frame, keeping its value, in the same undo step") {
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 0, KeyPin::ClipStart)).state;
        CHECK(opacityCurve(s, tl.seq, a).keys[0].pin == KeyPin::ClipStart);
        EditSession session(s);
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, a, edit::Edge::Head, f(3)); }));
        CHECK(session.undoCount() == 1);
        const ProjectState& after = *session.snapshot();
        CHECK(clipOf(after, tl.seq, a).sourceIn == f(3));
        CHECK(keys(opacityCurve(after, tl.seq, a)) == "3:0 4:100 6:50"); // the unpinned keys stay on the picture
        CHECK(opacityCurve(after, tl.seq, a).keys[0].pin == KeyPin::ClipStart);
        // And again, further: the pin keeps following; undo takes the whole trim back, key included.
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, a, edit::Edge::Head, f(2)); }));
        CHECK(keys(opacityCurve(*session.snapshot(), tl.seq, a)) == "4:100 5:0 6:50");
        REQUIRE(session.undo());
        CHECK(keys(opacityCurve(*session.snapshot(), tl.seq, a)) == "3:0 4:100 6:50");
        REQUIRE(session.undo());
        CHECK(keys(opacityCurve(*session.snapshot(), tl.seq, a)) == "0:0 4:100 6:50");
    }
    SECTION("a pinned key moved off its end is inert: the pin is kept but does not follow") {
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 0, KeyPin::ClipStart)).state;
        s = ok(edit::moveKeyframe(s, tl.seq, a, kOpacity, 0, f(2))).state;
        EditSession session(s);
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, a, edit::Edge::Head, f(1)); }));
        CHECK(keys(opacityCurve(*session.snapshot(), tl.seq, a)) == "2:0 4:100 6:50");
    }
    SECTION("unpinning leaves the key where it is") {
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 0, KeyPin::ClipStart)).state;
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 0, KeyPin::None)).state;
        CHECK(opacityCurve(s, tl.seq, a).keys[0].pin == KeyPin::None);
        EditSession session(s);
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, a, edit::Edge::Head, f(3)); }));
        CHECK(keys(opacityCurve(*session.snapshot(), tl.seq, a)) == "0:0 4:100 6:50");
    }
    SECTION("pinning a key elsewhere moves it onto the end, replacing a key already there") {
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 1, KeyPin::ClipStart)).state; // the key at 4
        CHECK(keys(opacityCurve(s, tl.seq, a)) == "0:100 6:50");
        CHECK(opacityCurve(s, tl.seq, a).keys[0].pin == KeyPin::ClipStart);
        CHECK_FALSE(edit::setKeyframePin(s, tl.seq, a, kOpacity, 5, KeyPin::ClipEnd)); // no such key
    }
    SECTION("a slip moves both ends: pinned keys at each follow") {
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 0, KeyPin::ClipStart)).state;
        s = ok(edit::addKeyframe(s, tl.seq, a, kOpacity, Keyframe{clipOf(s, tl.seq, a).endCurveTime(), 0.25})).state;
        const std::size_t last = opacityCurve(s, tl.seq, a).keys.size() - 1;
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, last, KeyPin::ClipEnd)).state;
        const Ticks end0 = clipOf(s, tl.seq, a).endCurveTime();
        EditSession session(s);
        REQUIRE(session.apply("Slip", [&](const ProjectState& st) { return edit::slip(st, tl.seq, std::span(&a, 1), f(2)); }));
        const Curve& curve = opacityCurve(*session.snapshot(), tl.seq, a);
        CHECK(curve.keys.front().time == f(2));
        CHECK(curve.keys.back().time == end0 + f(2));
        CHECK(curve.keys.back().value == 0.25);
        CHECK(curve.isWellFormed());
    }
    SECTION("pins are saved with the keys and load back; unpinned keys write nothing") {
        const auto plain = io::toJson(s);
        REQUIRE(plain);
        CHECK(plain->find("pinTo") == std::string::npos);
        s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 0, KeyPin::ClipStart)).state;
        const auto text = io::toJson(s);
        REQUIRE(text);
        CHECK(text->find("\"pinTo\"") != std::string::npos);
        const auto loaded = io::fromJson(*text);
        REQUIRE(loaded);
        CHECK(opacityCurve(*loaded, tl.seq, a).keys[0].pin == KeyPin::ClipStart);
        CHECK(opacityCurve(*loaded, tl.seq, a).keys[1].pin == KeyPin::None);
    }
}

TEST_CASE("A keyframe pinned to a clip's end follows a tail trim; a razor cut leaves it with the half that owns the end (card 120)",
          "[model][keyframes][pin]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    ProjectState s = ok(edit::addKeyframe(tl.state, tl.seq, a, kOpacity, Keyframe{f(1), 1.0})).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, kOpacity, Keyframe{clipOf(s, tl.seq, a).endCurveTime(), 0.0})).state;
    s = ok(edit::setKeyframePin(s, tl.seq, a, kOpacity, 1, KeyPin::ClipEnd)).state;
    const Ticks end0 = clipOf(s, tl.seq, a).endCurveTime();

    SECTION("a tail trim") {
        EditSession session(s);
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, a, edit::Edge::Tail, f(-4)); }));
        const Ticks end1 = clipOf(*session.snapshot(), tl.seq, a).endCurveTime();
        CHECK(end1 < end0);
        const Curve& curve = opacityCurve(*session.snapshot(), tl.seq, a);
        REQUIRE(curve.keys.size() == 2);
        CHECK(curve.keys[1].time == end1);
        CHECK(curve.keys[1].value == 0.0); // the fade-out still ends on the last frame
    }
    SECTION("a razor cut: the right half keeps the end (and the key); the left half's trim does not drag it") {
        EditSession session(s);
        const TrackId v1 = tl.track("V1");
        REQUIRE(session.apply("Add Edit", [&](const ProjectState& st) { return edit::addEdit(st, tl.seq, f(6), std::span(&v1, 1)); }));
        const Sequence& seq = *session.snapshot()->findSequence(tl.seq);
        const Track& track = *seq.findTrack(v1);
        REQUIRE(track.clips.size() == 2);
        const ClipId right = track.clips[1].id;
        // Both halves share one curve, and the key still sits at the original end, which is the right half's end.
        CHECK(opacityCurve(*session.snapshot(), tl.seq, a).keys[1].time == end0);
        CHECK(clipOf(*session.snapshot(), tl.seq, right).endCurveTime() == end0);
        // Trimming the right half's tail now drags it.
        REQUIRE(session.apply("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, tl.seq, right, edit::Edge::Tail, f(-2)); }));
        const Ticks end1 = clipOf(*session.snapshot(), tl.seq, right).endCurveTime();
        CHECK(end1 < end0);
        CHECK(opacityCurve(*session.snapshot(), tl.seq, right).keys[1].time == end1);
    }
}

// ---- 120: track keyframes that ripple ------------------------------------------------------------------------

namespace {

edit::EffectTarget trackTarget(const Timeline& tl, const char* label) { return edit::EffectTarget::ofTrack(tl.track(label)); }

/// A track's fader keyframes as "frame:dB" in order.
std::string faderKeys(const ProjectState& state, const Timeline& tl, const char* label) {
    const Track* track = state.findSequence(tl.seq)->findTrack(tl.track(label));
    const Curve* curve = track->volume.curve();
    if (curve == nullptr) {
        return "constant";
    }
    std::string out;
    for (const Keyframe& k : curve->keys) {
        if (!out.empty()) {
            out += ' ';
        }
        out += std::to_string(ticksToFrames(k.time, kRate)) + ":" + std::to_string(static_cast<int>(k.value));
    }
    return out;
}

ProjectState withFaderKeys(ProjectState s, const Timeline& tl, const char* label, std::initializer_list<std::pair<int, double>> keys) {
    for (const auto& [frame, db] : keys) {
        s = ok(edit::addKeyframe(s, tl.seq, trackTarget(tl, label), edit::ParamRef::of(ParamId::VolumeDb), Keyframe{f(frame), db})).state;
    }
    return s;
}

ProjectState rippleKeys(ProjectState s, const Timeline& tl, bool on) {
    edit::SequenceSettingsChange change;
    change.rippleTrackKeyframes = on;
    return ok(edit::setSequenceSettings(s, tl.seq, change)).state;
}

edit::SourceClip sourceOf(const Timeline& tl, char letter, std::int64_t frames) {
    return edit::SourceClip{tl.media(letter), {f(0), f(frames)},
                            {{{StreamKind::Video, 0}, tl.track("V1")}, {{StreamKind::Audio, 0}, tl.track("A1")}}};
}

} // namespace

TEST_CASE("Ripple Track Keyframes: track keys move with the material on an extract, an insert and a ripple delete (card 120)",
          "[model][keyframes][ripple]") {
    Timeline tl(R"(
        V1 tgt     |Aaaaaaaaaaaa|
        A1 tgt     |Aaaaaaaaaaaa|
        A2 nosync  |Bbbbbbbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s = withFaderKeys(tl.state, tl, "A1", {{2, 0.0}, {4, -12.0}, {6, -6.0}, {10, 0.0}});
    s = withFaderKeys(s, tl, "A2", {{2, 0.0}, {6, -6.0}, {10, 0.0}});
    CHECK_FALSE(s.findSequence(tl.seq)->rippleTrackKeyframes); // off by default: nothing changes for existing projects
    auto marked = ok(edit::markIn(s, tl.seq, f(3))).state;
    marked = ok(edit::markOut(marked, tl.seq, f(4))).state; // [3, 5)

    SECTION("off: an extract leaves every key where it is") {
        CHECK(faderKeys(ok(edit::extract(marked, tl.seq)).state, tl, "A1") == "2:0 4:-12 6:-6 10:0");
    }
    SECTION("on: an extract removes the keys inside the cut and moves later ones back, on the sync-locked track only") {
        const auto r = ok(edit::extract(rippleKeys(marked, tl, true), tl.seq));
        CHECK(faderKeys(r.state, tl, "A1") == "2:0 4:-6 8:0");        // 4 was inside [3, 5); 6 -> 4; 10 -> 8
        CHECK(faderKeys(r.state, tl, "A2") == "2:0 6:-6 10:0");       // sync lock off: it does not ripple
        requireValid(r.state);
    }
    SECTION("on: an insert edit moves keys at or after the insert point later") {
        const auto r = ok(edit::insertEdit(rippleKeys(s, tl, true), tl.seq, sourceOf(tl, 'S', 3), f(4)));
        CHECK(faderKeys(r.state, tl, "A1") == "2:0 7:-12 9:-6 13:0");
        CHECK(faderKeys(r.state, tl, "A2") == "2:0 6:-6 10:0");
    }
    SECTION("on: a ripple delete of a clip") {
        Timeline two(R"(
            V1 tgt |AaaaBbbbCccc|
            A1 tgt |AaaaBbbbCccc|
        )");
        two.linkMatching();
        ProjectState t = withFaderKeys(two.state, two, "A1", {{1, 0.0}, {5, -3.0}, {9, -6.0}});
        t = rippleKeys(t, two, true);
        const ClipId b = two.clip("V1@4");
        const auto r = ok(edit::rippleDeleteClips(t, two.seq, std::span(&b, 1)));
        CHECK(faderKeys(r.state, two, "A1") == "1:0 5:-6"); // the key at 5 was inside Bbbb; the one at 9 moves to 5
    }
    SECTION("on, and every key inside the cut: the curve keeps its last key at the cut instead of vanishing") {
        Timeline one(R"(
            V1 tgt |Aaaaaaaa|
            A1 tgt |Aaaaaaaa|
        )");
        one.linkMatching();
        ProjectState t = withFaderKeys(one.state, one, "A1", {{3, -3.0}, {4, -9.0}});
        t = rippleKeys(t, one, true);
        auto m = ok(edit::markIn(t, one.seq, f(2))).state;
        m = ok(edit::markOut(m, one.seq, f(5))).state;
        CHECK(faderKeys(ok(edit::extract(m, one.seq)).state, one, "A1") == "2:-9");
    }
    SECTION("the setting is a sequence setting that saves and loads back") {
        const ProjectState on = rippleKeys(s, tl, true);
        const auto text = io::toJson(on);
        REQUIRE(text);
        CHECK(text->find("\"rippleTrackKeyframes\"") != std::string::npos);
        const auto loaded = io::fromJson(*text);
        REQUIRE(loaded);
        CHECK(loaded->findSequence(tl.seq)->rippleTrackKeyframes);
        const auto plain = io::toJson(s);
        REQUIRE(plain);
        CHECK(plain->find("rippleTrackKeyframes") == std::string::npos);
        EditSession session(s);
        edit::SequenceSettingsChange change;
        change.rippleTrackKeyframes = true;
        REQUIRE(session.apply("Ripple Track Keyframes", [&](const ProjectState& st) { return edit::setSequenceSettings(st, tl.seq, change); }));
        CHECK(session.undoCount() == 1);
        REQUIRE(session.undo());
        CHECK_FALSE(session.snapshot()->findSequence(tl.seq)->rippleTrackKeyframes);
    }
}
