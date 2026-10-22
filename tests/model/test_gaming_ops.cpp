// C.16 (gaming): instant replay, dead-time detection and removal, zoom punch, camera shake and vertical / square versions.

#include "NestedTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GamingOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/TimeRemap.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) { return state.findSequence(seq)->findClip(id)->clip(); }

const Clip* clipStartingAt(const ProjectState& state, SequenceId seq, std::string_view track, Ticks t) {
    const Sequence& s = *state.findSequence(seq);
    for (const auto* list : {&s.videoTracks, &s.audioTracks}) {
        for (const auto& tr : *list) {
            const std::string label = std::string(tr->kind == TrackKind::Video ? "V" : "A") +
                                      std::to_string(s.trackPosition(tr->id)->index + 1);
            if (label != track) {
                continue;
            }
            for (const Clip& c : tr->clips) {
                if (c.start() == t) {
                    return &c;
                }
            }
        }
    }
    return nullptr;
}

/// A one-text-layer template standing in for the REPLAY stinger.
GraphicTemplate stinger() {
    Graphic g;
    g.name = "Replay";
    GraphicLayer text;
    text.id = 1;
    text.kind = LayerKind::Text;
    text.text.runs = {TextRun{"REPLAY", {}}};
    g.layers.push_back(text);
    g.responsive = ResponsiveTime{Ticks::fromSeconds(2), Ticks{kTicksPerSecond / 2}, Ticks{kTicksPerSecond / 4}};
    TemplateField label;
    label.id = "label";
    label.label = "Label";
    label.kind = FieldKind::Text;
    label.layer = 1;
    label.property = "text";
    auto t = edit::makeTemplate(g, "test.replay", "Replay", {label});
    REQUIRE(t.has_value());
    return *t;
}

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

} // namespace

// ---- Instant replay -----------------------------------------------------------------------------------------------------

TEST_CASE("Instant replay inserts a ramped slow-motion copy of the moment with a stinger and flashes (C.16)", "[model][gaming][replay]") {
    Timeline tl(R"(
        V1 |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbb|
        A1 |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s0 = ok(edit::addMarker(tl.state, tl.seq, Marker{f(40), f(2), "After"})).state;
    s0 = ok(edit::addMarker(s0, tl.seq, Marker{f(5), Ticks{}, "Before"})).state;
    const GraphicTemplate tmpl = stinger();
    ReplayOptions o;
    o.range = TimeRange::fromStartEnd(f(10), f(30));
    o.label = &tmpl;
    FieldValue text;
    text.text = "รีเพลย์";
    o.labelValues.emplace_back("label", text);
    const auto r = edit::instantReplay(s0, tl.seq, o);
    REQUIRE(r.has_value());
    const ProjectState& s = r->edit.state;
    requireValid(s);
    // 20 frames at half speed with 0.2 s ramps: 7 + 20 + 7 frames.
    CHECK(r->moment == TimeRange::fromStartEnd(f(10), f(30)));
    CHECK(r->replay == TimeRange{f(30), f(34)});
    REQUIRE(r->clips.size() == 1); // video only: the replay is silent
    const Clip& replay = clipOf(s, tl.seq, r->clips[0]);
    CHECK(replay.timelineRange == TimeRange{f(30), f(34)});
    REQUIRE(replay.isRemapped());
    CHECK(replay.sourceTimeAt(f(30)) == f(10));
    // Monotonic, inside the moment's source, starting near 100 %, half speed in the middle, back near 100 % at the end.
    Ticks previous = replay.sourceTimeAt(f(30));
    for (int i = 31; i < 64; ++i) {
        const Ticks now = replay.sourceTimeAt(f(i));
        CHECK(now >= previous);
        CHECK(now < f(30));
        previous = now;
    }
    CHECK(seconds(replay.sourceTimeAt(f(31)) - replay.sourceTimeAt(f(30))) > 0.8 * seconds(f(1)));
    CHECK(seconds(replay.sourceTimeAt(f(47)) - replay.sourceTimeAt(f(37))) == Approx(seconds(f(5))).margin(seconds(f(1)) * 0.1));
    CHECK(seconds(replay.sourceTimeAt(f(63)) - replay.sourceTimeAt(f(62))) > 0.8 * seconds(f(1)));
    // The rest of A follows the replay; A1 opened the same gap (sync lock); B moved by the replay's length.
    const Clip* rest = clipStartingAt(s, tl.seq, "V1", f(64));
    REQUIRE(rest);
    CHECK(rest->sourceIn == f(30));
    CHECK(clipStartingAt(s, tl.seq, "A1", f(64)) != nullptr);
    CHECK(clipStartingAt(s, tl.seq, "A1", f(30)) == nullptr);
    CHECK(clipStartingAt(s, tl.seq, "V1", f(74)) != nullptr);
    // The stinger over the replay on a new V2, with the field value.
    REQUIRE(r->label.isValid());
    const Clip& label = clipOf(s, tl.seq, r->label);
    CHECK(label.timelineRange == TimeRange{f(30), f(34)});
    REQUIRE(label.graphic());
    REQUIRE(label.graphic()->instance.has_value());
    CHECK(label.graphic()->instance->templateId == "test.replay");
    CHECK(label.graphic()->layers[0].text.text() == "รีเพลย์");
    CHECK(s.findSequence(tl.seq)->trackPosition(s.findSequence(tl.seq)->findClip(r->label)->track->id)->index == 1);
    // Dip-to-white flashes at both cuts.
    REQUIRE(r->flashes.size() == 2);
    const Track& v1 = *s.findSequence(tl.seq)->videoTracks[0];
    REQUIRE(v1.transitions.size() == 2);
    for (const Transition& t : v1.transitions) {
        CHECK(t.effect == "dipToWhite");
    }
    // Markers after the moment rode along; earlier ones stayed.
    const auto& markers = s.findSequence(tl.seq)->markers;
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].start == f(5));
    CHECK(markers[1].start == f(74));
    // One edit, saved and read back the same.
    CHECK_FALSE(r->edit.changes.empty());
    CHECK(r->edit.playhead == f(30));
    const auto back = io::fromJson(io::toJson(s).value());
    REQUIRE(back.has_value());
    CHECK(clipOf(*back, tl.seq, r->clips[0]) == replay);
}

