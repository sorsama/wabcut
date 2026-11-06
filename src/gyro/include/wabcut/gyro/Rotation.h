#pragma once

// Rotations from the gyroscope (docs/motion.md "Stabilize with the camera's gyro"): unit quaternions, the integral of the
// angular velocity, and the camera's orientation at chosen times. Pure maths.
//
// A gyroscope measures the camera's angular velocity in the camera's own axes (body rates), so the orientation advances
// by q <- q * exp(omega * dt) (the new rotation applied in the camera frame, quaternion product on the right). Only
// differences between orientations are ever used (a frame against its neighbours or against its smoothed self), so the
// start orientation is the identity and a slow drift of the sensor's bias is part of the camera path the smoothing
// separates from the shake.

#include <wabcut/gyro/Gyro.h>

#include <array>
#include <span>
#include <vector>

namespace wabcut::gyro {

struct Quat {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool operator==(const Quat&) const = default;
};

Quat operator*(const Quat& a, const Quat& b);
Quat conjugate(const Quat& q);
Quat normalized(const Quat& q);
/// exp of the rotation vector `v` (radians, axis * angle).
Quat fromRotationVector(const Vec3& v);
/// The rotation vector of `q` (the shortest rotation: angle in [0, pi]).
Vec3 toRotationVector(const Quat& q);
/// Row-major 3x3 matrix turning vectors of the rotated frame into vectors of the reference frame.
std::array<double, 9> toMatrix(const Quat& q);
/// Spherical interpolation along the shortest arc; `t` in 0..1.
Quat slerp(const Quat& a, const Quat& b, double t);

/// The camera's orientation at each of `times` (seconds, ascending), integrated from the first reading of `track`
/// (identity there). A time before the first reading gives the identity, after the last reading the last orientation.
std::vector<Quat> orientationAt(const GyroTrack& track, std::span<const double> times);

/// The integral of the angular velocity (rad) over any interval of a track, in O(log n): built for the readings
/// around [from, to] only, so a calibration over a few seconds of a long recording stays cheap.
class RateIntegral {
public:
    RateIntegral(const GyroTrack& track, double from, double to);
    /// The integral over [t0, t1] (t1 may be before t0: the negative). Outside the readings the rate is taken as 0.
    Vec3 between(double t0, double t1) const;
    bool empty() const { return m_times.size() < 2; }

private:
    Vec3 at(double t) const;
    std::vector<double> m_times;
    std::vector<Vec3> m_omega;
    std::vector<Vec3> m_cumulative;
};

} // namespace wabcut::gyro
