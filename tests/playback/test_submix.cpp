// p02 audio: submix tracks, track output assignment and sends (Premiere's Audio Track Mixer routing) through the player
// and through OfflineMixdown: levels, pre/post-fader sends, mono submixes, mute and solo with routing, latency
// compensation of submix effects, chained submixes, preview == export.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <wabcut/model/SubmixOps.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <string>
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
SyntheticSignal noiseSignal(float amplitude, std::uint32_t seed) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Noise;
    s.amplitude = amplitude;
    s.seed = seed;
    return s;
}
SyntheticSignal tone(double hz, float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}

double gainOf(double db) { return std::pow(10.0, db / 20.0); }

EffectInstance effect(Project& p, std::string id, std::vector<EffectParam> params = {}) {
    EffectInstance e;
    e.id = p.state.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    for (EffectParam& param : params) {
        e.set(std::move(param.key), std::move(param.value));
    }
    return e;
}

/// Adds a submix track (format, effects, fader) and returns its id.
TrackId addSubmix(Project& p, ChannelFormat format, EffectStack effects = {}, double volumeDb = 0.0, double pan = 0.0) {
    Track t;
    t.id = p.state.ids.allocate<TrackId>();
    t.kind = TrackKind::Audio;
    t.format = format;
    t.effects = std::move(effects);
    t.volume = ParamValue(volumeDb);
    t.pan = ParamValue(pan);
    Sequence seq = *p.state.sequences.front();
    seq.submixTracks.push_back(std::make_shared<const Track>(std::move(t)));
    const TrackId id = seq.submixTracks.back()->id;
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    return id;
}

/// Changes audio track `index` (or, with `submix`, submix track `index`) through `edit`.
template <typename Edit>
void changeTrack(Project& p, std::size_t index, Edit&& edit, bool submix = false) {
    Sequence seq = *p.state.sequences.front();
    auto& list = submix ? seq.submixTracks : seq.audioTracks;
    Track t = *list[index];
    edit(t);
    list[index] = std::make_shared<const Track>(std::move(t));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

TrackSend sendTo(TrackId submix, double levelDb, bool preFader = false) {
    TrackSend s;
    s.submix = submix;
    s.level = ParamValue(levelDb);
    s.preFader = preFader;
    return s;
}

/// The export path from `start`, `frames` frames, in uneven pieces.
std::vector<float> exported(const Project& p, Ticks start, std::size_t frames) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(frames * 2);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(frames - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

std::int64_t firstMismatch(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b, std::size_t bFrom,
                           std::size_t samples, float tolerance = 0.0f) {
    for (std::size_t i = 0; i < samples; ++i) {
        if (std::fabs(a[aFrom + i] - b[bFrom + i]) > tolerance) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

/// The sample of channel `channel` at frame `frame` of an interleaved stereo stream.
float at(const std::vector<float>& s, std::size_t frame, int channel = 0) {
    return s[frame * 2 + static_cast<std::size_t>(channel)];
}

} // namespace

TEST_CASE("A track assigned to a submix plays through the submix's effects and fader", "[playback][submix]") {
    Project p;
    p.media('A', level(0.25f));
    p.addTrack({{'A', ms(0), ms(1000), ms(0)}});
    const TrackId s1 = addSubmix(p, ChannelFormat::Stereo, {effect(p, "wabcut.audio.gain", {{"gain", ParamValue(-6.0)}})}, -6.0);
    changeTrack(p, 0, [&](Track& t) { t.output = s1; });
    REQUIRE(usesSubmixRouting(p.sequenceRef()));
    const auto offline = exported(p, Ticks{}, 48000);
    const double expected = 0.25 * gainOf(-6.0) * gainOf(-6.0);
    CHECK(at(offline, 24000, 0) == Approx(expected).epsilon(1e-5));
    CHECK(at(offline, 24000, 1) == Approx(expected).epsilon(1e-5));
    Rig rig(p);
    rig.player->play();
    rig.pullFor(1000);
    CHECK(at(rig.stream, 24000, 0) == Approx(expected).epsilon(1e-5));
    CHECK(firstMismatch(rig.stream, 0, offline, 0, offline.size()) == -1);
    // The submix's meter reads what it outputs; the track's its post-fader level.
    CHECK(rig.player->submixMeter(0).peak(0) == Approx(expected).epsilon(1e-3));
    CHECK(rig.player->trackMeter(0).peak(0) == Approx(0.25).epsilon(1e-3));
}

TEST_CASE("A post-fader send adds the track at its level and a pre-fader send ignores the track fader", "[playback][submix]") {
    Project p;
    p.media('A', level(0.2f));
    p.addTrack({{'A', ms(0), ms(1000), ms(0)}});
    const TrackId s1 = addSubmix(p, ChannelFormat::Stereo);
    changeTrack(p, 0, [&](Track& t) {
        t.volume = ParamValue(-6.0);
        t.sends.push_back(sendTo(s1, -12.0));
    });
    const double fader = gainOf(-6.0);
    SECTION("post-fader: the send follows the fader") {
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000) == Approx(0.2 * fader + 0.2 * fader * gainOf(-12.0)).epsilon(1e-5));
    }
    SECTION("pre-fader: the send does not") {
        changeTrack(p, 0, [](Track& t) { t.sends[0].preFader = true; });
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000) == Approx(0.2 * fader + 0.2 * gainOf(-12.0)).epsilon(1e-5));
    }
    SECTION("a muted send adds nothing") {
        changeTrack(p, 0, [](Track& t) { t.sends[0].muted = true; });
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000) == Approx(0.2 * fader).epsilon(1e-5));
    }
    SECTION("a send's pan is a balance on the stereo mix") {
        changeTrack(p, 0, [](Track& t) { t.sends[0].pan = ParamValue(1.0); });
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000, 0) == Approx(0.2 * fader).epsilon(1e-5));
        CHECK(at(offline, 12000, 1) == Approx(0.2 * fader + 0.2 * fader * gainOf(-12.0)).epsilon(1e-5));
    }
}