TEST_CASE("Instant replay of a range keeps clips across a cut in step and can replay the audio (C.16)", "[model][gaming][replay]") {
    Timeline tl(R"(
        V2 |..Ffffffffffffff....|
        V1 |AaaaaaaaaaBbbbbbbbbb|
        A1 |AaaaaaaaaaBbbbbbbbbb|
    )");
    tl.linkMatching();
    ReplayOptions o;
    o.range = TimeRange::fromStartEnd(f(5), f(15));
    o.includeAudio = true;
    o.flash = false;
    const auto r = edit::instantReplay(tl.state, tl.seq, o);
    REQUIRE(r.has_value());
    const ProjectState& s = r->edit.state;
    requireValid(s);
    // 10 frames: 3-frame ramps around 10 frames at half speed = 16 frames.
    CHECK(r->replay == TimeRange{f(15), f(16)});
    CHECK(r->clips.size() == 5); // A and B on V1 and A1, F on V2
    const Clip* a = clipStartingAt(s, tl.seq, "V1", f(15));
    const Clip* b = clipStartingAt(s, tl.seq, "V1", f(23));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->end() == f(23));
    CHECK(b->end() == f(31));
    CHECK(a->sourceTimeAt(f(15)) == f(5));
    CHECK(b->sourceTimeAt(f(23)) == f(0));
    const Clip* f2 = clipStartingAt(s, tl.seq, "V2", f(15));
    REQUIRE(f2);
    CHECK(f2->end() == f(31));
    // The audio copies play at a constant slower speed over the same spans, linked with their pictures.
    const Clip* aa = clipStartingAt(s, tl.seq, "A1", f(15));
    const Clip* ab = clipStartingAt(s, tl.seq, "A1", f(23));
    REQUIRE(aa);
    REQUIRE(ab);
    CHECK_FALSE(aa->isRemapped());
    CHECK(static_cast<double>(aa->speed.num) / aa->speed.den == Approx(5.0 / 8.0).margin(0.01));
    CHECK(aa->end() == f(23));
    CHECK(aa->linkGroup.isValid());
    CHECK(aa->linkGroup == a->linkGroup);
    CHECK(ab->linkGroup == b->linkGroup);
    CHECK(aa->linkGroup != ab->linkGroup);
    CHECK(r->flashes.empty());
    CHECK_FALSE(r->label.isValid());
}

