// Plane geometry of the stabilizers (Geometry.h).

#include <wabcut/motion/Geometry.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wabcut::motion {

Matrix3 multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            double s = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                s += a[i * 3 + k] * b[k * 3 + j];
            }
            r[i * 3 + j] = s;
        }
    }
    return r;
}

std::optional<Matrix3> invert(const Matrix3& m) {
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || std::abs(det) < 1e-15) {
        return std::nullopt;
    }
    const double k = 1.0 / det;
    Matrix3 r{c00 * k,
              (m[2] * m[7] - m[1] * m[8]) * k,
              (m[1] * m[5] - m[2] * m[4]) * k,
              c01 * k,
              (m[0] * m[8] - m[2] * m[6]) * k,
              (m[2] * m[3] - m[0] * m[5]) * k,
              c02 * k,
              (m[1] * m[6] - m[0] * m[7]) * k,
              (m[0] * m[4] - m[1] * m[3]) * k};
    if (!std::all_of(r.begin(), r.end(), [](double v) { return std::isfinite(v); })) {
        return std::nullopt;
    }
    return r;
}

std::optional<Matrix3> homographyFromQuad(const std::array<std::array<double, 2>, 4>& from,
                                          const std::array<std::array<double, 2>, 4>& to) {
    // Eight equations in h0..h7 (h8 = 1), Gaussian elimination with partial pivoting.
    std::array<std::array<double, 9>, 8> a{};
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = from[i][0];
        const double y = from[i][1];
        const double u = to[i][0];
        const double v = to[i][1];
        a[2 * i] = {x, y, 1.0, 0.0, 0.0, 0.0, -x * u, -y * u, u};
        a[2 * i + 1] = {0.0, 0.0, 0.0, x, y, 1.0, -x * v, -y * v, v};
    }
    double scale = 0.0;
    for (const auto& row : a) {
        for (const double v : row) {
            scale = std::max(scale, std::abs(v));
        }
    }
    if (!std::isfinite(scale) || scale <= 0.0) {
        return std::nullopt;
    }
    for (std::size_t col = 0; col < 8; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < 8; ++r) {
            if (std::abs(a[r][col]) > std::abs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot][col]) < 1e-12 * scale) {
            return std::nullopt; // degenerate: three collinear points
        }
        std::swap(a[pivot], a[col]);
        for (std::size_t r = 0; r < 8; ++r) {
            if (r == col) {
                continue;
            }
            const double f = a[r][col] / a[col][col];
            for (std::size_t c = col; c < 9; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }
    Matrix3 h{};
    for (std::size_t i = 0; i < 8; ++i) {
        h[i] = a[i][8] / a[i][i];
    }
    h[8] = 1.0;
    if (!std::all_of(h.begin(), h.end(), [](double v) { return std::isfinite(v); })) {
        return std::nullopt;
    }
    return h;
}

Matrix3 similarityAbout(double cx, double cy, double x, double y, double radians, double scale) {
    const double a = scale * std::cos(radians);
    const double d = scale * std::sin(radians);
    return {a, -d, cx + x - (a * cx - d * cy), d, a, cy + y - (d * cx + a * cy), 0.0, 0.0, 1.0};
}

Matrix3 pixelCentresToFractions(const Matrix3& m, double width, double height) {
    // A fraction f is the pixel-centre coordinate f * size - 0.5: N = S^-1 T(0.5) m T(-0.5) S.
    const Matrix3 toCentres{width, 0.0, -0.5, 0.0, height, -0.5, 0.0, 0.0, 1.0};
    const Matrix3 toFractions{1.0 / width, 0.0, 0.5 / width, 0.0, 1.0 / height, 0.5 / height, 0.0, 0.0, 1.0};
    return multiply(toFractions, multiply(m, toCentres));
}

} // namespace wabcut::motion
