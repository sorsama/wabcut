#pragma once

// Plane geometry of the stabilizers (P.2): 3 x 3 projective maps, row-major: (x', y', w') = m (x, y, 1).

#include <array>
#include <optional>

namespace wabcut::motion {

using Matrix3 = std::array<double, 9>;

inline constexpr Matrix3 kIdentity3{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

/// a b (apply b first).
Matrix3 multiply(const Matrix3& a, const Matrix3& b);
/// The inverse, or nullopt when singular (or not finite).
std::optional<Matrix3> invert(const Matrix3& m);
/// The map taking the four points `from` exactly to `to` (a corner pin), m[8] = 1; nullopt when three of either are
/// collinear or the result is not finite.
std::optional<Matrix3> homographyFromQuad(const std::array<std::array<double, 2>, 4>& from,
                                          const std::array<std::array<double, 2>, 4>& to);
/// Rotation by `radians` (clockwise on screen, y down) and `scale` about (cx, cy), then a move by (x, y).
Matrix3 similarityAbout(double cx, double cy, double x, double y, double radians, double scale);
/// `m`, given in pixels with pixel centres at integers (the trackers' convention) of a `width` x `height` picture, in
/// fractions of the picture with the origin at its top-left corner (render::CompositeLayer::warp's convention).
Matrix3 pixelCentresToFractions(const Matrix3& m, double width, double height);

} // namespace wabcut::motion
