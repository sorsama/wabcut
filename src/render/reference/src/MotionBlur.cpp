#include <wabcut/render/reference/MotionBlur.h>

#include <cmath>

namespace wabcut::render::reference {

Image drawPlaced(const Image& picture, int width, int height, const Placement2D& p) {
    Image out(width, height);
    const double det = p.a * p.d - p.b * p.c;
    if (out.empty() || picture.empty() || !std::isfinite(det) || det == 0.0) {
        return out;
    }
    // canvas -> picture: the inverse affine.
    const double ia = p.d / det;
    const double ib = -p.b / det;
    const double ic = -p.c / det;
    const double id = p.a / det;
    const double ie = -(ia * p.e + ic * p.f);
    const double iff = -(ib * p.e + id * p.f);
    const double pw = picture.width;
    const double ph = picture.height;
    constexpr double kShare = 1.0 / (kSubSamples * kSubSamples);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Rgba sum{};
            for (int sy = 0; sy < kSubSamples; ++sy) {
                for (int sx = 0; sx < kSubSamples; ++sx) {
                    const double cx = x + (sx + 0.5) / kSubSamples;
                    const double cy = y + (sy + 0.5) / kSubSamples;
                    const double px = ia * cx + ic * cy + ie;
                    const double py = ib * cx + id * cy + iff;
                    if (!(px >= 0.0 && px < pw && py >= 0.0 && py < ph)) {
                        continue;
                    }
                    const Rgba t = picture.atD(static_cast<int>(px), static_cast<int>(py));
                    for (std::size_t k = 0; k < 4; ++k) {
                        sum[k] += t[k];
                    }
                }
            }
            out.setD(x, y, {sum[0] * kShare, sum[1] * kShare, sum[2] * kShare, sum[3] * kShare});
        }
    }
    return out;
}

Image motionBlurred(const Image& picture, int width, int height, std::span<const Placement2D> placements) {
    Image out(width, height);
    if (out.empty() || placements.empty()) {
        return out;
    }
    const double share = 1.0 / static_cast<double>(placements.size());
    std::vector<double> sum(out.rgba.size(), 0.0);
    for (const Placement2D& p : placements) {
        const Image one = drawPlaced(picture, width, height, p);
        for (std::size_t i = 0; i < sum.size(); ++i) {
            sum[i] += static_cast<double>(one.rgba[i]);
        }
    }
    for (std::size_t i = 0; i < sum.size(); ++i) {
        out.rgba[i] = static_cast<float>(sum[i] * share);
    }
    return out;
}

} // namespace wabcut::render::reference
