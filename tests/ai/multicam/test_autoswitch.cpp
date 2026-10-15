// AI-13 automatic switching: the rules on deterministic timelines (minimum hold, never mid-word, wide on crosstalk,
// variety, the J-cut lead, reaction shots, the opening, the presets), then multicam.autoSwitch through the engine on
// the synthetic podcast: a dry run changes nothing, the cuts are one undo step of ordinary angle cuts that stay
// editable, and every cut says why.

#include "CommandHarness.h"
#include "MulticamTestUtil.h"

#include <wabcut/ai/multicam/AutoSwitch.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::multicam;
using namespace wabcut::ai::multicam::test;
using command::Json;

namespace {

/// Two speakers (0 and 1) with close-ups on angles 0 and 1, a wide angle 2; segments from (start, end, kind, who).
struct Seg {
    double start, end;
    SegmentKind kind;
    std::vector<std::size_t> who;
};
SpeakerTimeline timelineOf(double duration, std::vector<Seg> segs, std::vector<Reaction> reactions = {}) {
    SpeakerTimeline t;
    t.duration = secs(duration);
    t.frame = secs(0.1);
    t.speakers = {ai::multicam::Speaker{"a", "Host", {}, 3, 0}, ai::multicam::Speaker{"b", "Guest", {}, 4, 0}};
    t.angles = {AngleAssignment{0, AngleRole::CloseUp, {0}, 0.9, false, "", {}}, AngleAssignment{1, AngleRole::CloseUp, {1}, 0.9, false, "", {}},
                AngleAssignment{2, AngleRole::Wide, {0, 1}, 0.9, false, "", {}}};
    double at = 0;
    for (const Seg& s : segs) {
        if (s.start > at) {
            t.segments.push_back(SpeakerSegment{TimeRange::fromStartEnd(secs(at), secs(s.start)), SegmentKind::Silence, {}, 0.9});
        }
        t.segments.push_back(SpeakerSegment{TimeRange::fromStartEnd(secs(s.start), secs(s.end)), s.kind, s.who, 0.9});
        at = s.end;
    }
    if (at < duration) {
        t.segments.push_back(SpeakerSegment{TimeRange::fromStartEnd(secs(at), secs(duration)), SegmentKind::Silence, {}, 0.9});
    }
    t.reactions = std::move(reactions);
    return t;
}

const std::vector<std::string> kNames{"Cam A", "Cam B", "Cam C"};

std::string dump(const SwitchPlan& plan) {
    std::string out;
    for (const PlannedCut& c : plan.cuts) {
        out += std::to_string(static_cast<double>(c.at.value) / static_cast<double>(kTicksPerSecond)) + " " + c.explanation + "\n";
    }
    return out;
}

SwitchOptions plain() {
    SwitchOptions o = *presetOptions("interview");
    o.openWide = false;
    o.varietyAfter = Ticks{};
    o.lead = Ticks{};
    return o;
}

} // namespace

TEST_CASE("Switching: the active speaker's close-up, held at least the minimum, cut a little early", "[ai][multicam][autoswitch]") {
    // Speakers alternate every 0.8 s for 4 s, then the guest from 10 s.
    const SpeakerTimeline t = timelineOf(20, {{0, 0.8, SegmentKind::Single, {0}}, {0.8, 1.6, SegmentKind::Single, {1}},
                                              {1.6, 2.4, SegmentKind::Single, {0}}, {2.4, 3.2, SegmentKind::Single, {1}},
                                              {3.2, 9.8, SegmentKind::Single, {0}}, {10.0, 20, SegmentKind::Single, {1}}});
    SwitchOptions o = plain();
    o.minHold = secs(2.0);
    const SwitchPlan plan = planSwitches(t, {}, kNames, TimeRange{Ticks{}, secs(20)}, o);
    REQUIRE(plan.cuts.size() >= 2);
    CHECK(plan.cuts.front().at == Ticks{});
    CHECK(plan.cuts.front().angle == 0u);
    CHECK(plan.cuts.front().reason == CutReason::Opening);
    for (std::size_t i = 1; i < plan.cuts.size(); ++i) {
        INFO(plan.cuts[i].explanation);
        CHECK(plan.cuts[i].at - plan.cuts[i - 1].at >= o.minHold);
        CHECK(plan.cuts[i].angle != plan.cuts[i - 1].angle);
    }
    CHECK(plan.stats.shortestShot >= 2.0 - 1e-9);
    // The guest's close-up from 10 s.
    CHECK(plan.cuts.back().angle == 1u);
    CHECK(plan.cuts.back().at == secs(10.0));
    CHECK(plan.cuts.back().explanation == "Cut to Cam B: Guest starts talking");

    // The lead: the same cut a quarter second early.
    o.lead = secs(0.25);
    const SwitchPlan early = planSwitches(t, {}, kNames, TimeRange{Ticks{}, secs(20)}, o);
    CHECK(early.cuts.back().at == secs(9.75));
    CHECK(early.cuts.back().explanation.find("0.25 s early") != std::string::npos);
}

