#pragma once

// Gain, pan and limiter math shared by the mixer, the meters and the UI. All functions are pure, noexcept and
// RT-safe.

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::audio {

/// Gains at or below this are treated as silence (-inf dB). Fader UIs map their bottom stop here.
inline constexpr float kSilenceDb = -144.0f;

/// dB (amplitude, 20·log10) to a linear factor. Anything <= kSilenceDb (including -inf) gives exactly 0.
inline float dbToLinear(float db) noexcept {
    if (!(db > kSilenceDb)) { // also catches NaN
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

/// Linear factor to dB. Values <= dbToLinear(kSilenceDb) (and negative or NaN input) return kSilenceDb.
inline float linearToDb(float linear) noexcept {
    constexpr float kFloor = 6.3095734e-8f; // 10^(-144/20)
    if (!(linear > kFloor)) {
        return kSilenceDb;
    }
    return 20.0f * std::log10(linear);
}

/// Left/right gain pair.
struct StereoGains {
    float left = 1.0f;
    float right = 1.0f;
};

/// Constant-power pan law for a mono signal into stereo, -3 dB at centre.
/// `pan` is clamped to [-1 (hard left), +1 (hard right)]. With theta = (pan + 1)·pi/4 the gains are
/// (cos theta, sin theta), so left² + right² == 1 everywhere: perceived loudness stays constant across the
/// sweep, centre is 1/sqrt(2) (-3.01 dB) per side, and the hard positions are (1, 0) and (0, 1).
inline StereoGains constantPowerPan(float pan) noexcept {
    const double p = std::isnan(pan) ? 0.0 : std::clamp(static_cast<double>(pan), -1.0, 1.0);
    const double theta = (p + 1.0) * (std::numbers::pi / 4.0);
    return {static_cast<float>(std::cos(theta)), static_cast<float>(std::sin(theta))};
}

/// Balance for a signal that is already stereo, built on the same constant-power taper but normalised so the
/// centre is unity: gains are min(1, sqrt(2)·cos theta) and min(1, sqrt(2)·sin theta). Centre leaves a stereo
/// clip untouched (as in Premiere Pro; computed in double so it is exactly 1.0f); moving towards one side
/// attenuates the other along the -3 dB curve until it is silent at the hard position, while the near side
/// never exceeds unity.
inline StereoGains stereoBalance(float pan) noexcept {
    const double p = std::isnan(pan) ? 0.0 : std::clamp(static_cast<double>(pan), -1.0, 1.0);
    const double theta = (p + 1.0) * (std::numbers::pi / 4.0);
    constexpr double kSqrt2 = std::numbers::sqrt2;
    return {static_cast<float>(std::min(1.0, kSqrt2 * std::cos(theta))),
            static_cast<float>(std::min(1.0, kSqrt2 * std::sin(theta)))};
}

/// Where the soft clip guard starts to bend: -1 dBFS.
inline constexpr float kSoftClipThreshold = 0.89125094f;

/// Output guard against overs. Transparent (bit-exact) for |x| <= kSoftClipThreshold. Above it, the excess is
/// compressed with a tanh knee: y = sign(x)·(t + (1 - t)·tanh((|x| - t) / (1 - t))). That is continuous in
/// value and slope at the threshold, monotonic, odd-symmetric and bounded: |y| <= 1 for every input,
/// including ±inf (in exact arithmetic 1 is only approached; in float it is reached for |x| above ~1.87,
/// i.e. +5.4 dBFS). NaN becomes 0 so a misbehaving source can never send NaN to the device.
inline float softClip(float x) noexcept {
    constexpr float t = kSoftClipThreshold;
    if (std::isnan(x)) {
        return 0.0f;
    }
    const float a = std::fabs(x);
    if (a <= t) {
        return x;
    }
    const float y = t + (1.0f - t) * std::tanh((a - t) / (1.0f - t));
    return std::copysign(std::min(y, 1.0f), x);
}

} // namespace wabcut::audio
