// A.8 voice-over recording: the recorder's pass (count-in, pre-roll, punch-in/out, post-roll), sample-accurate
// placement with known output and input latencies, the loopback latency measurement and its calibration, input
// monitoring, and a real input device ([audio-device]).

#include "VoiceOverBooth.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// A sawtooth with a distinct value on every sample over its period (alignment checks).
SyntheticSignal ramp() {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 0.1; // a 10 s period
    s.amplitude = 0.5f;
    return s;
}

/// The program at timeline frame k: track A1's clip of media 'A' (0 dB, centre), channel 0.
float program(const Project& p, std::int64_t k) {
    if (k < 0) {
        return 0.0f;
    }
    return p.feed->sample(p.mediaId('A'), 0, k, 0, k48k) * audio::stereoBalance(0.0f).left;
}

/// Samples of `take` that differ from the program placed `shift` frames later than `startFrame`.
std::size_t mismatches(const Project& p, const std::vector<float>& take, std::int64_t startFrame, std::int64_t shift = 0) {
    std::size_t bad = 0;
    for (std::size_t i = 0; i < take.size(); ++i) {
        if (take[i] != program(p, startFrame + static_cast<std::int64_t>(i) - shift)) {
            ++bad;
        }
    }
    return bad;
}

Project programProject() {
    Project p;
    p.media('A', ramp());
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(60), Ticks{}}});
    return p;
}

VoiceOverOptions passOptions(const std::filesystem::path& file, Ticks in, std::optional<Ticks> out) {
    VoiceOverOptions o;
    o.punchIn = in;
    o.punchOut = out;
    o.preroll = ms(500);
    o.postroll = ms(250);
    o.countIn = 1;
    o.file = file;
    o.bitsPerSample = 32; // float: the take compares bit for bit
    return o;
}

} // namespace

TEST_CASE("A voice-over take lands where it was heard, to the sample: output and input latency compensated", "[playback][voiceover]") {
    Project p = programProject();
    Booth booth(p, {.outputLatency = 480, .reportedInputLatency = 240, .actualInputLatency = 240});
    const auto dir = voiceOverScratch("placement");
    const VoiceOverOptions options = passOptions(dir / "take.wav", Ticks::fromSeconds(2), Ticks::fromSeconds(3));
    const auto original = booth.rig.engine.source();
    REQUIRE(booth.recorder->start(options));
    CHECK(booth.rig.engine.source() != original); // the pass's source wraps the player's
    CHECK(booth.recorder->status().state == VoiceOverState::CountIn);
    CHECK_FALSE(booth.recorder->start(options)); // one pass at a time

    booth.run(100);
    CHECK(booth.recorder->status().state == VoiceOverState::CountIn);
    CHECK(booth.recorder->status().countInRemaining == 1);
    REQUIRE(booth.runUntil(VoiceOverState::Preroll, 2.0));
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 1.0));
    REQUIRE(booth.runUntil(VoiceOverState::Postroll, 1.5));
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 1.5));
    CHECK(booth.rig.engine.isPaused()); // the transport stopped after the post-roll

    const auto result = booth.recorder->stop();
    REQUIRE(result);
    CHECK(booth.rig.engine.source() == original);
    CHECK(booth.recorder->status().state == VoiceOverState::Idle);
    // Placement: the take starts at the punch-in and is the punch range long, to the sample.
    CHECK(result->start == Ticks::fromSeconds(2));
    CHECK(result->frames == 48000);
    CHECK(result->duration == Ticks::fromSeconds(1));
    CHECK(result->punchedOut);
    CHECK(result->sampleRate == 48000);
    CHECK(result->channels == 1);
    CHECK(result->droppedFrames == 0);
    CHECK(result->device == "Null input");
    CHECK(result->latencyCompensation == samplesToTicks(240, k48k));
    // The microphone heard the speakers: the take is the program at its place, bit for bit (0 samples off).
    const std::vector<float> take = wavSamples(result->file);
    REQUIRE(take.size() == 48000);
    CHECK(mismatches(p, take, 96000) == 0);

    // The count-in: one beep on the count's second (the program muted), then the pre-roll plays the program.
    const Ticks countStart = Ticks::fromSeconds(2) - ms(500) - Ticks::fromSeconds(1);
    const std::int64_t beepAt = booth.frameOf(countStart);
    REQUIRE(beepAt >= 0);
    float beepPeak = 0.0f;
    for (std::int64_t f = beepAt; f < beepAt + 4800; ++f) {
        beepPeak = std::max(beepPeak, std::fabs(booth.rig.stream[static_cast<std::size_t>(f) * 2]));
    }
    CHECK(beepPeak == Approx(0.25f).margin(0.01f));
    bool quiet = true;
    for (std::int64_t f = beepAt + 7200; f < beepAt + 48000; ++f) {
        quiet = quiet && booth.rig.stream[static_cast<std::size_t>(f) * 2] == 0.0f;
    }
    CHECK(quiet); // the program is muted until the pre-roll
    const std::int64_t prerollAt = booth.frameOf(Ticks::fromSeconds(2) - ms(500));
    REQUIRE(prerollAt >= 0);
    CHECK(booth.rig.stream[static_cast<std::size_t>(prerollAt + 100) * 2] == program(p, 72000 + 100));
}