TEST_CASE("Switching: never in the middle of a word", "[ai][multicam][autoswitch]") {
    const SpeakerTimeline t = timelineOf(20, {{0, 9.9, SegmentKind::Single, {0}}, {10.0, 20, SegmentKind::Single, {1}}});
    // A word from 9.8 to 10.15 covers the cut at 10.0 (lead 0); the next word starts at 10.25.
    const std::vector<TimeRange> words{TimeRange::fromStartEnd(secs(9.3), secs(9.7)), TimeRange::fromStartEnd(secs(9.8), secs(10.15)),
                                       TimeRange::fromStartEnd(secs(10.25), secs(10.6))};
    SwitchOptions o = plain();
    const SwitchPlan plan = planSwitches(t, words, kNames, TimeRange{Ticks{}, secs(20)}, o);
    REQUIRE(plan.cuts.size() == 2);
    const PlannedCut& cut = plan.cuts.back();
    CHECK((cut.at == secs(9.8) || cut.at == secs(10.15)));
    for (const TimeRange& w : words) {
        CHECK_FALSE((cut.at > w.start && cut.at < w.end()));
    }
    CHECK(cut.moved.value != 0);
    CHECK(cut.explanation.find("pause between words") != std::string::npos);
    CHECK(plan.stats.snapped == 1);

    // Too far from any gap: kept, counted and warned about.
    o.maxSnap = secs(0.05);
    const std::vector<TimeRange> long_{TimeRange::fromStartEnd(secs(9.0), secs(11.0))};
    const SwitchPlan stuck = planSwitches(t, long_, kNames, TimeRange{Ticks{}, secs(20)}, o);
    CHECK(stuck.stats.midWord == 1);
    CHECK_FALSE(stuck.warnings.empty());

    // Turned off: the cut stays where the speech starts.
    o.avoidMidWord = false;
    CHECK(planSwitches(t, words, kNames, TimeRange{Ticks{}, secs(20)}, o).cuts.back().at == secs(10.0));
}

