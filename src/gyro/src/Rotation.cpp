// Quaternions and the integral of the gyroscope's angular velocity (Rotation.h).

#include <wabcut/gyro/Rotation.h>

#include <algorithm>
#include <cmath>

namespace wabcut::gyro {

namespace {

constexpr double kTiny = 1e-12;

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Quat conjugate(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }

Quat normalized(const Quat& q) {
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(n > kTiny) || !std::isfinite(n)) {
        return {};
    }
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

Quat fromRotationVector(const Vec3& v) {
    const double angle = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(angle > kTiny) || !std::isfinite(angle)) {
        return {1.0, 0.5 * v.x, 0.5 * v.y, 0.5 * v.z}; // first order: the identity for a zero or damaged vector
    }
    const double s = std::sin(0.5 * angle) / angle;
    return {std::cos(0.5 * angle), v.x * s, v.y * s, v.z * s};
}

Vec3 toRotationVector(const Quat& in) {
    Quat q = normalized(in);
    if (q.w < 0.0) {
        q = {-q.w, -q.x, -q.y, -q.z};
    }
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(s > kTiny)) {
        return {2.0 * q.x, 2.0 * q.y, 2.0 * q.z};
    }
    const double angle = 2.0 * std::atan2(s, q.w);
    const double k = angle / s;
    return {q.x * k, q.y * k, q.z * k};
}

std::array<double, 9> toMatrix(const Quat& in) {
    const Quat q = normalized(in);
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz),       2.0 * (xz + wy),
            2.0 * (xy + wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx),
            2.0 * (xz - wy),       2.0 * (yz + wx),       1.0 - 2.0 * (xx + yy)};
}

Quat slerp(const Quat& a, const Quat& b, double t) {
    const Quat delta = conjugate(a) * b;
    return normalized(a * fromRotationVector(toRotationVector(delta) * std::clamp(t, 0.0, 1.0)));
}

std::vector<Quat> orientationAt(const GyroTrack& track, std::span<const double> times) {
    std::vector<Quat> out(times.size());
    const std::size_t n = std::min(track.times.size(), track.omega.size());
    if (n == 0) {
        return out;
    }
    Quat q;              // orientation at track.times[i]
    std::size_t i = 0;   // the reading before (or at) the next query
    for (std::size_t k = 0; k < times.size(); ++k) {
        const double t = times[k];
        if (!(t > track.times[0])) {
            out[k] = {};
            continue;
        }
        // Advance to the interval containing t, integrating whole intervals on the way.
        while (i + 1 < n && track.times[i + 1] <= t) {
            const double dt = track.times[i + 1] - track.times[i];
            const Vec3 mean = (track.omega[i] + track.omega[i + 1]) * 0.5;
            if (dt > 0.0 && finite(mean)) {
                q = normalized(q * fromRotationVector(mean * dt));
            }
            ++i;
        }
        if (i + 1 >= n) { // past the last reading: the rate is taken as zero
            out[k] = q;
            continue;
        }
        const double span = track.times[i + 1] - track.times[i];
        const double f = span > 0.0 ? (t - track.times[i]) / span : 0.0;
        const Vec3 here = track.omega[i] + (track.omega[i + 1] - track.omega[i]) * f;
        const Vec3 mean = (track.omega[i] + here) * 0.5;
        out[k] = finite(mean) ? normalized(q * fromRotationVector(mean * (t - track.times[i]))) : q;
    }
    return out;
}

RateIntegral::RateIntegral(const GyroTrack& track, double from, double to) {
    const std::size_t n = std::min(track.times.size(), track.omega.size());
    if (n < 2 || !(to >= from)) {
        return;
    }
    // One reading of margin either side, so the interpolation at the ends has both of its readings.
    const auto begin = std::upper_bound(track.times.begin(), track.times.begin() + static_cast<std::ptrdiff_t>(n), from);
    const auto end = std::lower_bound(track.times.begin(), track.times.begin() + static_cast<std::ptrdiff_t>(n), to);
    std::size_t lo = static_cast<std::size_t>(begin - track.times.begin());
    std::size_t hi = static_cast<std::size_t>(end - track.times.begin());
    lo = lo > 0 ? lo - 1 : 0;
    hi = std::min(hi + 1, n - 1);
    if (hi <= lo) {
        return;
    }
    m_times.assign(track.times.begin() + static_cast<std::ptrdiff_t>(lo), track.times.begin() + static_cast<std::ptrdiff_t>(hi) + 1);
    m_omega.assign(track.omega.begin() + static_cast<std::ptrdiff_t>(lo), track.omega.begin() + static_cast<std::ptrdiff_t>(hi) + 1);
    m_cumulative.assign(m_times.size(), Vec3{});
    for (std::size_t i = 1; i < m_times.size(); ++i) {
        const Vec3 mean = (m_omega[i - 1] + m_omega[i]) * 0.5;
        const double dt = m_times[i] - m_times[i - 1];
        m_cumulative[i] = m_cumulative[i - 1] + (finite(mean) && dt > 0.0 ? mean * dt : Vec3{});
    }
}

Vec3 RateIntegral::at(double t) const {
    if (empty() || !(t > m_times.front())) {
        return {};
    }
    if (!(t < m_times.back())) {
        return m_cumulative.back(); // past the last reading the rate is taken as zero
    }
    const auto it = std::upper_bound(m_times.begin(), m_times.end(), t);
    const std::size_t i = static_cast<std::size_t>(it - m_times.begin()) - 1;
    const double span = m_times[i + 1] - m_times[i];
    const double f = span > 0.0 ? (t - m_times[i]) / span : 0.0;
    const Vec3 here = m_omega[i] + (m_omega[i + 1] - m_omega[i]) * f;
    const Vec3 mean = (m_omega[i] + here) * 0.5;
    return m_cumulative[i] + (finite(mean) ? mean * (t - m_times[i]) : Vec3{});
}

Vec3 RateIntegral::between(double t0, double t1) const { return at(t1) - at(t0); }

} // namespace wabcut::gyro
