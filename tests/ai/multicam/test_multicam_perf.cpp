// AI-13 performance: speaker analysis and switching on a 60-minute, 3-camera, 2-microphone podcast (the synthetic
// conversation repeated: 90 rounds of host, guest, crosstalk, a long monologue with laughter, silence). The signals are
// in memory here, so this measures the analysis itself (gathering the evidence, fusing it, planning the cuts and
// cutting the clip); decoding the microphones is measured in the ai_multicam_media suite and face detection in AI-9's.

#include "CommandHarness.h"
#include "MulticamTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>

using namespace wabcut;
using namespace wabcut::ai::multicam;
using namespace wabcut::ai::multicam::test;
using command::Json;

namespace {

std::vector<Talk> hourScript() {
    std::vector<Talk> script;
    for (int round = 0; round < 90; ++round) {
        const double o = round * 40.0;
        for (const Talk& t : podcastScript()) {
            script.push_back(Talk{t.start + o, t.end + o, t.who});
        }
    }
    return script;
}

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("Speaker analysis of a 60-minute 3-camera podcast takes seconds", "[ai][multicam][benchmark]") {
    const auto t0 = std::chrono::steady_clock::now();
    Studio s(hourScript(), false, 3600.0);
    const double setup = since(t0);

    const auto t1 = std::chrono::steady_clock::now();
    auto services = s.services();
    auto run = speakersOf(s.state, s.multicam, services.get(), {});
    REQUIRE(run);
    const double analysis = since(t1);
    const SpeakerTimeline& t = run->timeline;
    REQUIRE(t.speakers.size() == 2);
    CHECK(t.wide() == 2u);
    CHECK(t.closeUpOf(0).has_value());
    CHECK(t.closeUpOf(1).has_value());
    // The 90 rounds are found: speech, crosstalk and silence in every one.
    std::size_t overlaps = 0, silences = 0;
    for (const SpeakerSegment& seg : t.segments) {
        overlaps += seg.kind == SegmentKind::Overlap ? 1 : 0;
        silences += seg.kind == SegmentKind::Silence && seg.range.duration >= secs(1.5) ? 1 : 0;
    }
    CHECK(overlaps >= 90);
    CHECK(silences >= 89);

    CommandHarness h(s.tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, services}, ai::edit::editingCommands());
    h.session.reset(s.state);
    const auto t2 = std::chrono::steady_clock::now();
    const auto done = h.ok("multicam.autoSwitch", Json{{"sequence", s.tl.seq.value}, {"clip", s.picture.value}, {"preset", "interview"}});
    const double switching = since(t2);
    const std::size_t cuts = done.data["cuts"].size();
    CHECK(cuts > 400);

    std::printf("[perf] AI-13 60-min 3-camera podcast: fixture %.2f s, speaker analysis %.2f s (%.0fx real time), "
                "analysis + %zu cuts through multicam.autoSwitch %.2f s\n",
                setup, analysis, 3600.0 / std::max(analysis, 1e-3), cuts, switching);
    CHECK(analysis < 20.0);
    CHECK(switching < 40.0);
}
