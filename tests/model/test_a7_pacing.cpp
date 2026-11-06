// Stream A, batch A7 (0.7), model: the pacing heat strip's analysis (card 180): the cuts of the picture, and the grades a long
// shot, stillness, silence and slow speech give a stretch.

#include "TimelineFixture.h"

#include <wabcut/model/GamingOps.h>
#include <wabcut/model/PacingMap.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Ticks secondsT(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

/// One clip of `count` frames of media `letter` (from source frame `sourceIn`) as the fixture's picture text.
std::string clipText(char letter, int count, int sourceIn = 0) {
    std::string s(1, letter);
    if (sourceIn > 0) {
        s += std::to_string(sourceIn); // the digits are frames of the clip too
    }
    return s + std::string(static_cast<std::size_t>(std::max<int>(0, count - static_cast<int>(s.size()))), static_cast<char>(letter + 32));
}

/// A span of `seconds` that is all picture.
PacingInputs spanOf(double seconds) {
    PacingInputs in;
    in.start = Ticks{};
    in.end = secondsT(seconds);
    in.picture.push_back(TimeRange::fromStartEnd(in.start, in.end));
    return in;
}

/// A cut every second.
void cutEverySecond(PacingInputs& in, int seconds) {
    for (int i = 1; i < seconds; ++i) {
        in.cuts.push_back(secondsT(i));
    }
}

ActivityCurve flat(double seconds, double value, double stepSeconds = 0.1) {
    ActivityCurve c;
    c.start = Ticks{};
    c.step = secondsT(stepSeconds);
    c.values.assign(static_cast<std::size_t>(seconds / stepSeconds), value);
    return c;
}

} // namespace

TEST_CASE("Pacing: the cuts of the picture are where the top visible clip changes, not through edits (card 180)",
          "[model][pacing]") {
    SECTION("cuts, a gap, and a through edit that is no cut") {
        // A 12 s, B 4 s, a 2 s gap, C 1 s, D 2 s continued by D (the next source frames): no cut inside D.
        Timeline tl("V1 |" + clipText('A', 300) + clipText('B', 100) + std::string(50, '.') + clipText('C', 25) +
                    clipText('D', 50) + clipText('D', 50, 50) + "|");
        const PacingInputs in = pacingInputsOf(tl.sequence());
        REQUIRE(in.picture.size() == 2);
        CHECK(in.picture[0].start == Ticks{});
        CHECK(in.picture[0].end() == f(400));
        CHECK(in.picture[1].start == f(450));
        CHECK(in.picture[1].end() == f(575));
        // 12 s (A to B), 16 s (the picture ends), 18 s (it begins), 19 s (C to D) and 23 s (it ends); the join inside D is none.
        const std::vector<Ticks> want{f(300), f(400), f(450), f(475), f(575)};
        CHECK(in.cuts == want);
        CHECK(in.end == f(575));
    }
    SECTION("a clip over another is a cut-away: the top clip decides; a hidden track counts for nothing") {
        Timeline tl(R"(
            V3 off |....Cccc.....|
            V2     |....Bbbb.....|
            V1     |Aaaaaaaaaaaaa|
        )");
        const PacingInputs in = pacingInputsOf(tl.sequence());
        CHECK(in.cuts == std::vector<Ticks>{f(4), f(8), f(13)}); // the last is where the picture ends
        REQUIRE(in.picture.size() == 1);
        CHECK(in.picture[0].end() == f(13));
    }
    SECTION("a soloed video track is the only picture") {
        Timeline tl(R"(
            V2 solo |....Bbbb.....|
            V1      |Aaaaaaaaaaaaa|
        )");
        const PacingInputs in = pacingInputsOf(tl.sequence());
        REQUIRE(in.picture.size() == 1);
        CHECK(in.picture[0].start == f(4));
        CHECK(in.picture[0].end() == f(8));
        CHECK(in.cuts == std::vector<Ticks>{f(4), f(8)});
    }
    SECTION("an empty sequence has nothing to grade") {
        Timeline tl("V1 |...|");
        const PacingInputs in = pacingInputsOf(tl.sequence());
        CHECK(in.picture.empty());
        CHECK(in.cuts.empty());
        CHECK(pacingCells(in).empty());
    }
}