TEST_CASE("Switching: wide for crosstalk and laughter, variety on a long monologue, the opening wide shot", "[ai][multicam][autoswitch]") {
    const SpeakerTimeline t = timelineOf(60, {{0, 10, SegmentKind::Single, {0}}, {10, 12, SegmentKind::Overlap, {0, 1}},
                                              {12, 20, SegmentKind::Single, {1}}, {20, 20.3, SegmentKind::Overlap, {0, 1}},
                                              {20.3, 25, SegmentKind::Single, {1}}, {25, 26, SegmentKind::NonSpeech, {}},
                                              {26, 60, SegmentKind::Single, {0}}});
    SwitchOptions o = plain();
    o.minHold = secs(1.5);
    o.varietyAfter = secs(12);
    o.varietyLength = secs(3);
    const SwitchPlan plan = planSwitches(t, {}, kNames, TimeRange{Ticks{}, secs(60)}, o);
    INFO(dump(plan));
    const auto cutAt = [&](double s) -> const PlannedCut* {
        for (const PlannedCut& c : plan.cuts) {
            if (c.at == secs(s)) {
                return &c;
            }
        }
        return nullptr;
    };
    REQUIRE(cutAt(10) != nullptr);
    CHECK(cutAt(10)->angle == 2u);
    CHECK(cutAt(10)->reason == CutReason::Overlap);
    CHECK(cutAt(10)->explanation == "Wide on Cam C: Host and Guest talk over each other");
    CHECK(cutAt(20) == nullptr); // 0.3 s of crosstalk is not enough to go wide
    REQUIRE(cutAt(25) != nullptr);
    CHECK(cutAt(25)->reason == CutReason::Laughter);
    // The host is back at 26 s, but the 1 s laughter shot is held the minimum 1.5 s first.
    REQUIRE(cutAt(26.5) != nullptr);
    CHECK(cutAt(26.5)->angle == 0u);
    // 33.5 s on the host: a variety wide shot after 12 s, then back.
    REQUIRE(cutAt(38.5) != nullptr);
    CHECK(cutAt(38.5)->reason == CutReason::Variety);
    CHECK(cutAt(38.5)->explanation == "Wide on Cam C: variety after 12 s on Host");
    REQUIRE(cutAt(41.5) != nullptr);
    CHECK(cutAt(41.5)->reason == CutReason::Back);
    CHECK(cutAt(41.5)->angle == 0u);
    CHECK(plan.stats.varietyCuts >= 1);

    o.openWide = true;
    o.openLength = secs(3);
    const SwitchPlan opened = planSwitches(t, {}, kNames, TimeRange{Ticks{}, secs(60)}, o);
    CHECK(opened.cuts.front().angle == 2u);
    CHECK(opened.cuts.front().reason == CutReason::Opening);
    CHECK(opened.cuts[1].at == secs(3));
    CHECK(opened.cuts[1].angle == 0u);
}

TEST_CASE("Switching: reaction shots and the presets", "[ai][multicam][autoswitch]") {
    const SpeakerTimeline t = timelineOf(40, {{0, 40, SegmentKind::Single, {0}}}, {Reaction{TimeRange{secs(15), secs(0.8)}, 1, 0.9, "laughs"}});
    SwitchOptions o = plain();
    o.reactions = true;
    o.minHold = secs(1.0);
    const SwitchPlan plan = planSwitches(t, {}, kNames, TimeRange{Ticks{}, secs(40)}, o);
    INFO(dump(plan));
    bool reaction = false, back = false;
    for (const PlannedCut& c : plan.cuts) {
        if (c.reason == CutReason::Reaction) {
            reaction = true;
            CHECK(c.angle == 1u);
            CHECK(c.at == secs(15));
            CHECK(c.explanation == "Cut to Cam B: Guest laughs");
        }
        back = back || (c.reason == CutReason::Back && c.angle == 0u);
    }
    CHECK(reaction);
    CHECK(back);
    o.reactions = false;
    for (const PlannedCut& c : planSwitches(t, {}, kNames, TimeRange{Ticks{}, secs(40)}, o).cuts) {
        CHECK(c.reason != CutReason::Reaction);
    }

    // The presets: calm cuts least, energetic most, on the same busy conversation.
    std::vector<Seg> busy;
    for (int i = 0; i < 30; ++i) {
        busy.push_back(Seg{i * 4.0, i * 4.0 + 3.8, SegmentKind::Single, {static_cast<std::size_t>(i % 2)}});
    }
    const SpeakerTimeline b = timelineOf(120, busy);
    const auto count = [&](const char* preset) { return planSwitches(b, {}, kNames, TimeRange{Ticks{}, secs(120)}, *presetOptions(preset)).cuts.size(); };
    CHECK(count("calm") <= count("interview"));
    CHECK(count("interview") <= count("energetic"));
    CHECK_FALSE(presetOptions("frantic").has_value());
    CHECK(presetNames() == std::vector<std::string>{"calm", "interview", "energetic"});
}

