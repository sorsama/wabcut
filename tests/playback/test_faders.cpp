// A.3: the track fader and panner (Track::volume / Track::pan) after the track's effects and the master fader
// (Sequence::masterVolume) after the master effects, keyframable in timeline time on the parameter grid: preview ==
// export sample for sample with exact ramp values, the pan law, Premiere's mute/solo, live fader edits, and the live
// loudness tap of TimelineAudioPlayer against libebur128 on the same samples.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <wabcut/audio/dsp/Loudness.h>

#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal level(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

SyntheticSignal tone(double hz, float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}

SyntheticSignal noiseSignal(float amplitude, std::uint32_t seed) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Noise;
    s.amplitude = amplitude;
    s.seed = seed;
    return s;
}

/// Edits audio track `index` of the project in place.
template <typename F>
void editTrack(Project& p, std::size_t index, F change) {
    Track t = p.track(index);
    change(t);
    p.state = Project::withTrack(p.state, index, std::move(t));
}

void setMasterVolume(Project& p, ParamValue volume) {
    Sequence seq = *p.state.sequences.front();
    seq.masterVolume = std::move(volume);
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

ParamValue curve(std::vector<std::pair<Ticks, double>> keys) {
    Curve c;
    for (const auto& [t, v] : keys) {
        c.keys.push_back(Keyframe{t, v});
    }
    return ParamValue(std::move(c));
}

std::vector<float> exported(const Project& p, Ticks start, std::size_t frames, OfflineMixdownOptions options = {}) {
    options.renderer.edgeFade = Ticks{};
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(frames * 2);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(frames - done, 300 + done % 777); // uneven pieces
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

std::int64_t firstMismatch(const std::vector<float>& a, const std::vector<float>& b, std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        if (a[i] != b[i]) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

double gainOf(double db) { return std::pow(10.0, db / 20.0); }

std::size_t frameAt(Ticks t) { return static_cast<std::size_t>(t.value / kTicksPerFrame); }

} // namespace

TEST_CASE("Track fader, pan and master gain: preview == export sample for sample, ramps exact on the parameter grid",
          "[playback][faders][export]") {
    Project p;
    p.media('A', level(0.5f));
    p.media('B', tone(220.0, 0.4f));
    p.addTrack({{'A', Ticks{}, ms(2000), Ticks{}}});
    p.addTrack({{'B', Ticks{}, ms(2000), Ticks{}}});
    // A1: down 12 dB over 500 ms, panned towards the left over 1 s; A2: -6 dB, a little right; master: -3 dB over 1 s.
    editTrack(p, 0, [](Track& t) {
        t.volume = curve({{ms(0), 0.0}, {ms(500), -12.0}});
        t.pan = curve({{ms(0), 0.0}, {ms(1000), -0.5}});
    });
    editTrack(p, 1, [](Track& t) {
        t.volume = ParamValue(-6.0);
        t.pan = ParamValue(0.3);
    });
    setMasterVolume(p, curve({{ms(0), 0.0}, {ms(1000), -3.0}}));

    Rig rig(p);
    rig.player->play();
    rig.pullFor(2000);
    const auto offline = exported(p, Ticks{}, 2000 * 48);
    CHECK(firstMismatch(rig.stream, offline, offline.size()) == -1);
    CHECK(rig.player->stats().underrunFrames == 0);

    // Exact values with A2 removed: frame 12000 (250 ms) is a grid point (12000 = 375 x 32).
    Project solo;
    solo.media('A', level(0.5f));
    solo.addTrack({{'A', Ticks{}, ms(2000), Ticks{}}});
    editTrack(solo, 0, [&](Track& t) {
        t.volume = p.track(0).volume;
        t.pan = p.track(0).pan;
    });
    setMasterVolume(solo, p.sequenceRef().masterVolume);
    const auto alone = exported(solo, Ticks{}, 600 * 48);
    const std::size_t f = frameAt(ms(250));
    const double track = gainOf(-6.0);   // halfway down the 12 dB ramp
    const double master = gainOf(-0.75); // a quarter of the way down the 3 dB ramp
    const audio::StereoGains balance = audio::stereoBalance(-0.125f);
    CHECK(alone[f * 2] == Approx(0.5 * track * balance.left * master).epsilon(1e-6));
    CHECK(alone[f * 2 + 1] == Approx(0.5 * track * balance.right * master).epsilon(1e-6));
    // Past the last key the level holds; before the first it starts there.
    CHECK(alone[frameAt(ms(550)) * 2 + 1] ==
          Approx(0.5 * gainOf(-12.0) * audio::stereoBalance(-0.275f).right * gainOf(-0.75 * 550.0 / 250.0)).epsilon(1e-5));
    // Between grid points the gain is the linear blend of the grid points (sample-accurate ramp).
    const double g0 = alone[f * 2 + 1];
    const double g32 = alone[(f + 32) * 2 + 1];
    CHECK(alone[(f + 8) * 2 + 1] == Approx(g0 + (g32 - g0) * 0.25).epsilon(1e-5));
}

TEST_CASE("Track pan law: unity-centre balance on the constant-power taper", "[playback][faders][pan]") {
    for (const double pan : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
        INFO(pan);
        Project p;
        p.media('A', level(0.5f));
        p.addTrack({{'A', Ticks{}, ms(200), Ticks{}}});
        editTrack(p, 0, [&](Track& t) { t.pan = ParamValue(pan); });
        const double theta = (pan + 1.0) * std::numbers::pi / 4.0;
        const double left = std::min(1.0, std::numbers::sqrt2 * std::cos(theta));
        const double right = std::min(1.0, std::numbers::sqrt2 * std::sin(theta));
        const auto out = exported(p, Ticks{}, 100 * 48);
        CHECK(out[2000 * 2] == Approx(0.5 * left).margin(1e-6));
        CHECK(out[2000 * 2 + 1] == Approx(0.5 * right).margin(1e-6));
        Rig rig(p);
        rig.player->play();
        rig.pullFor(100);
        CHECK(firstMismatch(rig.stream, out, out.size()) == -1);
    }
    // Centre leaves the stereo mix untouched; a hard pan silences the far side.
    CHECK(audio::stereoBalance(0.0f).left == 1.0f);
    CHECK(audio::stereoBalance(-1.0f).right == 0.0f);
}

TEST_CASE("Mute and solo as in Premiere: mute and output off beat solo, no track is solo-safe", "[playback][faders][solo]") {
    struct Case {
        const char* name;
        bool mute[3];
        bool solo[3];
        bool enabled[3];
        float expected;
    };
    const Case cases[] = {
        {"nothing", {false, false, false}, {false, false, false}, {true, true, true}, 0.7f},
        {"A1 muted", {true, false, false}, {false, false, false}, {true, true, true}, 0.6f},
        {"A2 soloed", {false, false, false}, {false, true, false}, {true, true, true}, 0.2f},
        {"A2 and A3 soloed", {false, false, false}, {false, true, true}, {true, true, true}, 0.6f},
        {"A2 soloed and muted: mute wins, the others stay silent", {false, true, false}, {false, true, false}, {true, true, true}, 0.0f},
        {"A2 soloed, A3 muted", {false, false, true}, {false, true, false}, {true, true, true}, 0.2f},
        {"A2 output off", {false, false, false}, {false, false, false}, {true, false, true}, 0.5f},
        {"A1 soloed with its output off", {false, false, false}, {true, false, false}, {false, true, true}, 0.0f},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        Project p;
        p.media('A', level(0.1f));
        p.media('B', level(0.2f));
        p.media('C', level(0.4f));
        p.addTrack({{'A', Ticks{}, ms(300), Ticks{}}});
        p.addTrack({{'B', Ticks{}, ms(300), Ticks{}}});
        p.addTrack({{'C', Ticks{}, ms(300), Ticks{}}});
        for (std::size_t i = 0; i < 3; ++i) {
            editTrack(p, i, [&](Track& t) {
                t.muted = c.mute[i];
                t.solo = c.solo[i];
                t.enabled = c.enabled[i];
            });
        }
        const auto offline = exported(p, Ticks{}, 200 * 48);
        CHECK(offline[frameAt(ms(100)) * 2] == Approx(c.expected).margin(1e-6));
        Rig rig(p);
        rig.player->play();
        rig.pullFor(200);
        CHECK(rig.stream[frameAt(ms(100)) * 2] == Approx(c.expected).margin(1e-6));
    }
}

TEST_CASE("Fader edits while playing are heard from the next block: tracks and the master", "[playback][faders][live]") {
    Project p;
    p.media('A', level(0.5f));
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(300);
    const SequenceId seq = p.sequence;
    const TrackId a1 = p.track(0).id;
    auto track = edit::setParameter(*rig.session.snapshot(), seq, edit::EffectTarget::ofTrack(a1),
                                    edit::ParamRef::of(ParamId::VolumeDb), ParamValue(-12.0));
    REQUIRE(track);
    REQUIRE(rig.session.commit("fader", std::move(track->state)));
    rig.player->feedOnce();
    rig.pullFor(400);
    CHECK(rig.stream[frameAt(ms(250)) * 2] == 0.5f);                                    // before the edit
    CHECK(rig.stream[frameAt(ms(310)) * 2] == Approx(0.5 * gainOf(-12.0)).epsilon(1e-6)); // the next block
    CHECK(rig.stream[frameAt(ms(650)) * 2] == Approx(0.5 * gainOf(-12.0)).epsilon(1e-6));
    CHECK(rig.player->stats().underrunFrames == 0);
    CHECK(rig.player->stats().editFlushes == 0); // the callback applies the fader: nothing queued is rendered again

    const std::size_t before = rig.stream.size() / 2;
    auto master = edit::setParameter(*rig.session.snapshot(), seq, edit::EffectTarget::masterAudio(),
                                     edit::ParamRef::of(ParamId::VolumeDb), ParamValue(-6.0));
    REQUIRE(master);
    REQUIRE(rig.session.commit("master", std::move(master->state)));
    rig.player->feedOnce(); // publishes the master program
    rig.pullFor(20);
    // The master fader is a published program: the next block already has it.
    CHECK(rig.stream[(before + 480 + 10) * 2] == Approx(0.5 * gainOf(-12.0) * gainOf(-6.0)).epsilon(1e-6));
}

TEST_CASE("A live fader override is heard at once and ramps over 10 ms and clears (automation passes)",
          "[playback][faders][live]") {
    // TimelineAudioPlayer::setLiveTrackMix: Touch / Latch / Write passes hear the dragged fader at once; the model is
    // never touched (the UI writes the pass as keys when it ends).
    Project p;
    p.media('A', level(0.5f));
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(100);
    const TrackId a1 = p.track(0).id;
    const std::uint64_t revision = rig.session.revision();
    const auto nextFrame = [&] { return rig.stream.size() / 2; };

    // -12 dB: the very next block moves, a linear ramp of 480 frames (10 ms), never a step larger than the ramp's.
    REQUIRE(rig.player->setLiveTrackMix(a1, -12.0, std::nullopt));
    std::size_t at = nextFrame();
    rig.pullFor(30);
    const double target = 0.5 * gainOf(-12.0);
    const double step = (0.5 - target) / 480.0;
    CHECK(rig.stream[at * 2] < 0.5f);
    double worst = 0.0;
    bool monotonic = true;
    for (std::size_t k = 1; k < 480; ++k) {
        const double d = rig.stream[(at + k - 1) * 2] - rig.stream[(at + k) * 2];
        worst = std::max(worst, std::fabs(d));
        monotonic = monotonic && d >= 0.0;
    }
    CHECK(monotonic);
    CHECK(worst <= step * 1.01);
    CHECK(rig.stream[(at + 479) * 2] == Approx(target).epsilon(1e-5));
    CHECK(rig.stream[(at + 1000) * 2] == Approx(target).epsilon(1e-5));
    CHECK(rig.stream[(at + 1000) * 2 + 1] == Approx(target).epsilon(1e-5));

    // The panner: hard left silences the right side; the level stays.
    REQUIRE(rig.player->setLiveTrackMix(a1, -12.0, -1.0));
    at = nextFrame();
    rig.pullFor(30);
    CHECK(rig.stream[(at + 600) * 2] == Approx(target).epsilon(1e-5));
    CHECK(rig.stream[(at + 600) * 2 + 1] == Approx(0.0).margin(1e-6));

    // Cleared: back on the evaluated fader (0 dB, centre) after the ramp, exactly.
    REQUIRE(rig.player->setLiveTrackMix(a1, std::nullopt, std::nullopt));
    at = nextFrame();
    rig.pullFor(30);
    CHECK(rig.stream[(at + 240) * 2] < 0.5f); // on the way back
    CHECK(rig.stream[(at + 600) * 2] == 0.5f);
    CHECK(rig.stream[(at + 600) * 2 + 1] == 0.5f);

    // The master fader (TrackId{}).
    REQUIRE(rig.player->setLiveTrackMix(TrackId{}, -6.0, std::nullopt));
    at = nextFrame();
    rig.pullFor(30);
    CHECK(rig.stream[(at + 600) * 2] == Approx(0.5 * gainOf(-6.0)).epsilon(1e-5));

    // Stopping the transport ends the pass: the override is gone when playback resumes.
    REQUIRE(rig.player->setLiveTrackMix(a1, -20.0, std::nullopt));
    rig.player->pause();
    rig.pullFor(20);
    rig.player->play();
    at = nextFrame();
    rig.pullFor(30);
    CHECK(rig.stream[(at + 600) * 2] == 0.5f);

    // The model never changed; a track the player does not play is refused.
    CHECK(rig.session.revision() == revision);
    CHECK(rig.session.snapshot()->sequences.front()->audioTracks[0]->volume == ParamValue());
    CHECK_FALSE(rig.player->setLiveTrackMix(TrackId{424242}, -6.0, std::nullopt));
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("The live loudness tap measures what leaves the player exactly like libebur128 offline",
          "[playback][faders][loudness]") {
    Project p;
    p.media('A', noiseSignal(0.25f, 3));
    p.media('M', tone(440.0, 0.3f));
    p.addTrack({{'A', Ticks{}, ms(12000), Ticks{}}});
    p.addTrack({{'M', Ticks{}, ms(12000), Ticks{}}});
    editTrack(p, 1, [](Track& t) { t.volume = curve({{ms(2000), 0.0}, {ms(6000), -18.0}}); }); // the music ducks
    setMasterVolume(p, ParamValue(-2.0));
    Rig rig(p);
    rig.player->play();
    rig.pullFor(10000);
    const audio::LoudnessReading live = rig.player->loudness();
    REQUIRE(live.frames > 0);
    CHECK(live.droppedFrames == 0);
    // The same samples (the tap's frames are the tail of what the engine delivered) through libebur128.
    const std::size_t total = rig.stream.size() / 2;
    REQUIRE(static_cast<std::size_t>(live.frames) <= total);
    audio::dsp::LoudnessMeter offline(2, 48000);
    offline.add(rig.stream.data() + (total - static_cast<std::size_t>(live.frames)) * 2, static_cast<std::size_t>(live.frames));
    std::printf("[loudness] live tap vs libebur128 offline on %lld frames: integrated %.4f / %.4f LUFS, momentary %.4f / %.4f, "
                "short-term %.4f / %.4f, true peak %.4f / %.4f dBTP, dropped %llu\n",
                static_cast<long long>(live.frames), live.integratedLufs, offline.integratedLufs(), live.momentaryLufs,
                offline.momentaryLufs(), live.shortTermLufs, offline.shortTermLufs(), live.truePeakDbtp, offline.truePeakDbtp(),
                static_cast<unsigned long long>(live.droppedFrames));
    CHECK(live.integratedLufs == Approx(offline.integratedLufs()).margin(0.1));
    CHECK(live.momentaryLufs == Approx(offline.momentaryLufs()).margin(0.1));
    CHECK(live.shortTermLufs == Approx(offline.shortTermLufs()).margin(0.1));
    CHECK(live.truePeakDbtp == Approx(offline.truePeakDbtp()).margin(0.1));
    CHECK(live.maxMomentaryLufs == Approx(offline.maxMomentaryLufs()).margin(0.1));
    CHECK(std::isfinite(live.integratedLufs));

    // Reset: integration starts again from the next audio.
    rig.player->resetLoudness();
    rig.pullFor(2000);
    const audio::LoudnessReading after = rig.player->loudness();
    CHECK(after.resets == live.resets + 1);
    CHECK(after.frames <= 2000 * 48);
    CHECK(after.frames > 1900 * 48);
    audio::dsp::LoudnessMeter tail(2, 48000);
    const std::size_t now = rig.stream.size() / 2;
    tail.add(rig.stream.data() + (now - static_cast<std::size_t>(after.frames)) * 2, static_cast<std::size_t>(after.frames));
    CHECK(after.integratedLufs == Approx(tail.integratedLufs()).margin(0.1));

    // play() after a pause starts a new integration ("since play start").
    rig.player->pause();
    rig.pullFor(100);
    rig.player->play();
    rig.pullFor(500);
    CHECK(rig.player->loudness().resets == after.resets + 1);
    CHECK(rig.player->loudness().frames <= 500 * 48);

    // Off: nothing is measured.
    rig.player->setLoudnessEnabled(false);
    const auto frozen = rig.player->loudness().frames;
    rig.pullFor(300);
    CHECK(rig.player->loudness().frames == frozen);
}
