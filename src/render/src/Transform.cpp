#include <wabcut/render/PreparedComposite.h>

#include <cmath>
#include <numbers>

namespace wabcut::render {

Affine2D Affine2D::inverted() const {
    const double det = determinant();
    if (det == 0.0) {
        return {0, 0, 0, 0, 0, 0};
    }
    const double ia = d / det;
    const double ib = -b / det;
    const double ic = -c / det;
    const double id = a / det;
    return {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
}

Affine2D layerToCanvas(const LayerTransform& t, double frameWidth, double frameHeight, double pixelAspect,
                       CanvasSize canvas) {
    const double par = pixelAspect > 0.0 ? pixelAspect : 1.0;
    // Exact quarter turns keep exact matrices (cos/sin of 90° are not exactly 0/1 in floating point).
    double cs = 0.0;
    double sn = 0.0;
    const double turns = t.rotationDegrees / 90.0;
    if (turns == std::floor(turns)) {
        const long long q = ((static_cast<long long>(turns) % 4) + 4) % 4;
        cs = q == 0 ? 1.0 : q == 2 ? -1.0 : 0.0;
        sn = q == 1 ? 1.0 : q == 3 ? -1.0 : 0.0;
    } else {
        const double rad = t.rotationDegrees * std::numbers::pi / 180.0;
        cs = std::cos(rad);
        sn = std::sin(rad);
    }
    // y points down, so the standard rotation matrix turns clockwise on screen.
    const Affine2D centreFrame{1, 0, 0, 1, -frameWidth / 2.0, -frameHeight / 2.0};
    const Affine2D aspect{par, 0, 0, 1, 0, 0};
    const Affine2D anchor{1, 0, 0, 1, -t.anchor.x, -t.anchor.y};
    const Affine2D scale{t.scale, 0, 0, t.scale, 0, 0};
    const Affine2D rotate{cs, sn, -sn, cs, 0, 0};
    const Affine2D place{1, 0, 0, 1, canvas.width / 2.0 + t.position.x, canvas.height / 2.0 + t.position.y};
    return place * rotate * scale * anchor * aspect * centreFrame;
}

} // namespace wabcut::render
