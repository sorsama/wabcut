// AI-13 active-speaker detection: the speaker timeline of the synthetic podcast from its transcripts, microphone levels
// and faces (who speaks when, crosstalk, silence, a reaction and laughter), which camera shows whom, each cue alone,
// bleed and duplicate words, and the user's angle assignments.

#include "MulticamTestUtil.h"

#include <wabcut/ai/multicam/Speakers.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai::multicam;
using namespace wabcut::ai::multicam::test;

namespace {

SpeakerRun runOn(const Studio& s, bool withSignals = true, SpeakerOptions options = {}) {
    auto services = s.services(withSignals);
    auto run = speakersOf(s.state, s.multicam, services.get(), options);
    REQUIRE(run);
    return std::move(*run);
}

std::size_t speakerNamed(const SpeakerTimeline& t, const std::string& name) {
    const auto it = std::ranges::find_if(t.speakers, [&](const ai::multicam::Speaker& s) { return s.name == name; });
    INFO("speaker " << name);
    REQUIRE(it != t.speakers.end());
    return static_cast<std::size_t>(it - t.speakers.begin());
}

const SpeakerSegment& at(const SpeakerTimeline& t, double seconds) {
    const SpeakerSegment* s = t.segmentAt(secs(seconds));
    REQUIRE(s != nullptr);
    return *s;
}

const AngleAssignment& angle(const SpeakerTimeline& t, std::size_t a) {
    const auto it = std::ranges::find_if(t.angles, [&](const AngleAssignment& x) { return x.angle == a; });
    INFO("angle " << a);
    REQUIRE(it != t.angles.end());
    return *it;
}

} // namespace

TEST_CASE("The speaker timeline: who speaks when, crosstalk, silence, a reaction and laughter", "[ai][multicam][speakers]") {
    Studio s;
    const SpeakerRun run = runOn(s);
    const SpeakerTimeline& t = run.timeline;
    REQUIRE(t.speakers.size() == 2);
    const std::size_t host = speakerNamed(t, "Mic 1");
    const std::size_t guest = speakerNamed(t, "Mic 2");
    CHECK(t.speakers[host].mic == 3u);
    CHECK(t.speakers[guest].mic == 4u);
    CHECK(t.speakers[host].seconds > 18.0);
    CHECK(t.speakers[guest].seconds > 14.0);

    CHECK(at(t, 4.0).kind == SegmentKind::Single);
    CHECK(at(t, 4.0).speakers == std::vector<std::size_t>{host});
    CHECK(at(t, 11.0).speakers == std::vector<std::size_t>{guest});
    CHECK(at(t, 16.0).kind == SegmentKind::Overlap);
    CHECK(at(t, 20.0).speakers == std::vector<std::size_t>{host});
    CHECK(at(t, 22.2).speakers == std::vector<std::size_t>{host}); // the guest's "Wow!" is a reaction, not crosstalk
    CHECK(at(t, 34.0).speakers == std::vector<std::size_t>{guest});
    CHECK(at(t, 39.0).kind == SegmentKind::Silence);
    CHECK(at(t, 4.0).confidence > 0.3);
    // The segments cover the source without gaps.
    REQUIRE_FALSE(t.segments.empty());
    CHECK(t.segments.front().range.start == Ticks{});
    for (std::size_t i = 1; i < t.segments.size(); ++i) {
        CHECK(t.segments[i].range.start == t.segments[i - 1].range.end());
    }
    // The guest's "Wow!" and laughter while the host speaks are reactions.
    bool reacts = false, laughs = false;
    for (const Reaction& r : t.reactions) {
        reacts = reacts || (r.speaker == guest && r.kind == "reacts" && r.range.start >= secs(21.5) && r.range.start <= secs(22.5));
        laughs = laughs || (r.speaker == guest && r.kind == "laughs" && r.range.start >= secs(24.8) && r.range.start <= secs(25.3));
    }
    CHECK(reacts);
    CHECK(laughs);

    // Cam A is the host's close-up, Cam B the guest's, Cam C the wide shot.
    CHECK(angle(t, 0).role == AngleRole::CloseUp);
    CHECK(angle(t, 0).speakers == std::vector<std::size_t>{host});
    CHECK(angle(t, 1).role == AngleRole::CloseUp);
    CHECK(angle(t, 1).speakers == std::vector<std::size_t>{guest});
    CHECK(angle(t, 2).role == AngleRole::Wide);
    CHECK(t.closeUpOf(host) == 0u);
    CHECK(t.closeUpOf(guest) == 1u);
    CHECK(t.wide() == 2u);
    REQUIRE(angle(t, 0).face.has_value());
    CHECK(angle(t, 0).reason.find("lips") != std::string::npos);
    // The mic angles have no picture: no assignment.
    CHECK(t.angles.size() == 3);
}

