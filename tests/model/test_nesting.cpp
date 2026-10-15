// C.8: nested sequences and multicam - nest / unnest round trips, cycles, time mapping and redactions through nested
// clips, multicam sources synced every way, angle switches and trims across them, flatten, project files and the
// leaf resolver (Nesting.h, Resolve.h).

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

ProjectState withMedia(ProjectState s, MediaId id, const std::function<void(MediaItem&)>& change) {
    for (MediaPtr& m : s.media) {
        if (m->id == id) {
            auto copy = std::make_shared<MediaItem>(*m);
            change(*copy);
            m = std::move(copy);
        }
    }
    return s;
}

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

/// "A2 C3": each leaf's media letter and source frame.
std::string leaves(const ProjectState& state, const std::vector<ResolvedLeaf>& found) {
    std::string out;
    for (const ResolvedLeaf& l : found) {
        out += (out.empty() ? "" : " ") + state.findMedia(l.clip->media)->name + std::to_string(l.sourceTime.value / f(1).value);
    }
    return out;
}

/// The nest picture shared by several tests: a dissolve at A|B on V1 and a blur on A.
struct Nested {
    Timeline tl{R"(
        V2     |..C3cc....|
        V1 tgt |AaaaB10b..|
        A1 tgt |AaaaB10b..|
    )"};
    ProjectState before;
    ProjectState state;
    SequenceId nested;

    Nested() {
        tl.linkMatching();
        edit::TransitionSpec dissolve{tl.clip("V1@0"), tl.clip("V1@4"), f(1), f(1)};
        before = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), dissolve)).state;
        const ClipId a = tl.clip("V1@0");
        before = ok(edit::addEffect(before, tl.seq, std::span<const ClipId>(&a, 1), "wabcut.blur.gaussian")).state;
        const std::vector<ClipId> selection{tl.clip("V1@0"), tl.clip("V1@4"), tl.clip("V2@2")};
        auto r = edit::nestClips(before, tl.seq, selection, "Nest");
        REQUIRE(r);
        requireValid(r->edit.state);
        state = r->edit.state;
        nested = r->nested;
    }
};

} // namespace

TEST_CASE("Nest: the selection becomes a sequence, replaced by one linked nested clip per kind", "[model][nesting]") {
    Nested n;
    CHECK_TIMELINE(n.state, n.tl.seq, R"(
        V2 |..........|
        V1 |Nnnnnnnn..|
        A1 |Nnnnnnnn..|
    )");
    CHECK(links(n.state, n.tl.seq) == "V1@0+A1@0");
    const Sequence& nested = *n.state.findSequence(n.nested);
    CHECK(nested.name == "Nest");
    CHECK(nested.bin == n.tl.sequence().bin);
    CHECK(nested.duration == f(8));
    CHECK_TIMELINE(n.state, n.nested, R"(
        V2 |..C3cc..|
        V1 |AaaaB10b|
        A1 |AaaaB10b|
    )");
    // Links, the transition and the effect went along; the clips kept their ids.
    CHECK(links(n.state, n.nested) == "V1@0+A1@0 V1@4+A1@4");
    REQUIRE(nested.videoTracks[0]->transitions.size() == 1);
    CHECK(clipIn(n.state, n.nested, "V1@0").id == n.tl.clip("V1@0"));
    CHECK(clipIn(n.state, n.nested, "V1@0").effects().size() == 1);
    // The nested clips play the nested sequence from its start.
    const Clip& v = clipIn(n.state, n.tl.seq, "V1@0");
    CHECK(v.sequence == n.nested);
    CHECK(v.sourceIn == Ticks{});
    CHECK(v.name() == "Nest");
    CHECK(clipIn(n.state, n.tl.seq, "A1@0").stream == StreamRef{StreamKind::Audio, 0});
}