TEST_CASE("The loopback measurement finds input latency the device did not report; the calibration removes it",
          "[playback][voiceover][latency]") {
    Project p = programProject();
    // The input says 5 ms (240 frames) but takes 37 frames more.
    Booth booth(p, {.outputLatency = 480, .reportedInputLatency = 240, .actualInputLatency = 277});
    const auto dir = voiceOverScratch("loopback");

    // 1. The loopback pass: clicks every 250 ms instead of the program, measured where they came back.
    VoiceOverOptions loop = passOptions(dir / "loopback.wav", Ticks::fromSeconds(1), Ticks::fromSeconds(3));
    loop.loopback = true;
    loop.countIn = 0;
    loop.preroll = Ticks{};
    loop.postroll = Ticks{};
    REQUIRE(booth.recorder->start(loop));
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 5.0));
    const auto measured = booth.recorder->stop();
    REQUIRE(measured);
    REQUIRE(measured->loopback);
    const LoopbackResult& lb = *measured->loopback;
    CHECK(lb.clicksFound == 7); // 1.25 s .. 2.75 s
    CHECK(lb.residual == samplesToTicks(37, k48k));
    CHECK(lb.reportedOutput == samplesToTicks(480, k48k));
    CHECK(lb.reportedInput == samplesToTicks(240, k48k));
    CHECK(lb.roundTrip == samplesToTicks(480 + 240 + 37, k48k));

    // 2. Without the calibration a take is 37 samples late: what was heard at t lands at t + 37 samples.
    REQUIRE(booth.recorder->start(passOptions(dir / "uncalibrated.wav", Ticks::fromSeconds(5), Ticks::fromSeconds(6))));
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 4.0));
    const auto late = booth.recorder->stop();
    REQUIRE(late);
    const std::vector<float> lateTake = wavSamples(late->file);
    REQUIRE(late->start == Ticks::fromSeconds(5));
    CHECK(mismatches(p, lateTake, 240000, 37) == 0);
    CHECK(mismatches(p, lateTake, 240000) > 47000);

    // 3. With it, the take is exact again.
    VoiceOverOptions calibrated = passOptions(dir / "calibrated.wav", Ticks::fromSeconds(8), Ticks::fromSeconds(9));
    calibrated.calibration = lb.residual;
    REQUIRE(booth.recorder->start(calibrated));
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 4.0));
    const auto exact = booth.recorder->stop();
    REQUIRE(exact);
    CHECK(exact->start == Ticks::fromSeconds(8));
    CHECK(exact->latencyCompensation == samplesToTicks(277, k48k));
    const std::vector<float> exactTake = wavSamples(exact->file);
    REQUIRE(exactTake.size() == 48000);
    CHECK(mismatches(p, exactTake, 384000) == 0);
}

TEST_CASE("Punch-in at the playhead with a manual stop; a pass stopped before the punch-in records nothing", "[playback][voiceover]") {
    Project p = programProject();
    Booth booth(p, {.outputLatency = 256, .reportedInputLatency = 128, .actualInputLatency = 128});
    const auto dir = voiceOverScratch("manual");

    // No Out: the take runs from the punch-in until stop().
    REQUIRE(booth.recorder->start(passOptions(dir / "manual.wav", Ticks::fromSeconds(4), std::nullopt)));
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(1200);
    CHECK(booth.recorder->status().recorded > ms(1000));
    const auto result = booth.recorder->stop();
    REQUIRE(result);
    CHECK_FALSE(result->punchedOut);
    CHECK(result->start == Ticks::fromSeconds(4));
    CHECK(result->duration > ms(1000));
    CHECK(result->duration < ms(1300));
    const std::vector<float> take = wavSamples(result->file);
    CHECK(mismatches(p, take, 4 * 48000) == 0);
    CHECK(booth.rig.engine.isPaused());

    // Stopped during the count-in: nothing recorded, the empty file removed.
    const auto early = dir / "early.wav";
    REQUIRE(booth.recorder->start(passOptions(early, Ticks::fromSeconds(10), Ticks::fromSeconds(11))));
    booth.run(300);
    const auto none = booth.recorder->stop();
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(early));

    // cancel() ends a pass and deletes its file.
    const auto cancelled = dir / "cancelled.wav";
    REQUIRE(booth.recorder->start(passOptions(cancelled, Ticks::fromSeconds(12), Ticks::fromSeconds(13))));
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(200);
    booth.recorder->cancel();
    CHECK_FALSE(std::filesystem::exists(cancelled));
    CHECK(booth.recorder->status().state == VoiceOverState::Idle);
    CHECK_FALSE(booth.recorder->stop());

    // Refused passes.
    CHECK(booth.recorder->start(passOptions(dir / "x.wav", Ticks::fromSeconds(3), Ticks::fromSeconds(3))).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(booth.recorder->start(passOptions(dir / "no-such-folder" / "x.wav", Ticks::fromSeconds(3), std::nullopt)).error().code ==
          ErrorCode::IoError);
    booth.input.stop();
    CHECK(booth.recorder->start(passOptions(dir / "y.wav", Ticks::fromSeconds(3), std::nullopt)).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("A punch-in at the start of the sequence counts in before 0", "[playback][voiceover]") {
    Project p = programProject();
    Booth booth(p, {.outputLatency = 480, .reportedInputLatency = 240, .actualInputLatency = 240});
    const auto dir = voiceOverScratch("at-zero");
    REQUIRE(booth.recorder->start(passOptions(dir / "zero.wav", Ticks{}, ms(500))));
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 5.0));
    const auto result = booth.recorder->stop();
    REQUIRE(result);
    CHECK(result->start == Ticks{});
    CHECK(result->frames == 24000);
    CHECK(mismatches(p, wavSamples(result->file), 0) == 0);
}

