// Clip, track and master audio effects through the player and through OfflineMixdown (export): preview == export
// sample for sample, latency compensation, sample-accurate keyframes, through edits, transitions, reversed clips and
// live edits.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <wabcut/audio/dsp/Loudness.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal tone(double hz, float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}
SyntheticSignal level(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}
SyntheticSignal noiseSignal(float amplitude, std::uint32_t seed) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Noise;
    s.amplitude = amplitude;
    s.seed = seed;
    return s;
}
SyntheticSignal ramp(float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 1.0;
    s.amplitude = amplitude;
    return s;
}

EffectInstance effect(ProjectState& state, std::string id, std::vector<EffectParam> params = {}) {
    EffectInstance e;
    e.id = state.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    for (EffectParam& param : params) {
        e.set(std::move(param.key), std::move(param.value));
    }
    return e;
}
EffectInstance effect(Project& p, std::string id, std::vector<EffectParam> params = {}) {
    return effect(p.state, std::move(id), std::move(params));
}

Clip clipWith(Project& p, const ClipSpec& spec, EffectStack effects = {}) {
    Clip c = p.makeClip(spec);
    if (!effects.empty()) {
        c.setEffects(std::move(effects));
    }
    return c;
}

void addTrack(Project& p, std::vector<Clip> clips, EffectStack effects = {}, std::vector<Transition> transitions = {}) {
    Track track;
    track.id = p.state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    for (Clip& c : clips) {
        track.clips.push_back(std::move(c));
    }
    track.effects = std::move(effects);
    track.transitions = std::move(transitions);
    Sequence seq = *p.state.sequences.front();
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

void setMaster(Project& p, EffectStack effects) {
    Sequence seq = *p.state.sequences.front();
    seq.audioEffects = std::move(effects);
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

/// The export path from `start`, `frames` frames.
std::vector<float> exported(const Project& p, Ticks start, std::size_t frames, Ticks edgeFade = {}) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = edgeFade;
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(frames * 2);
    // Uneven pieces: the result must not depend on them.
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(frames - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

/// Index of the first sample where the two differ (beyond `tolerance`), or -1.
std::int64_t firstMismatch(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b, std::size_t bFrom,
                           std::size_t samples, float tolerance = 0.0f) {
    for (std::size_t i = 0; i < samples; ++i) {
        if (std::fabs(a[aFrom + i] - b[bFrom + i]) > tolerance) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

/// A busy project: a dialogue track with EQ, compressor, limiter, Reduce Noise and an animated gain; a music track
/// with a volume fade and a track compressor; a master true-peak limiter.
void busyProject(Project& p) {
    p.media('A', noiseSignal(0.3f, 7));
    p.media('M', tone(220.0, 0.6f));
    addTrack(p,
             {clipWith(p, {'A', ms(100), ms(1500), ms(250)},
                       {effect(p, "wabcut.audio.eq3", {{"midGain", ParamValue(-6.0)}, {"lowGain", ParamValue(4.0)}}),
                        effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-24.0)}, {"ratio", ParamValue(4.0)}}),
                        effect(p, "wabcut.audio.gain",
                               {{"gain", ParamValue(Curve{{Keyframe{ms(400), 0.0}, Keyframe{ms(1200), -12.0}}})}}),
                        effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-3.0)}}),
                        effect(p, "wabcut.audio.denoise", {{"amount", ParamValue(0.7)}})})});
    Clip music = p.makeClip({'M', ms(0), ms(2500), ms(0), -3.0});
    ClipProperties props = music.properties();
    props[ParamId::VolumeDb] = ParamValue(Curve{{Keyframe{ms(0), -144.0}, Keyframe{ms(500), 0.0},
                                                 Keyframe{ms(2000), 0.0}, Keyframe{ms(2500), -40.0}}});
    music.setProperties(props);
    addTrack(p, {music}, {effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-12.0)}})});
    setMaster(p, {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-1.0)}})});
}

} // namespace