TEST_CASE("Instant replay without a ramp is a constant slow motion and refuses what it cannot slow (C.16)", "[model][gaming][replay]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa..........|
        A1 |Aaaaaaaaaa..........|
    )");
    ReplayOptions o;
    o.clip = tl.clip("V1@0");
    o.ramp = false;
    o.speed = 0.25;
    o.flash = false;
    const auto r = edit::instantReplay(tl.state, tl.seq, o);
    REQUIRE(r.has_value());
    requireValid(r->edit.state);
    CHECK(r->replay == TimeRange{f(10), f(40)});
    const Clip& replay = clipOf(r->edit.state, tl.seq, r->clips[0]);
    CHECK(replay.sourceTimeAt(f(14)) == f(1)); // exactly a quarter speed: straight remap segments
    CHECK(replay.sourceTimeAt(f(30)) == f(5));

    // Errors.
    ReplayOptions both = o;
    both.range = TimeRange{f(0), f(5)};
    CHECK_FALSE(edit::instantReplay(tl.state, tl.seq, both).has_value());
    ReplayOptions none;
    CHECK_FALSE(edit::instantReplay(tl.state, tl.seq, none).has_value());
    ReplayOptions slow = o;
    slow.speed = 0.01;
    CHECK_FALSE(edit::instantReplay(tl.state, tl.seq, slow).has_value());
    ReplayOptions gap;
    gap.range = TimeRange::fromStartEnd(f(12), f(18));
    CHECK_FALSE(edit::instantReplay(tl.state, tl.seq, gap).has_value());
    ReplayOptions audio;
    audio.clip = tl.clip("A1@0");
    CHECK_FALSE(edit::instantReplay(tl.state, tl.seq, audio).has_value());
    // A reversed clip cannot be slowed this way.
    edit::SpeedChange reverse;
    reverse.speed = Speed{1, 1};
    reverse.reversed = true;
    const ClipId v1 = tl.clip("V1@0");
    const ProjectState reversed = ok(edit::setSpeed(tl.state, tl.seq, std::span<const ClipId>(&v1, 1), reverse)).state;
    const auto refused = edit::instantReplay(reversed, tl.seq, o);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("reverse") != std::string::npos);
}

// ---- Dead time ----------------------------------------------------------------------------------------------------------

TEST_CASE("Dead time is found where game audio and motion are both low and kept where unmeasured (C.16)", "[model][gaming][deadtime]") {
    // 10 s of A (250 frames), then B without activity.
    std::string v = "A" + std::string(249, 'a') + "B" + std::string(49, 'b');
    Timeline tl("    V1 |" + v + "|\n    A1 |" + v + "|\n");
    tl.linkMatching();
    // A is loud and busy except from 2 s to 7 s; B's activity is unknown.
    MediaActivity a;
    a.step = Ticks{kTicksPerSecond / 10};
    for (int i = 0; i < 400; ++i) {
        const bool quiet = i >= 20 && i < 70;
        a.audio.push_back(quiet ? 0.02F : 0.8F);
        a.motion.push_back(quiet ? 0.01F : 0.6F);
    }
    a.source = "test";
    REQUIRE_FALSE(mediaActivityProblem(a).has_value());
    const MediaId mediaA = tl.media('A');
    const ActivityLookup lookup = [&](MediaId id) { return id == mediaA ? &a : nullptr; };
    DeadTimeOptions o;
    o.minSeconds = 2.0;
    o.handleSeconds = 0.3;
    const auto found = findDeadTime(tl.state, tl.seq, lookup, o);
    REQUIRE(found.has_value());
    REQUIRE(found->ranges.size() == 1);
    const DeadTimeRange& r = found->ranges[0];
    CHECK(seconds(r.stretch.start) == Approx(2.0).margin(0.31));
    CHECK(seconds(r.stretch.end()) == Approx(7.0).margin(0.31));
    CHECK(seconds(r.remove.start - r.stretch.start) >= 0.3);
    CHECK(seconds(r.stretch.end() - r.remove.end()) >= 0.3 - 1e-9);
    CHECK(snapToFrame(r.remove.start, kRate) == r.remove.start);
    CHECK(snapToFrame(r.remove.end(), kRate) == r.remove.end());
    CHECK(r.activity < found->threshold);
    REQUIRE(found->unmeasured.size() == 1);
    CHECK(found->unmeasured[0] == tl.media('B'));
    CHECK(found->tracks.size() == 2); // V1 and A1
    // Higher sensitivity finds at least as much; a longer minimum finds nothing.
    DeadTimeOptions longer = o;
    longer.minSeconds = 8.0;
    CHECK(findDeadTime(tl.state, tl.seq, lookup, longer)->ranges.empty());
    // Bounds.
    DeadTimeOptions bad = o;
    bad.sensitivity = 1.5;
    CHECK_FALSE(findDeadTime(tl.state, tl.seq, lookup, bad).has_value());
    MediaActivity broken = a;
    broken.audio[3] = 1.5F;
    CHECK(mediaActivityProblem(broken).has_value());
    broken = a;
    broken.step = Ticks{};
    CHECK(mediaActivityProblem(broken).has_value());
    // Removing it: every track loses the same time.
    const auto removed = edit::removeTimeRanges(tl.state, tl.seq, std::span<const TimeRange>(&r.remove, 1));
    REQUIRE(removed.has_value());
    requireValid(removed->state);
    CHECK(removed->state.findSequence(tl.seq)->contentEnd() == f(300) - r.remove.duration);
}