TEST_CASE("Input monitoring plays the newest input through the output, with a bounded and reported latency",
          "[playback][voiceover][monitor]") {
    Project p; // nothing on the timeline: the output is the monitor alone
    p.media('A', ramp());
    p.addTrack({});
    Booth booth(p, {.outputLatency = 480,
                    .reportedInputLatency = 240,
                    .actualInputLatency = 240,
                    .speakersInRoom = false,
                    .channels = 2,
                    .voice = [](std::int64_t) { return 0.2f; }});
    const auto dir = voiceOverScratch("monitor");
    VoiceOverOptions options = passOptions(dir / "monitor.wav", Ticks::fromSeconds(1), Ticks::fromSeconds(2));
    options.monitor = true;
    options.countInBeep = false;
    options.monitorGainDb = -6.0;
    REQUIRE(booth.recorder->start(options));
    booth.run(200);
    const std::vector<float> block(booth.rig.stream.end() - 960, booth.rig.stream.end()); // the last block
    const float gain = std::pow(10.0f, -6.0f / 20.0f);
    CHECK(block[0] == Approx(0.2f * gain));
    CHECK(block[1] == Approx(0.2f * gain)); // the input on both channels
    // Input buffer + one period of backlog + the output latency.
    CHECK(booth.recorder->status().monitorLatency == samplesToTicks(240 + 256 + 480, k48k));
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 4.0));
    const auto result = booth.recorder->stop();
    REQUIRE(result);
    CHECK(result->channels == 2);
    CHECK(result->frames == 48000);
    const std::vector<float> take = wavSamples(result->file);
    REQUIRE(take.size() == 2 * 48000);
    CHECK(take.front() == 0.2f);
    CHECK(take.back() == 0.2f);
    // Monitoring ends with the pass.
    booth.rig.player->play();
    booth.run(50);
    CHECK(booth.rig.pull()[0] == 0.0f);
}

TEST_CASE("findClicks finds the first sample of each click, one per burst", "[playback][voiceover][latency]") {
    std::vector<float> s(48000, 0.001f);
    for (const std::size_t at : {std::size_t{1000}, std::size_t{13000}, std::size_t{25000}}) {
        s[at] = 0.9f;
        s[at + 1] = 0.9f;
        s[at + 2] = -0.5f;
    }
    CHECK(findClicks(s, 1, 48000) == std::vector<std::int64_t>{1000, 13000, 25000});
    std::vector<float> stereo(2 * 1000, 0.0f);
    stereo[2 * 500] = 0.5f;
    stereo[2 * 600 + 1] = 0.9f; // channel 1 is not looked at
    CHECK(findClicks(stereo, 2, 48000) == std::vector<std::int64_t>{500});
}

TEST_CASE("A real input device records through the recorder's capture path", "[audio-device][voiceover]") {
    auto devices = audio::AudioInput::enumerateInputDevices();
    if (!devices || devices->empty()) {
        SKIP("no audio input device");
    }
    audio::AudioInput input;
    if (auto started = input.start(); !started) {
        SKIP("the input could not be opened: " << started.error().message);
    }
    // Bounded: at most 2 s for the first second of audio.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::vector<float> buffer(4096);
    std::int64_t frames = 0;
    while (std::chrono::steady_clock::now() < deadline && frames < 48000) {
        frames += static_cast<std::int64_t>(input.read(buffer));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(frames > 0);
    CHECK(input.stats().callbacks > 0);
    CHECK(input.captureLine().valid);
    CHECK_FALSE(input.deviceName().empty());
    CHECK(input.latency().bufferFrames > 0);
    const audio::InputLevels levels = input.levels();
    CHECK(std::isfinite(levels.rms[0]));
    WARN("input '" << input.deviceName() << "' (" << input.backendName() << "): " << input.latency().bufferFrames
                   << " frames of input latency reported, " << frames << " frames captured");
    input.stop();
}