TEST_CASE("A mono submix folds its input to mono and pans it with the constant-power law", "[playback][submix]") {
    Project p;
    SyntheticSignal s = level(0.4f);
    s.channelScale = 0.5f; // L 0.4, R 0.2
    p.media('A', s);
    p.addTrack({{'A', ms(0), ms(1000), ms(0)}});
    const TrackId s1 = addSubmix(p, ChannelFormat::Mono);
    changeTrack(p, 0, [&](Track& t) { t.output = s1; });
    SECTION("centre: -3 dB on each side") {
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000, 0) == Approx(0.3 * 0.70710678).epsilon(1e-5));
        CHECK(at(offline, 12000, 1) == Approx(0.3 * 0.70710678).epsilon(1e-5));
    }
    SECTION("panned hard left") {
        changeTrack(p, 0, [](Track& t) { t.pan = ParamValue(-1.0); }, true);
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000, 0) == Approx(0.3).epsilon(1e-5));
        CHECK(std::fabs(at(offline, 12000, 1)) < 1e-6);
    }
}

TEST_CASE("Submixes chain into each other and the Mix in order", "[playback][submix]") {
    Project p;
    p.media('A', level(0.5f));
    p.addTrack({{'A', ms(0), ms(1000), ms(0)}});
    // Made in the opposite order of processing: S2 feeds S1.
    const TrackId s1 = addSubmix(p, ChannelFormat::Stereo, {effect(p, "wabcut.audio.gain", {{"gain", ParamValue(-6.0)}})});
    const TrackId s2 = addSubmix(p, ChannelFormat::Stereo, {}, -3.0);
    changeTrack(p, 1, [&](Track& t) { t.output = s1; }, true);
    changeTrack(p, 0, [&](Track& t) { t.output = s2; });
    REQUIRE_FALSE(audioRoutingProblem(p.sequenceRef()).has_value());
    const auto offline = exported(p, Ticks{}, 24000);
    CHECK(at(offline, 12000) == Approx(0.5 * gainOf(-3.0) * gainOf(-6.0)).epsilon(1e-5));
}

TEST_CASE("Mute and solo follow the routing: a soloed submix sounds with what feeds it", "[playback][submix]") {
    Project p;
    p.media('A', level(0.1f));
    p.media('B', level(0.02f));
    p.addTrack({{'A', ms(0), ms(1000), ms(0)}});
    p.addTrack({{'B', ms(0), ms(1000), ms(0)}});
    const TrackId s1 = addSubmix(p, ChannelFormat::Stereo);
    changeTrack(p, 0, [&](Track& t) { t.output = s1; });
    SECTION("solo the submix: A1 through it, A2 silent") {
        changeTrack(p, 0, [](Track& t) { t.solo = true; }, true);
        Rig rig(p);
        rig.player->play();
        rig.pullFor(500);
        CHECK(at(rig.stream, 12000) == Approx(0.1).epsilon(1e-5));
        CHECK(firstMismatch(rig.stream, 0, exported(p, Ticks{}, 24000), 0, 24000 * 2) == -1);
    }
    SECTION("solo the track: its submix sounds too") {
        changeTrack(p, 0, [](Track& t) { t.solo = true; });
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000) == Approx(0.1).epsilon(1e-5));
    }
    SECTION("mute the submix: what it carries is silent") {
        changeTrack(p, 0, [](Track& t) { t.muted = true; }, true);
        Rig rig(p);
        rig.player->play();
        rig.pullFor(500);
        CHECK(at(rig.stream, 12000) == Approx(0.02).epsilon(1e-5));
    }
    SECTION("a muted track sends nothing") {
        changeTrack(p, 1, [&](Track& t) {
            t.muted = true;
            t.sends.push_back(sendTo(s1, 0.0));
        });
        const auto offline = exported(p, Ticks{}, 24000);
        CHECK(at(offline, 12000) == Approx(0.1).epsilon(1e-5));
    }
}