TEST_CASE("Unnest puts the nested contents back inline exactly", "[model][nesting]") {
    Nested n;
    const auto back = ok(edit::unnestClip(n.state, n.tl.seq, clipAt(n.state, n.tl.seq, "V1@0")));
    CHECK_TIMELINE(back.state, n.tl.seq, R"(
        V2 |..C3cc....|
        V1 |AaaaB10b..|
        A1 |AaaaB10b..|
    )");
    CHECK(links(back.state, n.tl.seq) == "V1@0+A1@0 V1@4+A1@4");
    const Sequence& seq = *back.state.findSequence(n.tl.seq);
    REQUIRE(seq.videoTracks[0]->transitions.size() == 1);
    CHECK(seq.videoTracks[0]->transitions[0].effect == "crossDissolve");
    CHECK(clipIn(back.state, n.tl.seq, "V1@0").effects().size() == 1);
    CHECK(clipIn(back.state, n.tl.seq, "V1@0").id != n.tl.clip("V1@0")); // copies: the nested sequence stays
    CHECK(back.state.findSequence(n.nested));

    SECTION("a transition across the edge of what it shows is not exact") {
        const auto trimmed = ok(edit::trimEdge(n.state, n.tl.seq, clipAt(n.state, n.tl.seq, "V1@0"), edit::Edge::Head, f(4)));
        const auto r = edit::unnestClip(trimmed.state, n.tl.seq, clipAt(trimmed.state, n.tl.seq, "V1@4"));
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("transition") != std::string::npos);
    }
}

TEST_CASE("Unnesting a trimmed nested clip brings back only what it shows", "[model][nesting]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbbbbb|
    )");
    const auto nested = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0"), tl.clip("V1@4")}, "N");
    REQUIRE(nested);
    const auto trimmed = ok(edit::trimEdge(nested->edit.state, tl.seq, nested->clips[0], edit::Edge::Head, f(6)));
    const auto back = ok(edit::unnestClip(trimmed.state, tl.seq, nested->clips[0]));
    CHECK_TIMELINE(back.state, tl.seq, R"(
        V1 |......B2bbbb|
    )");
}

TEST_CASE("Nest and unnest refuse what they cannot do exactly", "[model][nesting]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCccc|
    )");
    // B lies between A and C on V1, where the nested clip goes.
    const std::vector<ClipId> gap{tl.clip("V1@0"), tl.clip("V1@8")};
    const auto refused = edit::nestClips(tl.state, tl.seq, gap, "N");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("not selected") != std::string::npos);
    CHECK_FALSE(edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{}, "N"));

    Nested n;
    const ClipId nestedClip = clipAt(n.state, n.tl.seq, "V1@0");
    SECTION("retimed") {
        edit::SpeedChange twice;
        twice.speed = Speed{2, 1};
        const auto fast = ok(edit::setSpeed(n.state, n.tl.seq, std::vector<ClipId>{nestedClip}, twice));
        const auto r = edit::unnestClip(fast.state, n.tl.seq, nestedClip);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("retimed") != std::string::npos);
    }
    SECTION("effects on the nested clip") {
        const auto blurred = ok(edit::addEffect(n.state, n.tl.seq, std::span<const ClipId>(&nestedClip, 1), "wabcut.blur.gaussian"));
        CHECK_FALSE(edit::unnestClip(blurred.state, n.tl.seq, nestedClip));
    }
    SECTION("a missing track") {
        const auto oneTrack = ok(edit::removeTrack(n.state, n.tl.seq, n.tl.track("V2")));
        const auto r = edit::unnestClip(oneTrack.state, n.tl.seq, nestedClip);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("V2") != std::string::npos);
    }
    SECTION("the nested sequence stays while it is used") {
        CHECK_FALSE(edit::removeSequence(n.state, n.nested));
        const auto lastVideo = edit::removeTrack(n.state, n.nested, n.state.findSequence(n.nested)->videoTracks[0]->id);
        CHECK(lastVideo); // V2 still gives the picture
    }
}