TEST_CASE("Preview and export render clip, track and master effects sample for sample alike",
          "[playback][effects][export]") {
    Project p;
    busyProject(p);
    SECTION("from the start") {
        Rig rig(p);
        rig.player->play();
        rig.pullFor(2600);
        const auto offline = exported(p, Ticks{}, 2600 * 48);
        CHECK(firstMismatch(rig.stream, 0, offline, 0, offline.size()) == -1);
        CHECK(rig.player->stats().underrunFrames == 0);
        CHECK(rig.player->stats().feedErrors == 0);
    }
    SECTION("from a seek point, effects settled on the same pre-roll") {
        Rig rig(p, Rig::Options{Ticks{}, Ticks{}, false, ms(50)});
        rig.player->play();
        rig.pullFor(100);
        rig.player->seek(ms(900));
        const auto before = rig.stream.size() / 2;
        rig.pullFor(1200);
        const auto offline = exported(p, ms(900), 1000 * 48);
        // The seek lands on the next block and the feeder refills after it; the block after that (480 frames on)
        // plays the timeline at 900 ms (P.1: the preroll ends once the tracks are refilled, before its 50 ms).
        CHECK(firstMismatch(rig.stream, (before + 480) * 2, offline, 0, offline.size()) == -1);
    }
}

TEST_CASE("Essential Sound's effects render alike in preview and export: filter, DeHum, DeEsser, expander, reverb, width",
          "[playback][effects][export][essential]") {
    // A.7: every new effect on a clip, a track or the master, some animated, with the record (data only) in the stack.
    Project p;
    p.media('A', noiseSignal(0.3f, 11));
    p.media('M', tone(330.0, 0.5f));
    EffectInstance record = effect(p, "wabcut.audio.essentialSound");
    record.setData(EffectData{"type", std::string("dialogue")});
    addTrack(p, {clipWith(p, {'A', ms(50), ms(1800), ms(100)},
                          {record, effect(p, "wabcut.audio.filter", {{"lowCut", ParamValue(120.0)}, {"highCut", ParamValue(6000.0)}}),
                           effect(p, "wabcut.audio.dehum", {{"frequency", ParamValue(60.0)}, {"reduction", ParamValue(24.0)}}),
                           effect(p, "wabcut.audio.expander", {{"threshold", ParamValue(-30.0)}}),
                           effect(p, "wabcut.audio.deess", {{"threshold", ParamValue(-40.0)}}),
                           effect(p, "wabcut.audio.reverb",
                                  {{"mix", ParamValue(Curve{{Keyframe{ms(200), 0.0}, Keyframe{ms(1400), 0.6}}})}, {"decay", ParamValue(900.0)}})})});
    addTrack(p, {p.makeClip({'M', ms(0), ms(2400), ms(0), -6.0})},
             {effect(p, "wabcut.audio.width", {{"width", ParamValue(Curve{{Keyframe{ms(0), 1.0}, Keyframe{ms(2000), 1.8}}})},
                                               {"bassMono", ParamValue(150.0)}}),
              effect(p, "wabcut.audio.reverb", {{"size", ParamValue(0.8)}, {"predelay", ParamValue(30.0)}})});
    setMaster(p, {effect(p, "wabcut.audio.filter", {{"lowCut", ParamValue(30.0)}, {"slope", ParamValue(48.0)}}),
                  effect(p, "wabcut.audio.width", {{"width", ParamValue(0.9)}})});
    SECTION("from the start") {
        Rig rig(p);
        rig.player->play();
        rig.pullFor(2500);
        const auto offline = exported(p, Ticks{}, 2500 * 48);
        CHECK(firstMismatch(rig.stream, 0, offline, 0, offline.size()) == -1);
        CHECK(rig.player->stats().underrunFrames == 0);
    }
    SECTION("from a seek point") {
        Rig rig(p, Rig::Options{Ticks{}, Ticks{}, false, ms(50)});
        rig.player->play();
        rig.pullFor(100);
        rig.player->seek(ms(700));
        const auto before = rig.stream.size() / 2;
        rig.pullFor(1200);
        const auto offline = exported(p, ms(700), 1000 * 48);
        CHECK(firstMismatch(rig.stream, (before + 480) * 2, offline, 0, offline.size()) == -1);
    }
}