TEST_CASE("Removing time ranges ripples every unlocked track and the markers (C.16)", "[model][gaming][deadtime]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
    )");
    ProjectState s = ok(edit::addMarker(tl.state, tl.seq, Marker{f(5), f(2), "Mid"})).state;
    Marker inside{f(6), Ticks{}, "Inside"};
    s = ok(edit::addMarker(s, tl.seq, inside)).state;
    Marker hl{f(8), f(2), "Clutch"};
    hl.highlight = MarkerHighlight{0.9, HighlightConfidence::High, {"ace"}, f(9), "highlights"};
    s = ok(edit::addMarker(s, tl.seq, hl)).state;
    const std::vector<TimeRange> ranges{TimeRange::fromStartEnd(f(6), f(7)), TimeRange::fromStartEnd(f(2), f(4))};
    const edit::EditResult r = ok(edit::removeTimeRanges(s, tl.seq, ranges));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaA4A7a|
        A1 |AaA4A7a|
    )");
    const auto& markers = r.state.findSequence(tl.seq)->markers;
    REQUIRE(markers.size() == 3);
    CHECK(markers[0].start == f(3)); // 5 - 2
    CHECK(markers[0].duration == f(1)); // [5, 7) lost frame 6
    CHECK(markers[1].start == f(4)); // inside a removed range: its start
    CHECK(markers[2].start == f(5));
    REQUIRE(markers[2].highlight.has_value());
    CHECK(markers[2].highlight->peak == f(6));
    // Without rippleMarkers they stay.
    const edit::EditResult still = ok(edit::removeTimeRanges(s, tl.seq, ranges, false));
    CHECK(still.state.findSequence(tl.seq)->markers[0].start == f(5));
    // A locked track keeps its time.
    edit::TrackStateChange lock;
    lock.locked = true;
    const ProjectState locked = ok(edit::setTrackState(s, tl.seq, tl.track("A1"), lock)).state;
    const edit::EditResult partly = ok(edit::removeTimeRanges(locked, tl.seq, ranges));
    CHECK_TIMELINE(partly.state, tl.seq, R"(
        V1 |AaA4A7a...|
        A1 |Aaaaaaaaaa|
    )");
    CHECK_FALSE(edit::removeTimeRanges(s, tl.seq, std::vector<TimeRange>{TimeRange{f(2), Ticks{}}}).has_value());
}

