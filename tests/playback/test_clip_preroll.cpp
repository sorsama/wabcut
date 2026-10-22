// p02 audio: a clip's time-based effects (reverb, delay) carry on when playback starts inside the clip: the voice first
// hears the clip again from up to its effects' settle time earlier (TrackRendererOptions::maxClipPreroll), so a start
// mid-clip sounds as if playback had run through (no click, no missing tail), the same in preview and export.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

SyntheticSignal noiseSignal(float amplitude, std::uint32_t seed) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Noise;
    s.amplitude = amplitude;
    s.seed = seed;
    return s;
}

std::vector<float> mixdown(const Project& p, Ticks start, std::size_t frames, Ticks clipPreroll) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.renderer.maxClipPreroll = clipPreroll;
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(frames * 2);
    mix.render(out);
    return out;
}

/// Error energy of `b` against `a[offset ..]` over `samples`, relative to `a`'s energy there, dB.
double relativeErrorDb(const std::vector<float>& a, std::size_t offset, const std::vector<float>& b, std::size_t samples) {
    double err = 0.0;
    double ref = 1e-30;
    for (std::size_t i = 0; i < samples; ++i) {
        const double d = static_cast<double>(a[offset + i]) - b[i];
        err += d * d;
        ref += static_cast<double>(a[offset + i]) * a[offset + i];
    }
    return 10.0 * std::log10(err / ref + 1e-30);
}

} // namespace

TEST_CASE("A clip reverb started inside its clip sounds as if playback had run through", "[playback][effects][preroll]") {
    Project p;
    p.media('A', noiseSignal(0.3f, 4));
    Clip clip = p.makeClip({'A', ms(0), ms(3000), ms(0)});
    EffectInstance reverb;
    reverb.id = p.state.ids.allocate<EffectInstanceId>();
    reverb.effect = "wabcut.audio.reverb";
    reverb.set("mix", ParamValue(1.0));
    reverb.set("decay", ParamValue(1200.0));
    clip.setEffects({reverb});
    Track track;
    track.id = p.state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    track.clips.push_back(std::move(clip));
    Sequence seq = *p.state.sequences.front();
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));

    const std::size_t from = 72000; // 1.5 s in
    const std::size_t length = 48000;
    const auto through = mixdown(p, Ticks{}, from + length, Ticks::fromSeconds(2));
    // The first 100 ms after the start, where a cold reverb would be missing its tail.
    const std::size_t head = 4800 * 2;
    SECTION("with the clip pre-roll: the same tail") {
        const auto started = mixdown(p, frames(static_cast<std::int64_t>(from)), length, Ticks::fromSeconds(2));
        CHECK(relativeErrorDb(through, from * 2, started, head) < -40.0);
        CHECK(relativeErrorDb(through, from * 2, started, length * 2) < -40.0);
    }
    SECTION("without it the tail is missing at first") {
        // Only the renderer's own short restart pre-roll: the long reverb tail is still building.
        const auto cold = mixdown(p, frames(static_cast<std::int64_t>(from)), length, Ticks{});
        const auto started = mixdown(p, frames(static_cast<std::int64_t>(from)), length, Ticks::fromSeconds(2));
        CHECK(relativeErrorDb(through, from * 2, cold, head) > relativeErrorDb(through, from * 2, started, head) + 10.0);
    }
    SECTION("preview starting there plays the same samples as export") {
        Rig rig(p, Rig::Options{Ticks{}, Ticks{}, false, ms(50)});
        rig.player->play();
        rig.pullFor(100);
        rig.player->seek(frames(static_cast<std::int64_t>(from)));
        const auto before = rig.stream.size() / 2;
        rig.pullFor(700);
        const auto offline = mixdown(p, frames(static_cast<std::int64_t>(from)), 24000, Ticks::fromSeconds(2));
        bool same = true;
        for (std::size_t i = 0; i < offline.size() && same; ++i) {
            same = rig.stream[(before + 480) * 2 + i] == offline[i];
        }
        CHECK(same);
    }
}