TEST_CASE("Lookahead effects are latency-compensated: transparent settings change nothing", "[playback][effects]") {
    Project dry;
    dry.media('A', ramp(0.4f));
    addTrack(dry, {dry.makeClip({'A', ms(37), ms(900), ms(13)})});
    Project wet;
    wet.media('A', ramp(0.4f));
    // A limiter far above the signal, and Reduce Noise at amount 0 (the dry signal, delayed by its 30 ms latency).
    addTrack(wet, {clipWith(wet, {'A', ms(37), ms(900), ms(13)},
                            {effect(wet, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}, {"lookahead", ParamValue(20.0)}}),
                             effect(wet, "wabcut.audio.denoise", {{"amount", ParamValue(0.0)}})})},
             {effect(wet, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}})});
    setMaster(wet, {effect(wet, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}, {"lookahead", ParamValue(10.0)}})});
    const auto a = exported(dry, Ticks{}, 48000);
    const auto b = exported(wet, Ticks{}, 48000);
    CHECK(firstMismatch(a, 0, b, 0, a.size()) == -1);
    Rig rig(wet);
    rig.player->play();
    rig.pullFor(1000);
    CHECK(firstMismatch(a, 0, rig.stream, 0, a.size()) == -1);
}

TEST_CASE("Volume keyframes fade sample-accurately on the parameter grid", "[playback][effects][keyframes]") {
    Project p;
    p.media('A', level(0.5f));
    Clip c = p.makeClip({'A', ms(0), ms(2000), ms(0)});
    ClipProperties props = c.properties();
    // Keys in curve (media) time: a 60 dB fade-in over 1 s from 200 ms, then a hold step to -6 dB at 1.6 s.
    props[ParamId::VolumeDb] = ParamValue(Curve{{Keyframe{ms(200), -60.0}, Keyframe{ms(1200), 0.0, Interpolation::Hold},
                                                 Keyframe{ms(1600), -6.0}}});
    c.setProperties(props);
    const Curve curve = *props[ParamId::VolumeDb].curve();
    addTrack(p, {c});
    const auto out = exported(p, Ticks{}, 96000);
    double worst = 0.0;
    for (std::int64_t k = 0; k < 96000; ++k) {
        const double expected = 0.5 * std::pow(10.0, curve.valueAt(frames(k)) / 20.0);
        const double got = out[static_cast<std::size_t>(k) * 2];
        // Except inside the one 32-frame cell that holds the step, the gain follows the curve to within 0.05 dB.
        if (k >= 1600 * 48 - 32 && k < 1600 * 48 + 32) {
            continue;
        }
        worst = std::max(worst, std::fabs(20.0 * std::log10(got / expected)));
    }
    CHECK(worst < 0.05);
    // Exactly the key values where the keys sit on cell boundaries (200 ms = 9600 frames = 300 cells).
    CHECK(out[9600 * 2] == Approx(0.5 * std::pow(10.0, -3.0)).epsilon(1e-6));
    CHECK(out[57600 * 2] == Approx(0.5).epsilon(1e-6));
    CHECK(out[90000 * 2] == Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-6));
    // The step is spread over one cell (no click), not smeared further.
    CHECK(out[(76800 + 32) * 2] == Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-6));
    CHECK(out[(76800 - 33) * 2] == Approx(0.5).epsilon(1e-6));
}