TEST_CASE("Pacing: a long shot, stillness, silence and slow speech each grade a stretch (card 180)", "[model][pacing]") {
    const PacingOptions options;

    SECTION("no cut for 30 s: the whole stretch drags, for the long shot") {
        PacingInputs in = spanOf(30);
        in.motion = flat(30, 0.5); // plenty moving
        const auto cells = pacingCells(in, options);
        REQUIRE(cells.size() == 30);
        CHECK(cells.front().shotSeconds == 30.0F);
        CHECK(cells.front().reasons == kPacingLongShot);
        const auto stretches = pacingStretches(cells, in.end, options);
        REQUIRE(stretches.size() == 1);
        CHECK(stretches[0].level == 3);
        CHECK(stretches[0].range.start == Ticks{});
        CHECK(stretches[0].range.end() == in.end);
        CHECK(stretches[0].longestShot == 30.0);
        CHECK(stretches[0].meanMotion == 0.5);
    }
    SECTION("cuts every four seconds are fine, with or without motion measured") {
        PacingInputs in = spanOf(40);
        for (int i = 1; i < 10; ++i) {
            in.cuts.push_back(secondsT(4.0 * i));
        }
        CHECK(pacingStretches(pacingCells(in, options), in.end, options).empty());
        in.motion = flat(40, 0.0);
        CHECK(pacingStretches(pacingCells(in, options), in.end, options).empty());
    }
    SECTION("a ten second shot that hardly moves: the stillness is the reason, the length alone is not") {
        PacingInputs in = spanOf(10);
        in.motion = flat(10, 0.0);
        const auto stretches = pacingStretches(pacingCells(in, options), in.end, options);
        REQUIRE_FALSE(stretches.empty());
        CHECK((stretches.back().reasons & kPacingStill) != 0);
        CHECK((stretches.back().reasons & kPacingLongShot) == 0);
        CHECK(stretches.back().level == 2);
        // Not measured: nothing is said about stillness.
        in.motion = {};
        CHECK(pacingStretches(pacingCells(in, options), in.end, options).empty());
    }
    SECTION("silence counts after two seconds and in full after six; a blip of sound ends the run") {
        PacingInputs in = spanOf(20);
        cutEverySecond(in, 20); // no long shot to confuse it
        in.audio = flat(20, 0.0);
        in.audio.values[75] = 0.6F; // a sound at 7.5 s: two runs, 8 s and 12 s (the blip's cell is not silent)
        const auto cells = pacingCells(in, options);
        CHECK(cells[3].silentSeconds == 7.0F);
        CHECK(cells[3].reasons == kPacingSilence);
        CHECK(cells[7].silentSeconds == 0.0F);
        const auto stretches = pacingStretches(cells, in.end, options);
        REQUIRE(stretches.size() == 2);
        CHECK(stretches[0].level == 3);
        CHECK(stretches[0].longestSilence == 7.0);
        CHECK(stretches[1].longestSilence == 12.0);
    }
    SECTION("quiet for only one and a half seconds is below the two that count") {
        PacingInputs brief = spanOf(20);
        cutEverySecond(brief, 20);
        brief.audio = flat(20, 0.4);
        for (std::size_t i = 50; i < 65; ++i) {
            brief.audio.values[i] = 0.0F;
        }
        CHECK(pacingStretches(pacingCells(brief, options), brief.end, options).empty());
    }
    SECTION("slow speech: thirty words a minute is slow, a lively delivery is not, nobody speaking is no speech to grade") {
        PacingInputs slow = spanOf(60);
        cutEverySecond(slow, 60);
        PacingInputs lively = slow;
        for (int i = 0; i < 29; ++i) {
            slow.words.push_back(secondsT(2.0 * i + 1)); // a word every two seconds: 30 a minute
        }
        for (int i = 0; i < 200; ++i) {
            lively.words.push_back(secondsT(0.3 * i + 0.1)); // 200 a minute
        }
        const auto slowStretches = pacingStretches(pacingCells(slow, options), slow.end, options);
        REQUIRE_FALSE(slowStretches.empty());
        CHECK((slowStretches.front().reasons & kPacingSlowSpeech) != 0);
        CHECK(slowStretches.front().slowestWpm > 0.0);
        CHECK(slowStretches.front().slowestWpm <= 40.0);
        CHECK(pacingStretches(pacingCells(lively, options), lively.end, options).empty());
        PacingInputs nobody = slow;
        nobody.words.clear();
        CHECK(pacingStretches(pacingCells(nobody, options), nobody.end, options).empty());
    }
    SECTION("what is not measured yet is unknown, not silent") {
        PacingInputs in = spanOf(20);
        cutEverySecond(in, 20);
        in.audio = flat(20, 0.0);
        in.unmeasured.push_back(TimeRange::fromStartEnd(Ticks{}, secondsT(20)));
        CHECK(pacingStretches(pacingCells(in, options), in.end, options).empty());
    }
    SECTION("a gap has no shot to grade; the same inputs give the same strip; very long spans get wider cells") {
        PacingInputs in = spanOf(40);
        in.picture = {TimeRange::fromStartEnd(Ticks{}, secondsT(10))};
        in.cuts = {secondsT(10)};
        const auto cells = pacingCells(in, options);
        CHECK(cells[25].shotSeconds == 0.0F);
        CHECK(cells[25].score == 0.0F);
        const auto again = pacingCells(in, options);
        REQUIRE(again.size() == cells.size());
        for (std::size_t i = 0; i < cells.size(); ++i) {
            CHECK(again[i].score == cells[i].score);
        }
        const PacingInputs huge = spanOf(60.0 * 3600 * 100);
        CHECK(pacingCells(huge, options).size() <= 20'000);
    }
    SECTION("a four second silence is a stretch of level 1; stretches below level 3 shorter than the minimum are left off") {
        PacingInputs in = spanOf(12);
        cutEverySecond(in, 12);
        in.audio = flat(12, 0.5);
        for (std::size_t i = 40; i < 80; ++i) {
            in.audio.values[i] = 0.0F; // 4 s of silence, cells 4 to 7
        }
        const auto kept = pacingStretches(pacingCells(in, options), in.end, options);
        REQUIRE(kept.size() == 1);
        CHECK(kept[0].level == 1);
        CHECK(kept[0].range.start == secondsT(4));
        CHECK(kept[0].range.end() == secondsT(8));
        PacingOptions strict = options;
        strict.minStretchSeconds = 10;
        CHECK(pacingStretches(pacingCells(in, strict), in.end, strict).empty());
        // ... but a stretch that drags (level 3) stays however short the minimum says: a six second silence.
        for (std::size_t i = 40; i < 100; ++i) {
            in.audio.values[i] = 0.0F;
        }
        const auto drags = pacingStretches(pacingCells(in, strict), in.end, strict);
        REQUIRE(drags.size() == 1);
        CHECK(drags[0].level == 3);
    }
    SECTION("options out of range are named") {
        PacingOptions bad;
        bad.longShotSeconds = 30;
        bad.veryLongShotSeconds = 10;
        CHECK(pacingOptionsProblem(bad).has_value());
        CHECK_FALSE(pacingOptionsProblem(PacingOptions{}).has_value());
    }
}

TEST_CASE("Pacing: the sequence's measured curves feed the analysis and unmeasured media stay unknown (card 180)",
          "[model][pacing]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    PacingInputs in = pacingInputsOf(tl.sequence());

    SECTION("with nothing measured the clips' time is unknown, never silent") {
        addPacingActivity(in, tl.state, tl.seq, [](MediaId) -> const MediaActivity* { return nullptr; });
        REQUIRE_FALSE(in.unmeasured.empty());
        CHECK(in.unmeasured.front().start == Ticks{});
        for (const PacingCell& c : pacingCells(in)) {
            CHECK(c.silentSeconds == 0.0F);
            CHECK(c.motion < 0.0F);
        }
    }
    SECTION("measured: the curves are the sequence's and nothing is unknown") {
        MediaActivity activity;
        activity.step = secondsT(0.1);
        for (int i = 0; i < 100; ++i) {
            activity.audio.push_back(i >= 60 ? 0.8F : 0.0F); // sound arrives at 6 s
            activity.motion.push_back(i >= 60 ? 0.8F : 0.0F);
        }
        const MediaId media = tl.media('A');
        addPacingActivity(in, tl.state, tl.seq, [&](MediaId id) -> const MediaActivity* { return id == media ? &activity : nullptr; });
        CHECK(in.unmeasured.empty());
        CHECK_FALSE(in.audio.values.empty());
        CHECK_FALSE(in.motion.values.empty());
        const auto cells = pacingCells(in);
        REQUIRE_FALSE(cells.empty());
        CHECK(cells.front().motion >= 0.0F);
    }
}