TEST_CASE("Dead time reads through multicam and nested clips and cuts them in place (0.2)", "[model][gaming][deadtime][multicam]") {
    // 10 s of A on V1/A1 (quiet from 2 s to 7 s); B (the second camera) is busy all along.
    std::string v = "A" + std::string(249, 'a');
    Timeline tl("    V1 |" + v + "|\n    A1 |" + v + "|\n", {{'A', 250}, {'B', 250}});
    tl.linkMatching();
    MediaActivity a;
    a.step = Ticks{kTicksPerSecond / 10};
    MediaActivity b = a;
    for (int i = 0; i < 100; ++i) {
        const bool quiet = i >= 20 && i < 70;
        a.audio.push_back(quiet ? 0.02F : 0.8F);
        a.motion.push_back(quiet ? 0.01F : 0.6F);
        b.audio.push_back(0.8F);
        b.motion.push_back(0.6F);
    }
    const MediaId mediaA = tl.media('A');
    const MediaId mediaB = tl.media('B');
    const ActivityLookup lookup = [&](MediaId id) { return id == mediaA ? &a : id == mediaB ? &b : nullptr; };
    DeadTimeOptions o;
    o.minSeconds = 2.0;
    const auto flat = findDeadTime(tl.state, tl.seq, lookup, o);
    REQUIRE(flat.has_value());
    REQUIRE(flat->ranges.size() == 1);

    SECTION("a two-angle multicam clip: the active angle's media at the angles' sync") {
        // Angle 1 = A, angle 2 = B, both synced at 0; the clip shows and plays A over the whole 10 s.
        PlacedMulticam mc = placeMulticam(tl.state, tl.seq, {mediaA, mediaB}, {Ticks{}, Ticks{}}, TimeRange{Ticks{}, f(250)}, Ticks{});
        CHECK(mediaPlayedBy(mc.state, mc.state.findSequence(tl.seq)->findClip(mc.video)->clip()) == std::vector<MediaId>{mediaA});
        const auto found = findDeadTime(mc.state, tl.seq, lookup, o);
        REQUIRE(found.has_value());
        REQUIRE(found->ranges.size() == 1);
        CHECK(found->ranges[0].stretch == flat->ranges[0].stretch);
        CHECK(found->ranges[0].remove == flat->ranges[0].remove);
        CHECK(found->unmeasured.empty());
        CHECK(found->tracks.size() == 2); // V1 (the multicam clip counts as gameplay) and A1
        // Removing it cuts the multicam clips in place: the pieces stay multicam clips of the same angles.
        const edit::EditResult removed = ok(edit::removeTimeRanges(mc.state, tl.seq, std::span<const TimeRange>(&found->ranges[0].remove, 1)));
        const Sequence& after = *removed.state.findSequence(tl.seq);
        CHECK(after.contentEnd() == f(250) - found->ranges[0].remove.duration);
        for (const auto* tracks : {&after.videoTracks, &after.audioTracks}) {
            for (const TrackPtr& t : *tracks) {
                for (const Clip& c : t->clips) {
                    CHECK(c.sequence == mc.multicam);
                    CHECK(c.angles == AngleSet::of(0));
                }
            }
        }
        // Switched to angle 2 (B, busy): no dead time.
        const edit::EditResult switched = ok(edit::switchMulticamAngle(mc.state, tl.seq, mc.video, std::nullopt, 1, edit::AngleTarget::Both));
        const auto busy = findDeadTime(switched.state, tl.seq, lookup, o);
        REQUIRE(busy.has_value());
        CHECK(busy->ranges.empty());
    }

    SECTION("a nested clip: its sequence's clips through the nest's time map") {
        const edit::NestResult nest = nestEverything(tl.state, tl.seq);
        const Sequence& outer = *nest.edit.state.findSequence(tl.seq);
        REQUIRE(outer.videoTracks[0]->clips.size() == 1);
        REQUIRE(outer.videoTracks[0]->clips[0].isNested());
        const auto found = findDeadTime(nest.edit.state, tl.seq, lookup, o);
        REQUIRE(found.has_value());
        REQUIRE(found->ranges.size() == 1);
        CHECK(found->ranges[0].remove == flat->ranges[0].remove);
        const edit::EditResult removed = ok(edit::removeTimeRanges(nest.edit.state, tl.seq, std::span<const TimeRange>(&found->ranges[0].remove, 1)));
        for (const Clip& c : removed.state.findSequence(tl.seq)->videoTracks[0]->clips) {
            CHECK(c.sequence == nest.nested); // still nested
        }
    }
}

// ---- Zoom punch and shake -------------------------------------------------------------------------------------------------

TEST_CASE("Zoom punch keys Scale to ease in - hold and ease out around its hit (C.16)", "[model][gaming][punch]") {
    Timeline tl("    V1 |A" + std::string(49, 'a') + "|\n");
    const ClipId clip = tl.clip("V1@0");
    // An existing key outside the punch stays.
    Keyframe k;
    k.time = f(45);
    k.value = 2.0;
    const ProjectState s0 = ok(edit::addKeyframe(tl.state, tl.seq, clip, edit::ParamRef::of(ParamId::Scale), k)).state;
    ZoomPunchOptions o;
    o.scale = 1.5;
    o.duration = f(10);
    o.attack = 0.2;
    o.release = 0.4;
    const edit::EditResult r = ok(edit::zoomPunch(s0, tl.seq, clip, f(20), o));
    const Clip& c = clipOf(r.state, tl.seq, clip);
    CHECK(c.parameterAt(ParamId::Scale, f(10)) == Approx(2.0));
    CHECK(c.parameterAt(ParamId::Scale, f(18)) == Approx(2.0));
    CHECK(c.parameterAt(ParamId::Scale, f(20)) == Approx(3.0));
    CHECK(c.parameterAt(ParamId::Scale, f(22)) == Approx(3.0));
    CHECK(c.parameterAt(ParamId::Scale, f(24)) == Approx(3.0));
    CHECK(c.parameterAt(ParamId::Scale, f(26)) < 3.0);
    CHECK(c.parameterAt(ParamId::Scale, f(28)) == Approx(2.0));
    CHECK(c.parameterAt(ParamId::Scale, f(45)) == Approx(2.0));
    const Curve* curve = c.properties()[ParamId::Scale].curve();
    REQUIRE(curve);
    CHECK(curve->keys.size() == 5);
    CHECK(curve->keys[1].interpolation == Interpolation::EaseInOut);
    CHECK(c.properties()[ParamId::PositionX].isConstant()); // no focus: Position untouched

    // A focus keeps that point of the frame in place.
    ZoomPunchOptions focused = o;
    focused.focus = std::array<double, 2>{0.75, 0.5};
    const edit::EditResult fr = ok(edit::zoomPunch(tl.state, tl.seq, clip, f(20), focused));
    const Clip& fc = clipOf(fr.state, tl.seq, clip);
    CHECK(fc.parameterAt(ParamId::PositionX, f(20)) == Approx(-0.5 * 480.0)); // 1.5 x 0 + (1 - 1.5) x 480
    CHECK(fc.parameterAt(ParamId::PositionX, f(18)) == Approx(0.0));
    // No release: it holds, then cuts back.
    ZoomPunchOptions cut = o;
    cut.release = 0.0;
    const edit::EditResult cr = ok(edit::zoomPunch(tl.state, tl.seq, clip, f(20), cut));
    const Clip& cc = clipOf(cr.state, tl.seq, clip);
    CHECK(cc.parameterAt(ParamId::Scale, f(27)) == Approx(1.5));
    CHECK(cc.parameterAt(ParamId::Scale, f(28)) == Approx(1.0));
    // Errors.
    CHECK_FALSE(edit::zoomPunch(tl.state, tl.seq, clip, f(60), o).has_value());
    ZoomPunchOptions bad = o;
    bad.scale = 1.0;
    CHECK(zoomPunchProblem(bad).has_value());
    bad = o;
    bad.attack = 0.7;
    bad.release = 0.5;
    CHECK(zoomPunchProblem(bad).has_value());
    bad = o;
    bad.easing = Interpolation::Hold;
    CHECK(zoomPunchProblem(bad).has_value());
}