TEST_CASE("A through edit keeps the effect state flowing; a real cut restarts it", "[playback][effects]") {
    auto build = [](Project& p, bool cut) {
        p.media('A', noiseSignal(0.5f, 3));
        const EffectStack fx{effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-30.0)}, {"release", ParamValue(400.0)}}),
                             effect(p, "wabcut.audio.eq3", {{"highGain", ParamValue(-9.0)}})};
        if (!cut) {
            addTrack(p, {clipWith(p, {'A', ms(0), ms(1000), ms(0)}, fx)});
            return;
        }
        Clip a = clipWith(p, {'A', ms(0), ms(1000), ms(0)}, fx);
        Clip b = a.splitOff(ms(400)); // a razor cut: both halves share one content block
        b.id = p.state.ids.allocate<ClipId>();
        addTrack(p, {a, b});
    };
    Project whole;
    build(whole, false);
    Project razored;
    build(razored, true);
    const auto x = exported(whole, Ticks{}, 48000, ms(2));
    const auto y = exported(razored, Ticks{}, 48000, ms(2));
    REQUIRE(razored.track(0).clips.size() == 2);
    REQUIRE(continuesSeamlessly(razored.track(0).clips[0], razored.track(0).clips[1]));
    CHECK(firstMismatch(x, 0, y, 0, x.size()) == -1);

    // Slip the second half by 50 ms: now it is a cut. Both sides fade (2 ms) and the second voice starts fresh.
    Project slipped;
    build(slipped, true);
    Track t = slipped.track(0);
    t.clips.mutableAt(1).sourceIn += ms(50);
    slipped.state = Project::withTrack(slipped.state, 0, t);
    const auto z = exported(slipped, Ticks{}, 48000, ms(2));
    CHECK(firstMismatch(x, 0, z, 0, 19200 * 2 - 96 * 2) == -1); // identical up to the fade-out
    CHECK(std::fabs(z[19199 * 2]) < std::fabs(x[19199 * 2]) / 50.0f + 1e-6f); // faded out at the cut
    CHECK(std::fabs(z[19200 * 2]) < 0.5f / 90.0f);                          // and fading in after it
}

TEST_CASE("Audio transitions crossfade with the transition's law", "[playback][effects][transitions]") {
    for (const std::string law : {"constantPower", "constantGain", "exponentialFade"}) {
        INFO(law);
        Project p;
        p.media('A', level(0.5f));
        p.media('B', level(0.25f));
        Clip a = p.makeClip({'A', ms(0), ms(500), ms(0)});
        Clip b = p.makeClip({'B', ms(500), ms(500), ms(1000)});
        Transition t;
        t.id = p.state.ids.allocate<TransitionId>();
        t.outgoing = a.id;
        t.incoming = b.id;
        t.before = ms(100);
        t.after = ms(100);
        t.effect = law;
        addTrack(p, {a, b}, {}, {t});
        const auto out = exported(p, Ticks{}, 48000, ms(2));
        for (const double at : {400.0, 450.0, 500.0, 550.0, 599.0}) {
            const auto k = static_cast<std::size_t>(at * 48.0);
            const double prog = (at - 400.0) / 200.0;
            // 0.2: Exponential Fade, each side over log10(1 + 9x).
            const double gIn = law == "constantGain"      ? prog
                               : law == "exponentialFade" ? std::log10(1.0 + 9.0 * prog)
                                                          : std::sin(prog * std::numbers::pi / 2.0);
            const double gOut = law == "constantGain"      ? 1.0 - prog
                                : law == "exponentialFade" ? std::log10(1.0 + 9.0 * (1.0 - prog))
                                                           : std::cos(prog * std::numbers::pi / 2.0);
            INFO("at " << at << " ms");
            CHECK(out[k * 2] == Approx(0.5 * gOut + 0.25 * gIn).margin(1e-4));
        }
        // Outside the transition each clip plays alone, without cut fades at the edit point.
        CHECK(out[static_cast<std::size_t>(300 * 48) * 2] == Approx(0.5f));
        CHECK(out[static_cast<std::size_t>(700 * 48) * 2] == Approx(0.25f));
    }
}

