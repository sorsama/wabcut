// 0.6 (P1 round, #54): loudness QC - where the loudest moments and the true-peak overs are. truePeakOvers() merges the
// hot 100 ms blocks a measurement kept into overs above a ceiling, and audio.loudness reports where (timecode and
// ticks on the timeline) with the overs above "oversAbove". Pure: the analyzer is a fake that says where.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <optional>
#include <string_view>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

/// Exact ticks of a whole number of milliseconds.
Ticks ms(std::int64_t millis) { return Ticks{millis * (kTicksPerSecond / 1000)}; }
Ticks s(std::int64_t seconds) { return ms(seconds * 1000); }

/// Hot blocks of 100 ms at the given (milliseconds from the measured start, dBTP).
LoudnessStats withBlocks(const std::vector<std::pair<std::int64_t, double>>& blocks) {
    LoudnessStats stats;
    stats.peakFloorDbtp = -6.0;
    for (const auto& [at, db] : blocks)
        stats.peakBlocks.push_back({ms(at), ms(100), db});
    return stats;
}

/// Says the loudest momentary window starts 2 s into what it measures, the short-term one at 1 s, the true peak at
/// 2.3 s, with hot blocks at 2.2-2.5 s (0.4 dBTP at most) and 5.0-5.1 s (-0.5 dBTP).
class WhereLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState&, SequenceId, const LoudnessTarget&,
                                                std::optional<TimeRange> range, command::ExecutionControl*) override {
        LoudnessStats stats = withBlocks({{2200, -0.8}, {2300, 0.4}, {2400, -0.2}, {5000, -0.5}, {7000, -3.0}});
        stats.integratedLufs = -16.0;
        stats.truePeakDbtp = 0.4;
        stats.maxMomentaryLufs = -9.0;
        stats.maxShortTermLufs = -12.0;
        stats.duration = range ? range->duration : s(10);
        stats.start = range ? range->start : Ticks{};
        stats.maxMomentaryAt = s(2);
        stats.maxShortTermAt = s(1);
        stats.truePeakAt = ms(2300);
        return stats;
    }
};

/// A very hot programme (card 166): the measurement kept only the blocks above -2 dBTP, one over of them at 5 s.
class HotLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState&, SequenceId, const LoudnessTarget&,
                                                std::optional<TimeRange> range, command::ExecutionControl*) override {
        LoudnessStats stats = withBlocks({{5000, -0.5}, {5100, -1.5}});
        stats.peakFloorDbtp = -2.0;
        stats.truePeakDbtp = -0.5;
        stats.duration = range ? range->duration : s(10);
        stats.start = range ? range->start : Ticks{};
        stats.truePeakAt = ms(5000);
        return stats;
    }
};

/// Silence: nothing is anywhere.
class SilentLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState&, SequenceId, const LoudnessTarget&,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        LoudnessStats stats;
        stats.duration = s(10);
        return stats;
    }
};

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

CommandHarness harness(std::shared_ptr<LoudnessAnalyzer> loudness) {
    return CommandHarness(Timeline(kPicture),
                          command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(std::move(loudness), nullptr)},
                          ai::edit::editingCommands());
}

} // namespace

TEST_CASE("True-peak overs: neighbouring hot blocks merge into ranges above the ceiling", "[audio][edit][loudness][qc]") {
    // Blocks at 1.0-1.3 s (a gap of one block at 1.3 s is bridged: 1.4 s joins), 3.0 s alone, 6.0 s below the ceiling.
    LoudnessStats stats = withBlocks({{1000, -0.5}, {1100, 0.2}, {1200, -0.9}, {1400, -0.7}, {3000, -0.3}, {6000, -2.0}});
    stats.start = s(100); // the measured range starts 100 s into the sequence

    std::size_t total = 0;
    const auto overs = truePeakOvers(stats, -1.0, 200, &total);
    REQUIRE(overs.size() == 2);
    CHECK(total == 2);
    CHECK(overs[0].range == TimeRange::fromStartEnd(ms(101000), ms(101500)));
    CHECK(overs[0].truePeakDbtp == 0.2);
    CHECK(overs[1].range == TimeRange{ms(103000), ms(100)});
    CHECK(overs[1].truePeakDbtp == -0.3);

    // A higher ceiling: only the blocks above it count, and the gap they leave (1.2-1.5 s) splits nothing more.
    const auto strict = truePeakOvers(stats, -0.6, 200, &total);
    REQUIRE(strict.size() == 2);
    CHECK(total == 2);
    CHECK(strict[0].range == TimeRange::fromStartEnd(ms(101000), ms(101200)));
    CHECK(strict[1].range == TimeRange{ms(103000), ms(100)});

    // Two blocks apart is two overs.
    const auto apart = truePeakOvers(withBlocks({{0, -0.5}, {300, -0.5}}), -1.0);
    CHECK(apart.size() == 2);
}