TEST_CASE("A sequence never contains itself", "[model][nesting]") {
    Nested n;
    const auto insertInto = [&](SequenceId into, SequenceId source) {
        const Sequence& dest = *n.state.findSequence(into);
        edit::SourceClip clip;
        clip.sequence = source;
        clip.sourceRange = {Ticks{}, f(2)};
        clip.patches = edit::defaultSourcePatches(dest, *n.state.findSequence(source));
        return edit::overwriteEdit(n.state, into, clip, Ticks{});
    };
    CHECK_FALSE(insertInto(n.nested, n.nested));
    const auto parentIntoNested = insertInto(n.nested, n.tl.seq);
    REQUIRE_FALSE(parentIntoNested);
    CHECK(parentIntoNested.error().message.find("never contains itself") != std::string::npos);
    CHECK(sequenceContains(n.state, n.tl.seq, n.nested));
    CHECK_FALSE(sequenceContains(n.state, n.nested, n.tl.seq));

    // A hand-made cycle (a corrupt file) fails validation.
    ProjectState broken = n.state;
    auto nested = std::make_shared<Sequence>(*broken.findSequence(n.nested));
    auto track = std::make_shared<Track>(*nested->videoTracks[1]);
    Clip loop = clipIn(n.state, n.tl.seq, "V1@0");
    loop.id = broken.ids.allocate<ClipId>();
    loop.sequence = n.tl.seq;
    loop.linkGroup = {};
    loop.timelineRange = {f(6), f(2)};
    track->clips.push_back(loop);
    nested->videoTracks[1] = track;
    broken = broken.withSequence(nested);
    const auto invalid = validate(broken, Validation::Structure);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message.find("contain each other") != std::string::npos);
    CHECK(nestingCycle(broken).has_value());
}

TEST_CASE("A nested clip's speed, reverse and remap map into nested time exactly", "[model][nesting][resolve]") {
    Nested n;
    const ClipId nestedClip = clipAt(n.state, n.tl.seq, "V1@0");
    const auto at = [&](const ProjectState& s, std::int64_t frame) {
        const Clip& c = s.findSequence(n.tl.seq)->findClip(nestedClip)->clip();
        return leaves(s, resolveLeaves(s, n.tl.seq, c, f(frame)));
    };
    CHECK(at(n.state, 0) == "A0");
    CHECK(at(n.state, 2) == "A2 C3");
    CHECK(at(n.state, 6) == "B12");

    edit::SpeedChange twice;
    twice.speed = Speed{2, 1};
    const auto fast = ok(edit::setSpeed(n.state, n.tl.seq, std::vector<ClipId>{nestedClip}, twice));
    CHECK(at(fast.state, 1) == "A2 C3");
    CHECK(at(fast.state, 3) == "B12");

    edit::SpeedChange back;
    back.speed = Speed{1, 1};
    back.reversed = true;
    const auto reversed = ok(edit::setSpeed(n.state, n.tl.seq, std::vector<ClipId>{nestedClip}, back));
    CHECK(at(reversed.state, 0) == "B13");
    CHECK(at(reversed.state, 7) == "A0");

    ProjectState remapped = ok(edit::setTimeRemapEnabled(n.state, n.tl.seq, nestedClip, true)).state;
    remapped = ok(edit::addTimeRemapHold(remapped, n.tl.seq, nestedClip, f(2), f(3))).state;
    CHECK(at(remapped, 4) == "A2 C3"); // the held nested frame
    CHECK(at(remapped, 7) == "A4 B10 C5"); // nested 4: inside the dissolve

    // The path names the nested clip and the nested time it shows.
    const Clip& c = n.state.findSequence(n.tl.seq)->findClip(nestedClip)->clip();
    const auto found = resolveLeaves(n.state, n.tl.seq, c, f(2));
    REQUIRE(found.size() == 2);
    REQUIRE(found[0].path.size() == 1);
    CHECK(found[0].path[0].clip->id == nestedClip);
    CHECK(found[0].path[0].nestedTime == f(2));
    CHECK(found[0].sequence == n.nested);
    CHECK(found[0].trackIndex == 0);
    CHECK(found[1].trackIndex == 1);
}

