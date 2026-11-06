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
    // Two ratios, two places (145 / 156). The frame's own pixel aspect ratio belongs in the frame's axes, before the
    // turn. The canvas ratio is a property of the monitor's x axis (the canvas is shown widened by it): it turns the
    // "seen" picture back into canvas pixels, so it comes AFTER the rotation, or a rotated clip would be squeezed and a
    // 30 degree turn would shear (the anchor, rotation and scale live in seen units; the position is in canvas pixels).
    const double canvasPar = canvas.pixelAspect > 0.0 ? canvas.pixelAspect : 1.0;
    const double framePar = pixelAspect > 0.0 ? pixelAspect : 1.0;
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
    const Affine2D aspect{framePar, 0, 0, 1, 0, 0};
    const Affine2D flip{t.mirrorX ? -1.0 : 1.0, 0, 0, 1, 0, 0}; // card 154: about the frame centre, stored pixels
    const Affine2D anchor{1, 0, 0, 1, -t.anchor.x, -t.anchor.y};
    const Affine2D scale{t.scale * t.aspectX, 0, 0, t.scale, 0, 0}; // 0.7 (card #12): Scale Width over Scale
    const Affine2D rotate{cs, sn, -sn, cs, 0, 0};
    const Affine2D toCanvasPixels{1.0 / canvasPar, 0, 0, 1, 0, 0}; // seen -> canvas pixels, after the turn
    const Affine2D place{1, 0, 0, 1, canvas.width / 2.0 + t.position.x, canvas.height / 2.0 + t.position.y};
    return place * toCanvasPixels * rotate * scale * anchor * flip * aspect * centreFrame;
}

void applyDisplayOrientation(LayerTransform& transform, int quarterTurns, bool mirrored) {
    const int quarter = transformQuarterTurns(quarterTurns, mirrored);
    transform.mirrorX = mirrored;
    if (quarter != 0) {
        transform.rotationDegrees += 90.0 * quarter;
        transform.anchor = toTransformAnchor(transform.anchor, quarter);
        if (quarter % 2 != 0 && transform.aspectX != 1.0) {
            // 0.7 (card #12): the clip's Scale Width / Scale Height are along the displayed axes; the stored picture's
            // axes are swapped by an odd number of quarter turns.
            transform.setScaleXY(transform.scale, transform.scaleX());
        }
    }
}

} // namespace wabcut::render
