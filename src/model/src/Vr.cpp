#include <wabcut/model/Vr.h>

#include <algorithm>
#include <cmath>

namespace wabcut {

std::string_view toString(VrProjection projection) {
    switch (projection) {
    case VrProjection::None:
        return "none";
    case VrProjection::Equirectangular360:
        return "equirect360";
    case VrProjection::Equirectangular180:
        return "equirect180";
    }
    return "none";
}

std::optional<VrProjection> vrProjectionFromString(std::string_view text) {
    for (const VrProjection p :
         {VrProjection::None, VrProjection::Equirectangular360, VrProjection::Equirectangular180}) {
        if (toString(p) == text) {
            return p;
        }
    }
    return std::nullopt;
}

std::string_view toString(StereoLayout layout) {
    switch (layout) {
    case StereoLayout::Mono:
        return "mono";
    case StereoLayout::TopBottom:
        return "topBottom";
    case StereoLayout::SideBySide:
        return "sideBySide";
    }
    return "mono";
}

std::optional<StereoLayout> stereoLayoutFromString(std::string_view text) {
    for (const StereoLayout l : {StereoLayout::Mono, StereoLayout::TopBottom, StereoLayout::SideBySide}) {
        if (toString(l) == text) {
            return l;
        }
    }
    return std::nullopt;
}

std::optional<std::string> vrFormatProblem(const VrFormat& format) {
    if (!format.isVr() && format.stereo != StereoLayout::Mono) {
        return std::string("a stereoscopic layout needs a VR projection");
    }
    return std::nullopt;
}

std::array<int, 2> vrEyeSize(const VrFormat& format, int width, int height) {
    if (format.isVr() && format.stereo == StereoLayout::TopBottom) {
        return {width, std::max(1, height / 2)};
    }
    if (format.isVr() && format.stereo == StereoLayout::SideBySide) {
        return {std::max(1, width / 2), height};
    }
    return {width, height};
}

bool vrAspectSuits(const VrFormat& format, int width, int height) {
    if (!format.isVr() || width <= 0 || height <= 0) {
        return true;
    }
    const auto eye = vrEyeSize(format, width, height);
    const double aspect = static_cast<double>(eye[0]) / static_cast<double>(eye[1]);
    const double wanted = format.projection == VrProjection::Equirectangular360 ? 2.0 : 1.0;
    return std::abs(aspect / wanted - 1.0) <= 0.05;
}

namespace vr {

Mat3 identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += a[static_cast<std::size_t>(r * 3 + k)] * b[static_cast<std::size_t>(k * 3 + c)];
            }
            out[static_cast<std::size_t>(r * 3 + c)] = sum;
        }
    }
    return out;
}

Mat3 transpose(const Mat3& m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }

Vec3 apply(const Mat3& m, Vec3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

Vec3 normalized(Vec3 v) {
    const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 0.0)) {
        return {0.0, 0.0, 1.0};
    }
    return {v.x / length, v.y / length, v.z / length};
}

