#pragma once

// Small geometry types shared by the render headers.

namespace wabcut::render {

/// A 2D point or offset in pixels (render's own type: the model stores motion as separate X/Y parameters).
struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr bool operator==(const Vec2&) const = default;
};

} // namespace wabcut::render