TEST_CASE("Camera shake is a seeded decaying pattern of Position keys (C.16)", "[model][gaming][shake]") {
    Timeline tl("    V1 |A" + std::string(49, 'a') + "|\n");
    const ClipId clip = tl.clip("V1@0");
    ShakeOptions o;
    o.intensity = 30.0;
    o.duration = Ticks{kTicksPerSecond / 2};
    o.frequency = 14.0;
    o.seed = 7;
    // The pattern: on frames, zero at both ends, decaying, alternating sideways.
    const auto pattern = shakePattern(o, 7, kRate);
    REQUIRE(pattern.size() == 8); // 0, 2, 4, ..., 12, 13 frames
    CHECK(pattern.front().time == Ticks{});
    CHECK(pattern.back().time == f(13));
    CHECK(pattern.back().dx == 0.0);
    for (std::size_t i = 1; i + 1 < pattern.size(); ++i) {
        CHECK(snapToFrame(pattern[i].time, kRate) == pattern[i].time);
        const double u = seconds(pattern[i].time) / seconds(f(13));
        CHECK(std::abs(pattern[i].dx) <= 30.0 * (1 - u) * (1 - u) + 1e-9);
        CHECK(std::abs(pattern[i].dy) <= 30.0 * (1 - u) * (1 - u) + 1e-9);
        if (i + 2 < pattern.size()) {
            CHECK((pattern[i].dx > 0) != (pattern[i + 1].dx > 0));
        }
    }
    CHECK(shakePattern(o, 7, kRate).size() == pattern.size());
    CHECK(shakePattern(o, 7, kRate)[1].dx == pattern[1].dx);
    CHECK(shakePattern(o, 8, kRate)[1].dx != pattern[1].dx);

    const edit::EditResult r = ok(edit::shake(tl.state, tl.seq, clip, f(10), o));
    const Clip& c = clipOf(r.state, tl.seq, clip);
    CHECK(c.parameterAt(ParamId::PositionX, f(10)) == Approx(0.0));
    CHECK(c.parameterAt(ParamId::PositionX, f(12)) == Approx(pattern[1].dx));
    CHECK(c.parameterAt(ParamId::PositionY, f(12)) == Approx(pattern[1].dy));
    CHECK(c.parameterAt(ParamId::PositionX, f(23)) == Approx(0.0));
    CHECK(c.parameterAt(ParamId::Scale, f(12)) > 1.0); // cover
    CHECK(c.parameterAt(ParamId::Scale, f(23)) == Approx(1.0));
    CHECK(c.properties()[ParamId::Rotation].isConstant());
    // The same request makes the same shake; rotation adds Rotation keys.
    CHECK(clipOf(ok(edit::shake(tl.state, tl.seq, clip, f(10), o)).state, tl.seq, clip) == c);
    ShakeOptions turning = o;
    turning.rotation = 2.0;
    turning.cover = false;
    const edit::EditResult rr = ok(edit::shake(tl.state, tl.seq, clip, f(10), turning));
    const Clip& rc = clipOf(rr.state, tl.seq, clip);
    CHECK(rc.properties()[ParamId::Rotation].isCurve());
    CHECK(rc.properties()[ParamId::Scale].isConstant());
    // Seed 0: derived from the clip and the time, still deterministic.
    ShakeOptions derived = o;
    derived.seed = 0;
    CHECK(clipOf(ok(edit::shake(tl.state, tl.seq, clip, f(10), derived)).state, tl.seq, clip) ==
          clipOf(ok(edit::shake(tl.state, tl.seq, clip, f(10), derived)).state, tl.seq, clip));
    // Near the clip's end the shake is cut short and still ends on the base value.
    const edit::EditResult lr = ok(edit::shake(tl.state, tl.seq, clip, f(45), o));
    const Clip& late = clipOf(lr.state, tl.seq, clip);
    CHECK(late.parameterAt(ParamId::PositionX, f(49)) == Approx(0.0));
    ShakeOptions bad = o;
    bad.intensity = 0.0;
    CHECK(shakeProblem(bad).has_value());
}