TEST_CASE("Reversed clips play their samples backwards", "[playback][effects]") {
    Project p;
    p.media('A', ramp(0.5f));
    Clip c = p.makeClip({'A', ms(0), ms(500), ms(100)});
    c.reversed = true;
    addTrack(p, {c});
    const auto out = exported(p, Ticks{}, 24000);
    std::size_t mismatches = 0;
    for (std::int64_t k = 0; k < 24000; ++k) {
        const Ticks t = frames(k);
        const std::int64_t n = ticksToSamples(c.sourceTimeAt(t), k48k);
        if (std::fabs(out[static_cast<std::size_t>(k) * 2] - p.feed->sample(c.media, 0, n, 0, k48k)) > 1e-6f) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("Effects do what their parameters say through the whole chain", "[playback][effects]") {
    SECTION("a clip EQ cut at the mid band") {
        Project p;
        p.media('A', tone(2500.0, 0.25f));
        addTrack(p, {clipWith(p, {'A', ms(0), ms(1000), ms(0)},
                              {effect(p, "wabcut.audio.eq3", {{"midFrequency", ParamValue(2500.0)},
                                                              {"midGain", ParamValue(-9.0)}, {"midQ", ParamValue(2.0)}})})});
        const auto out = exported(p, Ticks{}, 48000);
        double peak = 0.0;
        for (std::size_t k = 24000; k < 48000; ++k) {
            peak = std::max(peak, static_cast<double>(std::fabs(out[k * 2])));
        }
        CHECK(20.0 * std::log10(peak / 0.25) == Approx(-9.0).margin(0.15));
    }
    SECTION("a track compressor and a master true-peak limiter") {
        Project p;
        p.media('A', tone(997.0, 1.6f)); // +4 dBFS
        p.media('B', tone(3000.0, 1.0f));
        addTrack(p, {p.makeClip({'A', ms(0), ms(2000), ms(0)})},
                 {effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-10.0)}, {"ratio", ParamValue(2.0)}})});
        addTrack(p, {p.makeClip({'B', ms(500), ms(1000), ms(0)})});
        setMaster(p, {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-1.0)}})});
        const auto out = exported(p, Ticks{}, 96000);
        double peak = 0.0;
        for (const float v : out) {
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        CHECK(peak <= std::pow(10.0, -1.0 / 20.0) + 1e-6);
        audio::dsp::LoudnessMeter meter(2, 48000);
        meter.add(out.data(), out.size() / 2);
        CHECK(meter.result().truePeakDbtp <= -1.0 + 0.02);
    }
}

TEST_CASE("Editing an effect during playback is heard within one lead window; master edits swap without a gap",
          "[playback][effects][edit]") {
    Project p;
    p.media('A', tone(1000.0, 0.25f));
    const EffectInstance eq = effect(p, "wabcut.audio.eq3", {{"midGain", ParamValue(0.0)}, {"midQ", ParamValue(1.0)}});
    addTrack(p, {clipWith(p, {'A', ms(0), ms(5000), ms(0)}, {eq})});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(500);
    // Cut the mid band by 12 dB while playing.
    Clip edited = p.track(0).clips[0];
    EffectStack stack = edited.effects();
    stack[0].set("midGain", ParamValue(-12.0));
    edited.setEffects(stack);
    Track track = p.track(0);
    track.clips.mutableAt(0) = edited;
    REQUIRE(rig.session.commit("eq", Project::withTrack(*rig.session.snapshot(), 0, track)));
    rig.player->feedOnce();
    rig.pullFor(500);
    auto peakAround = [&](double atMs) {
        double peak = 0.0;
        const auto from = static_cast<std::size_t>(atMs * 48.0);
        for (std::size_t k = from; k < from + 960; ++k) {
            peak = std::max(peak, static_cast<double>(std::fabs(rig.stream[k * 2])));
        }
        return 20.0 * std::log10(peak / 0.25);
    };
    CHECK(peakAround(400.0) == Approx(0.0).margin(0.05));
    CHECK(peakAround(900.0) == Approx(-12.0).margin(0.2)); // within the 250 ms lead of the edit at 500 ms

    // Adding a master limiter during playback: the program swaps at a block boundary; the audio keeps flowing.
    ProjectState next = *rig.session.snapshot();
    Sequence seq = *next.sequences.front();
    seq.audioEffects = {effect(next, "wabcut.audio.limiter", {{"ceiling", ParamValue(-30.0)}})};
    next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    REQUIRE(rig.session.commit("limiter", std::move(next)));
    rig.player->feedOnce();
    rig.pullFor(500);
    CHECK(peakAround(1450.0) <= -30.0 + 12.0 + 0.1); // -30 dBFS relative to the 0.25 (-12 dBFS) tone
    CHECK(peakAround(1450.0) > -30.0 + 12.0 - 1.0);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("Shuttle audition keeps the pitch", "[playback][effects][shuttle]") {
    Project p;
    p.media('A', tone(440.0, 0.4f));
    addTrack(p, {p.makeClip({'A', ms(0), ms(10000), ms(0)})});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(100);
    rig.player->setSpeed({2, 1});
    const auto from = rig.stream.size() / 2 + 2400; // after the stretcher's start
    rig.pullFor(1000);
    // Count rising zero crossings over 900 ms of 2x playback: still 440 Hz.
    int crossings = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t k = from + 1; k < from + 43200; ++k) {
        if (rig.stream[(k - 1) * 2] < 0.0f && rig.stream[k * 2] >= 0.0f) {
            first = crossings == 0 ? k : first;
            last = k;
            ++crossings;
        }
    }
    REQUIRE(crossings > 10);
    CHECK(48000.0 * (crossings - 1) / static_cast<double>(last - first) == Approx(440.0).epsilon(0.005));
    // The clock ran at 2x: 100 ms + 2 x 1000 ms.
    CHECK(rig.player->position().value == Approx(static_cast<double>(ms(2100).value)).epsilon(0.002));
}

