#include <wabcut/audio/Mixer.h>
#include <wabcut/audio/OfflineRenderer.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using Catch::Approx;

namespace {

constexpr int kRate = 48000;

std::vector<float> constantStereo(std::size_t frames, float left, float right) {
    std::vector<float> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        v[2 * i] = left;
        v[2 * i + 1] = right;
    }
    return v;
}

/// Mix one block of constant stereo inputs through a fresh mixer (first block: no gain ramps) and return the
/// first output frame.
StereoGains mixOnce(const std::vector<float>& levels, const std::function<void(Mixer&)>& setup) {
    Mixer mixer(static_cast<int>(levels.size()), 2, kRate);
    setup(mixer);
    std::vector<float> out(64 * 2, -99.0f);
    mixer.beginBlock(out);
    for (std::size_t b = 0; b < levels.size(); ++b) {
        const auto in = constantStereo(64, levels[b], levels[b]);
        mixer.addBus(static_cast<int>(b), in, 2);
    }
    mixer.endBlock();
    return {out[0], out[1]};
}

} // namespace

TEST_CASE("Mixer passes a centred 0 dB stereo bus through bit-exactly and applies fader gain", "[audio][mixer]") {
    Mixer mixer(1, 2, kRate);
    std::vector<float> in(256 * 2);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = std::sin(0.01f * static_cast<float>(i)) * 0.7f;
    }
    std::vector<float> out(in.size());
    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.endBlock();
    CHECK(out == in);

    Mixer quiet(1, 2, kRate);
    quiet.setBusGainDb(0, -6.0206f);
    quiet.beginBlock(out);
    quiet.addBus(0, in, 2);
    quiet.endBlock();
    for (std::size_t i = 0; i < in.size(); ++i) {
        CHECK(out[i] == Approx(in[i] * 0.5f).margin(1e-6));
    }
}

TEST_CASE("Mixer pans mono buses with the -3 dB constant-power law", "[audio][mixer][pan]") {
    auto panMono = [](float pan) {
        Mixer mixer(1, 2, kRate);
        mixer.setBusPan(0, pan);
        const std::vector<float> in(32, 0.5f);
        std::vector<float> out(64);
        mixer.beginBlock(out);
        mixer.addBus(0, in, 1);
        mixer.endBlock();
        return StereoGains{out[0], out[1]};
    };
    const StereoGains centre = panMono(0.0f);
    CHECK(centre.left == Approx(0.5f * 0.70710678f).epsilon(1e-6));
    CHECK(centre.right == Approx(0.5f * 0.70710678f).epsilon(1e-6));
    const StereoGains left = panMono(-1.0f);
    CHECK(left.left == Approx(0.5f));
    CHECK(left.right == Approx(0.0f).margin(1e-7));
    const StereoGains right = panMono(1.0f);
    CHECK(right.left == Approx(0.0f).margin(1e-7));
    CHECK(right.right == Approx(0.5f));
    const StereoGains half = panMono(0.5f);
    CHECK(half.left == Approx(0.5f * 0.38268343f).epsilon(1e-5));
    CHECK(half.right == Approx(0.5f * 0.92387953f).epsilon(1e-5));

    // Stereo buses use the unity-centre balance instead.
    const StereoGains balanced = mixOnce({0.5f}, [](Mixer& m) { m.setBusPan(0, -1.0f); });
    CHECK(balanced.left == Approx(0.5f));
    CHECK(balanced.right == Approx(0.0f).margin(1e-7));
}

TEST_CASE("Mixer sums buses and honours mute and solo", "[audio][mixer][solo]") {
    const std::vector<float> levels{0.1f, 0.2f, 0.4f};
    CHECK(mixOnce(levels, [](Mixer&) {}).left == Approx(0.7f));
    CHECK(mixOnce(levels, [](Mixer& m) { m.setBusMuted(1, true); }).left == Approx(0.5f));
    CHECK(mixOnce(levels, [](Mixer& m) { m.setBusSoloed(2, true); }).left == Approx(0.4f));
    CHECK(mixOnce(levels, [](Mixer& m) {
              m.setBusSoloed(0, true);
              m.setBusSoloed(2, true);
          }).left == Approx(0.5f));
    // Mute wins over solo: a soloed+muted bus is silent and still silences the others.
    CHECK(mixOnce(levels, [](Mixer& m) {
              m.setBusSoloed(1, true);
              m.setBusMuted(1, true);
          }).left == Approx(0.0f).margin(1e-9));
    CHECK(mixOnce(levels, [](Mixer& m) {
              m.setBusGainDb(0, kSilenceDb);
              m.setBusGainDb(2, -6.0206f);
          }).left == Approx(0.2f + 0.2f).epsilon(1e-5));
}

TEST_CASE("Mixer ramps gain and mute changes across one block", "[audio][mixer][ramp]") {
    Mixer mixer(1, 2, kRate);
    const auto in = constantStereo(100, 0.5f, 0.5f); // below the soft clip knee
    std::vector<float> out(200);

    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.endBlock();
    CHECK(out[0] == 0.5f);
    CHECK(out[199] == 0.5f);

    mixer.setBusGainDb(0, -6.0206f);
    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.endBlock();
    const float target = dbToLinear(-6.0206f);
    CHECK(out[0] == Approx(0.5f * (1.0f + (target - 1.0f) * 0.01f))); // first frame moves 1/100 of the way
    CHECK(out[2 * 49] == Approx(0.5f * (1.0f + (target - 1.0f) * 0.5f))); // halfway
    CHECK(out[2 * 99] == Approx(0.5f * target).epsilon(1e-7));            // lands exactly on the target

    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.endBlock();
    CHECK(out[0] == Approx(0.5f * target).epsilon(1e-7));
    CHECK(out[199] == Approx(0.5f * target).epsilon(1e-7));

    mixer.setBusMuted(0, true); // click-free mute: ramps down, then stays at zero
    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.endBlock();
    CHECK(out[0] > 0.0f);
    CHECK(out[2 * 99] == 0.0f);
    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.endBlock();
    CHECK(out[0] == 0.0f);
    CHECK(out[199] == 0.0f);
}