TEST_CASE("Leaves inside nested sequences: transitions, nested nests, disabled tracks and the past-the-end rule",
          "[model][nesting][resolve]") {
    Nested n;
    const ClipId nestedClip = clipAt(n.state, n.tl.seq, "V1@0");
    const Clip& c = n.state.findSequence(n.tl.seq)->findClip(nestedClip)->clip();
    // Inside the dissolve (nested 3..5) both A and B are active.
    const auto mid = resolveLeaves(n.state, n.tl.seq, c, f(4));
    CHECK(leaves(n.state, mid) == "A4 B10 C5");
    REQUIRE(mid[0].transition.transition);
    CHECK_FALSE(mid[0].transition.incoming);
    CHECK(mid[1].transition.incoming);
    CHECK(mid[0].transition.progress == 0.5);

    // Nest the nested clip again: two levels, outermost first.
    const auto twice = edit::nestClips(n.state, n.tl.seq, std::vector<ClipId>{nestedClip}, "Outer");
    REQUIRE(twice);
    requireValid(twice->edit.state);
    const Clip& outer = clipIn(twice->edit.state, n.tl.seq, "V1@0");
    const auto deep = resolveLeaves(twice->edit.state, n.tl.seq, outer, f(6));
    CHECK(leaves(twice->edit.state, deep) == "B12");
    REQUIRE(deep[0].path.size() == 2);
    CHECK(deep[0].path[0].sequence == n.tl.seq);
    CHECK(deep[0].path[1].sequence == twice->nested);
    CHECK(resolveSequenceLeaves(twice->edit.state, n.tl.seq, StreamKind::Audio, f(1)).size() == 1);

    // A disabled nested track shows nothing; past the nested sequence's end there is nothing.
    edit::TrackStateChange off;
    off.enabled = false;
    const auto hidden = ok(edit::setTrackState(n.state, n.nested, n.state.findSequence(n.nested)->videoTracks[1]->id, off));
    CHECK(leaves(hidden.state, resolveLeaves(hidden.state, n.tl.seq, c, f(2))) == "A2");
    const auto longer = ok(edit::trimEdge(n.state, n.tl.seq, nestedClip, edit::Edge::Tail, f(2)));
    CHECK(longer.state.findSequence(n.tl.seq)->findClip(nestedClip)->clip().duration() == f(8)); // the nested end bounds trims
}

TEST_CASE("Source-anchored redactions apply to media inside nested clips", "[model][nesting][resolve]") {
    Nested n;
    Redaction r;
    r.media = n.tl.media('A');
    r.name = "Plate";
    r.source = {f(1), f(2)};
    r.audio = RedactionAudio::Bleep;
    const auto added = edit::addRedaction(n.state, r);
    REQUIRE(added);
    const ClipId video = clipAt(added->state, n.tl.seq, "V1@0");
    const ClipId audio = clipAt(added->state, n.tl.seq, "A1@0");
    const auto redactions = [&](const ProjectState& s, ClipId id, std::int64_t frame) {
        const Clip& c = s.findSequence(n.tl.seq)->findClip(id)->clip();
        const auto found = resolveLeaves(s, n.tl.seq, c, f(frame));
        REQUIRE_FALSE(found.empty());
        return found[0].redactions.size();
    };
    CHECK(redactions(added->state, video, 2) == 1);
    CHECK(redactions(added->state, audio, 2) == 1);
    CHECK(redactions(added->state, video, 0) == 0);
    // Excepting the nested clip excepts what it holds.
    const auto excepted = edit::setRedactionOverride(added->state, added->id, video, false);
    REQUIRE(excepted);
    CHECK(redactions(*excepted, video, 2) == 0);
    CHECK(redactions(*excepted, audio, 2) == 1);
}