TEST_CASE("Submix latency is compensated: a direct path and a send through a lookahead limiter stay aligned",
          "[playback][submix][latency]") {
    Project p;
    p.media('A', noiseSignal(0.2f, 3));
    p.addTrack({{'A', ms(0), ms(1500), ms(0)}});
    const Project dry = p; // the same track, no routing
    const TrackId s1 = addSubmix(p, ChannelFormat::Stereo, {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(0.0)}})});
    changeTrack(p, 0, [&](Track& t) { t.sends.push_back(sendTo(s1, 0.0)); });
    const auto plain = exported(dry, Ticks{}, 60000);
    const auto routed = exported(p, Ticks{}, 60000);
    // The limiter never acts on a 0.2 signal under a 0 dB ceiling: its path is the track delayed by its lookahead, and
    // compensation delays the direct path the same, so the mix is exactly twice the track, on time.
    double worst = 0.0;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(routed[i] - 2.0f * plain[i])));
    }
    CHECK(worst < 1e-5);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(1250);
    CHECK(firstMismatch(rig.stream, 0, routed, 0, 60000 * 2) == -1);
}

TEST_CASE("Preview and export route submixes and sends sample for sample alike", "[playback][submix][export]") {
    Project p;
    p.media('A', noiseSignal(0.25f, 5));
    p.media('M', tone(330.0, 0.4f));
    p.addTrack({{'A', ms(50), ms(1800), ms(100)}});
    p.addTrack({{'M', ms(0), ms(2200), ms(0), -3.0}});
    const TrackId verb = addSubmix(p, ChannelFormat::Stereo,
                                   {effect(p, "wabcut.audio.reverb", {{"mix", ParamValue(1.0)}, {"decay", ParamValue(800.0)}})}, -4.0);
    const TrackId bus = addSubmix(p, ChannelFormat::Stereo,
                                  {effect(p, "wabcut.audio.compressor", {{"threshold", ParamValue(-20.0)}}),
                                   effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-2.0)}})});
    const TrackId mono = addSubmix(p, ChannelFormat::Mono, {}, -2.0, 0.4);
    changeTrack(p, 0, [&](Track& t) {
        t.output = bus;
        t.volume = ParamValue(Curve{{Keyframe{ms(200), 0.0}, Keyframe{ms(1500), -10.0}}});
        TrackSend send = sendTo(verb, -6.0);
        send.level = ParamValue(Curve{{Keyframe{ms(0), -20.0}, Keyframe{ms(1000), 0.0}}});
        t.sends.push_back(send);
        t.sends.push_back(sendTo(mono, -9.0, true));
    });
    changeTrack(p, 1, [&](Track& t) { t.sends.push_back(sendTo(verb, -10.0, true)); });
    changeTrack(p, 0, [&](Track& t) { t.output = bus; }, true); // the reverb into the bus with the limiter
    Sequence seq = *p.state.sequences.front();
    seq.audioEffects = {effect(p, "wabcut.audio.limiter", {{"ceiling", ParamValue(-1.0)}})};
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    REQUIRE_FALSE(audioRoutingProblem(p.sequenceRef()).has_value());
    SECTION("from the start") {
        Rig rig(p);
        rig.player->play();
        rig.pullFor(2300);
        const auto offline = exported(p, Ticks{}, 2300 * 48);
        CHECK(firstMismatch(rig.stream, 0, offline, 0, offline.size()) == -1);
        CHECK(rig.player->stats().underrunFrames == 0);
        float loudest = 0.0f;
        for (const float v : offline) {
            loudest = std::max(loudest, std::fabs(v));
        }
        CHECK(loudest > 0.01f);
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

TEST_CASE("Routing changes while playing are heard without restarting the stream", "[playback][submix]") {
    Project p;
    p.media('A', level(0.2f));
    p.addTrack({{'A', ms(0), ms(3000), ms(0)}});
    const TrackId s1 = addSubmix(p, ChannelFormat::Stereo, {}, -6.0);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(300);
    CHECK(at(rig.stream, rig.stream.size() / 2 - 1) == Approx(0.2).epsilon(1e-5));
    // Assign A1 to S1 (an edit): within a lead window the track plays through the submix's fader.
    changeTrack(p, 0, [&](Track& t) { t.output = s1; });
    REQUIRE(rig.session.commit("route", p.state));
    rig.pullFor(500);
    CHECK(at(rig.stream, rig.stream.size() / 2 - 1) == Approx(0.2 * gainOf(-6.0)).epsilon(1e-5));
    // A live override of the submix's fader (an automation pass) is heard at once.
    REQUIRE(rig.player->setLiveTrackMix(s1, 0.0, std::nullopt));
    rig.pullFor(100);
    CHECK(at(rig.stream, rig.stream.size() / 2 - 1) == Approx(0.2).epsilon(1e-5));
}