TEST_CASE("True-peak overs: at most the asked number listed and the total counted; the floor holds", "[audio][edit][loudness][qc]") {
    // 300 single hot blocks a second apart: 300 overs.
    std::vector<std::pair<std::int64_t, double>> blocks;
    for (int i = 0; i < 300; ++i)
        blocks.emplace_back(std::int64_t{i} * 1000, -0.5);
    const LoudnessStats stats = withBlocks(blocks);
    std::size_t total = 0;
    const auto overs = truePeakOvers(stats, -1.0, 200, &total);
    CHECK(overs.size() == 200);
    CHECK(total == 300);
    CHECK(overs.front().range.start == Ticks{});
    CHECK(overs.back().range.start == s(199));

    // A ceiling below the floor the blocks were kept at is the floor: nothing quieter than it was kept to list.
    LoudnessStats quiet = withBlocks({{0, -5.0}, {2000, -7.0}});
    CHECK(truePeakOvers(quiet, -20.0).size() == 1);
    // No blocks: no overs.
    CHECK(truePeakOvers(LoudnessStats{}, -1.0).empty());
}

TEST_CASE("audio.loudness says where the loudest moments and the true peak are and lists the overs", "[audio][edit][loudness][qc]") {
    CommandHarness h = harness(std::make_shared<WhereLoudness>());
    const command::CommandResult r = h.ok("audio.loudness");
    const Json& mix = r.data["results"][0];
    CHECK(mix["maxMomentaryAt"]["ticks"] == s(2).value);
    CHECK(mix["maxShortTermAt"]["ticks"] == s(1).value);
    CHECK(mix["truePeakAt"]["ticks"] == ms(2300).value);
    CHECK(mix["truePeakAt"]["time"].is_string());
    CHECK_FALSE(mix["truePeakAt"]["time"].get<std::string>().empty());
    // Default ceiling -1 dBTP: 2.2-2.5 s (the highest 0.4) and 5.0-5.1 s; the -3 dBTP block is not an over.
    CHECK(mix["oversAbove"] == -1.0);
    CHECK(mix["overCount"] == 2);
    REQUIRE(mix["overs"].size() == 2);
    CHECK(mix["overs"][0]["startTicks"] == ms(2200).value);
    CHECK(mix["overs"][0]["endTicks"] == ms(2500).value);
    CHECK(mix["overs"][0]["truePeakDbtp"] == 0.4);
    CHECK(mix["overs"][0]["start"].is_string());
    CHECK(mix["overs"][1]["startTicks"] == s(5).value);

    // A stricter ceiling ("oversAbove" 0): only the 0.4 dBTP block.
    const command::CommandResult strict = h.ok("audio.loudness", {{"oversAbove", 0.0}});
    const Json& one = strict.data["results"][0];
    CHECK(one["overCount"] == 1);
    REQUIRE(one["overs"].size() == 1);
    CHECK(one["overs"][0]["startTicks"] == ms(2300).value);
    CHECK(one["overs"][0]["endTicks"] == ms(2400).value);
    // Out of range is refused.
    CHECK(h.errorOf("audio.loudness", {{"oversAbove", 3.0}}) == "InvalidParams");

    // A range: the places are on the timeline (the range's start plus the offset).
    const command::CommandResult part = h.ok("audio.loudness", {{"from", s(4).value}, {"to", s(9).value}});
    const Json& later = part.data["results"][0];
    CHECK(later["truePeakAt"]["ticks"] == ms(6300).value);
    CHECK(later["overs"][0]["startTicks"] == ms(6200).value);
}

TEST_CASE("audio.loudness of a very hot programme says when its overs are counted above a higher ceiling", "[audio][edit][loudness][qc]") {
    CommandHarness h = harness(std::make_shared<HotLoudness>());
    // The ceiling asked for (the default -1) is above what the measurement kept: exact, nothing flagged.
    const Json exact = h.ok("audio.loudness").data["results"][0];
    CHECK(exact["oversTruncated"] == false);
    CHECK(exact["oversAbove"] == -1.0);
    CHECK(exact["overCount"] == 1);
    // A ceiling below the floor (-3 dBTP asked, -2 kept): counted above the floor, and said so.
    const Json low = h.ok("audio.loudness", {{"oversAbove", -3.0}}).data["results"][0];
    CHECK(low["oversTruncated"] == true);
    CHECK(low["oversAbove"] == -2.0);
    CHECK(low["overCount"] == 1);
    REQUIRE(low["overs"].size() == 1);
    CHECK(low["overs"][0]["startTicks"] == s(5).value);
    // The default analyzers (the -6 dBTP floor) never flag the whole allowed range -6..0.
    CommandHarness plain = harness(std::make_shared<WhereLoudness>());
    CHECK(plain.ok("audio.loudness", {{"oversAbove", -6.0}}).data["results"][0]["oversTruncated"] == false);
}

TEST_CASE("audio.loudness of silence says nowhere and no overs", "[audio][edit][loudness][qc]") {
    CommandHarness h = harness(std::make_shared<SilentLoudness>());
    const Json mix = h.ok("audio.loudness").data["results"][0];
    CHECK(mix["maxMomentaryAt"].is_null());
    CHECK(mix["maxShortTermAt"].is_null());
    CHECK(mix["truePeakAt"].is_null());
    CHECK(mix["overCount"] == 0);
    CHECK(mix["overs"].empty());
}