TEST_CASE("Multicam sources sync their angles by timecode, marks, markers, audio or by hand", "[model][multicam]") {
    Timeline tl(R"(
        V1 tgt |....|
        A1 tgt |....|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const MediaId c = tl.media('C');
    ProjectState s = tl.state;
    const auto offsets = [&](const ProjectState& state, edit::MulticamSpec spec) {
        spec.angles = {a, b, c};
        const auto made = edit::createMulticam(state, spec);
        REQUIRE(made);
        requireValid(made->state);
        const Sequence& mc = *made->state.findSequence(made->id);
        REQUIRE(mc.multicam);
        std::vector<std::int64_t> out;
        for (const MulticamAngle& angle : mc.multicam->angles) {
            out.push_back(angle.offset.value / f(1).value);
            // Each angle's clips sit at its offset: video on its track, both audio streams on two tracks.
            CHECK(mc.findTrack(angle.video)->clips.front().start() == angle.offset);
            REQUIRE(angle.audio.size() == 2);
            CHECK(mc.findTrack(angle.audio[1])->clips.front().stream == StreamRef{StreamKind::Audio, 1});
        }
        CHECK(mc.videoTracks.size() == 3);
        CHECK(mc.audioTracks.size() == 6);
        CHECK(mc.multicam->angles[1].name == "B");
        return out;
    };
    edit::MulticamSpec spec;
    spec.sync = MulticamSync::Timecode;
    CHECK_FALSE(edit::createMulticam(s, edit::MulticamSpec{"", {}, {a, b}, MulticamSync::Timecode})); // no timecode
    ProjectState tc = withMedia(s, a, [](MediaItem& m) { m.info.startTimecode = Ticks::fromSeconds(36000); });
    tc = withMedia(tc, b, [](MediaItem& m) { m.info.startTimecode = Ticks::fromSeconds(36000) + f(5); });
    tc = withMedia(tc, c, [](MediaItem& m) { m.info.startTimecode = Ticks::fromSeconds(36000) + f(2); });
    CHECK(offsets(tc, spec) == std::vector<std::int64_t>{0, 5, 2});

    spec.sync = MulticamSync::InPoints;
    ProjectState marks = withMedia(s, a, [](MediaItem& m) { m.markIn = f(10); });
    marks = withMedia(marks, b, [](MediaItem& m) { m.markIn = f(4); m.markOut = f(90); });
    marks = withMedia(marks, c, [](MediaItem& m) { m.markIn = f(7); });
    CHECK(offsets(marks, spec) == std::vector<std::int64_t>{0, 6, 3});
    spec.sync = MulticamSync::OutPoints;
    marks = withMedia(marks, a, [](MediaItem& m) { m.markOut = f(100); });
    CHECK(offsets(marks, spec) == std::vector<std::int64_t>{900, 910, 0}); // C ends at its 1000 frames

    spec.sync = MulticamSync::Markers;
    ProjectState markers = s;
    for (const auto& [id, frame] : {std::pair{a, 20}, std::pair{b, 25}, std::pair{c, 20}}) {
        markers = withMedia(markers, id, [&](MediaItem& m) { m.markers = {Marker{f(frame), Ticks{}, "clap"}}; });
    }
    CHECK(offsets(markers, spec) == std::vector<std::int64_t>{5, 0, 5});

    spec.sync = MulticamSync::Audio;
    CHECK_FALSE(edit::createMulticam(s, edit::MulticamSpec{"", {}, {a, b}, MulticamSync::Audio})); // no analysis
    const OffsetTableAudioSync clap({{a, Ticks{}}, {b, f(-3)}, {c, f(4)}});
    spec.audioSync = &clap;
    CHECK(offsets(s, spec) == std::vector<std::int64_t>{3, 0, 7});

    spec.sync = MulticamSync::Manual;
    spec.offsets = {Ticks{}, f(10), f(20)};
    CHECK(offsets(s, spec) == std::vector<std::int64_t>{0, 10, 20});
    spec.offsets = {Ticks{}, f(10), kMaxMulticamOffset + f(1)};
    spec.angles = {a, b, c};
    CHECK_FALSE(edit::createMulticam(s, spec));
}

namespace {

/// A three-angle multicam source "M" (A, B, C in sync) cut into V1/A1 for 20 frames: video angle 1 (A), audio angle 1.
struct Multicam {
    Timeline tl{R"(
        V1 tgt |....................|
        A1 tgt |....................|
        A2     |....................|
    )"};
    SequenceId source;
    ProjectState state;

    Multicam() {
        edit::MulticamSpec spec;
        spec.name = "M";
        spec.angles = {tl.media('A'), tl.media('B'), tl.media('C')};
        spec.sync = MulticamSync::Manual;
        const auto made = edit::createMulticam(tl.state, spec);
        REQUIRE(made);
        source = made->id;
        edit::SourceClip clip;
        clip.sequence = source;
        clip.sourceRange = {Ticks{}, f(20)};
        clip.patches = edit::defaultSourcePatches(tl.sequence(), *made->state.findSequence(source));
        state = ok(edit::overwriteEdit(made->state, tl.seq, clip, Ticks{})).state;
    }
    std::string angles(const ProjectState& s, std::string_view track) const {
        std::string out;
        for (const Clip& c : s.findSequence(tl.seq)->findTrack(tl.track(track))->clips) {
            out += (out.empty() ? "" : " ") + std::to_string(c.angles.first() + 1) + "@" +
                   std::to_string(c.start().value / f(1).value);
        }
        return out;
    }
};

} // namespace

TEST_CASE("Angle switches cut at the switch time; trims and rolls work across angle cuts", "[model][multicam]") {
    Multicam m;
    CHECK_TIMELINE(m.state, m.tl.seq, R"(
        V1 |Mmmmmmmmmmmmmmmmmmmm|
        A1 |Mmmmmmmmmmmmmmmmmmmm|
        A2 |....................|
    )");
    CHECK(links(m.state, m.tl.seq) == "V1@0+A1@0");
    CHECK(m.angles(m.state, "V1") == "1@0");

    ProjectState s = ok(edit::switchMulticamAngle(m.state, m.tl.seq, clipAt(m.state, m.tl.seq, "V1@0"), f(5), 1,
                                                  edit::AngleTarget::Video)).state;
    s = ok(edit::switchMulticamAngle(s, m.tl.seq, clipAt(s, m.tl.seq, "V1@5"), f(12), 2, edit::AngleTarget::Both)).state;
    CHECK(m.angles(s, "V1") == "1@0 2@5 3@12");
    CHECK(m.angles(s, "A1") == "1@0 3@12");
    CHECK_TIMELINE(s, m.tl.seq, R"(
        V1 |MmmmmM5mmmmmM12mmmmm|
        A1 |MmmmmmmmmmmmM12mmmmm|
        A2 |....................|
    )");
    const auto shows = [&](const ProjectState& state, std::int64_t frame) {
        return leaves(state, resolveSequenceLeaves(state, m.tl.seq, StreamKind::Video, f(frame)));
    };
    CHECK(shows(s, 4) == "A4");
    CHECK(shows(s, 6) == "B6");
    CHECK(shows(s, 13) == "C13");
    CHECK(leaves(s, resolveSequenceLeaves(s, m.tl.seq, StreamKind::Audio, f(13))) == "C13 C13"); // both of C's streams

    // A roll moves the angle cut; the source stays in sync.
    s = ok(edit::rollEdit(s, m.tl.seq, clipAt(s, m.tl.seq, "V1@0"), edit::Edge::Tail, f(2), edit::LinkMode::IgnoreLinks)).state;
    CHECK(m.angles(s, "V1") == "1@0 2@7 3@12");
    CHECK(shows(s, 6) == "A6");
    CHECK(shows(s, 7) == "B7");
    // A ripple trim of an angle piece's head: the later cut moves earlier, the source stays in sync.
    const auto rippled = ok(edit::rippleTrim(s, m.tl.seq, clipAt(s, m.tl.seq, "V1@7"), edit::Edge::Head, f(2),
                                             edit::LinkMode::IgnoreLinks));
    CHECK(m.angles(rippled.state, "V1") == "1@0 2@7 3@10");
    CHECK(shows(rippled.state, 7) == "B9");
    CHECK(shows(rippled.state, 10) == "C12");
    // A whole clip switches without a cut; out of range angles and times are refused.
    const auto whole = ok(edit::switchMulticamAngle(s, m.tl.seq, clipAt(s, m.tl.seq, "V1@7"), std::nullopt, 0,
                                                    edit::AngleTarget::Video));
    CHECK(m.angles(whole.state, "V1") == "1@0 1@7 3@12");
    CHECK_FALSE(edit::switchMulticamAngle(s, m.tl.seq, clipAt(s, m.tl.seq, "V1@0"), f(3), 7));
    CHECK_FALSE(edit::switchMulticamAngle(s, m.tl.seq, clipAt(s, m.tl.seq, "V1@0"), f(30), 1));
}

TEST_CASE("Flatten replaces a multicam clip with the angle clips it plays", "[model][multicam]") {
    Multicam m;
    const ProjectState b = ok(edit::switchMulticamAngle(m.state, m.tl.seq, clipAt(m.state, m.tl.seq, "V1@0"), std::nullopt,
                                                        1, edit::AngleTarget::Both)).state;
    const auto flat = ok(edit::flattenMulticam(b, m.tl.seq, clipAt(b, m.tl.seq, "V1@0")));
    CHECK_TIMELINE(flat.state, m.tl.seq, R"(
        V1 |Bbbbbbbbbbbbbbbbbbbb|
        A1 |Bbbbbbbbbbbbbbbbbbbb|
        A2 |Bbbbbbbbbbbbbbbbbbbb|
    )");
    CHECK(links(flat.state, m.tl.seq) == "V1@0+A1@0+A2@0");
    CHECK(clipIn(flat.state, m.tl.seq, "A2@0").stream == StreamRef{StreamKind::Audio, 1});

    // Not exact: an effect on the multicam clip, or a clip in the way below the audio.
    const ClipId v = clipAt(b, m.tl.seq, "V1@0");
    const auto blurred = ok(edit::addEffect(b, m.tl.seq, std::span<const ClipId>(&v, 1), "wabcut.blur.gaussian"));
    CHECK_FALSE(edit::flattenMulticam(blurred.state, m.tl.seq, v));
    edit::SourceClip media;
    media.media = m.tl.media('D');
    media.sourceRange = {Ticks{}, f(4)};
    media.patches = {{{StreamKind::Audio, 0}, m.tl.track("A2")}};
    const auto blocked = ok(edit::overwriteEdit(b, m.tl.seq, media, f(2)));
    const auto refused = edit::flattenMulticam(blocked.state, m.tl.seq, v);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("in the way") != std::string::npos);
}

