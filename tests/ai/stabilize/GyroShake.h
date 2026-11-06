#pragma once

// A known synthetic shake for the gyro tests (0.7, card #130): a camera that pans right at 0.1 rad/s while it jitters at 5 to
// 13 Hz by up to 0.04 rad, and the gyroscope that would record it, so what the stabilizer makes of the gyro can be checked
// against the truth.

#include <wabcut/ai/stabilize/GyroMotion.h>
#include <wabcut/gyro/Gyro.h>
#include <wabcut/gyro/Rotation.h>

#include <cmath>
#include <numbers>

namespace wabcut::test::gyro_shake {

using wabcut::gyro::Camera;
using wabcut::gyro::GyroTrack;
using wabcut::gyro::Quat;
using wabcut::gyro::Vec3;

/// The camera's rotation angles about x, y, z at time t (a rotation vector), zero jitter at t = 0 only for x.
inline Vec3 shakeAngles(double t) {
    const double pan = 0.1 * t;
    return {0.020 * std::sin(2.0 * std::numbers::pi * 7.0 * t) + 0.010 * std::sin(2.0 * std::numbers::pi * 11.0 * t + 1.0),
            pan + 0.026 * std::sin(2.0 * std::numbers::pi * 9.0 * t + 0.5) + 0.012 * std::sin(2.0 * std::numbers::pi * 13.0 * t),
            0.008 * std::sin(2.0 * std::numbers::pi * 5.0 * t + 2.0)};
}
/// The same camera without the jitter: what a perfect stabilizer shows.
inline Vec3 panAngles(double t) { return {0.0, 0.1 * t, 0.0}; }

inline Quat orientationOf(const Vec3& angles) { return wabcut::gyro::fromRotationVector(angles); }

/// The gyroscope of the shake: body rates from the orientation chain, `rate` readings per second from `from` to `to`; the
/// clock of the gyro runs `delay` seconds behind the picture's, and the readings' axes are turned by `axes` (index of
/// ai::stabilize::applyGyroAxes; 0 leaves them).
inline GyroTrack gyroOfShake(double from, double to, double rate, double delay = 0.0, int axes = 0) {
    GyroTrack track;
    track.camera = Camera::GoPro;
    track.make = "GoPro";
    track.model = "HERO-Test";
    track.format = "GoPro GPMF";
    track.axesChecked = true;
    const double h = 1e-4;
    for (double t = from; t <= to + 1e-9; t += 1.0 / rate) {
        const Quat a = orientationOf(shakeAngles(t - h));
        const Quat b = orientationOf(shakeAngles(t + h));
        Vec3 w = wabcut::gyro::toRotationVector(wabcut::gyro::conjugate(a) * b);
        w = {w.x / (2 * h), w.y / (2 * h), w.z / (2 * h)};
        track.times.push_back(t + delay);
        track.omega.push_back(axes == 0 ? w : wabcut::ai::stabilize::applyGyroAxes(axes, w));
    }
    track.rateHz = rate;
    return track;
}

} // namespace wabcut::test::gyro_shake
