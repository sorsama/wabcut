// Music Remix's planner (A.7) on a synthetic song with a known bar structure (MusicTestSignals.h), analysed the way the
// media analyzer does it (ai/analysis's beat tracker, then the beat features): remixes within one beat of the target,
// joins on beats and on the bar grid, the intro and the outro kept, and no clicks at the joins (spectral flux). The
// analysis time of a five-minute track is reported.

#include "MusicTestSignals.h"

#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/audio/edit/Remix.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;

namespace {

Ticks secs(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

const MusicAnalysis& songAnalysis() { return defaultSongAnalysis(); }

const SyntheticSong& song() { return defaultSong(); }

/// Spectral flux (the summed positive change of the magnitude spectrum, 1024-point Hann frames, hop 128) per frame.
std::vector<double> flux(const std::vector<float>& x, int rate, std::vector<double>* centres = nullptr) {
    constexpr int kN = 1024;
    constexpr int kHop = 128;
    std::vector<double> previous(kN / 2 + 1, 0.0);
    std::vector<double> out;
    for (std::size_t start = 0; start + kN <= x.size(); start += kHop) {
        std::vector<double> mag(kN / 2 + 1, 0.0);
        // A direct DFT of a 1024-point frame is too slow for minutes of audio: a radix-2 FFT.
        std::vector<std::complex<double>> v(kN);
        for (int i = 0; i < kN; ++i) {
            v[static_cast<std::size_t>(i)] = x[start + static_cast<std::size_t>(i)] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kN));
        }
        for (std::size_t i = 1, j = 0; i < kN; ++i) {
            std::size_t bit = kN >> 1;
            for (; (j & bit) != 0; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(v[i], v[j]);
            }
        }
        for (std::size_t len = 2; len <= kN; len <<= 1) {
            const std::complex<double> w = std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(len));
            for (std::size_t i = 0; i < kN; i += len) {
                std::complex<double> wk = 1.0;
                for (std::size_t k = 0; k < len / 2; ++k) {
                    const auto t = wk * v[i + k + len / 2];
                    v[i + k + len / 2] = v[i + k] - t;
                    v[i + k] += t;
                    wk *= w;
                }
            }
        }
        double f = 0.0;
        for (int k = 0; k <= kN / 2; ++k) {
            mag[static_cast<std::size_t>(k)] = std::abs(v[static_cast<std::size_t>(k)]);
            f += std::max(0.0, mag[static_cast<std::size_t>(k)] - previous[static_cast<std::size_t>(k)]);
        }
        previous = std::move(mag);
        out.push_back(f);
        if (centres != nullptr) {
            centres->push_back((static_cast<double>(start) + kN / 2.0) / rate);
        }
    }
    return out;
}

/// The largest flux in [t - window, t + window] seconds.
double peakFlux(const std::vector<double>& f, const std::vector<double>& centres, double t, double window) {
    double peak = 0.0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (std::fabs(centres[i] - t) <= window) {
            peak = std::max(peak, f[i]);
        }
    }
    return peak;
}

} // namespace

TEST_CASE("The synthetic song's beats and bars are found where they are", "[audio][edit][remix]") {
    const MusicAnalysis& a = songAnalysis();
    CHECK(a.bpm == Approx(120.0).margin(1.0));
    REQUIRE(a.beats.size() >= 140); // the tracker leaves out weak beats at the ends (the quiet intro and outro)
    // Every detected beat of the verses and choruses (8 .. 72 s) within 20 ms of a true one; in the sparse intro and
    // outro (pads and a kick on 1 and 3) the tracker puts the off-beats on what attack it finds, within 70 ms.
    const double beat = song().beatSeconds();
    double worst = 0.0;
    double worstEnds = 0.0;
    for (const Ticks t : a.beats) {
        const double s = t.toSeconds();
        const double error = std::fabs(s - std::round(s / beat) * beat);
        if (s >= 8.0 && s <= 72.0) {
            worst = std::max(worst, error);
        } else {
            worstEnds = std::max(worstEnds, error);
        }
    }
    CHECK(worst < 0.02);
    CHECK(worstEnds < 0.07);
    REQUIRE_FALSE(a.downbeats.empty());
    // Downbeats on bar lines (every 2 s).
    for (const Ticks d : a.downbeats) {
        const double s = d.toSeconds();
        CHECK(std::fabs(s - std::round(s / 2.0) * 2.0) < 0.02);
    }
    CHECK(a.features.size() + 1 == a.beats.size());
}