TEST_CASE("Changing an angle's sync moves its clips in the source", "[model][multicam]") {
    Multicam m;
    const ProjectState b = ok(edit::switchMulticamAngle(m.state, m.tl.seq, clipAt(m.state, m.tl.seq, "V1@0"), std::nullopt,
                                                        1, edit::AngleTarget::Video)).state;
    const auto synced = ok(edit::setMulticamSync(b, m.source, 1, f(5)));
    requireValid(synced.state);
    const Sequence& mc = *synced.state.findSequence(m.source);
    CHECK(mc.multicam->sync == MulticamSync::Manual);
    CHECK(mc.multicam->angles[1].offset == f(5));
    CHECK(mc.duration == f(1005));
    CHECK(leaves(synced.state, resolveSequenceLeaves(synced.state, m.tl.seq, StreamKind::Video, f(8))) == "B3");
    CHECK(leaves(synced.state, resolveSequenceLeaves(synced.state, m.tl.seq, StreamKind::Video, f(2))).empty());
    CHECK_FALSE(edit::setMulticamSync(b, m.source, 1, Ticks{} - f(1)));
    CHECK_FALSE(edit::setMulticamSync(b, m.tl.seq, 0, f(1))); // not a multicam source
    // Angle tracks stay while the angle does.
    CHECK_FALSE(edit::removeTrack(synced.state, m.source, mc.multicam->angles[0].video));
}