TEST_CASE("Each cue alone still gives a timeline; bleed and duplicate words are discounted", "[ai][multicam][speakers]") {
    SECTION("transcripts alone: the speakers, no camera roles until you set them") {
        Studio s;
        const SpeakerRun run = runOn(s, false);
        const SpeakerTimeline& t = run.timeline;
        REQUIRE(t.speakers.size() == 2);
        CHECK(at(t, 4.0).kind == SegmentKind::Single);
        CHECK(at(t, 16.0).kind == SegmentKind::Overlap);
        CHECK_FALSE(t.wide().has_value());
        // Your assignments fill it in.
        SpeakerOptions options;
        options.overrides = {{0, AngleRole::CloseUp, {"Mic 1"}}, {1, AngleRole::CloseUp, {"Mic 2"}}, {2, AngleRole::Wide, {}}};
        const SpeakerRun set = runOn(s, false, options);
        CHECK(set.timeline.closeUpOf(speakerNamed(set.timeline, "Mic 1")) == 0u);
        CHECK(set.timeline.wide() == 2u);
        CHECK(angle(set.timeline, 2).confirmed);
    }
    SECTION("levels alone: one speaker per microphone, bleed below the speaker") {
        Studio s;
        s.transcripts = std::make_shared<ai::edit::MapTranscriptProvider>(); // no transcripts
        const SpeakerRun run = runOn(s, true);
        const SpeakerTimeline& t = run.timeline;
        REQUIRE(t.speakers.size() == 2);
        CHECK(at(t, 4.0).kind == SegmentKind::Single);
        CHECK(at(t, 11.0).kind == SegmentKind::Single);
        CHECK(at(t, 4.0).speakers != at(t, 11.0).speakers);
        CHECK(at(t, 39.0).kind == SegmentKind::Silence);
    }
    SECTION("levels alone with a camera's own sound: the room's, not a speaker") {
        SpeakerEvidence ev;
        ev.duration = secs(20);
        ev.angles = {{0, "Cam A", true, true}, {1, "Mic 1", false, true}, {2, "Mic 2", false, true}};
        const auto envelope = [&](std::size_t angle, auto level) {
            MicEnvelope m{angle, Ticks{}, secs(0.05), {}};
            for (double t = 0; t < 20; t += 0.05) {
                m.db.push_back(level(t));
            }
            return m;
        };
        ev.mics = {envelope(0, [](double t) { return t < 9 ? -24.0f : t < 16 ? -26.0f : -60.0f; }),        // the room: everyone
                   envelope(1, [](double t) { return t < 9 ? -18.0f : t < 16 ? -40.0f : -60.0f; }),        // host, then bleed
                   envelope(2, [](double t) { return t < 9 ? -40.0f : t < 16 ? -18.0f : -60.0f; })};       // bleed, then guest
        const SpeakerTimeline t = analyzeSpeakers(ev);
        REQUIRE(t.speakers.size() == 2);
        CHECK(t.speakers[0].name == "Mic 1");
        CHECK(t.speakers[1].name == "Mic 2");
        CHECK(at(t, 4.0).kind == SegmentKind::Single);
        CHECK(at(t, 4.0).speakers == std::vector<std::size_t>{0});
        CHECK(at(t, 13.0).speakers == std::vector<std::size_t>{1});
        CHECK(std::ranges::any_of(t.notes, [](const std::string& n) { return n.find("the room's") != std::string::npos; }));
    }
    SECTION("faces alone: one speaker per close-up camera") {
        SpeakerEvidence ev;
        ev.duration = secs(20);
        ev.angles = {{0, "Cam A", true, false}, {1, "Cam B", true, false}};
        for (std::size_t a = 0; a < 2; ++a) {
            FaceTrack ft;
            ft.angle = a;
            for (double x = 0; x < 20; x += 0.2) {
                const bool speaking = a == 0 ? x < 10 : x >= 10;
                ft.points.push_back(FacePoint{secs(x), 0.3f, 0.2f, 0.3f, 0.4f, speaking ? 0.5f : 0.02f});
                ev.faceCounts.push_back(FaceCount{a, secs(x), 1});
            }
            ev.faces.push_back(ft);
        }
        const SpeakerTimeline t = analyzeSpeakers(ev);
        REQUIRE(t.speakers.size() == 2);
        CHECK(at(t, 5.0).speakers == std::vector<std::size_t>{0});
        CHECK(at(t, 15.0).speakers == std::vector<std::size_t>{1});
        CHECK(t.closeUpOf(0) == 0u);
    }
    SECTION("the same word heard by both mics counts once, for the louder one") {
        SpeakerEvidence ev;
        ev.duration = secs(4);
        ev.angles = {{0, "Mic 1", false, true}, {1, "Mic 2", false, true}};
        for (double x = 0.5; x < 3.0; x += 0.4) {
            ev.words.push_back(SpokenWord{TimeRange{secs(x), secs(0.3)}, 0, 0, {}, "hello", 0.9f});
            ev.words.push_back(SpokenWord{TimeRange{secs(x + 0.02), secs(0.3)}, 1, 0, {}, "hello", 0.5f}); // bleed
        }
        MicEnvelope m1{0, Ticks{}, secs(0.05), std::vector<float>(80, -60.0f)};
        MicEnvelope m2{1, Ticks{}, secs(0.05), std::vector<float>(80, -60.0f)};
        for (std::size_t i = 10; i < 60; ++i) {
            m1.db[i] = -18.0f;
            m2.db[i] = -40.0f;
        }
        ev.mics = {m1, m2};
        const SpeakerTimeline t = analyzeSpeakers(ev);
        // Mic 2's words were the bleed: dropped, so Mic 2 never "speaks".
        for (const SpeakerSegment& seg : t.segments) {
            CHECK(seg.kind != SegmentKind::Overlap);
        }
        CHECK(at(t, 1.0).kind == SegmentKind::Single);
    }
}
