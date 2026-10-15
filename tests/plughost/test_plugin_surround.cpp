// Review wave 5, A5-P1-2 end to end: on a 5.1 master the CLAP test gain sounds exactly like the built-in
// wabcut.audio.gain, on a stereo clip, on its track and on the master. A stereo track with the default puck plays on L
// and R only: C, LFE, Ls and Rs stay silent, and L/R match the built-in effect to the sample after the plugin delay
// compensation.
//
// Before the fix every chain of a 5.1 master ran at six channels and the plugin's processor clamped itself to two and
// walked the six-channel buffer as stereo frames: the first third of each buffer went through the plugin scrambled
// (L/R, C/LFE, Ls/Rs of one frame as three stereo frames) and delayed by its latency, the rest passed dry and
// undelayed. C, LFE, Ls and Rs carried the clip and L/R did not match. Now clip and track effects run at the track's
// channels before its 5.1 panner (A5-P2-10), and a plugin on the six-channel master takes L/R and passes the other
// channels through, delayed alike.

#include "PlughostFixture.h"

#include "../playback/PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <cmath>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

enum class Stage { Clip, Track, Master };

const char* nameOf(Stage stage) {
    switch (stage) {
    case Stage::Clip:
        return "on the clip";
    case Stage::Track:
        return "on the track";
    case Stage::Master:
        return "on the master";
    }
    return "?";
}

/// A1: one stereo clip for 1 s (a ramp on L, the same ramp at -0.5 times on R: every sample its own value) on a 5.1
/// master, with `gain` on `stage`.
Project surroundProject(const EffectInstance& gain, Stage stage) {
    Project p;
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 1.0;
    s.amplitude = 0.5f;
    s.channelGains = {1.0f, -0.5f};
    p.media('A', s);
    p.addTrack({{'A', Ticks{}, ms(1000), Ticks{}}});
    Track track = p.track(0);
    if (stage == Stage::Clip) {
        track.clips.mutableAt(0).setEffects({gain});
    } else if (stage == Stage::Track) {
        track.effects = {gain};
    }
    p.state = Project::withTrack(p.state, 0, track);
    Sequence seq = *p.state.sequences.front();
    seq.masterFormat = ChannelFormat::Surround51;
    if (stage == Stage::Master) {
        seq.audioEffects = {gain};
    }
    p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    return p;
}

/// The offline mix (what export renders) at the master's six channels.
std::vector<float> render51(const Project& p, std::size_t frames) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.project = std::make_shared<const ProjectState>(p.state);
    OfflineMixdown mix(p.state.sequences.front(), 6, 48000, Ticks{}, *p.feed, options);
    std::vector<float> out(frames * 6);
    mix.render(out);
    return out;
}

} // namespace

TEST_CASE("On a 5.1 master the CLAP test gain matches the built-in gain on clip and track and master stacks",
          "[plughost][channels][surround][export]") {
    TestPlugins plugins;
    EffectInstance builtin;
    builtin.id = EffectInstanceId{900};
    builtin.effect = "wabcut.audio.gain";
    builtin.set("gain", ParamValue(-6.0));
    constexpr std::size_t kFrames = 48000;
    std::uint64_t id = 901;
    for (const Stage stage : {Stage::Clip, Stage::Track, Stage::Master}) {
        INFO(nameOf(stage));
        const Project withPlugin = surroundProject(pluginEffect(plugins.clap(), id++, {{"p1", ParamValue(-6.0)}}), stage);
        const Project withBuiltin = surroundProject(builtin, stage);
        const std::vector<float> plugin = render51(withPlugin, kFrames);
        const std::vector<float> reference = render51(withBuiltin, kFrames);
        std::size_t frontWrong = 0;   // L/R frames where the plugin's mix differs from the built-in effect's
        std::size_t otherSound = 0;   // C, LFE, Ls, Rs samples that are not silent (the plugin's mix)
        std::size_t otherSoundRef = 0; // the same in the built-in effect's mix
        for (std::size_t f = 0; f < kFrames; ++f) {
            for (std::size_t c = 0; c < 6; ++c) {
                const float got = plugin[f * 6 + c];
                const float want = reference[f * 6 + c];
                if (c < 2) {
                    frontWrong += std::fabs(got - want) > 1e-6f ? 1 : 0;
                } else {
                    otherSound += got != 0.0f ? 1 : 0;
                    otherSoundRef += want != 0.0f ? 1 : 0;
                }
            }
        }
        CHECK(frontWrong == 0);
        CHECK(otherSound == 0);
        CHECK(otherSoundRef == 0);
        // Not two silences alike: the reference is the clip at -6 dB, its left on L and its right on R, where it lies.
        const std::size_t k = 1000;
        const float left = withBuiltin.feed->sample(withBuiltin.mediaId('A'), 0, static_cast<std::int64_t>(k), 0, k48k);
        const float right = withBuiltin.feed->sample(withBuiltin.mediaId('A'), 0, static_cast<std::int64_t>(k), 1, k48k);
        CHECK(std::fabs(left) > 0.1f);
        CHECK(std::fabs(reference[k * 6 + 0] - left * dbGain(-6.0)) <= 1e-6f);
        CHECK(std::fabs(reference[k * 6 + 1] - right * dbGain(-6.0)) <= 1e-6f);
    }
}