Mat3 rotation(double yawDegrees, double pitchDegrees, double rollDegrees) {
    const double d2r = kPi / 180.0;
    const double cy = std::cos(yawDegrees * d2r);
    const double sy = std::sin(yawDegrees * d2r);
    const double cp = std::cos(pitchDegrees * d2r);
    const double sp = std::sin(pitchDegrees * d2r);
    const double cr = std::cos(rollDegrees * d2r);
    const double sr = std::sin(rollDegrees * d2r);
    const Mat3 yaw{cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    const Mat3 pitch{1, 0, 0, 0, cp, sp, 0, -sp, cp};
    const Mat3 roll{cr, sr, 0, -sr, cr, 0, 0, 0, 1};
    return multiply(yaw, multiply(pitch, roll));
}

double longitudeOf(Vec3 d) { return std::atan2(d.x, d.z); }

double latitudeOf(Vec3 d) {
    const Vec3 n = normalized(d);
    return std::asin(std::clamp(n.y, -1.0, 1.0));
}

Vec3 directionAt(VrProjection projection, double u, double v) {
    const double span = projection == VrProjection::Equirectangular180 ? kPi : 2.0 * kPi;
    const double lon = (u - 0.5) * span;
    const double lat = (0.5 - v) * kPi;
    return {std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
}

EquirectUv uvOf(VrProjection projection, Vec3 d) {
    const double lon = longitudeOf(d);
    const double lat = latitudeOf(d);
    EquirectUv out;
    out.v = 0.5 - lat / kPi;
    if (projection == VrProjection::Equirectangular180) {
        out.u = lon / kPi + 0.5;
        out.inside = std::abs(lon) <= kPi / 2.0;
    } else {
        out.u = lon / (2.0 * kPi) + 0.5;
        out.u -= std::floor(out.u); // longitude +180 and -180 are the same meridian
    }
    return out;
}

std::array<double, 4> eyeRect(StereoLayout layout, int eye) {
    const bool right = eye != 0;
    switch (layout) {
    case StereoLayout::TopBottom:
        return right ? std::array<double, 4>{0.0, 0.5, 1.0, 1.0} : std::array<double, 4>{0.0, 0.0, 1.0, 0.5};
    case StereoLayout::SideBySide:
        return right ? std::array<double, 4>{0.5, 0.0, 1.0, 1.0} : std::array<double, 4>{0.0, 0.0, 0.5, 1.0};
    case StereoLayout::Mono:
        break;
    }
    return {0.0, 0.0, 1.0, 1.0};
}

Vec3 viewDirection(const Mat3& viewRotation, double nx, double ny) {
    return normalized(vr::apply(viewRotation, Vec3{nx, ny, 1.0})); // qualified: std::apply is found by ADL on std::array
}

namespace {

EquirectUv inEyeRect(const EquirectUv& eyeUv, const std::array<double, 4>& rect) {
    EquirectUv out = eyeUv;
    out.u = rect[0] + eyeUv.u * (rect[2] - rect[0]);
    out.v = rect[1] + eyeUv.v * (rect[3] - rect[1]);
    return out;
}

} // namespace

EquirectUv viewUv(const VrFormat& format, const Mat3& viewRotation, double fovDegrees, double aspect, double px,
                  double py, int eye) {
    const double fov = std::clamp(fovDegrees, 1.0, 170.0);
    const double tanHalf = std::tan(fov * kPi / 360.0);
    const double nx = (px - 0.5) * 2.0 * tanHalf;
    const double ny = (0.5 - py) * 2.0 * tanHalf / (aspect > 0.0 ? aspect : 1.0);
    const Vec3 d = viewDirection(viewRotation, nx, ny);
    const VrProjection projection = format.isVr() ? format.projection : VrProjection::Equirectangular360;
    return inEyeRect(uvOf(projection, d), eyeRect(format.isVr() ? format.stereo : StereoLayout::Mono, eye));
}

EquirectUv reorientUv(const VrFormat& format, const Mat3& content, double u, double v) {
    const VrProjection projection = format.isVr() ? format.projection : VrProjection::Equirectangular360;
    const StereoLayout layout = format.isVr() ? format.stereo : StereoLayout::Mono;
    int eye = 0;
    if (layout == StereoLayout::TopBottom) {
        eye = v >= 0.5 ? 1 : 0;
    } else if (layout == StereoLayout::SideBySide) {
        eye = u >= 0.5 ? 1 : 0;
    }
    const std::array<double, 4> rect = eyeRect(layout, eye);
    const double lu = (u - rect[0]) / (rect[2] - rect[0]);
    const double lv = (v - rect[1]) / (rect[3] - rect[1]);
    const Vec3 out = directionAt(projection, lu, lv);
    return inEyeRect(uvOf(projection, vr::apply(transpose(content), out)), rect);
}

} // namespace vr
} // namespace wabcut