TEST_CASE("Remix plans land within a beat of the target, join on beats, and keep the intro and the outro",
          "[audio][edit][remix]") {
    const MusicAnalysis& a = songAnalysis();
    const double beat = song().beatSeconds();
    const TimeRange whole{Ticks{}, secs(song().seconds())};
    for (const double target : {30.0, 45.0, 60.0, 79.0, 100.0, 150.0, 300.0}) {
        INFO("target " << target << " s");
        RemixOptions options;
        options.target = secs(target);
        auto plan = planRemix(a, whole, options);
        REQUIRE(plan);
        CHECK(std::fabs(plan->duration.toSeconds() - target) <= beat);
        // Pieces back to back from the song's start to its end.
        REQUIRE_FALSE(plan->segments.empty());
        CHECK(plan->segments.front().sourceStart == whole.start);
        CHECK(plan->segments.back().sourceEnd == whole.end());
        std::int64_t total = 0;
        for (const RemixSegment& s : plan->segments) {
            CHECK(s.sourceEnd > s.sourceStart);
            total += s.duration().value;
        }
        CHECK(total == plan->duration.value);
        CHECK(plan->joins.size() + 1 == plan->segments.size());
        // The intro (4 bars, 8 s) and the outro (the last 4 bars, from 72 s) are found and kept.
        CHECK(plan->introEnd.toSeconds() == Approx(8.0).margin(0.05));
        CHECK(plan->outroStart.toSeconds() == Approx(72.0).margin(0.05));
        if (!plan->joins.empty()) {
            CHECK(plan->segments.front().sourceEnd + plan->crossfade >= plan->introEnd);
            CHECK(plan->segments.back().sourceStart <= plan->outroStart);
        }
        // Joins: from and to detected beats, on the bar grid but for at most one (a target that is not a whole number of
        // bars away needs one jump of a beat or two inside the bar).
        int offBar = 0;
        for (const RemixJoin& j : plan->joins) {
            CHECK(std::ranges::find(a.beats, j.fromBeat) != a.beats.end());
            CHECK(std::ranges::find(a.beats, j.toBeat) != a.beats.end());
            CHECK(j.fromBeat >= plan->introEnd);
            CHECK(j.toBeat >= plan->introEnd);
            CHECK(j.fromBeat <= plan->outroStart);
            CHECK(j.toBeat <= plan->outroStart);
            offBar += j.onBar ? 0 : 1;
            CHECK(j.confidence >= 0.0);
            CHECK(j.confidence <= 1.0);
        }
        CHECK(offBar <= 1);
        // A cut sits just before the beat it lands on (the crossfade ends on the attack), on the sample grid.
        for (std::size_t k = 0; k < plan->joins.size(); ++k) {
            const std::int64_t cut = plan->segments[k + 1].sourceStart.value;
            CHECK(std::llabs(cut - (plan->joins[k].toBeat.value - plan->crossfade.value / 2)) <= kRemixCutGrid / 2);
            CHECK(cut % kRemixCutGrid == 0);
            CHECK(plan->segments[k].sourceEnd.value % kRemixCutGrid == 0);
        }
    }
    // Where the song repeats itself the joins are seamless: verse + chorus is 32 s, and half a section (8 s, a turn of
    // the four-bar progression) jumps between the same music too. These targets need only such joins.
    for (const double target : {48.0, 112.0, 64.0, 144.0}) {
        INFO("seamless target " << target << " s");
        RemixOptions options;
        options.target = secs(target);
        auto plan = planRemix(a, whole, options);
        REQUIRE(plan);
        REQUIRE_FALSE(plan->joins.empty());
        CHECK(std::fabs(plan->duration.toSeconds() - target) <= beat);
        for (const RemixJoin& j : plan->joins) {
            CHECK(j.onBar);
            CHECK(j.confidence > 0.8);
        }
    }
    // Already the length (within half a beat): one piece, no join.
    RemixOptions same;
    same.target = secs(song().seconds() + 0.2);
    auto plan = planRemix(a, whole, same);
    REQUIRE(plan);
    CHECK(plan->segments.size() == 1);
    CHECK(plan->joins.empty());
}

