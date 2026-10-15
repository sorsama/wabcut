// A.9 audio channels in the engine: channel maps (Modify > Audio Channels presets, mutes, per-channel volume), track
// formats (mono, adaptive, a 5.1 source on a stereo track), the 5.1 master with its panner and LFE send, the ITU-R
// BS.775 downmix, preview == offline, and a master switched to 5.1 while playing. Wave-5 review fixes: the panner on
// the parameter grid (A5-P2-1), the downmix guarded (A5-P2-2), the format change under a voice-over wrap (A5-P2-3),
// effects at the track's format before its panner (A5-P2-10).

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/ChannelLayout.h>
#include <wabcut/playback/audio/OfflineMixdown.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <wabcut/audio/AudioSource.h>

#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/Effects.h>

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// A source with a distinct sample on every channel: channel c = ramp · gains[c].
SyntheticSignal multichannel(std::vector<float> gains) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 1.0;
    s.amplitude = 0.5f;
    s.channelGains = std::move(gains);
    return s;
}

void setStreamChannels(Project& p, char letter, int channels) {
    for (MediaPtr& m : p.state.media) {
        if (m->id == p.mediaId(letter)) {
            MediaItem copy = *m;
            copy.info.audio.front().channels = channels;
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
}

void editTrack(Project& p, std::size_t index, const std::function<void(Track&)>& change) {
    Track t = p.track(index);
    change(t);
    p.state = Project::withTrack(p.state, index, std::move(t));
}

void setClipMap(Project& p, std::size_t track, AudioChannelMap map) {
    editTrack(p, track, [&](Track& t) {
        for (std::size_t i = 0; i < t.clips.size(); ++i) {
            ClipContent content = t.clips[i].content();
            content.channels = map;
            t.clips.mutableAt(i).setContent(std::move(content));
        }
    });
}

void setMaster(Project& p, ChannelFormat format) {
    Sequence seq = *p.state.sequences.front();
    seq.masterFormat = format;
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

/// The offline mix (what export renders), with the project (media maps and channel counts).
std::vector<float> offline(const Project& p, int channels, std::size_t frames) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.project = std::make_shared<const ProjectState>(p.state);
    OfflineMixdown mix(p.state.sequences.front(), channels, 48000, Ticks{}, *p.feed, options);
    std::vector<float> out(frames * static_cast<std::size_t>(channels));
    mix.render(out);
    return out;
}

float src(const Project& p, std::int64_t n, int channel) { return p.feed->sample(p.mediaId('A'), 0, n, channel, k48k); }

std::int64_t firstMismatch(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t samples) {
    for (std::size_t i = from; i < samples; ++i) {
        if (a[i] != b[i]) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

Project stereoProject() {
    Project p;
    p.media('A', multichannel({1.0f, -0.5f}));
    p.addTrack({{'A', Ticks{}, ms(2000), Ticks{}}});
    return p;
}

/// Plays another source as it is: what a voice-over pass's source does with the program (VoiceOverRecorder).
class Forward final : public audio::AudioSource {
public:
    explicit Forward(std::shared_ptr<audio::AudioSource> inner) : m_inner(std::move(inner)) {}
    void render(std::span<float> out, int channels, Rate rate, Ticks start) noexcept override {
        m_inner->render(out, channels, rate, start);
    }
    bool readyFor(std::uint64_t generation) const noexcept override { return m_inner->readyFor(generation); }

private:
    std::shared_ptr<audio::AudioSource> m_inner;
};

double rmsOf(const std::vector<float>& s, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(s[i]) * s[i];
    }
    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

void setClipEffects(Project& p, std::size_t track, EffectStack effects) {
    editTrack(p, track, [&](Track& t) {
        for (std::size_t i = 0; i < t.clips.size(); ++i) {
            t.clips.mutableAt(i).setEffects(effects);
        }
    });
}

EffectInstance audioEffect(Project& p, std::string id, std::vector<EffectParam> params) {
    EffectInstance e;
    e.id = p.state.ids.allocate<EffectInstanceId>();
    e.effect = std::move(id);
    for (EffectParam& param : params) {
        e.set(std::move(param.key), std::move(param.value));
    }
    return e;
}

} // namespace

TEST_CASE("Channel map presets route the source channels sample for sample", "[playback][channels]") {
    struct Case {
        const char* name;
        AudioChannelMap map;
        std::function<std::pair<float, float>(float, float)> expected; // (left, right) of source (L, R)
    };
    AudioChannelMap mutedRight = AudioChannelMap::identity(2);
    mutedRight.channels[1].muted = true;
    const std::vector<Case> cases = {
        {"identity", AudioChannelMap::identity(2), [](float l, float r) { return std::pair{l, r}; }},
        {"fill left with right", AudioChannelMap::fill(1), [](float, float r) { return std::pair{r, r}; }},
        {"fill right with left", AudioChannelMap::fill(0), [](float l, float) { return std::pair{l, l}; }},
        {"swap", AudioChannelMap::swap(), [](float l, float r) { return std::pair{r, l}; }},
        {"mono of channel 2", AudioChannelMap::mono(1), [](float, float r) { return std::pair{kMinus3dB * r, kMinus3dB * r}; }},
        {"sum to mono", AudioChannelMap::sumToMono(2),
         [](float l, float r) { return std::pair{kMinus3dB * ((l + r) * 0.5f), kMinus3dB * ((l + r) * 0.5f)}; }},
        {"right muted", mutedRight, [](float l, float) { return std::pair{l, 0.0f}; }},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        Project p = stereoProject();
        setClipMap(p, 0, c.map);
        const std::vector<float> out = offline(p, 2, 4800);
        std::size_t bad = 0;
        for (std::int64_t k = 0; k < 4800; ++k) {
            const auto [l, r] = c.expected(src(p, k, 0), src(p, k, 1));
            if (out[static_cast<std::size_t>(k) * 2] != Approx(l).margin(1e-7) ||
                out[static_cast<std::size_t>(k) * 2 + 1] != Approx(r).margin(1e-7)) {
                ++bad;
            }
        }
        CHECK(bad == 0);
    }
    // The identity map plays exactly what the unmapped clip plays.
    Project plain = stereoProject();
    Project identity = stereoProject();
    setClipMap(identity, 0, AudioChannelMap::identity(2));
    CHECK(firstMismatch(offline(plain, 2, 4800), offline(identity, 2, 4800), 0, 9600) == -1);
}

TEST_CASE("A media item's map applies to its clips; the clip's own map wins", "[playback][channels]") {
    Project p = stereoProject();
    for (MediaPtr& m : p.state.media) {
        MediaItem copy = *m;
        copy.channels = AudioChannelMap::swap();
        m = std::make_shared<const MediaItem>(std::move(copy));
    }
    std::vector<float> out = offline(p, 2, 480);
    CHECK(out[200] == src(p, 100, 1));
    CHECK(out[201] == src(p, 100, 0));
    setClipMap(p, 0, AudioChannelMap::fill(0));
    out = offline(p, 2, 480);
    CHECK(out[201] == src(p, 100, 0));
    CHECK(out[200] == src(p, 100, 0));
}

TEST_CASE("Per-channel volume is keyframable on the parameter grid", "[playback][channels]") {
    Project p = stereoProject();
    AudioChannelMap map = AudioChannelMap::identity(2);
    Curve keys;
    keys.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{Ticks::fromSeconds(1), -20.0}};
    map.channels[0].gainDb = ParamValue(keys);
    setClipMap(p, 0, map);
    const std::vector<float> out = offline(p, 2, 48000);
    const std::size_t f = 12000; // 0.25 s: a grid point (375 x 32), -5 dB, away from the ramp's zero at 0.5 s
    CHECK(out[f * 2] == Approx(src(p, f, 0) * std::pow(10.0, -5.0 / 20.0)).epsilon(1e-5));
    CHECK(out[f * 2 + 1] == src(p, f, 1)); // the other channel untouched
    // Between grid points the gain is the blend of the grid points' gains.
    const double g0 = out[f * 2] / src(p, f, 0);
    const double g32 = out[(f + 32) * 2] / src(p, f + 32, 0);
    CHECK(out[(f + 8) * 2] / src(p, f + 8, 0) == Approx(g0 + (g32 - g0) * 0.25).epsilon(1e-5));
}

TEST_CASE("Track formats: mono, adaptive, and a 5.1 source on a stereo track (ITU-R BS.775)", "[playback][channels]") {
    {
        INFO("a stereo source on a mono track");
        Project p = stereoProject();
        editTrack(p, 0, [](Track& t) { t.format = ChannelFormat::Mono; });
        const std::vector<float> out = offline(p, 2, 480);
        const float mono = 0.5f * src(p, 300, 0) + 0.5f * src(p, 300, 1);
        CHECK(out[600] == Approx(kMinus3dB * mono).margin(1e-7));
        CHECK(out[601] == Approx(kMinus3dB * mono).margin(1e-7));
    }
    const std::vector<float> gains{1.0f, 0.5f, 0.25f, 0.9f, 0.125f, 0.0625f}; // L R C LFE Ls Rs
    {
        INFO("a 5.1 source on a stereo track");
        Project p;
        p.media('A', multichannel(gains));
        setStreamChannels(p, 'A', 6);
        p.addTrack({{'A', Ticks{}, ms(500), Ticks{}}});
        setClipMap(p, 0, AudioChannelMap::identity(6));
        const std::vector<float> out = offline(p, 2, 480);
        const auto s = [&](int c) { return src(p, 200, c); };
        CHECK(out[400] == Approx(s(0) + kMinus3dB * s(2) + kMinus3dB * s(4)).margin(1e-6));
        CHECK(out[401] == Approx(s(1) + kMinus3dB * s(2) + kMinus3dB * s(5)).margin(1e-6));
    }
    {
        INFO("a 5.1 source on an adaptive track: channels pass through one to one");
        Project p;
        p.media('A', multichannel(gains));
        setStreamChannels(p, 'A', 6);
        p.addTrack({{'A', Ticks{}, ms(500), Ticks{}}});
        editTrack(p, 0, [](Track& t) { t.format = ChannelFormat::Adaptive; });
        const std::vector<float> out = offline(p, 2, 480);
        CHECK(out[400] == src(p, 200, 0));
        CHECK(out[401] == src(p, 200, 1));
    }
    // The downmix coefficients themselves.
    const ChannelMatrix down = formatMatrix(ChannelFormat::Surround51, 6, ChannelFormat::Stereo, 2);
    CHECK(down.at(0, 0) == 1.0f);
    CHECK(down.at(0, 2) == kMinus3dB);
    CHECK(down.at(0, 3) == 0.0f); // the LFE is not in a downmix
    CHECK(down.at(0, 4) == kMinus3dB);
    CHECK(down.at(0, 5) == 0.0f);
    CHECK(down.at(1, 1) == 1.0f);
    CHECK(down.at(1, 5) == kMinus3dB);
}

TEST_CASE("The 5.1 panner: speaker positions, constant power, the LFE send", "[playback][channels][surround]") {
    constexpr int L = 0, R = 1, C = 2, LFE = 3, Ls = 4, Rs = 5;
    const auto power = [](const std::array<float, 6>& g) {
        double sum = 0.0;
        for (const float v : g) {
            sum += static_cast<double>(v) * v;
        }
        return sum;
    };
    // At the rim a direction plays from its speaker (or the pair around it).
    const std::array<float, 6> front = surroundGains(0.0, 1.0);
    CHECK(front[C] == 1.0f);
    CHECK(front[L] == 0.0f);
    const std::array<float, 6> left = surroundGains(-30.0, 1.0);
    CHECK(left[L] == 1.0f);
    CHECK(left[C] == 0.0f);
    CHECK(surroundGains(110.0, 1.0)[Rs] == 1.0f);
    CHECK(surroundGains(-110.0, 1.0)[Ls] == 1.0f);
    const std::array<float, 6> between = surroundGains(15.0, 1.0); // between C and R
    CHECK(between[C] > 0.5f);
    CHECK(between[R] > 0.5f);
    CHECK(between[L] == 0.0f);
    CHECK(power(between) == Approx(1.0).epsilon(1e-5));
    const std::array<float, 6> behind = surroundGains(180.0, 1.0); // between Rs and Ls
    CHECK(behind[Ls] == Approx(behind[Rs]).epsilon(1e-5));
    CHECK(power(behind) == Approx(1.0).epsilon(1e-5));
    // At the centre all five speakers equally; the power always sums to 1; the LFE gets nothing from the panner.
    const std::array<float, 6> centre = surroundGains(0.0, 0.0);
    for (const int s : {L, R, C, Ls, Rs}) {
        CHECK(centre[static_cast<std::size_t>(s)] == Approx(1.0 / std::sqrt(5.0)).epsilon(1e-5));
    }
    std::mt19937 random(7);
    std::uniform_real_distribution<double> angle(-180.0, 180.0);
    std::uniform_real_distribution<double> focus(0.0, 1.0);
    for (int i = 0; i < 200; ++i) {
        const auto g = surroundGains(angle(random), focus(random));
        CHECK(power(g) == Approx(1.0).epsilon(1e-4));
        CHECK(g[LFE] == 0.0f);
    }
    // The track panner: a stereo input at front centre plays L and R; mono plays C; the LFE send is the inputs' mean.
    const ChannelMatrix stereo = surroundPanMatrix(2, 0.0, 1.0, 0.0);
    CHECK(stereo.at(L, 0) == 1.0f);
    CHECK(stereo.at(R, 1) == 1.0f);
    CHECK(stereo.at(C, 0) == 0.0f);
    const ChannelMatrix mono = surroundPanMatrix(1, 0.0, 1.0, 0.5);
    CHECK(mono.at(C, 0) == 1.0f);
    CHECK(mono.at(LFE, 0) == 0.5f);
    const ChannelMatrix rearLeft = surroundPanMatrix(1, -1.0, -1.0, 0.0); // the puck at the back left corner
    CHECK(rearLeft.at(Ls, 0) > 0.85f);
    CHECK(rearLeft.at(Ls, 0) > rearLeft.at(Rs, 0));
}

TEST_CASE("A 5.1 master: stereo and mono tracks through their panners, downmixed for stereo", "[playback][channels][surround]") {
    Project p;
    p.media('A', multichannel({1.0f, -0.5f}));
    p.addTrack({{'A', Ticks{}, ms(1000), Ticks{}}});                  // A1 stereo
    p.addTrack({{'A', Ticks{}, ms(1000), Ticks{}}});                  // A2 mono, LFE send
    editTrack(p, 1, [](Track& t) {
        t.format = ChannelFormat::Mono;
        t.surround.lfeDb = ParamValue(-6.0);
    });
    setMaster(p, ChannelFormat::Surround51);
    const std::vector<float> six = offline(p, 6, 4800);
    const std::size_t k = 1000;
    const float l = src(p, static_cast<std::int64_t>(k), 0);
    const float r = src(p, static_cast<std::int64_t>(k), 1);
    const float m = 0.5f * l + 0.5f * r;
    CHECK(six[k * 6 + 0] == Approx(l).margin(1e-7)); // A1's left on L
    CHECK(six[k * 6 + 1] == Approx(r).margin(1e-7)); // A1's right on R
    CHECK(six[k * 6 + 2] == Approx(m).margin(1e-7)); // A2 (mono) on C
    CHECK(six[k * 6 + 3] == Approx(m * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-5)); // A2's LFE send
    CHECK(six[k * 6 + 4] == 0.0f);
    CHECK(six[k * 6 + 5] == 0.0f);
    // A stereo export of the 5.1 master is its ITU-R BS.775 downmix.
    const std::vector<float> two = offline(p, 2, 4800);
    std::vector<float> expected(two.size());
    formatMatrix(ChannelFormat::Surround51, 6, ChannelFormat::Stereo, 2).apply(six.data(), expected.data(), 4800);
    CHECK(firstMismatch(two, expected, 0, two.size()) == -1);

    // Moving A2's puck to the left: its sound moves to L and Ls, at the same power.
    editTrack(p, 1, [](Track& t) {
        t.surround.x = ParamValue(-1.0);
        t.surround.y = ParamValue(0.0);
        t.surround.lfeDb = ParamValue(-144.0);
    });
    editTrack(p, 0, [](Track& t) { t.muted = true; });
    const std::vector<float> moved = offline(p, 6, 4800);
    double energy = 0.0;
    for (int c = 0; c < 6; ++c) {
        energy += static_cast<double>(moved[k * 6 + static_cast<std::size_t>(c)]) * moved[k * 6 + static_cast<std::size_t>(c)];
    }
    CHECK(energy == Approx(static_cast<double>(m) * m).epsilon(1e-4));
    CHECK(std::fabs(moved[k * 6 + 0]) > 0.0f);
    CHECK(std::fabs(moved[k * 6 + 4]) > 0.0f);
    CHECK(moved[k * 6 + 1] == 0.0f);
    CHECK(moved[k * 6 + 2] == 0.0f);
}

TEST_CASE("Preview equals offline: mapped clips, track formats and a 5.1 master downmixed for a stereo device",
          "[playback][channels][export]") {
    Project p;
    p.media('A', multichannel({1.0f, -0.5f}));
    p.addTrack({{'A', Ticks{}, ms(1500), Ticks{}}});
    p.addTrack({{'A', ms(100), ms(1000), Ticks{}}});
    setClipMap(p, 0, AudioChannelMap::fill(1));
    editTrack(p, 1, [](Track& t) {
        t.format = ChannelFormat::Mono;
        t.surround.x = ParamValue(0.6);
        t.surround.y = ParamValue(0.2);
    });
    for (const ChannelFormat master : {ChannelFormat::Stereo, ChannelFormat::Surround51}) {
        INFO(toString(master));
        setMaster(p, master);
        Rig rig(p);
        rig.player->play();
        rig.pullFor(1000);
        const std::vector<float> reference = offline(p, 2, 48000);
        CHECK(firstMismatch(rig.stream, reference, 0, reference.size()) == -1);
        CHECK(rig.player->stats().underrunFrames == 0);
        if (master == ChannelFormat::Surround51) {
            CHECK(rig.player->loudness().frames > 0); // the live tap measures the 5.1 mix
        }
    }
}

TEST_CASE("Switching the master to 5.1 while playing rebuilds the mix and keeps playing", "[playback][channels][surround]") {
    Project p = stereoProject();
    editTrack(p, 0, [](Track& t) {
        t.surround.x = ParamValue(1.0);
        t.surround.y = ParamValue(0.0); // on a 5.1 master the track plays from the right: R and Rs
    });
    Rig rig(p);
    rig.player->play();
    rig.pullFor(200);
    ProjectState next = *rig.session.snapshot();
    Sequence seq = *next.sequences.front();
    seq.masterFormat = ChannelFormat::Surround51;
    next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    REQUIRE(rig.session.commit("5.1", next));
    rig.player->feedOnce();
    rig.pullFor(600);
    // Well after the switch the stereo device hears the downmix of the 5.1 mix.
    Project surround = p;
    setMaster(surround, ChannelFormat::Surround51);
    const std::vector<float> reference = offline(surround, 2, 48000);
    const std::size_t from = 2 * 48 * 500; // 500 ms
    CHECK(firstMismatch(rig.stream, reference, from, 2 * 48 * 800) == -1);
    // The track's right-side placement: much less on the left of the downmix (at 0.625 s, away from the ramp's zero).
    const std::size_t at = 2 * 30000;
    CHECK(std::fabs(rig.stream[at]) < std::fabs(rig.stream[at + 1]));
}

TEST_CASE("A keyframed 5.1 panner moves on the parameter grid: the same samples in 37- or 1024-frame renders",
          "[playback][channels][surround]") {
    // Review A5-P2-1: the panner was evaluated once per render call (at the call's first frame), so its gains stepped
    // at every block boundary and renders cut into other blocks (the feeder's chunks, export's 1024) differed.
    Project p = stereoProject();
    editTrack(p, 0, [](Track& t) {
        t.format = ChannelFormat::Mono;
        Curve sweep;
        sweep.keys = {Keyframe{Ticks{}, -1.0}, Keyframe{ms(1000), 1.0}};
        t.surround.x = ParamValue(sweep);
        t.surround.y = ParamValue(0.0);
    });
    const Track track = p.track(0);
    constexpr std::size_t kFrames = 48000;
    const auto render = [&](std::size_t piece) {
        TrackRenderer renderer(6, k48k);
        renderer.setProject(std::make_shared<const ProjectState>(p.state));
        TrackRenderStats stats;
        std::vector<float> out(kFrames * 6);
        for (std::size_t done = 0; done < kFrames; done += piece) {
            const std::size_t n = std::min(piece, kFrames - done);
            const std::span<float> piece6(out.data() + done * 6, n * 6);
            renderer.render(track, frames(static_cast<std::int64_t>(done)), piece6, *p.feed, stats);
        }
        return out;
    };
    const std::vector<float> big = render(1024);
    const std::vector<float> small = render(37);
    CHECK(firstMismatch(big, small, 0, big.size()) == -1);
    // At a grid point (0.25 s = 375 cells) the gains are the panner's at that time exactly: the puck at x = -0.5.
    const std::size_t k = 12000;
    const ChannelMatrix expected = surroundPanMatrix(1, -0.5, 0.0, 0.0);
    const float m = 0.5f * src(p, static_cast<std::int64_t>(k), 0) + 0.5f * src(p, static_cast<std::int64_t>(k), 1);
    for (int c = 0; c < 6; ++c) {
        INFO(c);
        CHECK(big[k * 6 + static_cast<std::size_t>(c)] == Approx(expected.at(c, 0) * m).margin(1e-6));
    }
}

TEST_CASE("Preview equals offline with moving 5.1 panners", "[playback][channels][surround][export]") {
    // Review A5-P2-1: preview evaluated the panners once per feeder chunk, export once per 1024-frame block.
    Project p;
    p.media('A', multichannel({1.0f, -0.5f}));
    p.addTrack({{'A', Ticks{}, ms(1500), Ticks{}}});
    p.addTrack({{'A', ms(100), ms(1000), Ticks{}}});
    editTrack(p, 0, [](Track& t) {
        Curve back;
        back.keys = {Keyframe{Ticks{}, 1.0}, Keyframe{ms(1000), -1.0}};
        t.surround.y = ParamValue(back);
    });
    editTrack(p, 1, [](Track& t) {
        t.format = ChannelFormat::Mono;
        Curve across;
        across.keys = {Keyframe{ms(100), -1.0}, Keyframe{ms(900), 1.0}};
        t.surround.x = ParamValue(across);
        t.surround.y = ParamValue(0.3);
        Curve lfe;
        lfe.keys = {Keyframe{ms(100), -40.0}, Keyframe{ms(900), -6.0}};
        t.surround.lfeDb = ParamValue(lfe);
    });
    setMaster(p, ChannelFormat::Surround51);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(1000);
    const std::vector<float> reference = offline(p, 2, 48000);
    CHECK(firstMismatch(rig.stream, reference, 0, reference.size()) == -1);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("A stereo render of a loud 5.1 master stays within full scale (the downmix is guarded)",
          "[playback][channels][surround]") {
    // Review A5-P2-2: the ITU-R BS.775 downmix (L' = L + 0.71 C + 0.71 Ls) ran after the master's soft clip guard, so a
    // 5.1 mix near full scale on L, C and Ls reached 2.2 in a stereo export and on a stereo device.
    Project p;
    SyntheticSignal loud;
    loud.kind = SyntheticSignal::Kind::Sine;
    loud.frequency = 440.0;
    loud.amplitude = 0.9f;
    loud.channelGains = {1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f}; // L, C, Ls
    p.media('A', loud);
    setStreamChannels(p, 'A', 6);
    p.addTrack({{'A', Ticks{}, ms(1000), Ticks{}}});
    setClipMap(p, 0, AudioChannelMap::identity(6));
    editTrack(p, 0, [](Track& t) { t.format = ChannelFormat::Surround51; });
    setMaster(p, ChannelFormat::Surround51);
    const auto peak = [](const std::vector<float>& s, std::size_t to) {
        float worst = 0.0f;
        for (std::size_t i = 0; i < std::min(to, s.size()); ++i) {
            worst = std::max(worst, std::fabs(s[i]));
        }
        return worst;
    };
    const std::vector<float> six = offline(p, 6, 24000);
    CHECK(peak(six, six.size()) > 0.85f); // the 5.1 mix itself is near full scale
    const std::vector<float> two = offline(p, 2, 24000);
    CHECK(peak(two, two.size()) <= 1.0f);
    CHECK(peak(two, two.size()) > 0.9f);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(500);
    CHECK(peak(rig.stream, rig.stream.size()) <= 1.0f);
    // Preview still equals export.
    CHECK(firstMismatch(rig.stream, two, 0, two.size()) == -1);
}

TEST_CASE("On a 5.1 master a clip's reverb runs at its track's channels: nothing reaches the LFE or other speakers",
          "[playback][channels][surround][effects]") {
    // Review A5-P2-10: the 5.1 panner ran at the source read, so the clip's reverb ran on six channels: it took the
    // mean of all six and wrote its tail to every channel, the LFE included (and a stereo plug-in saw six channels).
    for (const ChannelFormat format : {ChannelFormat::Stereo, ChannelFormat::Mono}) {
        INFO(toString(format));
        Project p = stereoProject();
        editTrack(p, 0, [&](Track& t) { t.format = format; });
        setClipEffects(p, 0,
                       {audioEffect(p, "wabcut.audio.reverb", {{"mix", ParamValue(1.0)}, {"decay", ParamValue(800.0)}})});
        setMaster(p, ChannelFormat::Surround51);
        const std::vector<float> six = offline(p, 6, 24000);
        // Default puck (front centre): a stereo track on L and R, a mono one on C; nothing anywhere else.
        const std::vector<int> heard = format == ChannelFormat::Stereo ? std::vector<int>{0, 1} : std::vector<int>{2};
        for (int c = 0; c < 6; ++c) {
            INFO(c);
            double energy = 0.0;
            for (std::size_t f = 0; f < 24000; ++f) {
                energy += std::fabs(six[f * 6 + static_cast<std::size_t>(c)]);
            }
            if (std::find(heard.begin(), heard.end(), c) != heard.end()) {
                CHECK(energy > 0.0);
            } else {
                CHECK(energy == 0.0);
            }
        }
    }
}

TEST_CASE("A master format change waits while a voice-over pass wraps the program, then applies",
          "[playback][channels][surround]") {
    // Review A5-P2-3: the player replaced its source under the pass's wrapper. The wrapper went on playing the old
    // source, fed no more (silence for the rest of the pass), and put it back at the end (silence until a restart).
    Project p = stereoProject();
    Rig rig(p);
    rig.player->play();
    rig.pullFor(100);
    CHECK(rig.player->masterMeter().channels() == 2);
    std::shared_ptr<audio::AudioSource> wrapper;
    const auto program = rig.player->wrapEngineSource([&](std::shared_ptr<audio::AudioSource> inner) {
        wrapper = std::make_shared<Forward>(std::move(inner));
        return wrapper;
    });
    REQUIRE(program != nullptr);
    REQUIRE(rig.engine.source() == wrapper);
    ProjectState next = *rig.session.snapshot();
    Sequence seq = *next.sequences.front();
    seq.masterFormat = ChannelFormat::Surround51;
    next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    REQUIRE(rig.session.commit("5.1", next));
    const std::size_t during = rig.stream.size();
    rig.pullFor(300);
    // The program plays on under the wrapper, in the old format until the pass ends.
    CHECK(rig.engine.source() == wrapper);
    CHECK(rig.player->masterMeter().channels() == 2);
    CHECK(rmsOf(rig.stream, during, rig.stream.size()) > 0.01);
    rig.player->unwrapEngineSource(wrapper, program);
    rig.player->feedOnce(); // the format change applies now
    rig.pullFor(600);
    CHECK(rig.engine.source() != wrapper);
    CHECK(rig.engine.source() != program); // the rebuilt 5.1 source
    CHECK(rig.player->masterMeter().channels() == 6);
    const std::size_t end = rig.stream.size();
    CHECK(rmsOf(rig.stream, end - 2 * 48 * 200, end) > 0.01);
}
