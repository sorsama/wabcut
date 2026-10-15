// Loudness against the EBU test recipes, synthesised here (the EBU's own files are not under an open licence):
// EBU Tech 3341 (integrated loudness, gating, true peak) and Tech 3342 (loudness range), 48 kHz stereo 1 kHz tones
// whose levels are given per channel in dBFS (sine peak). Tolerances are the specifications': +-0.1 LU for loudness,
// +-1 LU for LRA, +0.2/-0.4 dB for true peak.

#include "DspTestUtil.h"

#include <wabcut/audio/LoudnessTap.h>
#include <wabcut/audio/dsp/Loudness.h>

#include <ebur128.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

/// Tones of 1 kHz at the given (dBFS, seconds) levels, back to back.
std::vector<float> program(const std::vector<std::pair<double, double>>& parts, int channels = 2) {
    std::vector<float> out;
    for (const auto& [db, seconds] : parts) {
        append(out, sine(1000.0, dbToAmp(db), seconds, channels));
    }
    return out;
}

dsp::LoudnessResult measure(const std::vector<float>& x, int channels = 2) {
    dsp::LoudnessMeter meter(channels, 48000);
    REQUIRE(meter.valid());
    // Feed in uneven pieces: the result must not depend on how the audio is handed over.
    std::size_t done = 0;
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    std::size_t piece = 1;
    while (done < frames) {
        const std::size_t n = std::min(piece, frames - done);
        meter.add(x.data() + done * static_cast<std::size_t>(channels), n);
        done += n;
        piece = piece * 3 % 7919 + 1;
    }
    return meter.result();
}

} // namespace

TEST_CASE("Integrated loudness: EBU Tech 3341 cases 1-4", "[audio][dsp][loudness]") {
    SECTION("case 1: -23 dBFS stereo tone is -23.0 LUFS") {
        CHECK(measure(program({{-23.0, 20.0}})).integratedLufs == Approx(-23.0).margin(0.1));
    }
    SECTION("case 2: -33 dBFS is -33.0 LUFS") {
        CHECK(measure(program({{-33.0, 20.0}})).integratedLufs == Approx(-33.0).margin(0.1));
    }
    SECTION("case 3: the relative gate drops the -36 dBFS parts") {
        CHECK(measure(program({{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}})).integratedLufs ==
              Approx(-23.0).margin(0.1));
    }
    SECTION("case 4: the absolute gate drops the -72 dBFS parts") {
        CHECK(measure(program({{-72.0, 10.0}, {-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}, {-72.0, 10.0}}))
                  .integratedLufs == Approx(-23.0).margin(0.1));
    }
    SECTION("a mono channel counts as dual mono") {
        CHECK(measure(program({{-23.0, 20.0}}, 1), 1).integratedLufs == Approx(-23.0).margin(0.1));
    }
    SECTION("silence has no integrated loudness and needs no gain") {
        const std::vector<float> silence(48000 * 2 * 5, 0.0f);
        const auto r = measure(silence);
        CHECK_FALSE(r.hasIntegrated());
        CHECK(dsp::normalizationGainDb(r, -14.0) == 0.0);
    }
}

TEST_CASE("Loudness range: EBU Tech 3342 cases 1-3", "[audio][dsp][loudness]") {
    CHECK(measure(program({{-20.0, 20.0}, {-30.0, 20.0}})).rangeLu == Approx(10.0).margin(1.0));
    CHECK(measure(program({{-20.0, 20.0}, {-15.0, 20.0}})).rangeLu == Approx(5.0).margin(1.0));
    CHECK(measure(program({{-40.0, 20.0}, {-20.0, 20.0}})).rangeLu == Approx(20.0).margin(1.0));
}