TEST_CASE("A loop plays through a master with lookahead without a gap at the wrap", "[playback][effects][loop]") {
    // The master's latency L is read ahead of the transport. In the last L frames before the loop's end the reads went
    // past it (the queue holds the wrapped audio there): silence and a resync at every wrap. Then the master restarted
    // at the loop's start (review A5-P2-7: a plug-in there replays its pipeline's priming).
    Project dry;
    dry.media('A', ramp(0.4f));
    addTrack(dry, {dry.makeClip({'A', ms(0), ms(2000), ms(0)})});
    Project wet;
    wet.media('A', ramp(0.4f));
    addTrack(wet, {wet.makeClip({'A', ms(0), ms(2000), ms(0)})});
    setMaster(wet, {effect(wet, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}, {"lookahead", ParamValue(10.0)}})});
    const auto straight = exported(dry, Ticks{}, 48 * 400);
    Rig rig(wet);
    rig.player->setLoop(TimeRange{ms(100), ms(200)}); // [100 ms, 300 ms)
    rig.player->feedOnce();
    rig.player->play();
    rig.pullFor(900);
    // Output frame k plays timeline frame t: straight to 300 ms, then the loop every 9600 frames.
    std::size_t mismatches = 0;
    for (std::int64_t k = 0; k < 48 * 900; ++k) {
        const std::int64_t t = k < 14400 ? k : 4800 + (k - 14400) % 9600;
        for (std::size_t c = 0; c < 2; ++c) {
            mismatches += rig.stream[static_cast<std::size_t>(k) * 2 + c] != straight[static_cast<std::size_t>(t) * 2 + c] ? 1 : 0;
        }
    }
    CHECK(mismatches == 0);
    CHECK(rig.player->stats().resyncs == 0);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("Master automation follows the loop's time across a wrap", "[playback][effects][loop][keyframes]") {
    // The master plays on across a wrap (above), so its clock moves to the loop's start there; without that its keyframes
    // went on being read past the loop's end (the master fader stuck at its last key).
    Project p;
    p.media('A', level(0.5f));
    addTrack(p, {p.makeClip({'A', ms(0), ms(2000), ms(0)})});
    setMaster(p, {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}, {"lookahead", ParamValue(10.0)}})});
    Sequence seq = *p.state.sequences.front();
    seq.masterVolume = ParamValue(Curve{{Keyframe{ms(100), 0.0}, Keyframe{ms(300), -40.0}}}); // over the loop
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    Rig rig(p);
    rig.player->setLoop(TimeRange{ms(100), ms(200)});
    rig.player->feedOnce();
    rig.player->play();
    rig.pullFor(700);
    const auto levelAt = [&](std::int64_t k) { return std::fabs(rig.stream[static_cast<std::size_t>(k) * 2]); };
    // 20 ms into every pass of the loop the fader is near 0 dB (about -5 dB); 20 ms before its end near -36 dB.
    for (const std::int64_t begin : {std::int64_t{4800}, std::int64_t{14400}, std::int64_t{24000}}) {
        INFO(begin);
        CHECK(levelAt(begin + 960) > 0.15f);
        CHECK(levelAt(begin + 9600 - 960) < 0.05f);
    }
}
