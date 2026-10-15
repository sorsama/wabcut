// A.5 part 3: nested-sequence and multicam audio (C.8, docs/internal/c8-nested-multicam-handoff.md). A nested clip plays its
// sequence's mix (the tracks that play there with their faders and effects, the nested master stage, then the clip's
// own volume), nested in nested, through a remap's 100 % spans and backwards; a multicam clip its active angles at
// their sync, crossfading where it switches angle; source-anchored redactions inside nested media; preview == export
// sample for sample; an edit inside a nested sequence is heard. (The callback's real-time audit: test_rt_audit.cpp.)

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
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
SyntheticSignal ramp() {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 1.0; // one sawtooth per second: every sample value is unique
    s.amplitude = 0.5f;
    s.channelScale = 0.5f;
    return s;
}
SyntheticSignal tone(double hz, float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}
SyntheticSignal noise(float amplitude, std::uint32_t seed) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Noise;
    s.amplitude = amplitude;
    s.seed = seed;
    return s;
}

double db(double gain) { return std::pow(10.0, gain / 20.0); }

EffectInstance effect(Project& p, std::string id, std::vector<EffectParam> params = {}) {
    EffectInstance e;
    e.id = p.state.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    for (EffectParam& param : params) {
        e.set(std::move(param.key), std::move(param.value));
    }
    return e;
}

Track audioTrack(Project& p, std::vector<Clip> clips, EffectStack effects = {}, ParamValue volume = {}) {
    Track track;
    track.id = p.state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    for (Clip& c : clips) {
        track.clips.push_back(std::move(c));
    }
    track.effects = std::move(effects);
    track.volume = std::move(volume);
    return track;
}

/// A clip of sequence `sequence` (nested; with `angles`, multicam) on the audio side.
Clip sequenceClip(Project& p, SequenceId sequence, Ticks start, Ticks duration, Ticks sourceIn, AngleSet angles = {}) {
    Clip c;
    c.id = p.state.ids.allocate<ClipId>();
    c.sequence = sequence;
    c.stream = {StreamKind::Audio, 0};
    c.timelineRange = {start, duration};
    c.sourceIn = sourceIn;
    c.angles = angles;
    return c;
}

/// Adds a sequence holding `tracks` (A1 first), `duration` long, for nesting.
SequenceId addSequence(Project& p, std::vector<Track> tracks, Ticks duration, std::optional<MulticamInfo> multicam = {}) {
    Sequence seq;
    seq.id = p.state.ids.allocate<SequenceId>();
    seq.name = multicam ? "Multicam" : "Nested";
    seq.duration = duration;
    seq.multicam = std::move(multicam);
    for (Track& t : tracks) {
        seq.audioTracks.push_back(std::make_shared<const Track>(std::move(t)));
    }
    const SequenceId id = seq.id;
    p.state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return id;
}

/// A multicam source: angle i plays `angles[i].first` (one audio track), synced at `angles[i].second`.
SequenceId addMulticam(Project& p, std::vector<std::pair<Track, Ticks>> angles, Ticks duration) {
    MulticamInfo info;
    info.sync = MulticamSync::Manual;
    std::vector<Track> tracks;
    for (std::size_t i = 0; i < angles.size(); ++i) {
        MulticamAngle angle;
        angle.name = "Camera " + std::to_string(i + 1);
        angle.audio = {angles[i].first.id};
        angle.offset = angles[i].second;
        info.angles.push_back(angle);
        tracks.push_back(std::move(angles[i].first));
    }
    return addSequence(p, std::move(tracks), duration, std::move(info));
}

/// `p.state` with sequence `id` changed by `change`.
template <class Change>
ProjectState changed(const ProjectState& state, SequenceId id, Change change) {
    Sequence seq = *state.findSequence(id);
    change(seq);
    return state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
}