TEST_CASE("A remix has no clicks at its joins", "[audio][edit][remix]") {
    const MusicAnalysis& a = songAnalysis();
    const SyntheticSong& s = song();
    std::vector<double> sourceCentres;
    const std::vector<double> sourceFlux = flux(s.samples, s.rate, &sourceCentres);
    for (const double target : {45.0, 150.0}) {
        RemixOptions options;
        options.target = secs(target);
        auto plan = planRemix(a, TimeRange{Ticks{}, secs(s.seconds())}, options);
        REQUIRE(plan);
        REQUIRE_FALSE(plan->joins.empty());
        const std::vector<float> out = renderRemix(*plan, s.samples, 1, s.rate);
        CHECK(static_cast<double>(out.size()) / s.rate == Approx(plan->duration.toSeconds()).margin(0.001));
        std::vector<double> centres;
        const std::vector<double> f = flux(out, s.rate, &centres);
        for (const RemixJoin& j : plan->joins) {
            // Around the join the remix's flux is the landing beat's own onset, no more: a click would add a broadband
            // jump on top of it.
            const double atJoin = peakFlux(f, centres, j.at.toSeconds(), 0.03);
            const double atBeat = peakFlux(sourceFlux, sourceCentres, j.toBeat.toSeconds(), 0.03);
            INFO("join at " << j.at.toSeconds() << " s, from " << j.fromBeat.toSeconds() << " to " << j.toBeat.toSeconds());
            CHECK(atJoin <= 1.25 * atBeat);
        }
    }
}

TEST_CASE("Remix keeps to a clip's part of the song and refuses targets it cannot reach", "[audio][edit][remix]") {
    const MusicAnalysis& a = songAnalysis();
    // The clip plays 10 s .. 70 s of the song.
    const TimeRange part{secs(10.0), secs(60.0)};
    RemixOptions options;
    options.target = secs(40.0);
    auto plan = planRemix(a, part, options);
    REQUIRE(plan);
    CHECK(plan->segments.front().sourceStart == part.start);
    CHECK(plan->segments.back().sourceEnd == part.end());
    for (const RemixSegment& s : plan->segments) {
        CHECK(s.sourceStart >= part.start);
        CHECK(s.sourceEnd <= part.end());
    }
    CHECK(std::fabs(plan->duration.toSeconds() - 40.0) <= song().beatSeconds());
    // Shorter than the intro and the outro together.
    options.target = secs(10.0);
    auto tooShort = planRemix(a, TimeRange{Ticks{}, secs(song().seconds())}, options);
    REQUIRE_FALSE(tooShort);
    CHECK(tooShort.error().code == ErrorCode::InvalidArgument);
    // Without keeping them it can.
    options.keepIntro = false;
    options.keepOutro = false;
    auto cut = planRemix(a, TimeRange{Ticks{}, secs(song().seconds())}, options);
    REQUIRE(cut);
    CHECK(std::fabs(cut->duration.toSeconds() - 10.0) <= song().beatSeconds());
    // No beats: nothing to remix.
    MusicAnalysis none;
    CHECK_FALSE(planRemix(none, part, options));
}

TEST_CASE("Remix analysis of a five-minute track", "[audio][edit][remix][perf]") {
    // 150 bars at 120 BPM: 5 minutes, the verse and chorus repeated.
    std::string sections = "IIII";
    while (sections.size() < 146) {
        sections += "AAAAAAAABBBBBBBB";
    }
    sections.resize(146);
    sections += "OOOO";
    const SyntheticSong long5 = synthesizeSong(sections);
    REQUIRE(long5.seconds() == Approx(300.0).margin(0.01));
    const auto t0 = std::chrono::steady_clock::now();
    const ai::analysis::MonoAudio audio{long5.samples, long5.rate, Ticks{}};
    auto beats = ai::analysis::detectBeats(audio);
    REQUIRE(beats);
    const auto t1 = std::chrono::steady_clock::now();
    MusicAnalysis a;
    a.bpm = beats->bpm;
    a.beatsPerBar = beats->beatsPerBar;
    a.beats = beats->beats;
    a.downbeats = beats->downbeats;
    a.features = beatFeatures(long5.samples, long5.rate, a.beats);
    const auto t2 = std::chrono::steady_clock::now();
    RemixOptions options;
    options.target = secs(120.0);
    auto plan = planRemix(a, TimeRange{Ticks{}, secs(long5.seconds())}, options);
    const auto t3 = std::chrono::steady_clock::now();
    REQUIRE(plan);
    CHECK(std::fabs(plan->duration.toSeconds() - 120.0) <= long5.beatSeconds());
    const double beatsSec = std::chrono::duration<double>(t1 - t0).count();
    const double featuresSec = std::chrono::duration<double>(t2 - t1).count();
    const double planSec = std::chrono::duration<double>(t3 - t2).count();
    WARN("5-minute track: beats " << beatsSec << " s, beat features " << featuresSec << " s, plan (to 2:00) " << planSec
                                  << " s; total " << beatsSec + featuresSec + planSec << " s (" << a.beats.size() << " beats, "
                                  << plan->joins.size() << " joins)");
    CHECK(beatsSec + featuresSec + planSec < 30.0);
}
