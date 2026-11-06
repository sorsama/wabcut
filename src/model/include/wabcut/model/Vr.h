#pragma once

// VR / 360 video (0.7): equirectangular pictures, mono or stereoscopic, and the maths that turns a direction in the
// sphere into a place in such a picture. Qt-free: the model, the media probe, the exporter, the CPU twins of the GPU
// shaders and the tests all use it.
//
// Conventions (the ones of FFmpeg's AVSphericalMapping and of Google's Spherical Video V2, so what the file says maps
// straight onto them): the viewer sits in the middle of the sphere; +x is to their right, +y up, +z straight ahead (the
// centre of the picture). A 360 picture covers the whole sphere (longitude -180..180 across, latitude +90 at the top to
// -90 at the bottom); a 180 picture (VR180) the front half: longitude -90..90 across the full width. Stereoscopic
// pictures hold both eyes in one frame, the left eye first: Top-Bottom (left eye on top) or Side-by-Side (left eye on
// the left).
//
// Rotations are in degrees and compose as FFmpeg's do: R = yaw * pitch * roll (the roll applies first). A positive
// yaw turns what is in front of the viewer toward their right, a positive pitch upward, a positive roll tilts it to
// the viewer's right (clockwise).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut {

enum class VrProjection : std::uint8_t {
    None,               ///< an ordinary flat picture
    Equirectangular360, ///< the whole sphere, 2:1 for one eye
    Equirectangular180  ///< the front half of the sphere (VR180), 1:1 for one eye
};

enum class StereoLayout : std::uint8_t { Mono, TopBottom, SideBySide };

/// What a picture is: flat, or which VR projection and eye layout. Saved only when it is VR.
struct VrFormat {
    VrProjection projection = VrProjection::None;
    StereoLayout stereo = StereoLayout::Mono;

    bool isVr() const { return projection != VrProjection::None; }
    bool isStereo() const { return isVr() && stereo != StereoLayout::Mono; }
    bool operator==(const VrFormat&) const = default;
};

std::string_view toString(VrProjection projection);  ///< "none", "equirect360", "equirect180"
std::optional<VrProjection> vrProjectionFromString(std::string_view text);
std::string_view toString(StereoLayout layout);      ///< "mono", "topBottom", "sideBySide"
std::optional<StereoLayout> stereoLayoutFromString(std::string_view text);

/// Why `format` is not a valid combination (a stereo layout on a flat picture), or nullopt.
std::optional<std::string> vrFormatProblem(const VrFormat& format);

/// The size of one eye's picture of a frame of `width` x `height` in `format` (the whole frame when mono).
std::array<int, 2> vrEyeSize(const VrFormat& format, int width, int height);

/// Whether the frame size suits the projection (a 360 eye is 2:1, a 180 eye 1:1), within 5 %: only a hint for the
/// interface, never a reason to refuse a setting.
bool vrAspectSuits(const VrFormat& format, int width, int height);

namespace vr {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 1.0;
};

/// A 3 x 3 matrix, row-major.
using Mat3 = std::array<double, 9>;

constexpr double kPi = 3.14159265358979323846;

Mat3 identity();
Mat3 multiply(const Mat3& a, const Mat3& b);
Mat3 transpose(const Mat3& m);
Vec3 apply(const Mat3& m, Vec3 v);
Vec3 normalized(Vec3 v);

/// yaw * pitch * roll, in degrees (see the header comment).
Mat3 rotation(double yawDegrees, double pitchDegrees, double rollDegrees);

/// A place in a picture, as fractions of one eye's width and height: (0, 0) is the top-left corner. `inside` is false
/// where the projection does not cover the direction (behind a 180 picture).
struct EquirectUv {
    double u = 0.0;
    double v = 0.0;
    bool inside = true;
};

/// Longitude and latitude (radians) of a direction: 0, 0 straight ahead; longitude grows to the right, latitude up.
double longitudeOf(Vec3 d);
double latitudeOf(Vec3 d);
/// The unit direction at (u, v) of one eye's picture.
Vec3 directionAt(VrProjection projection, double u, double v);
/// Where a direction lands in one eye's picture (u wraps for 360: in [0, 1)).
EquirectUv uvOf(VrProjection projection, Vec3 d);

/// The rectangle one eye occupies in the whole frame, as fractions: {u0, v0, u1, v1}. Mono: the whole frame; eye 0 is
/// the left eye, 1 the right.
std::array<double, 4> eyeRect(StereoLayout layout, int eye);

/// The direction a flat view looks at for the pixel at (nx, ny) of the view's plane: x to the right, y up, the plane at
/// distance 1, so nx = tan(horizontal angle). `viewRotation` turns the camera (its yaw / pitch / roll, see rotation()).
Vec3 viewDirection(const Mat3& viewRotation, double nx, double ny);

/// Where a viewer looking along `viewRotation` with horizontal field of view `fovDegrees` sees each pixel of a view
/// `aspect` (width / height) wide: the place in the whole frame `format` of `eye` (the left eye of a stereo picture).
/// (px, py) are in [0, 1], origin top-left. `inside` is false outside a 180 picture.
EquirectUv viewUv(const VrFormat& format, const Mat3& viewRotation, double fovDegrees, double aspect, double px,
                  double py, int eye = 0);

/// Where the picture's pixel (u, v) of the whole frame `format` comes from after the content is rotated by `content`
/// (VR Reorient): the place in the same frame; the eye of the pixel is kept.
EquirectUv reorientUv(const VrFormat& format, const Mat3& content, double u, double v);

/// Limits the interface and the commands keep to.
inline constexpr double kMinFov = 10.0;
inline constexpr double kMaxFov = 150.0;
inline constexpr double kDefaultFov = 90.0;
inline constexpr double kMaxPitch = 90.0;

} // namespace vr
} // namespace wabcut
