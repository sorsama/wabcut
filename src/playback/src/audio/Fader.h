#pragma once

// Internal: a level (dB) and a stereo balance applied to interleaved audio on the parameter grid. Values are taken at
// cell boundaries (audio::kParamCellFrames frames counted from the stream's origin) and ramped linearly in between,
// sample by sample, so the result does not depend on how the stream is cut into blocks. Shared by the clip
// Volume/Panner and the track fader (TrackRenderer, feeder thread) and the master fader (MasterBus, audio thread):
// preview and export apply them identically. RT-safe (no allocation; ParamValue::valueAt does not allocate).
//
// Pan law (A.3): the balance is audio::stereoBalance, the unity-centre balance on the constant-power taper. Centre
// leaves the stereo signal untouched; towards a side the far channel follows the -3 dB constant-power curve down to
// silence at the hard position, the near side stays at unity. Channels beyond the first two get the level only.

#include <wabcut/audio/Gain.h>
#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/Parameters.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wabcut::playback::detail {

/// dB to a linear factor; at or below the silence floor (including -inf) exactly 0.
inline double faderGain(double db) noexcept { return db <= audio::kSilenceDb ? 0.0 : std::pow(10.0, db / 20.0); }

/// Applies `volume` (dB) and `pan` (balance; null = none) to output frames [from, to) of the stream at `dst`
/// (interleaved, `channels` channels; `dst` holds frame `from`). `curveTimeOf(frame)` maps a frame number (counted
/// from the stream's origin) to the parameters' curve time.
template <typename CurveTimeOf>
void applyFader(const ParamValue& volume, const ParamValue* pan, std::int64_t from, std::int64_t to, float* dst,
                int channels, CurveTimeOf curveTimeOf) noexcept {
    if (to <= from || channels <= 0) {
        return;
    }
    const auto ch = static_cast<std::size_t>(channels);
    struct Point {
        double gain = 1.0;
        audio::StereoGains balance;
    };
    const auto at = [&](std::int64_t f) {
        const Ticks t = curveTimeOf(f);
        Point p;
        p.gain = faderGain(volume.valueAt(t));
        if (pan != nullptr) {
            p.balance = audio::stereoBalance(static_cast<float>(pan->valueAt(t)));
        }
        return p;
    };
    // Float arithmetic exactly as the clip Volume/Panner always did (existing renders stay bit-identical).
    const auto apply = [&](float* frame, float g, float l, float r) {
        if (ch >= 2) {
            frame[0] *= g * l;
            frame[1] *= g * r;
            for (std::size_t c = 2; c < ch; ++c) {
                frame[c] *= g;
            }
        } else {
            frame[0] *= g;
        }
    };
    if (volume.isConstant() && (pan == nullptr || pan->isConstant())) {
        const Point p = at(from);
        if (p.gain == 1.0 && p.balance.left == 1.0f && p.balance.right == 1.0f) {
            return;
        }
        const auto g = static_cast<float>(p.gain);
        for (std::int64_t k = 0; k < to - from; ++k) {
            apply(dst + static_cast<std::size_t>(k) * ch, g, p.balance.left, p.balance.right);
        }
        return;
    }
    constexpr std::int64_t cell = audio::kParamCellFrames;
    std::int64_t f = from;
    while (f < to) {
        const std::int64_t c0 = (f >= 0 ? f / cell : -((-f + cell - 1) / cell)) * cell;
        const std::int64_t c1 = c0 + cell;
        const Point a = at(c0);
        const Point b = at(c1);
        const std::int64_t end = to < c1 ? to : c1;
        for (; f < end; ++f) {
            const double u = static_cast<double>(f - c0) / cell;
            apply(dst + static_cast<std::size_t>(f - from) * ch, static_cast<float>(a.gain + (b.gain - a.gain) * u),
                  static_cast<float>(a.balance.left + (b.balance.left - a.balance.left) * u),
                  static_cast<float>(a.balance.right + (b.balance.right - a.balance.right) * u));
        }
    }
}

} // namespace wabcut::playback::detail