TEST_CASE("True peak finds inter-sample peaks the samples miss", "[audio][dsp][loudness]") {
    // fs/4 at 45 degrees: every sample is at 0.707 of the waveform's peak (-3 dB), the true peak is the amplitude.
    const auto x = sine(12000.0, dbToAmp(-6.0), 2.0, 2, 48000, std::numbers::pi / 4.0);
    const auto r = measure(x);
    CHECK(r.samplePeakDbfs == Approx(-9.01).margin(0.02));
    CHECK(r.truePeakDbtp >= -6.0 - 0.4);
    CHECK(r.truePeakDbtp <= -6.0 + 0.2);
}

TEST_CASE("Momentary and short-term maxima, and the normalisation gain", "[audio][dsp][loudness]") {
    const auto r = measure(program({{-30.0, 10.0}, {-18.0, 4.0}, {-30.0, 10.0}}));
    CHECK(r.maxMomentaryLufs == Approx(-18.0).margin(0.2));
    CHECK(r.maxShortTermLufs == Approx(-18.0).margin(0.3));
    CHECK(r.frames == 24 * 48000);
    const auto steady = measure(program({{-23.0, 20.0}}));
    CHECK(dsp::normalizationGainDb(steady, -14.0) == Approx(9.0).margin(0.1));
    CHECK(dsp::normalizationGainDb(steady, -16.0) == Approx(7.0).margin(0.1));
    CHECK(dsp::normalizationGainDb(steady, -23.0) == Approx(0.0).margin(0.1));
}

TEST_CASE("LoudnessTap: blocks pushed as an audio callback would are measured exactly as libebur128 offline",
          "[audio][dsp][loudness][tap]") {
    // A.3: EBU Tech 3341 case 1 and a programme of steps, pushed in callback-sized blocks, drained in manual mode.
    const std::vector<float> x = program({{-36.0, 3.0}, {-23.0, 4.0}, {-16.0, 2.0}, {-30.0, 3.0}});
    LoudnessTapOptions manual;
    manual.workerThread = false;
    LoudnessTap tap(2, 48000, manual);
    const std::size_t frames = x.size() / 2;
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(frames - done, 480 + done % 97);
        tap.push(std::span<const float>(x.data() + done * 2, n * 2));
        done += n;
        if (done % 4800 < 600) {
            tap.update(); // the worker's ~20 ms cadence, roughly
        }
    }
    tap.update();
    const LoudnessReading r = tap.reading();
    dsp::LoudnessMeter offline(2, 48000);
    offline.add(x.data(), frames);
    CHECK(r.frames == static_cast<std::int64_t>(frames));
    CHECK(r.droppedFrames == 0);
    CHECK(r.integratedLufs == Approx(offline.integratedLufs()).margin(0.1));
    CHECK(r.integratedLufs == Approx(measure(x).integratedLufs).margin(0.1));
    CHECK(r.momentaryLufs == Approx(-30.0).margin(0.1)); // the last 400 ms: the -30 dBFS tone
    CHECK(r.shortTermLufs == Approx(-30.0).margin(0.1));
    CHECK(r.maxMomentaryLufs == Approx(-16.0).margin(0.1));
    CHECK(r.truePeakDbtp == Approx(offline.truePeakDbtp()).margin(0.1));
    CHECK(r.truePeakDbtp == Approx(-16.0).margin(0.2)); // the loudest tone's peak (dbToAmp is the peak amplitude)

    // Reset: what is still queued is dropped; integration starts with the next audio.
    tap.push(std::span<const float>(x.data(), 4800 * 2));
    tap.reset();
    tap.update();
    CHECK(tap.reading().frames == 0);
    CHECK(tap.reading().resets == 1);
    const std::vector<float> loud = program({{-20.0, 2.0}});
    tap.push(loud);
    tap.update();
    CHECK(tap.reading().integratedLufs == Approx(-20.0).margin(0.1));
}

