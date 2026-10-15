// Wave-4 test gaps (redaction audio, I.6, AudioRedactions.h): a bleep stays sample-exact where the clip's effects add
// latency (the renderer reads ahead, the bleep is applied before the effects), through an audio transition's
// crossfade (each side bleeps its own media time, then the law applies), and at 44.1 kHz (the bleep follows media time
// at the output rate). Preview == export in each case.

#include "PlaybackFixture.h"

#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
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

void bleep(Project& p, char media, Ticks start, Ticks end) {
    Redaction r;
    r.media = p.mediaId(media);
    r.source = TimeRange::fromStartEnd(start, end);
    r.audio = RedactionAudio::Bleep;
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);
}

void addTrack(Project& p, std::vector<Clip> clips, std::vector<Transition> transitions = {}) {
    Track track;
    track.id = p.state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    for (Clip& c : clips) {
        track.clips.push_back(std::move(c));
    }
    track.transitions = std::move(transitions);
    Sequence seq = *p.state.sequences.front();
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

EffectInstance effect(Project& p, std::string id, std::vector<EffectParam> params) {
    EffectInstance e;
    e.id = p.state.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    for (EffectParam& param : params) {
        e.set(std::move(param.key), std::move(param.value));
    }
    return e;
}

std::vector<float> exported(const Project& p, std::size_t count, int rate = 48000) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.redactions = AudioRedactions::of(p.state);
    OfflineMixdown mix(p.state.sequences.front(), 2, rate, Ticks{}, *p.feed, options);
    std::vector<float> out(count * 2);
    for (std::size_t done = 0; done < count;) {
        const std::size_t n = std::min<std::size_t>(count - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

std::size_t at(Ticks t, Rate rate = k48k) { return static_cast<std::size_t>(t.value / rate.ticksPerUnit()); }

} // namespace

TEST_CASE("Redaction with latency: a bleep before lookahead effects lands on its exact samples", "[playback][redaction][latency]") {
    for (const char* which : {"denoise", "limiter"}) {
        INFO(which);
        Project p;
        p.media('A', level(0.25f));
        Clip clip = p.makeClip({'A', ms(0), ms(1000), ms(100)});
        // Transparent settings: Reduce Noise at amount 0 (1440 frames of latency), a limiter far above the signal.
        clip.setEffects({std::string(which) == "denoise"
                             ? effect(p, "wabcut.audio.denoise", {{"amount", ParamValue(0.0)}})
                             : effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}, {"lookahead", ParamValue(5.0)}})});
        addTrack(p, {clip});
        bleep(p, 'A', ms(400), ms(600)); // timeline 300 .. 500 ms
        const auto out = exported(p, at(ms(1000)));
        std::size_t wrong = 0;
        for (std::size_t k = at(ms(300)); k < at(ms(500)); ++k) {
            const float expected = AudioRedactions::bleepAt(ms(100) + frames(static_cast<std::int64_t>(k)));
            wrong += std::fabs(out[k * 2] - expected) > 1e-6f ? 1 : 0;
        }
        CHECK(wrong == 0);
        CHECK(out[at(ms(250)) * 2] == Approx(0.25f).margin(1e-6));
        CHECK(out[at(ms(550)) * 2] == Approx(0.25f).margin(1e-6));
        Rig rig(p);
        rig.player->play();
        rig.pullFor(1000);
        REQUIRE(rig.stream.size() >= out.size());
        float worst = 0.0f;
        for (std::size_t i = 0; i < out.size(); ++i) {
            worst = std::max(worst, std::fabs(out[i] - rig.stream[i]));
        }
        CHECK(worst == 0.0f); // preview == export
    }
}

TEST_CASE("Redaction through a crossfade: each side bleeps its own media time, then the transition's law",
          "[playback][redaction][transitions]") {
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
    t.effect = "constantPower";
    addTrack(p, {a, b}, {t});
    // A's media from 450 ms on, its transition handle (500 .. 600 ms of media) included.
    bleep(p, 'A', ms(450), ms(700));
    const auto out = exported(p, at(ms(1000)));
    std::size_t wrong = 0;
    for (std::size_t k = at(ms(450)); k < at(ms(600)); ++k) {
        const double time = static_cast<double>(k) / 48.0;
        const double gOut = time < 400.0 ? 1.0 : std::cos((time - 400.0) / 200.0 * std::numbers::pi / 2.0);
        const double gIn = time < 400.0 ? 0.0 : std::sin((time - 400.0) / 200.0 * std::numbers::pi / 2.0);
        const double expected = AudioRedactions::bleepAt(frames(static_cast<std::int64_t>(k))) * gOut + 0.25 * gIn;
        wrong += std::fabs(out[k * 2] - expected) > 1e-4 ? 1 : 0;
    }
    CHECK(wrong == 0);
    CHECK(out[at(ms(700)) * 2] == Approx(0.25f)); // B alone after the transition, never bleeped
    Rig rig(p);
    rig.player->play();
    rig.pullFor(1000);
    CHECK(std::equal(out.begin(), out.end(), rig.stream.begin()));
}

TEST_CASE("Redaction at 44.1 kHz: the bleep follows media time at the output rate, sample-exact", "[playback][redaction]") {
    Project p;
    p.media('A', level(0.5f));
    addTrack(p, {p.makeClip({'A', ms(0), ms(1000), ms(200)})});
    bleep(p, 'A', ms(500), ms(700)); // timeline 300 .. 500 ms
    constexpr Rate k441{44100, 1};
    const auto out = exported(p, at(ms(1000), k441), 44100);
    std::size_t wrong = 0;
    const std::int64_t tpf = k441.ticksPerUnit();
    for (std::size_t k = at(ms(300), k441) + 1; k < at(ms(500), k441); ++k) {
        const float expected = AudioRedactions::bleepAt(ms(200) + Ticks{static_cast<std::int64_t>(k) * tpf});
        wrong += out[k * 2] != expected || out[k * 2 + 1] != expected ? 1 : 0;
    }
    CHECK(wrong == 0);
    // The 5 ms ramps sit outside the range at this rate too.
    CHECK(out[at(ms(294), k441) * 2] == 0.5f);
    CHECK(out[at(ms(506), k441) * 2] == 0.5f);
}