TEST_CASE("multicam.autoSwitch: a dry run, one undo step of ordinary angle cuts, each explained, editable afterwards",
          "[ai][multicam][autoswitch][command]") {
    Studio s;
    CommandHarness h(s.tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, s.services()}, ai::edit::editingCommands());
    h.session.reset(s.state);
    const ProjectSnapshot before = h.state();
    const std::size_t undo = h.session.undoCount();

    const auto dry = h.run(Json{{"command", "multicam.autoSwitch"}, {"params", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}, {"preset", "interview"}}}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    CHECK(h.state() == before);

    const auto done = h.ok("multicam.autoSwitch", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}, {"preset", "interview"}, {"openWide", false}});
    CHECK(h.session.undoCount() == undo + 1);
    const Json& cuts = done.data["cuts"];
    REQUIRE(cuts.size() >= 4);
    for (const Json& c : cuts) {
        CHECK_FALSE(c["explanation"].get<std::string>().empty());
    }
    CHECK(done.data["speakers"].size() == 2);
    CHECK(done.data["angles"].size() == 3);
    // The picture is cut into pieces of the multicam source with one angle each: Cam A while the host speaks, Cam B
    // for the guest, Cam C for the crosstalk.
    const Sequence& seq = h.sequence(h.state());
    const Track& v1 = *seq.videoTracks.front();
    REQUIRE(v1.clips.size() == cuts.size());
    const auto angleAt = [&](double sec) {
        for (const Clip& c : v1.clips) {
            if (c.start() <= secs(sec) && secs(sec) < c.end()) {
                CHECK(c.sequence == s.multicam);
                return c.angles.first();
            }
        }
        return std::size_t{99};
    };
    CHECK(angleAt(4.0) == 0u);
    CHECK(angleAt(12.0) == 1u);
    CHECK(angleAt(16.2) == 2u);
    CHECK(angleAt(34.0) == 1u);
    // Ordinary angle cuts: switch one by hand afterwards.
    const ClipId piece = v1.clips[1].id;
    h.ok("multicam.switchAngle", Json{{"sequence", s.tl.seq.value}, {"clip", piece.value}, {"angle", 3}, {"target", "video"}});
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    CHECK(h.state() == before);

    // analysis.speakers reads the same and changes nothing.
    const auto speakers = h.ok("analysis.speakers", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}});
    CHECK(speakers.data["speakers"].size() == 2);
    CHECK(speakers.data["angles"][2]["role"] == "wide");
    CHECK(speakers.data["overlapSeconds"].get<double>() > 1.0);
    CHECK(h.state() == before);
    // Without the faces (quicker on long recordings): the same speakers from the transcripts and microphones.
    const int faceCalls = s.angleSignals->faceCalls;
    const auto quick = h.ok("analysis.speakers", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}, {"faces", false}});
    CHECK(s.angleSignals->faceCalls == faceCalls);
    CHECK(quick.data["faceTracks"] == 0);
    CHECK(quick.data["speakers"].size() == 2);
}

TEST_CASE("multicam.autoSwitch on a clip already in pieces: a cut just after a piece's start becomes its angle", "[ai][multicam][autoswitch][command]") {
    Studio s;
    CommandHarness h(s.tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, s.services()}, ai::edit::editingCommands());
    h.session.reset(s.state);
    // Split by hand at 8.12 s (onto the wide shot): the guest's cut is planned for 8.25 s (8.5 s, 0.25 s early).
    h.ok("multicam.switchAngle", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}, {"at", secs(8.12).value}, {"angle", 3},
                                      {"target", "video"}});
    const auto done = h.ok("multicam.autoSwitch", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}, {"preset", "interview"},
                                                       {"openWide", false}});
    const Track& v1 = *h.sequence(h.state()).videoTracks.front();
    const auto angleAt = [&](double sec) {
        for (const Clip& c : v1.clips) {
            if (c.start() <= secs(sec) && secs(sec) < c.end()) {
                return c.angles.first();
            }
        }
        return std::size_t{99};
    };
    CHECK(angleAt(4.0) == 0u);
    CHECK(angleAt(8.2) == 1u); // the guest from the piece's start, not the wide shot
    CHECK(angleAt(12.0) == 1u);
    CHECK(angleAt(16.2) == 2u);
    CHECK(angleAt(34.0) == 1u);
    // Every reported cut changes the picture.
    const Json& cuts = done.data["cuts"];
    for (std::size_t i = 1; i < cuts.size(); ++i) {
        CHECK(cuts[i]["angle"] != cuts[i - 1]["angle"]);
    }
    CHECK(done.data["planned"].size() >= cuts.size());
}