TEST_CASE("LoudnessTap: a full ring drops and counts; disabled pushes nothing; the worker publishes on its own",
          "[audio][dsp][loudness][tap]") {
    LoudnessTapOptions manual;
    manual.workerThread = false;
    manual.ringLength = std::chrono::milliseconds(100); // 4800 frames -> an 8192-frame ring
    LoudnessTap small(2, 48000, manual);
    const std::vector<float> second = program({{-23.0, 1.0}});
    small.push(second); // 48000 frames into 8192
    small.update();
    const LoudnessReading r = small.reading();
    CHECK(r.frames == 8192);
    CHECK(r.droppedFrames == 48000 - 8192);
    small.setEnabled(false);
    small.push(second);
    small.update();
    CHECK(small.reading().frames == 8192);
    CHECK(small.reading().droppedFrames == r.droppedFrames);

    LoudnessTap threaded(2, 48000); // the worker drains every 20 ms
    for (int block = 0; block < 300; ++block) {
        threaded.push(std::span<const float>(second.data() + static_cast<std::size_t>(block % 90) * 480 * 2, 480 * 2));
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    for (int i = 0; i < 200 && threaded.reading().frames < 300 * 480; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(threaded.reading().frames == 300 * 480);
    CHECK(threaded.reading().integratedLufs == Approx(-23.0).margin(0.1));
}

TEST_CASE("5.1 loudness: libebur128's channel weights, the LFE ignored, surrounds +1.5 dB (A.9)", "[audio][dsp][loudness][channels]") {
    // L R C LFE Ls Rs at different levels and frequencies, 10 s.
    constexpr int kChannels = 6;
    constexpr std::size_t kFrames = 480000;
    const double levels[kChannels] = {-20.0, -23.0, -18.0, -10.0, -26.0, -29.0};
    const double hz[kChannels] = {997.0, 1500.0, 440.0, 60.0, 3000.0, 2200.0};
    std::vector<float> x(kFrames * kChannels);
    for (std::size_t f = 0; f < kFrames; ++f) {
        for (int c = 0; c < kChannels; ++c) {
            x[f * kChannels + static_cast<std::size_t>(c)] = static_cast<float>(
                dbToAmp(levels[c]) * std::sin(2.0 * 3.14159265358979323846 * hz[c] * static_cast<double>(f) / 48000.0));
        }
    }
    const dsp::LoudnessResult ours = measure(x, kChannels);
    ebur128_state* direct = ebur128_init(kChannels, 48000, EBUR128_MODE_I);
    REQUIRE(direct != nullptr);
    const int roles[kChannels] = {EBUR128_LEFT, EBUR128_RIGHT, EBUR128_CENTER, EBUR128_UNUSED, EBUR128_LEFT_SURROUND,
                                  EBUR128_RIGHT_SURROUND};
    for (int c = 0; c < kChannels; ++c) {
        ebur128_set_channel(direct, static_cast<unsigned>(c), roles[c]);
    }
    REQUIRE(ebur128_add_frames_float(direct, x.data(), kFrames) == EBUR128_SUCCESS);
    double reference = 0.0;
    REQUIRE(ebur128_loudness_global(direct, &reference) == EBUR128_SUCCESS);
    ebur128_destroy(&direct);
    CHECK(ours.integratedLufs == Approx(reference).margin(1e-9));
    // The LFE does not count.
    std::vector<float> noLfe = x;
    for (std::size_t f = 0; f < kFrames; ++f) {
        noLfe[f * kChannels + 3] = 0.0f;
    }
    CHECK(measure(noLfe, kChannels).integratedLufs == Approx(ours.integratedLufs).margin(1e-9));
    // A tone on a surround channel weighs 1.41 (+1.5 dB) against the same tone on L.
    std::vector<float> front(kFrames * kChannels, 0.0f);
    std::vector<float> rear(kFrames * kChannels, 0.0f);
    for (std::size_t f = 0; f < kFrames; ++f) {
        const auto v = static_cast<float>(dbToAmp(-20.0) * std::sin(2.0 * 3.14159265358979323846 * 997.0 * static_cast<double>(f) / 48000.0));
        front[f * kChannels + 0] = v;
        rear[f * kChannels + 4] = v;
    }
    CHECK(measure(rear, kChannels).integratedLufs - measure(front, kChannels).integratedLufs ==
          Approx(10.0 * std::log10(1.41)).margin(0.01));
}