TEST_CASE("Nested and multicam clips round-trip through project files; unknown sources are kept", "[model][nesting][io]") {
    Multicam m;
    const auto nested = edit::nestClips(m.state, m.tl.seq, std::vector<ClipId>{clipAt(m.state, m.tl.seq, "V1@0")}, "N");
    REQUIRE(nested);
    ProjectState s = withMedia(nested->edit.state, m.tl.media('A'),
                               [](MediaItem& item) { item.info.startTimecode = Ticks::fromSeconds(3600); });
    const auto json = io::toJson(s);
    REQUIRE(json);
    CHECK(json->find("\"multicam\"") != std::string::npos);
    CHECK(json->find("\"type\": \"sequence\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == s);
    requireValid(*loaded);

    // A source kind from a newer build: kept, saved back unchanged, plays nothing.
    Timeline plain(R"(
        V1 tgt |Aaaa|
    )");
    std::string text = *io::toJson(plain.state);
    const std::string media = "\"media\": " + std::to_string(plain.media('A').value);
    const auto at = text.find(media);
    REQUIRE(at != std::string::npos);
    text.replace(at, media.size(), R"("source": {"layer": 7, "type": "hologram"})");
    const auto future = io::fromJson(text);
    REQUIRE(future);
    const Clip& kept = clipIn(*future, plain.seq, "V1@0");
    CHECK(kept.hasUnknownSource());
    CHECK_FALSE(kept.media.isValid());
    CHECK(resolveLeaves(*future, plain.seq, kept, f(1)).empty());
    const auto again = io::toJson(*future);
    REQUIRE(again);
    CHECK(again->find("\"hologram\"") != std::string::npos);
    CHECK(again->find("\"layer\": 7") != std::string::npos);
    CHECK(*io::fromJson(*again) == *future);
    // Edits move and trim it like any clip.
    CHECK(edit::trimEdge(*future, plain.seq, kept.id, edit::Edge::Tail, -f(1)));

    // An older (schema 9) project loads with its media clips unchanged.
    std::string old = *io::toJson(plain.state);
    const std::string version = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);
    old.replace(old.find(version), version.size(), "\"schemaVersion\": 9");
    const auto migrated = io::fromJson(old);
    REQUIRE(migrated);
    CHECK(*migrated == plain.state);
    // Malformed sources fail the load.
    std::string bad = *io::toJson(plain.state);
    bad.replace(bad.find(media), media.size(), R"("source": {"type": "sequence", "sequence": 0})");
    CHECK_FALSE(io::fromJson(bad));
}

TEST_CASE("A one-sided angle switch keeps the clip's tail transition on the right piece (E5-P2-1)", "[model][multicam]") {
    Multicam m;
    const ClipId whole = clipAt(m.state, m.tl.seq, "V1@0");
    edit::TransitionSpec fade; // a fade out at the multicam clip's tail
    fade.outgoing = whole;
    fade.before = f(2);
    const ProjectState faded = ok(edit::addTransition(m.state, m.tl.seq, m.tl.track("V1"), fade)).state;
    REQUIRE(faded.findSequence(m.tl.seq)->findTrack(m.tl.track("V1"))->transitions.size() == 1);

    for (const auto target : {edit::AngleTarget::Video, edit::AngleTarget::Both}) {
        const auto switched = edit::switchMulticamAngle(faded, m.tl.seq, whole, f(5), 1, target);
        REQUIRE(switched);
        requireValid(switched->state);
        const Track& v1 = *switched->state.findSequence(m.tl.seq)->findTrack(m.tl.track("V1"));
        REQUIRE(v1.transitions.size() == 1); // not pruned as "no longer adjacent"
        CHECK(v1.transitions[0].outgoing == clipAt(switched->state, m.tl.seq, "V1@5"));
        CHECK(m.angles(switched->state, "V1") == "1@0 2@5");
    }
}