// ---- Versions -------------------------------------------------------------------------------------------------------------

namespace {

Timeline gamingEdit() {
    Timeline tl(R"(
        V2 |Ffffffffff|
        V1 |Gggggggggg|
        A1 |Gggggggggg|
    )");
    tl.linkMatching();
    edit::TrackStateChange name;
    name.name = "Facecam";
    tl.state = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), name)).state;
    CaptionTrack captions;
    captions.name = "Subtitles";
    captions.cues = {CaptionCue{f(1), f(3), "GG", {}}};
    tl.state = ok(edit::setCaptionTrack(tl.state, tl.seq, captions)).state;
    return tl;
}

} // namespace

TEST_CASE("A Shorts version splits gameplay and facecam and moves the captions into the safe area (C.16)", "[model][gaming][version]") {
    Timeline tl = gamingEdit();
    CHECK(detectFacecamTrack(*tl.state.findSequence(tl.seq)) == tl.track("V2"));
    VersionOptions o;
    const auto v = edit::makeVersion(tl.state, tl.seq, o);
    REQUIRE(v.has_value());
    requireValid(v->state);
    const Sequence& made = *v->state.findSequence(v->sequence);
    CHECK(made.width == 1080);
    CHECK(made.height == 1920);
    CHECK(made.frameRate == tl.sequence().frameRate);
    CHECK(made.name == tl.sequence().name + " (Shorts)");
    CHECK(v->method == VersionMethod::Layout);
    CHECK(v->clipsLaidOut == 1);
    // The source is untouched; the copy has its own ids.
    CHECK(*v->state.findSequence(tl.seq) == tl.sequence());
    CHECK(made.videoTracks[0]->clips[0].id != tl.clip("V1@0"));
    // Vertical split: the gameplay crop and the facecam's.
    const Clip& game = made.videoTracks[0]->clips[0];
    CHECK(game.properties().value(ParamId::PositionY) < 0.0); // the gameplay's half is the top
    bool cropped = false;
    for (const EffectInstance& e : game.effects()) {
        cropped = cropped || (e.effect == "wabcut.crop" && e.findData(kLayoutDataKey));
    }
    CHECK(cropped);
    // Captions: custom placement above the platforms' bottom controls.
    REQUIRE(made.captionTracks.size() == 1);
    const CaptionStyle& style = made.captionTracks[0].style;
    CHECK(style.placement == CaptionPlacement::Custom);
    CHECK(style.positionY.constant() == Approx(0.68));
    CHECK(style.maxWidth <= 0.8);
    CHECK(v->captionTracks == 1);
    CHECK(tl.sequence().captionTracks[0].style.placement == CaptionPlacement::Bottom);
    CHECK_FALSE(v->changes.addedSequences.empty());
}

TEST_CASE("A square version blurs the fill and keeps the facecam on top of the sharp gameplay (C.16)", "[model][gaming][version]") {
    Timeline tl = gamingEdit();
    VersionOptions o;
    o.format = VersionFormat::Square;
    o.layout.corner = LayoutCorner::TopLeft;
    const auto v = edit::makeVersion(tl.state, tl.seq, o);
    REQUIRE(v.has_value());
    requireValid(v->state);
    const Sequence& made = *v->state.findSequence(v->sequence);
    CHECK(made.width == 1080);
    CHECK(made.height == 1080);
    // The facecam ends up above the sharp foreground copy (moved to a track above it when needed).
    std::size_t camIndex = 0;
    std::size_t foregroundIndex = 0;
    const MediaId g = tl.media('G');
    const MediaId f = tl.media('F');
    for (std::size_t i = 0; i < made.videoTracks.size(); ++i) {
        for (const Clip& c : made.videoTracks[i]->clips) {
            if (c.media == f) {
                camIndex = i;
            } else if (c.media == g && i > 0) {
                foregroundIndex = i;
            }
        }
    }
    CHECK(foregroundIndex > 0);
    CHECK(camIndex > foregroundIndex);
    CHECK(made.captionTracks[0].style.placement == CaptionPlacement::Bottom);
    CHECK(made.captionTracks[0].style.safeArea >= 0.08);
}

