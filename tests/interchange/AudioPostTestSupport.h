#pragma once

// Shared by the AAF and OMF export tests (P.2): a renderer that plays a known signal, and the audio-post test sequence.

#include "TimelineFixture.h"

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <vector>

namespace wabcut::interchange::test {

using wabcut::test::f;
using wabcut::test::ok;

/// A deterministic test signal, exact in binary floating point everywhere: a saw per channel from the timeline sample.
inline float signal(std::int64_t sample, int channel) {
    const std::int64_t period = 400 + 100 * channel;
    return static_cast<float>(sample % period) / static_cast<float>(period) - 0.5f;
}

/// Renders every clip as signal() of its timeline samples (what the export plan asks for), counting the requests.
class TestRenderer final : public AudioPostRenderer {
public:
    std::vector<AudioPostRenderRequest> requests;
    std::set<std::uint64_t> offline; ///< clips whose media is "offline"
    std::expected<void, Error> render(const ProjectState&, const AudioPostRenderRequest& request,
                                      const std::function<bool(std::span<const float>)>& sink) override {
        requests.push_back(request);
        if (offline.contains(request.clip.value)) {
            return std::unexpected(Error{ErrorCode::NotFound, "media offline"});
        }
        const Rate rate{request.sampleRate, 1};
        const std::int64_t first = ticksToSamples(request.range.start, rate);
        const std::int64_t frames = ticksToSamples(request.range.duration, rate);
        std::vector<float> block;
        for (std::int64_t at = 0; at < frames; at += 1000) {
            const std::int64_t n = std::min<std::int64_t>(1000, frames - at);
            block.assign(static_cast<std::size_t>(n * request.channels), 0.0f);
            for (std::int64_t i = 0; i < n; ++i) {
                for (int c = 0; c < request.channels; ++c) {
                    block[static_cast<std::size_t>(i * request.channels + c)] = signal(first + at + i, c);
                }
            }
            if (!sink(block)) {
                return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
            }
        }
        return {};
    }
};

inline std::chrono::system_clock::time_point fixedTime() {
    using namespace std::chrono;
    return sys_days{year{2026} / 10 / 15} + hours{12} + minutes{34} + seconds{56};
}

/// The audio-post test project: 25 fps, start timecode 01:00:00:00, a stereo dialogue track with a crossfade, a fade in
/// and clip volume keyframes, a mono effects track, track volume, markers.
inline wabcut::test::Timeline audioPostTimeline() {
    // B starts 5 frames into its media and C too, so they have handles; A starts at its media's first frame.
    wabcut::test::Timeline tl(R"(
        V1 |Aaaaaaaaaa..........|
        A1 |AaaaaaaaaaB5bbbbbbbb|
        A2 |....C5cccc..........|
    )");
    ProjectState& s = tl.state;
    s = ok(edit::setSequenceStartTimecode(s, tl.seq, Ticks::fromSeconds(3600))).state;
    const ClipId a = tl.clip("A1@0");
    const ClipId b = tl.clip("A1@10");
    edit::TransitionSpec cross;
    cross.outgoing = a;
    cross.incoming = b;
    cross.before = f(2);
    cross.after = f(2);
    cross.effect = "constantPower";
    s = ok(edit::addTransition(s, tl.seq, tl.track("A1"), cross)).state;
    edit::TransitionSpec fadeIn;
    fadeIn.incoming = wabcut::test::clipAt(s, tl.seq, "A2@4");
    fadeIn.after = f(3);
    fadeIn.effect = "constantGain";
    s = ok(edit::addTransition(s, tl.seq, tl.track("A2"), fadeIn)).state;
    // Clip volume: -6 dB on B, keyframes on A.
    s = ok(edit::setParameter(s, tl.seq, b, edit::ParamRef::of(ParamId::VolumeDb), ParamValue(-6.0))).state;
    Curve curve;
    curve.keys = {Keyframe{f(0), 0.0}, Keyframe{f(8), -12.0}};
    s = ok(edit::setParameter(s, tl.seq, a, edit::ParamRef::of(ParamId::VolumeDb), ParamValue(curve))).state;
    // Track volume on A2.
    s = ok(edit::setParameter(s, tl.seq, edit::EffectTarget::ofTrack(tl.track("A2")), edit::ParamRef::of(ParamId::VolumeDb),
                              ParamValue(-3.0)))
            .state;
    Marker m;
    m.start = f(5);
    m.name = "Mix note";
    m.comment = "Music up";
    s = ok(edit::addMarker(s, tl.seq, m)).state;
    return tl;
}

} // namespace wabcut::interchange::test
