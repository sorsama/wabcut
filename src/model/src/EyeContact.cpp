#include <wabcut/model/EyeContact.h>

#include <algorithm>
#include <cmath>

namespace wabcut {

namespace {

double smoothstep(double e0, double e1, double x) {
    if (e1 <= e0) {
        return x >= e1 ? 1.0 : 0.0;
    }
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool finite(double v) { return std::isfinite(v); }

} // namespace

bool GazeEye::valid() const {
    return finite(cx) && finite(cy) && finite(angle) && finite(halfWidth) && finite(upper) && finite(lower) && finite(irisX) &&
           finite(irisY) && finite(irisRadius) && halfWidth > 0.0 && upper >= 0.0 && lower >= 0.0 && upper + lower > 0.0 &&
           irisRadius > 0.0 && irisRadius < 0.6 * halfWidth && std::abs(irisX) < halfWidth && std::abs(irisY) < 2.0 * halfWidth;
}

EyeCorrection correctEye(const GazeEye& eye, double turn, const EyeContactSettings& s) {
    EyeCorrection r;
    if (!eye.valid()) {
        return r;
    }
    const double width = 2.0 * eye.halfWidth;
    const double middle = 0.5 * (eye.lower - eye.upper); // the middle of the opening above the eye's centre: y is down
    r.gazeX = eye.irisX / width;
    r.gazeY = (eye.irisY - middle) / width;
    r.openness = (eye.upper + eye.lower) / width;
    const double down = r.gazeY * 100.0;
    const double side = std::abs(r.gazeX) * 100.0;
    // Looking down inside [downFrom, downTo] (ramps of 5 and 10 points on the outside), not to a side further than sideLimit.
    const double inRange = smoothstep(s.downFrom - 5.0, s.downFrom, down) * (1.0 - smoothstep(s.downTo, s.downTo + 10.0, down));
    const double sideOk = 1.0 - smoothstep(s.sideLimit, s.sideLimit + 10.0, side);
    const double notBlinking = smoothstep(0.07, 0.14, r.openness);
    const double facing = 1.0 - smoothstep(0.30, 0.55, std::clamp(turn, 0.0, 1.0));
    r.weight = std::clamp(inRange * sideOk * notBlinking * facing, 0.0, 1.0);
    const double k = std::clamp(s.amount / 100.0, 0.0, 1.0) * r.weight;
    double sx = -eye.irisX * k;
    double sy = (middle - eye.irisY) * k;
    const double length = std::hypot(sx, sy);
    const double limit = 1.1 * eye.irisRadius;
    if (length > limit) {
        sx *= limit / length;
        sy *= limit / length;
    }
    r.shiftX = sx;
    r.shiftY = sy;
    r.active = std::hypot(sx, sy) > 1e-4 * eye.halfWidth;
    return r;
}

EyeContactSettings eyeContactSettingsOf(const std::array<double, 32>& p) {
    EyeContactSettings s;
    s.amount = std::clamp(p[0], 0.0, 100.0);
    s.downFrom = std::clamp(p[1], 0.0, 100.0);
    s.downTo = std::clamp(p[2], 0.0, 100.0);
    s.sideLimit = std::clamp(p[3], 0.0, 100.0);
    return s;
}

bool bakeEyeContact(std::array<double, 32>& params, const GazeFace& face, const EyeContactSettings& settings) {
    params[kEyeParamCount] = 0.0;
    std::array<EyeCorrection, 2> corrections{};
    bool any = false;
    for (std::size_t i = 0; i < 2; ++i) {
        corrections[i] = correctEye(face.eyes[i], face.turn, settings);
        any = any || corrections[i].active;
    }
    if (!any) {
        return false;
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        if (!corrections[i].active) {
            continue; // an eye that needs nothing is not drawn at all: it stays exactly as it was
        }
        const GazeEye& e = face.eyes[i];
        const std::size_t at = kEyeParamFirst + count * kEyeParamStride;
        params[at + 0] = e.cx;
        params[at + 1] = e.cy;
        params[at + 2] = e.angle;
        params[at + 3] = e.halfWidth;
        params[at + 4] = e.upper;
        params[at + 5] = e.lower;
        params[at + 6] = e.irisX;
        params[at + 7] = e.irisY;
        params[at + 8] = e.irisRadius;
        params[at + 9] = corrections[i].shiftX;
        params[at + 10] = corrections[i].shiftY;
        ++count;
    }
    params[kEyeParamCount] = static_cast<double>(count);
    return count > 0;
}

} // namespace wabcut