TEST_CASE("Versions without a facecam use the host's Auto Reframe or the blur fill (C.16)", "[model][gaming][version]") {
    Timeline tl(R"(
        V1 |Gggggggggg|
        A1 |Gggggggggg|
    )");
    CHECK_FALSE(detectFacecamTrack(tl.sequence()).has_value());
    // No reframer: blur fill.
    const auto blur = edit::makeVersion(tl.state, tl.seq, VersionOptions{});
    REQUIRE(blur.has_value());
    CHECK(blur->method == VersionMethod::BlurFill);
    CHECK(blur->clipsLaidOut == 1);
    requireValid(blur->state);
    // A reframer: it makes the sequence.
    int calls = 0;
    VersionOptions o;
    o.reframe = [&](const ProjectState& state, SequenceId source, int width, int height, std::string name)
        -> std::expected<ReframedSequence, Error> {
        ++calls;
        CHECK(source == tl.seq);
        CHECK(width == 1080);
        CHECK(height == 1920);
        auto copied = edit::copySequenceFrom(state, state, source, std::move(name));
        REQUIRE(copied.has_value());
        return ReframedSequence{copied->state, copied->id, {"reframed"}};
    };
    const auto reframed = edit::makeVersion(tl.state, tl.seq, o);
    REQUIRE(reframed.has_value());
    CHECK(calls == 1);
    CHECK(reframed->method == VersionMethod::Reframe);
    CHECK(reframed->notes == std::vector<std::string>{"reframed"});
    // Asking for a layout without a facecam, or a reframe without a reframer, fails; so does a portrait source.
    VersionOptions layout;
    layout.method = VersionMethod::Layout;
    CHECK_FALSE(edit::makeVersion(tl.state, tl.seq, layout).has_value());
    VersionOptions reframe;
    reframe.method = VersionMethod::Reframe;
    CHECK_FALSE(edit::makeVersion(tl.state, tl.seq, reframe).has_value());
    CHECK_FALSE(edit::makeVersion(blur->state, blur->sequence, VersionOptions{}).has_value());
    // A facecam region of the capture makes the split possible without a facecam track.
    VersionOptions region;
    region.facecamRegion = NormalizedRect{0.75, 0.7, 0.25, 0.3};
    const auto split = edit::makeVersion(tl.state, tl.seq, region);
    REQUIRE(split.has_value());
    CHECK(split->method == VersionMethod::Layout);
    requireValid(split->state);
    CHECK(versionFormatFromString("square") == VersionFormat::Square);
    CHECK(versionMethodFromString("blurFill") == VersionMethod::BlurFill);
    CHECK_FALSE(versionMethodFromString("crop").has_value());
}

TEST_CASE("The facecam track is found by name or by a picture-in-picture layout (C.16)", "[model][gaming][version]") {
    Timeline tl(R"(
        V3 |Tttttttttt|
        V2 |Ffffffffff|
        V1 |Gggggggggg|
    )");
    CHECK_FALSE(detectFacecamTrack(tl.sequence()).has_value());
    LayoutRequest pip;
    pip.gameplay = tl.clip("V1@0");
    pip.facecam = tl.clip("V2@0");
    const auto laid = edit::applyLayout(tl.state, tl.seq, pip);
    REQUIRE(laid.has_value());
    CHECK(detectFacecamTrack(*laid->edit.state.findSequence(tl.seq)) == tl.track("V2"));
    edit::TrackStateChange name;
    name.name = "Webcam";
    const ProjectState named = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V3"), name)).state;
    CHECK(detectFacecamTrack(*named.findSequence(tl.seq)) == tl.track("V3"));
    name.name = "\xE0\xB8\x81\xE0\xB8\xA5\xE0\xB9\x89\xE0\xB8\xAD\xE0\xB8\x87"; // Thai "camera"
    const ProjectState thai = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V3"), name)).state;
    CHECK(detectFacecamTrack(*thai.findSequence(tl.seq)) == tl.track("V3"));
}