void addTopTrack(Project& p, std::vector<Clip> clips, EffectStack effects = {}) {
    Track track = audioTrack(p, std::move(clips), std::move(effects));
    Sequence seq = *p.state.sequences.front();
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

/// The export path from `start` (the project's redactions and nested sequences, as export resolves them), read in
/// uneven pieces.
std::vector<float> exported(const Project& p, Ticks start, std::size_t count, TrackRendererOptions renderer = {Ticks{}}) {
    OfflineMixdownOptions options;
    options.renderer = renderer;
    options.redactions = AudioRedactions::of(p.state);
    options.project = std::make_shared<const ProjectState>(p.state);
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(count * 2);
    for (std::size_t done = 0; done < count;) {
        const std::size_t n = std::min<std::size_t>(count - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

std::int64_t firstMismatch(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b, std::size_t bFrom,
                           std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        if (a[aFrom + i] != b[bFrom + i]) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

double rms(const std::vector<float>& s, std::size_t fromFrame, std::size_t toFrame) {
    double sum = 0.0;
    for (std::size_t k = fromFrame; k < toFrame; ++k) {
        sum += static_cast<double>(s[k * 2]) * s[k * 2];
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(toFrame - fromFrame, 1)));
}

} // namespace

TEST_CASE("A nested clip plays its sequence's mix: track faders, the tracks that play, the nested master, its own volume",
          "[playback][nested]") {
    Project p;
    p.media('A', level(0.5f));
    p.media('B', level(0.25f));
    const SequenceId nested = addSequence(p,
                                          {audioTrack(p, {p.makeClip({'A', Ticks{}, frames(4800), Ticks{}})}, {}, ParamValue(-6.0)),
                                           audioTrack(p, {p.makeClip({'B', Ticks{}, frames(4800), Ticks{}})})},
                                          frames(4800));
    p.state = changed(p.state, nested, [](Sequence& s) { s.masterVolume = ParamValue(-6.0); });
    Clip clip = sequenceClip(p, nested, frames(480), frames(2400), frames(960));
    ClipProperties props;
    props.set(ParamId::VolumeDb, -3.0);
    clip.setProperties(props);
    addTopTrack(p, {clip});

    const auto expect = [&](const std::vector<float>& out, double inside) {
        CHECK(out[479 * 2] == 0.0f);
        CHECK(out[480 * 2] == Approx(inside).margin(1e-6));
        CHECK(out[1700 * 2 + 1] == Approx(inside).margin(1e-6));
        CHECK(out[2879 * 2] == Approx(inside).margin(1e-6));
        CHECK(out[2880 * 2] == 0.0f);
    };
    SECTION("every track plays, through its fader, the nested master fader and the clip's volume") {
        expect(exported(p, Ticks{}, 4800), (0.5 * db(-6.0) + 0.25) * db(-6.0) * db(-3.0));
    }
    SECTION("a muted nested track is silent") {
        p.state = changed(p.state, nested, [](Sequence& s) {
            Track t = *s.audioTracks[1];
            t.muted = true;
            s.audioTracks[1] = std::make_shared<const Track>(std::move(t));
        });
        expect(exported(p, Ticks{}, 4800), 0.5 * db(-6.0) * db(-6.0) * db(-3.0));
    }
    SECTION("while a nested track is soloed, only the soloed ones play") {
        p.state = changed(p.state, nested, [](Sequence& s) {
            Track t = *s.audioTracks[1];
            t.solo = true;
            s.audioTracks[1] = std::make_shared<const Track>(std::move(t));
        });
        expect(exported(p, Ticks{}, 4800), 0.25 * db(-6.0) * db(-3.0));
    }
    SECTION("the nested master effects run on the nested mix") {
        p.state = changed(p.state, nested, [&](Sequence& s) {
            s.audioEffects = {effect(p, "wabcut.audio.gain", {{"gain", ParamValue(-12.0)}})};
            s.masterVolume = ParamValue();
        });
        expect(exported(p, Ticks{}, 4800), (0.5 * db(-6.0) + 0.25) * db(-12.0) * db(-3.0));
    }
    SECTION("past the nested sequence's end the clip is silent") {
        p.state = changed(p.state, nested, [](Sequence& s) { s.duration = frames(3000); });
        const auto out = exported(p, Ticks{}, 4800);
        const double inside = (0.5 * db(-6.0) + 0.25) * db(-6.0) * db(-3.0);
        CHECK(out[2519 * 2] == Approx(inside).margin(1e-6)); // nested time 2999
        CHECK(out[2520 * 2] == 0.0f);                         // nested time 3000: the end
    }
    SECTION("without the project, a nested clip is silent") {
        OfflineMixdownOptions options;
        options.renderer.edgeFade = Ticks{};
        OfflineMixdown mix(p.state.sequences.front(), 2, 48000, Ticks{}, *p.feed, options);
        std::vector<float> out(4800 * 2);
        mix.render(out);
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
        CHECK(mix.feedErrors() == 0);
    }
}

TEST_CASE("Nested in nested: every level maps time exactly and applies its own volume", "[playback][nested]") {
    Project p;
    p.media('A', ramp());
    const SequenceId inner = addSequence(p, {audioTrack(p, {p.makeClip({'A', Ticks{}, frames(48000), frames(1000)})})}, frames(48000));
    Clip middleClip = sequenceClip(p, inner, frames(200), frames(30000), frames(300));
    ClipProperties props;
    props.set(ParamId::VolumeDb, -6.0);
    middleClip.setProperties(props);
    const SequenceId middle = addSequence(p, {audioTrack(p, {middleClip})}, frames(40000));
    addTopTrack(p, {sequenceClip(p, middle, frames(100), frames(9000), frames(700))});

    const auto out = exported(p, Ticks{}, 10000);
    // Top frame k -> middle time 700 + (k - 100) -> inner time 300 + (middle - 200) -> media frame 1000 + inner.
    std::size_t wrong = 0;
    for (std::int64_t k = 100; k < 9100; ++k) {
        const std::int64_t media = 1000 + 300 + (700 + (k - 100) - 200);
        for (int ch = 0; ch < 2; ++ch) {
            const double expected = p.feed->sample(p.mediaId('A'), 0, media, ch, k48k) * db(-6.0);
            wrong += std::fabs(out[static_cast<std::size_t>(k) * 2 + static_cast<std::size_t>(ch)] - expected) > 1e-6 ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
    CHECK(out[99 * 2] == 0.0f);
    CHECK(out[9100 * 2] == 0.0f);
    // The resolver agrees on the leaf and its media time.
    const Clip& top = p.sequenceRef().audioTracks[0]->clips[0];
    const auto leaves = resolveLeaves(p.state, p.sequence, top, frames(5000));
    REQUIRE(leaves.size() == 1);
    CHECK(leaves[0].sourceTime == frames(1000 + 300 + (700 + 4900 - 200)));
    CHECK(leaves[0].path.size() == 2);
}

TEST_CASE("A remapped nested clip is heard only in its 100 % spans, sample-accurately; a reversed one plays backwards",
          "[playback][nested][remap]") {
    Project p;
    p.media('A', ramp());
    const SequenceId nested = addSequence(p, {audioTrack(p, {p.makeClip({'A', Ticks{}, frames(96000), Ticks{}})})}, frames(96000));
    SECTION("remapped") {
        Clip clip = sequenceClip(p, nested, frames(100), frames(600), frames(500));
        ClipContent content = clip.content();
        const auto key = [](std::int64_t time, std::int64_t source, Interpolation mode = Interpolation::Linear) {
            return Keyframe{frames(time), static_cast<double>(frames(source).value), mode};
        };
        content.timeRemap = Curve{{key(500, 1000), key(600, 1100), key(700, 1300), key(800, 1300),
                                   key(900, 1200, Interpolation::EaseOut), key(1000, 1250), key(1100, 1350)}};
        clip.setContent(std::move(content));
        REQUIRE(clip.unitySpeedRanges() == std::vector<TimeRange>{{frames(100), frames(100)}, {frames(600), frames(100)}});
        addTopTrack(p, {clip});
        const auto out = exported(p, Ticks{}, 960);
        std::size_t wrong = 0;
        for (std::size_t k = 0; k < 960; ++k) {
            const bool audible = (k >= 100 && k < 200) || (k >= 600 && k < 700);
            const auto source = k < 200 ? 1000 + static_cast<std::int64_t>(k) - 100 : 1250 + static_cast<std::int64_t>(k) - 600;
            for (int ch = 0; ch < 2; ++ch) {
                const float expected = audible ? p.feed->sample(p.mediaId('A'), 0, source, ch, k48k) : 0.0f;
                wrong += out[2 * k + static_cast<std::size_t>(ch)] != expected ? 1 : 0;
            }
        }
        CHECK(wrong == 0);
        // A seek inside the second span reads the same samples.
        const auto seek = exported(p, frames(625), 100);
        CHECK(std::equal(seek.begin(), seek.end(), out.begin() + 1250));
    }
    SECTION("reversed, through pieces of the nested mix that do not depend on how it is read") {
        Clip clip = sequenceClip(p, nested, frames(100), frames(70000), frames(2000));
        clip.reversed = true;
        addTopTrack(p, {clip});
        const auto out = exported(p, Ticks{}, 70200);
        std::size_t wrong = 0;
        for (std::int64_t k = 100; k < 70100; ++k) {
            const std::int64_t source = 2000 + 70000 - 1 - (k - 100); // the clip's last source frame first
            wrong += out[static_cast<std::size_t>(k) * 2] != p.feed->sample(p.mediaId('A'), 0, source, 0, k48k) ? 1 : 0;
        }
        CHECK(wrong == 0);
        const auto seek = exported(p, frames(30011), 20000);
        CHECK(firstMismatch(seek, 0, out, 30011 * 2, seek.size()) == -1);
    }
}

TEST_CASE("Multicam audio plays the active angles at their sync offsets and crossfades where it switches angle",
          "[playback][multicam]") {
    Project p;
    p.media('A', tone(440.0, 0.5f));
    p.media('B', level(0.3f));
    // Angle 2 started recording 100 ms (4800 frames) after angle 1: its clip lies there in the source.
    const SequenceId mc = addMulticam(p,
                                      {{audioTrack(p, {p.makeClip({'A', Ticks{}, frames(96000), Ticks{}})}), Ticks{}},
                                       {audioTrack(p, {p.makeClip({'B', frames(4800), frames(91200), Ticks{}})}), frames(4800)}},
                                      frames(96000));
    // Angle 1 to frame 24000, then angle 2 (a multicam.switchAngle cut), then both.
    addTopTrack(p, {sequenceClip(p, mc, Ticks{}, frames(24000), frames(9600), AngleSet::of(0)),
                    sequenceClip(p, mc, frames(24000), frames(12000), frames(33600), AngleSet::of(1)),
                    sequenceClip(p, mc, frames(36000), frames(12000), frames(45600), AngleSet{0b11})});
    const auto a = [&](std::int64_t k) { return p.feed->sample(p.mediaId('A'), 0, 9600 + k, 0, k48k); };
    const auto b = [&](std::int64_t k) { return p.feed->sample(p.mediaId('B'), 0, 9600 + k - 4800, 0, k48k); };

    SECTION("a 10 ms crossfade around the switch, the angles in sync, no click") {
        const auto out = exported(p, Ticks{}, 48000);
        CHECK(out[1000 * 2] == Approx(a(1000)).margin(1e-6));
        CHECK(out[23000 * 2] == Approx(a(23000)).margin(1e-6));
        CHECK(out[30000 * 2] == Approx(b(30000)).margin(1e-6));
        CHECK(out[40000 * 2] == Approx(a(40000) + b(40000)).margin(1e-6));
        // The crossfade: 480 frames centred on the cut; the two ramps add up to 1.
        std::size_t wrong = 0;
        for (std::int64_t k = 23760; k < 24240; ++k) {
            const double out1 = (24240.0 - static_cast<double>(k)) / 481.0;
            const double in1 = (static_cast<double>(k) - 23760.0 + 1.0) / 481.0;
            const double expected = static_cast<float>(a(k) * out1) + static_cast<float>(b(k) * in1);
            wrong += std::fabs(out[static_cast<std::size_t>(k) * 2] - expected) > 1e-6 ? 1 : 0;
        }
        CHECK(wrong == 0);
        CHECK(maxStep(out) < 0.035); // the sine's own slope (0.029 per sample) plus 0.3 / 481
        // A switch to the same sound continues it: no dip at the cut from angle 2 to angles 1 + 2's B part.
        CHECK(out[35999 * 2] == Approx(b(35999) * (36240.0 - 35999.0) / 481.0 +
                                       (a(35999) + b(35999)) * (35999.0 - 35760.0 + 1.0) / 481.0)
                                    .margin(1e-6));
    }
    SECTION("without the crossfade the same switch clicks (the test can see a click)") {
        TrackRendererOptions hard{Ticks{}};
        hard.angleCrossfade = Ticks{};
        const auto out = exported(p, Ticks{}, 48000, hard);
        CHECK(maxStep(out) > 0.25);
    }
    SECTION("a multicam clip reads its angles' tracks whatever their flags") {
        p.state = changed(p.state, mc, [](Sequence& s) {
            Track t = *s.audioTracks[1];
            t.muted = true;
            t.enabled = false;
            s.audioTracks[1] = std::make_shared<const Track>(std::move(t));
        });
        const auto out = exported(p, Ticks{}, 48000);
        CHECK(out[30000 * 2] == Approx(b(30000)).margin(1e-6));
    }
}

TEST_CASE("Redactions inside nested media mute and bleep by media time; excepting the nested clip excepts its content",
          "[playback][nested][redaction]") {
    Project p;
    p.media('A', level(0.5f));
    const SequenceId nested = addSequence(p, {audioTrack(p, {p.makeClip({'A', Ticks{}, frames(48000), Ticks{}})})}, frames(48000));
    const Clip top = sequenceClip(p, nested, Ticks{}, frames(24000), frames(500));
    addTopTrack(p, {top});
    Redaction r;
    r.media = p.mediaId('A');
    r.source = TimeRange::fromStartEnd(frames(10500), frames(15500)); // top frames 10000 .. 15000
    r.audio = RedactionAudio::Bleep;
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);

    const auto out = exported(p, Ticks{}, 24000);
    std::size_t wrong = 0;
    for (std::int64_t k = 10000; k < 15000; ++k) {
        const float expected = AudioRedactions::bleepAt(frames(500 + k));
        wrong += out[static_cast<std::size_t>(k) * 2] != expected ? 1 : 0;
    }
    CHECK(wrong == 0);
    CHECK(out[9000 * 2] == 0.5f);
    CHECK(out[16000 * 2] == 0.5f);
    // The resolver names the redaction exactly there.
    const Clip& placed = p.sequenceRef().audioTracks[0]->clips[0];
    CHECK(resolveLeaves(p.state, p.sequence, placed, frames(12000)).front().redactions.size() == 1);
    CHECK(resolveLeaves(p.state, p.sequence, placed, frames(9000)).front().redactions.empty());
    // Preview applies it alike.
    {
        Rig rig(p);
        rig.player->play();
        rig.pullFor(500);
        CHECK(firstMismatch(rig.stream, 0, out, 0, 24000 * 2) == -1);
    }

    // Excepting the nested clip excepts everything inside it, in the resolver and in the sound.
    Redaction excepting = p.state.redactions.front();
    excepting.exceptions = {placed.id};
    auto updated = edit::updateRedaction(p.state, excepting);
    REQUIRE(updated);
    p.state = std::move(*updated);
    CHECK(resolveLeaves(p.state, p.sequence, placed, frames(12000)).front().redactions.empty());
    const auto clear = exported(p, Ticks{}, 24000);
    CHECK(std::all_of(clear.begin(), clear.end(), [](float v) { return v == 0.5f; }));
}

TEST_CASE("Preview and export render nested, nested-in-nested and multicam audio sample for sample alike",
          "[playback][nested][multicam][export]") {
    Project p;
    p.media('A', noise(0.3f, 7));
    p.media('M', tone(220.0, 0.6f));
    p.media('B', tone(330.0, 0.4f));
    Clip dialogue = p.makeClip({'A', Ticks{}, ms(2500), ms(100)});
    dialogue.setEffects({effect(p, "wabcut.audio.eq3", {{"midGain", ParamValue(-6.0)}}),
                         effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-24.0)}, {"ratio", ParamValue(4.0)}})});
    const SequenceId scene =
        addSequence(p,
                    {audioTrack(p, {dialogue}, {effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-18.0)}})},
                                ParamValue(Curve{{Keyframe{ms(0), -12.0}, Keyframe{ms(800), 0.0}}})),
                     audioTrack(p, {p.makeClip({'M', ms(500), ms(2000), Ticks{}, -6.0})})},
                    ms(3000));
    p.state = changed(p.state, scene, [&](Sequence& s) {
        s.audioEffects = {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-3.0)}})};
        s.masterVolume = ParamValue(Curve{{Keyframe{ms(0), -6.0}, Keyframe{ms(1500), 0.0}}});
    });
    Clip sceneClip = sequenceClip(p, scene, Ticks{}, ms(2000), ms(250));
    sceneClip.setEffects({effect(p, "wabcut.audio.gain", {{"gain", ParamValue(-2.0)}})});
    const SequenceId act = addSequence(p, {audioTrack(p, {sceneClip}), audioTrack(p, {p.makeClip({'B', ms(300), ms(900), Ticks{}, -9.0})})},
                                       ms(2500));
    const SequenceId mc = addMulticam(p,
                                      {{audioTrack(p, {p.makeClip({'A', Ticks{}, ms(4000), Ticks{}})}), Ticks{}},
                                       {audioTrack(p, {p.makeClip({'B', ms(100), ms(3900), Ticks{}})},
                                                   {effect(p, "wabcut.audio.eq3", {{"lowGain", ParamValue(4.0)}})}),
                                        ms(100)}},
                                      ms(4000));
    addTopTrack(p, {sequenceClip(p, act, ms(100), ms(1500), ms(100)), sequenceClip(p, scene, ms(1800), ms(700), ms(400))});
    addTopTrack(p, {sequenceClip(p, mc, Ticks{}, ms(800), ms(200), AngleSet::of(0)),
                    sequenceClip(p, mc, ms(800), ms(700), ms(1000), AngleSet::of(1)),
                    sequenceClip(p, mc, ms(1500), ms(900), ms(1700), AngleSet{0b11})});
    Sequence top = *p.state.sequences.front();
    top.audioEffects = {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-1.0)}})};
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(top));

    SECTION("from the start") {
        Rig rig(p);
        rig.player->play();
        rig.pullFor(2600);
        const auto offline = exported(p, Ticks{}, 2600 * 48);
        CHECK(firstMismatch(rig.stream, 0, offline, 0, offline.size()) == -1);
        CHECK(rms(offline, 200 * 48, 1500 * 48) > 0.01); // nested audio is there
        CHECK(rms(offline, 1900 * 48, 2400 * 48) > 0.01);
        CHECK(rig.player->stats().underrunFrames == 0);
        CHECK(rig.player->stats().feedErrors == 0);
    }
    SECTION("from a seek point, every level settled on the same pre-roll") {
        Rig rig(p, Rig::Options{Ticks{}, Ticks{}, false, ms(50)});
        rig.player->play();
        rig.pullFor(100);
        rig.player->seek(ms(900));
        const auto before = rig.stream.size() / 2;
        rig.pullFor(1200);
        const auto offline = exported(p, ms(900), 1000 * 48);
        CHECK(firstMismatch(rig.stream, (before + 480) * 2, offline, 0, offline.size()) == -1);
    }
}