TEST_CASE("Master soft clip guard bounds the output and the master meter latches overs", "[audio][mixer][clip]") {
    Mixer mixer(2, 2, kRate);
    const auto in = constantStereo(64, 0.8f, -0.8f);
    std::vector<float> out(128);
    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.addBus(1, in, 2);
    mixer.endBlock();
    CHECK(out[0] == Approx(softClip(1.6f)));
    CHECK(out[1] == Approx(softClip(-1.6f)));
    CHECK(out[0] < 1.0f);
    CHECK(out[1] > -1.0f);
    CHECK(mixer.masterMeter().peak(0) == Approx(1.6f)); // metered before the guard
    CHECK(mixer.masterMeter().clipped());
    mixer.masterMeter().clearClip();
    CHECK_FALSE(mixer.masterMeter().clipped());

    mixer.setSoftClipEnabled(false);
    mixer.beginBlock(out);
    mixer.addBus(0, in, 2);
    mixer.addBus(1, in, 2);
    mixer.endBlock();
    CHECK(out[0] == Approx(1.6f));

    Mixer master(1, 2, kRate);
    master.setMasterGainDb(-6.0206f);
    const auto unit = constantStereo(64, 1.0f, 1.0f);
    master.beginBlock(out);
    master.addBus(0, unit, 2);
    master.endBlock();
    CHECK(out[0] == Approx(0.5f).epsilon(1e-5));
}

TEST_CASE("Bus meters read post-fader peak and RMS and fall back when silent", "[audio][mixer][meter]") {
    Mixer mixer(2, 2, kRate, 512);
    mixer.setBusGainDb(0, -6.0206f);
    auto tone = std::make_shared<SineSource>(1000.0, 0.8f);
    std::vector<float> in(512 * 2);
    std::vector<float> out(512 * 2);
    Ticks t{};
    for (int block = 0; block < 2 * kRate / 512; ++block) { // 2 s
        tone->render(in, 2, Rate{kRate, 1}, t);
        t += samplesToTicks(512, Rate{kRate, 1});
        mixer.beginBlock(out);
        mixer.addBus(0, in, 2);
        if (block == 0) {
            mixer.addBus(1, in, 2); // bus 1 only plays the first block
        }
        mixer.endBlock();
    }
    const LevelMeter& meter = mixer.busMeter(0);
    CHECK(meter.peak(0) == Approx(0.4f).epsilon(1e-3));
    CHECK(meter.peak(1) == Approx(0.4f).epsilon(1e-3));
    CHECK(meter.rms(0) == Approx(0.4f / std::sqrt(2.0f)).epsilon(0.01));
    CHECK_FALSE(meter.clipped());
    // Bus 1 played one 512-frame block, then ~2 s of silence: the peak fell ~26 dB from 0.8 and the RMS, which
    // only reached ~0.106 (0.8/sqrt(2) charged for 10.7 ms of a 300 ms window), decayed by e^(-2/0.3) to ~0.004.
    CHECK(mixer.busMeter(1).peak(0) < 0.8f * 0.06f);
    CHECK(mixer.busMeter(1).rms(0) == Approx(0.00384f).epsilon(0.02));
    // Out-of-range access is harmless.
    CHECK(mixer.busMeter(7).peak(0) == 0.0f);
    mixer.setBusGainDb(-1, 3.0f);
    CHECK(mixer.busGainDb(-1) == 0.0f);
}

TEST_CASE("MixerSource renders its inputs through the mixer", "[audio][mixer][graph]") {
    auto low = std::make_shared<SineSource>(440.0, 0.5f);
    auto high = std::make_shared<SineSource>(1000.0, 0.25f);
    auto mix = std::make_shared<MixerSource>(std::vector<MixerInput>{{low, 1}, {high, 2}}, 2, kRate, 256);
    mix->mixer().setBusPan(0, -1.0f); // mono 440 Hz hard left; stereo 1 kHz centred

    constexpr std::int64_t kFrames = 4800;
    auto mixed = OfflineRenderer::create(mix, 2, Rate{kRate, 1}, 1000)->render(kFrames);
    auto lowRef = OfflineRenderer::create(low, 1, Rate{kRate, 1}, 256)->render(kFrames);
    auto highRef = OfflineRenderer::create(high, 2, Rate{kRate, 1}, 256)->render(kFrames);
    REQUIRE(mixed);
    REQUIRE(lowRef);
    REQUIRE(highRef);
    double maxError = 0.0;
    for (std::int64_t f = 0; f < kFrames; ++f) {
        const auto i = static_cast<std::size_t>(f);
        const double expectedL = (*lowRef)[i] + (*highRef)[2 * i];
        const double expectedR = (*highRef)[2 * i + 1];
        maxError = std::max(maxError, std::fabs((*mixed)[2 * i] - expectedL));
        maxError = std::max(maxError, std::fabs((*mixed)[2 * i + 1] - expectedR));
    }
    CHECK(maxError < 1e-6);

    // Asking for another format renders silence instead of garbage.
    std::vector<float> wrong(64 * 6, 1.0f);
    mix->render(wrong, 6, Rate{kRate, 1}, Ticks{});
    CHECK(std::ranges::all_of(wrong, [](float x) { return x == 0.0f; }));
}