TEST_CASE("An edit inside a nested sequence is heard within one lead window", "[playback][nested][edit]") {
    Project p;
    p.media('A', level(0.5f));
    const SequenceId nested = addSequence(p, {audioTrack(p, {p.makeClip({'A', Ticks{}, ms(4000), Ticks{}})})}, ms(4000));
    addTopTrack(p, {sequenceClip(p, nested, Ticks{}, ms(3000), Ticks{})});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(300);
    CHECK(rig.stream[(rig.stream.size() / 2 - 1) * 2] == 0.5f);
    const auto flushes = rig.player->stats().editFlushes;
    // Turn the nested track down by 6 dB: the parent's track did not change, its nested clip's sound did.
    const ProjectState quieter = changed(*rig.session.snapshot(), nested, [](Sequence& s) {
        Track t = *s.audioTracks[0];
        t.volume = ParamValue(-6.0);
        s.audioTracks[0] = std::make_shared<const Track>(std::move(t));
    });
    REQUIRE(rig.session.commit("nested fader", quieter));
    rig.pullFor(400);
    CHECK(rig.player->stats().editFlushes > flushes);
    CHECK(rig.stream[(rig.stream.size() / 2 - 1) * 2] == Approx(0.5 * db(-6.0)).margin(1e-6));
    CHECK(rig.player->stats().underrunFrames == 0);
}
