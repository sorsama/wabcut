#include <wabcut/color/ChartMatch.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <numeric>

namespace wabcut::color {

namespace {

// ---- Reference data ---------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, kChartPatches> kNames{
    "Dark skin",    "Light skin",   "Blue sky",     "Foliage",     "Blue flower", "Bluish green", "Orange",     "Purplish blue",
    "Moderate red", "Purple",       "Yellow green", "Orange yellow", "Blue",        "Green",        "Red",        "Yellow",
    "Magenta",      "Cyan",         "White",        "Neutral 8",   "Neutral 6.5", "Neutral 5",    "Neutral 3.5", "Black"};

// X-Rite's ColorChecker Classic CIE L*a*b* (D50), edition after November 2014 (published 2016).
constexpr std::array<std::array<double, 3>, kChartPatches> kLabAfter2014{{
    {37.54, 14.37, 14.92},   {64.66, 19.27, 17.5},   {49.32, -3.82, -22.54}, {43.46, -12.74, 22.72}, {54.94, 9.61, -24.79},
    {70.48, -32.26, -0.37},  {62.73, 35.83, 56.5},   {39.43, 10.75, -45.17}, {50.57, 48.64, 16.67},  {30.1, 22.54, -20.87},
    {71.77, -24.13, 58.19},  {71.51, 18.24, 67.37},  {28.37, 15.42, -49.8},  {54.38, -39.72, 32.27}, {42.43, 51.05, 28.62},
    {81.8, 2.67, 80.41},     {50.63, 51.28, -14.12}, {49.57, -29.71, -28.32}, {95.19, -1.03, 2.93},   {81.29, -0.57, 0.44},
    {66.89, -0.75, -0.06},   {50.76, -0.13, 0.14},   {35.63, -0.46, -0.48},  {20.64, 0.07, -0.46},
}};

// The edition before November 2014 (GretagMacbeth 2005 data).
constexpr std::array<std::array<double, 3>, kChartPatches> kLabBefore2014{{
    {37.986, 13.555, 14.059}, {65.711, 18.13, 17.81},  {49.927, -4.88, -21.905}, {43.139, -13.095, 21.905}, {55.112, 8.844, -25.399},
    {70.719, -33.397, -0.199}, {62.661, 36.067, 57.096}, {40.02, 10.41, -45.964}, {51.124, 48.239, 16.248},  {30.325, 22.976, -21.587},
    {72.532, -23.709, 57.255}, {71.941, 19.363, 67.857}, {28.778, 14.179, -50.297}, {55.261, -38.342, 31.37}, {42.101, 53.378, 28.19},
    {81.733, 4.039, 79.819},  {51.935, 49.986, -14.574}, {51.038, -28.631, -28.638}, {96.539, -0.425, 1.186}, {81.257, -0.638, -0.335},
    {66.766, -0.734, -0.504}, {50.867, -0.153, -0.27},  {35.656, -0.421, -1.231}, {20.461, -0.079, -0.973},
}};

// ---- Colour science ---------------------------------------------------------------------------------------------

const Mat3& srgbToXyzD50() {
    // sRGB (D65) primaries to XYZ, then the Bradford adaptation D65 -> D50 (Lindbloom's matrices).
    static const Mat3 m = multiply(Mat3{1.0478112, 0.0228866, -0.0501270, 0.0295424, 0.9904844, -0.0170491, -0.0092345, 0.0150436, 0.7521316},
                                   Mat3{0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041});
    return m;
}

const Mat3& xyzD50ToSrgb() {
    static const Mat3 m = inverse(srgbToXyzD50());
    return m;
}

constexpr Rgb kWhiteD50{0.96422, 1.0, 0.82521};
constexpr double kEpsilon = 216.0 / 24389.0;
constexpr double kKappa = 24389.0 / 27.0;

double labF(double t) { return t > kEpsilon ? std::cbrt(t) : (kKappa * t + 16.0) / 116.0; }

Rgb xyzToLab(const Rgb& xyz) {
    const double fx = labF(xyz[0] / kWhiteD50[0]), fy = labF(xyz[1] / kWhiteD50[1]), fz = labF(xyz[2] / kWhiteD50[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

Rgb labToXyz(const Rgb& lab) {
    const double fy = (lab[0] + 16.0) / 116.0, fx = lab[1] / 500.0 + fy, fz = fy - lab[2] / 200.0;
    const auto inv = [](double f) { return f * f * f > kEpsilon ? f * f * f : (116.0 * f - 16.0) / kKappa; };
    const double yr = lab[0] > kKappa * kEpsilon ? fy * fy * fy : lab[0] / kKappa;
    return {inv(fx) * kWhiteD50[0], yr * kWhiteD50[1], inv(fz) * kWhiteD50[2]};
}

double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

double hueAngle(double b, double a) {
    if (a == 0.0 && b == 0.0) {
        return 0.0;
    }
    double h = std::atan2(b, a) * 180.0 / std::numbers::pi;
    return h < 0.0 ? h + 360.0 : h;
}

struct Reference {
    std::array<Rgb, kChartPatches> lab{};    ///< published
    std::array<Rgb, kChartPatches> code{};   ///< what the monitor should show
    std::array<Rgb, kChartPatches> target{}; ///< L*a*b* of `code`
};

const Reference& referenceOf(ChartEdition edition) {
    static const auto build = [](const std::array<std::array<double, 3>, kChartPatches>& table) {
        Reference r;
        for (int i = 0; i < kChartPatches; ++i) {
            const auto& l = table[static_cast<std::size_t>(i)];
            r.lab[static_cast<std::size_t>(i)] = {l[0], l[1], l[2]};
            const Rgb linear = mul(xyzD50ToSrgb(), labToXyz({l[0], l[1], l[2]}));
            r.code[static_cast<std::size_t>(i)] = {srgbEncode(std::clamp(linear[0], 0.0, 1.0)), srgbEncode(std::clamp(linear[1], 0.0, 1.0)),
                                                   srgbEncode(std::clamp(linear[2], 0.0, 1.0))};
            r.target[static_cast<std::size_t>(i)] = srgbToLab(r.code[static_cast<std::size_t>(i)]);
        }
        return r;
    };
    static const Reference after = build(kLabAfter2014);
    static const Reference before = build(kLabBefore2014);
    return edition == ChartEdition::AfterNov2014 ? after : before;
}

// ---- Small linear algebra ---------------------------------------------------------------------------------------

struct Pt {
    double x = 0.0;
    double y = 0.0;
};

/// Solves the n x n system A x = b (A row-major) by Gaussian elimination with partial pivoting.
bool solveLinear(std::vector<double> a, std::vector<double> b, std::size_t n, std::vector<double>& x) {
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < n; ++r) {
            if (std::abs(a[r * n + col]) > std::abs(a[pivot * n + col])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot * n + col]) < 1e-12) {
            return false;
        }
        if (pivot != col) {
            for (std::size_t k = 0; k < n; ++k) {
                std::swap(a[col * n + k], a[pivot * n + k]);
            }
            std::swap(b[col], b[pivot]);
        }
        for (std::size_t r = col + 1; r < n; ++r) {
            const double f = a[r * n + col] / a[col * n + col];
            for (std::size_t k = col; k < n; ++k) {
                a[r * n + k] -= f * a[col * n + k];
            }
            b[r] -= f * b[col];
        }
    }
    x.assign(n, 0.0);
    for (std::size_t i = n; i-- > 0;) {
        double s = b[i];
        for (std::size_t k = i + 1; k < n; ++k) {
            s -= a[i * n + k] * x[k];
        }
        x[i] = s / a[i * n + i];
    }
    return true;
}

double determinant(const Mat3& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

Pt applyH(const Mat3& h, Pt p) {
    const double w = h[6] * p.x + h[7] * p.y + h[8];
    if (std::abs(w) < 1e-12) {
        return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    }
    return {(h[0] * p.x + h[1] * p.y + h[2]) / w, (h[3] * p.x + h[4] * p.y + h[5]) / w};
}

/// Least-squares homography src -> dst (Hartley-normalised direct linear transform), at least four points.
std::optional<Mat3> fitHomography(const std::vector<Pt>& src, const std::vector<Pt>& dst) {
    const std::size_t n = src.size();
    if (n < 4 || dst.size() != n) {
        return std::nullopt;
    }
    const auto normaliser = [](const std::vector<Pt>& pts) {
        Pt c{};
        for (const Pt& p : pts) {
            c.x += p.x / static_cast<double>(pts.size());
            c.y += p.y / static_cast<double>(pts.size());
        }
        double d = 0.0;
        for (const Pt& p : pts) {
            d += std::hypot(p.x - c.x, p.y - c.y) / static_cast<double>(pts.size());
        }
        const double s = d > 1e-12 ? std::numbers::sqrt2 / d : 1.0;
        return Mat3{s, 0, -s * c.x, 0, s, -s * c.y, 0, 0, 1};
    };
    const Mat3 ts = normaliser(src), td = normaliser(dst);
    std::vector<double> ata(64, 0.0), atb(8, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const Pt s = applyH(ts, src[i]), d = applyH(td, dst[i]);
        const double rows[2][8] = {{s.x, s.y, 1, 0, 0, 0, -d.x * s.x, -d.x * s.y}, {0, 0, 0, s.x, s.y, 1, -d.y * s.x, -d.y * s.y}};
        const double rhs[2] = {d.x, d.y};
        for (int r = 0; r < 2; ++r) {
            for (int a = 0; a < 8; ++a) {
                atb[static_cast<std::size_t>(a)] += rows[r][a] * rhs[r];
                for (int b = 0; b < 8; ++b) {
                    ata[static_cast<std::size_t>(a * 8 + b)] += rows[r][a] * rows[r][b];
                }
            }
        }
    }
    std::vector<double> h;
    if (!solveLinear(ata, atb, 8, h)) {
        return std::nullopt;
    }
    const Mat3 hn{h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], 1.0};
    if (std::abs(determinant(td)) < 1e-12) {
        return std::nullopt;
    }
    const Mat3 out = multiply(inverse(td), multiply(hn, ts));
    for (double v : out) {
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
    }
    return out;
}

/// Least-squares affine map src -> dst as a homography, at least three points.
std::optional<Mat3> fitAffine(const std::vector<Pt>& src, const std::vector<Pt>& dst) {
    const std::size_t n = src.size();
    if (n < 3 || dst.size() != n) {
        return std::nullopt;
    }
    std::vector<double> ata(9, 0.0), atu(3, 0.0), atv(3, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double r[3] = {src[i].x, src[i].y, 1.0};
        for (int a = 0; a < 3; ++a) {
            atu[static_cast<std::size_t>(a)] += r[a] * dst[i].x;
            atv[static_cast<std::size_t>(a)] += r[a] * dst[i].y;
            for (int b = 0; b < 3; ++b) {
                ata[static_cast<std::size_t>(a * 3 + b)] += r[a] * r[b];
            }
        }
    }
    std::vector<double> u, v;
    if (!solveLinear(ata, atu, 3, u) || !solveLinear(ata, atv, 3, v)) {
        return std::nullopt;
    }
    return Mat3{u[0], u[1], u[2], v[0], v[1], v[2], 0, 0, 1};
}

std::optional<Mat3> fitMap(const std::vector<Pt>& src, const std::vector<Pt>& dst) {
    // A perspective fit needs enough well spread points; fewer get the affine map.
    if (src.size() >= 8) {
        if (auto h = fitHomography(src, dst)) {
            return h;
        }
    }
    return fitAffine(src, dst);
}

std::optional<Mat3> invertMap(const Mat3& m) {
    if (std::abs(determinant(m)) < 1e-14) {
        return std::nullopt;
    }
    return inverse(m);
}

/// The map from chart coordinates to normalised picture coordinates.
std::optional<Mat3> chartMap(const ChartCorners& corners) {
    const std::vector<Pt> src{{0, 0}, {kChartWidth, 0}, {kChartWidth, kChartHeight}, {0, kChartHeight}};
    std::vector<Pt> dst;
    for (const ChartPoint& p : corners.p) {
        dst.push_back({p.x, p.y});
    }
    return fitHomography(src, dst);
}

// ---- Detection --------------------------------------------------------------------------------------------------

struct WorkImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;          ///< width * height * 3
    std::vector<std::uint8_t> valid; ///< 1 where the picture is there
};

WorkImage downscale(const ColorImage& image, int k) {
    WorkImage out;
    out.width = image.width / k;
    out.height = image.height / k;
    out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3, 0.0f);
    out.valid.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            double sum[3] = {0, 0, 0};
            int count = 0;
            for (int dy = 0; dy < k; ++dy) {
                for (int dx = 0; dx < k; ++dx) {
                    const float* p = image.rgba.data() + (static_cast<std::size_t>(y * k + dy) * image.width + (x * k + dx)) * 4;
                    if (p[3] >= 0.5f) {
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                        ++count;
                    }
                }
            }
            const std::size_t i = static_cast<std::size_t>(y) * out.width + x;
            if (count * 2 >= k * k) {
                out.valid[i] = 1;
                for (int c = 0; c < 3; ++c) {
                    out.rgb[i * 3 + static_cast<std::size_t>(c)] = static_cast<float>(sum[c] / count);
                }
            }
        }
    }
    return out;
}

struct Blob {
    Pt centre;
    double area = 0.0;
    Rgb colour{0, 0, 0};
};

/// Flat regions (little change between neighbouring pixels), shrunk by one pixel so patches that touch stay apart, that
/// look like filled squares or rectangles.
std::vector<Blob> flatBlobs(const WorkImage& im, double threshold) {
    const int w = im.width, h = im.height;
    std::vector<std::uint8_t> flat(static_cast<std::size_t>(w) * h, 0);
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (!im.valid[i] || !im.valid[i - 1] || !im.valid[i + 1] || !im.valid[i - static_cast<std::size_t>(w)] ||
                !im.valid[i + static_cast<std::size_t>(w)]) {
                continue;
            }
            double g = 0.0;
            for (int c = 0; c < 3; ++c) {
                const double gx = im.rgb[(i + 1) * 3 + static_cast<std::size_t>(c)] - im.rgb[(i - 1) * 3 + static_cast<std::size_t>(c)];
                const double gy = im.rgb[(i + static_cast<std::size_t>(w)) * 3 + static_cast<std::size_t>(c)] -
                                  im.rgb[(i - static_cast<std::size_t>(w)) * 3 + static_cast<std::size_t>(c)];
                g = std::max(g, std::hypot(gx, gy) * 0.5);
            }
            flat[i] = g < threshold ? 1 : 0;
        }
    }
    std::vector<std::uint8_t> core(flat.size(), 0);
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            core[i] = flat[i] && flat[i - 1] && flat[i + 1] && flat[i - static_cast<std::size_t>(w)] && flat[i + static_cast<std::size_t>(w)];
        }
    }
    std::vector<int> parent(core.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](int a) {
        while (parent[static_cast<std::size_t>(a)] != a) {
            parent[static_cast<std::size_t>(a)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(a)])];
            a = parent[static_cast<std::size_t>(a)];
        }
        return a;
    };
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (!core[i]) {
                continue;
            }
            if (core[i - 1]) {
                parent[static_cast<std::size_t>(find(static_cast<int>(i)))] = find(static_cast<int>(i - 1));
            }
            if (core[i - static_cast<std::size_t>(w)]) {
                parent[static_cast<std::size_t>(find(static_cast<int>(i)))] = find(static_cast<int>(i - static_cast<std::size_t>(w)));
            }
        }
    }
    struct Sums {
        double n = 0, x = 0, y = 0, xx = 0, xy = 0, yy = 0, r = 0, g = 0, b = 0;
    };
    std::vector<int> slot(core.size(), -1);
    std::vector<Sums> sums;
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (!core[i]) {
                continue;
            }
            const std::size_t root = static_cast<std::size_t>(find(static_cast<int>(i)));
            if (slot[root] < 0) {
                slot[root] = static_cast<int>(sums.size());
                sums.emplace_back();
            }
            Sums& s = sums[static_cast<std::size_t>(slot[root])];
            s.n += 1;
            s.x += x;
            s.y += y;
            s.xx += static_cast<double>(x) * x;
            s.xy += static_cast<double>(x) * y;
            s.yy += static_cast<double>(y) * y;
            s.r += im.rgb[i * 3];
            s.g += im.rgb[i * 3 + 1];
            s.b += im.rgb[i * 3 + 2];
        }
    }
    std::vector<Blob> out;
    const double minArea = std::max(9.0, 0.0002 * w * h), maxArea = 0.08 * w * h;
    for (const Sums& s : sums) {
        if (s.n < minArea || s.n > maxArea) {
            continue;
        }
        const double cx = s.x / s.n, cy = s.y / s.n;
        const double sxx = s.xx / s.n - cx * cx, syy = s.yy / s.n - cy * cy, sxy = s.xy / s.n - cx * cy;
        const double det = sxx * syy - sxy * sxy;
        if (det <= 1e-9) {
            continue;
        }
        const double half = (sxx + syy) / 2.0;
        const double root = std::sqrt(std::max(0.0, half * half - det));
        const double l1 = half + root, l2 = half - root;
        if (l2 <= 1e-9) {
            continue;
        }
        const double filled = s.n / (12.0 * std::sqrt(l1 * l2)); // 1 for a filled rectangle
        const double elongation = std::sqrt(l1 / l2);
        if (filled < 0.8 || filled > 1.25 || elongation > 1.7) {
            continue;
        }
        out.push_back({{cx + 0.5, cy + 0.5}, s.n, {s.r / s.n, s.g / s.n, s.b / s.n}});
    }
    return out;
}

/// Which blobs are about the size of a chart patch: the largest group whose areas stay within a factor of 1.9.
std::vector<Blob> sameSizeBlobs(const std::vector<Blob>& blobs) {
    std::vector<Blob> best;
    for (const Blob& candidate : blobs) {
        std::vector<Blob> group;
        for (const Blob& other : blobs) {
            if (other.area >= candidate.area / 1.9 && other.area <= candidate.area * 1.9) {
                group.push_back(other);
            }
        }
        if (group.size() > best.size()) {
            best = std::move(group);
        }
    }
    return best;
}

struct Cell {
    int a = 0; ///< lattice coordinates
    int b = 0;
    std::size_t blob = 0;
};

struct Placement {
    std::vector<std::pair<std::size_t, int>> patches; ///< blob -> chart patch index
    double residual = 1e9;
};

/// How well the measured colours fit the reference ones through a per-channel gain and offset (a cast and a tone
/// change), as a root mean square in code values.
double colourResidual(const std::vector<std::pair<Rgb, int>>& samples, ChartEdition edition) {
    const Reference& ref = referenceOf(edition);
    double total = 0.0;
    for (std::size_t c = 0; c < 3; ++c) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const double n = static_cast<double>(samples.size());
        for (const auto& [colour, index] : samples) {
            const double x = ref.code[static_cast<std::size_t>(index)][c], y = colour[c];
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double den = n * sxx - sx * sx;
        const double gain = std::abs(den) > 1e-12 ? (n * sxy - sx * sy) / den : 1.0;
        const double offset = (sy - gain * sx) / n;
        for (const auto& [colour, index] : samples) {
            const double e = colour[c] - (gain * ref.code[static_cast<std::size_t>(index)][c] + offset);
            total += e * e;
        }
    }
    return std::sqrt(total / (3.0 * static_cast<double>(samples.size())));
}

/// The neutral row must run light to dark in the order white .. black.
bool neutralsDescend(const std::vector<std::pair<Rgb, int>>& samples) {
    std::vector<std::pair<int, double>> row;
    for (const auto& [colour, index] : samples) {
        if (index >= 18) {
            row.push_back({index, luma(colour)});
        }
    }
    std::sort(row.begin(), row.end());
    if (row.size() < 3) {
        return true; // too little to say
    }
    for (std::size_t i = 1; i < row.size(); ++i) {
        if (row[i].second > row[i - 1].second + 0.01) {
            return false;
        }
    }
    return row.front().second > row.back().second + 0.05;
}

/// Turns and offsets a lattice patch cloud into the chart: the best of the eight orientations and every offset that
/// keeps it inside 6 x 4.
std::optional<Placement> placeLattice(const std::vector<Cell>& cells, const std::vector<Blob>& blobs, ChartEdition edition,
                                      double* runnerUp) {
    std::optional<Placement> best;
    double second = 1e9;
    for (int t = 0; t < 8; ++t) {
        std::vector<std::array<int, 2>> rc;
        int minC = 99, maxC = -99, minR = 99, maxR = -99;
        for (const Cell& cell : cells) {
            int c = (t & 4) ? cell.b : cell.a;
            int r = (t & 4) ? cell.a : cell.b;
            c = (t & 1) ? -c : c;
            r = (t & 2) ? -r : r;
            rc.push_back({c, r});
            minC = std::min(minC, c);
            maxC = std::max(maxC, c);
            minR = std::min(minR, r);
            maxR = std::max(maxR, r);
        }
        if (maxC - minC > kChartColumns - 1 || maxR - minR > kChartRows - 1) {
            continue;
        }
        for (int ox = -minC; ox <= kChartColumns - 1 - maxC; ++ox) {
            for (int oy = -minR; oy <= kChartRows - 1 - maxR; ++oy) {
                Placement p;
                std::vector<std::pair<Rgb, int>> samples;
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    const int index = (rc[i][1] + oy) * kChartColumns + (rc[i][0] + ox);
                    p.patches.push_back({cells[i].blob, index});
                    samples.push_back({blobs[cells[i].blob].colour, index});
                }
                if (!neutralsDescend(samples)) {
                    continue;
                }
                p.residual = colourResidual(samples, edition);
                if (!best || p.residual < best->residual) {
                    second = std::min(second, best ? best->residual : 1e9);
                    best = std::move(p);
                } else {
                    second = std::min(second, p.residual);
                }
            }
        }
    }
    if (runnerUp) {
        *runnerUp = second;
    }
    return best;
}

struct Candidate {
    Placement placement;
    ChartCorners corners;
    double runnerUp = 1e9;
};

/// Tries one lattice basis (an origin blob and two neighbour steps): refines it into a perspective lattice, places it on
/// the chart and fits the chart's corners.
std::optional<Candidate> tryBasis(const std::vector<Blob>& blobs, std::size_t origin, Pt u, Pt v, ChartEdition edition, int width,
                                  int height) {
    // Lattice coordinates through the current map (starts affine from u and v).
    std::optional<Mat3> toImage = Mat3{u.x, v.x, blobs[origin].centre.x, u.y, v.y, blobs[origin].centre.y, 0, 0, 1};
    std::vector<Cell> cells;
    double tolerance = 0.3;
    for (int round = 0; round < 4; ++round) {
        const auto toLattice = invertMap(*toImage);
        if (!toLattice) {
            return std::nullopt;
        }
        std::vector<Cell> next;
        std::vector<double> distance;
        for (std::size_t k = 0; k < blobs.size(); ++k) {
            const Pt l = applyH(*toLattice, blobs[k].centre);
            if (!std::isfinite(l.x) || !std::isfinite(l.y)) {
                continue;
            }
            const double ra = std::round(l.x), rb = std::round(l.y);
            const double err = std::max(std::abs(l.x - ra), std::abs(l.y - rb));
            if (err > tolerance || std::abs(ra) > 6 || std::abs(rb) > 6) {
                continue;
            }
            const Cell cell{static_cast<int>(ra), static_cast<int>(rb), k};
            const auto same = std::find_if(next.begin(), next.end(), [&](const Cell& c) { return c.a == cell.a && c.b == cell.b; });
            if (same == next.end()) {
                next.push_back(cell);
                distance.push_back(err);
            } else if (err < distance[static_cast<std::size_t>(same - next.begin())]) {
                *same = cell;
                distance[static_cast<std::size_t>(same - next.begin())] = err;
            }
        }
        if (next.size() < 9) {
            return std::nullopt;
        }
        cells = std::move(next);
        std::vector<Pt> src, dst;
        for (const Cell& c : cells) {
            src.push_back({static_cast<double>(c.a), static_cast<double>(c.b)});
            dst.push_back(blobs[c.blob].centre);
        }
        const auto fitted = fitMap(src, dst);
        if (!fitted) {
            return std::nullopt;
        }
        toImage = fitted;
        tolerance = 0.25;
    }
    int minA = 99, maxA = -99, minB = 99, maxB = -99;
    for (const Cell& c : cells) {
        minA = std::min(minA, c.a);
        maxA = std::max(maxA, c.a);
        minB = std::min(minB, c.b);
        maxB = std::max(maxB, c.b);
    }
    const int da = maxA - minA + 1, db = maxB - minB + 1;
    if (da < 2 || db < 2 || !((da <= kChartColumns && db <= kChartRows) || (da <= kChartRows && db <= kChartColumns))) {
        return std::nullopt;
    }
    Candidate out;
    const auto placed = placeLattice(cells, blobs, edition, &out.runnerUp);
    if (!placed || placed->residual > 0.16) {
        return std::nullopt;
    }
    out.placement = *placed;
    std::vector<Pt> chart, image;
    for (const auto& [blob, index] : out.placement.patches) {
        chart.push_back({kChartCellMiddle + (index % kChartColumns), kChartCellMiddle + (index / kChartColumns)});
        image.push_back(blobs[blob].centre);
    }
    const auto map = fitMap(chart, image);
    if (!map) {
        return std::nullopt;
    }
    const Pt corners[4] = {applyH(*map, {0, 0}), applyH(*map, {kChartWidth, 0}), applyH(*map, {kChartWidth, kChartHeight}),
                           applyH(*map, {0, kChartHeight})};
    for (int i = 0; i < 4; ++i) {
        out.corners.p[static_cast<std::size_t>(i)] = {corners[i].x / width, corners[i].y / height};
    }
    if (!chartCornersPlausible(out.corners)) {
        return std::nullopt;
    }
    return out;
}

std::optional<ChartDetection> detectIn(const WorkImage& im, double threshold, ChartEdition edition) {
    const std::vector<Blob> blobs = sameSizeBlobs(flatBlobs(im, threshold));
    if (blobs.size() < 10 || blobs.size() > 160) {
        return std::nullopt;
    }
    double area = 0.0;
    for (const Blob& b : blobs) {
        area += b.area / static_cast<double>(blobs.size());
    }
    const double side = std::sqrt(area) + 2.0; // the flat core is a pixel short on every side
    const double pitch = side * (1.0 + kChartGap);
    struct Hypothesis {
        std::size_t origin;
        Pt u, v;
        int score;
    };
    std::vector<Hypothesis> hypotheses;
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        for (std::size_t j = 0; j < blobs.size(); ++j) {
            if (i == j) {
                continue;
            }
            const Pt u{blobs[j].centre.x - blobs[i].centre.x, blobs[j].centre.y - blobs[i].centre.y};
            const double d = std::hypot(u.x, u.y);
            if (d < 0.75 * pitch || d > 1.4 * pitch) {
                continue;
            }
            for (const double sign : {1.0, -1.0}) {
                const Pt v{-u.y * sign, u.x * sign};
                const double det = u.x * v.y - u.y * v.x;
                int score = 0;
                std::vector<std::array<int, 2>> seen;
                for (const Blob& b : blobs) {
                    const double dx = b.centre.x - blobs[i].centre.x, dy = b.centre.y - blobs[i].centre.y;
                    const double la = (dx * v.y - dy * v.x) / det, lb = (u.x * dy - u.y * dx) / det;
                    const double ra = std::round(la), rb = std::round(lb);
                    if (std::abs(la - ra) > 0.25 || std::abs(lb - rb) > 0.25 || std::abs(ra) > 5 || std::abs(rb) > 5) {
                        continue;
                    }
                    const std::array<int, 2> key{static_cast<int>(ra), static_cast<int>(rb)};
                    if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                        seen.push_back(key);
                        ++score;
                    }
                }
                if (score >= 9) {
                    hypotheses.push_back({i, u, v, score});
                }
            }
        }
    }
    // stable: hypotheses tie on their score, and only the first 12 are tried - every standard library must try the same 12
    std::stable_sort(hypotheses.begin(), hypotheses.end(), [](const Hypothesis& a, const Hypothesis& b) { return a.score > b.score; });
    std::optional<Candidate> best;
    int tried = 0;
    for (const Hypothesis& h : hypotheses) {
        if (tried++ >= 12) {
            break;
        }
        const auto candidate = tryBasis(blobs, h.origin, h.u, h.v, edition, im.width, im.height);
        if (!candidate) {
            continue;
        }
        const double quality = static_cast<double>(candidate->placement.patches.size()) - 40.0 * candidate->placement.residual;
        const double bestQuality =
            best ? static_cast<double>(best->placement.patches.size()) - 40.0 * best->placement.residual : -1e9;
        if (quality > bestQuality) {
            best = candidate;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    ChartDetection out;
    out.corners = best->corners;
    out.patches = static_cast<int>(best->placement.patches.size());
    const double fit = std::clamp(1.0 - best->placement.residual / 0.16, 0.0, 1.0);
    const double separation = best->runnerUp < 1e8 ? std::clamp((best->runnerUp - best->placement.residual) / 0.08, 0.0, 1.0) : 1.0;
    out.confidence = std::clamp((static_cast<double>(out.patches) / kChartPatches) * (0.5 + 0.3 * fit + 0.2 * separation), 0.0, 1.0);
    return out;
}

// ---- Measuring --------------------------------------------------------------------------------------------------

Rgb sampleBilinear(const ColorImage& image, double px, double py) {
    const double fx = std::clamp(px - 0.5, 0.0, static_cast<double>(image.width - 1));
    const double fy = std::clamp(py - 0.5, 0.0, static_cast<double>(image.height - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    const double tx = fx - x0, ty = fy - y0;
    Rgb out{0, 0, 0};
    const auto at = [&](int x, int y, std::size_t c) {
        return static_cast<double>(image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + c]);
    };
    for (std::size_t c = 0; c < 3; ++c) {
        const double top = at(x0, y0, c) * (1 - tx) + at(x1, y0, c) * tx;
        const double bottom = at(x0, y1, c) * (1 - tx) + at(x1, y1, c) * tx;
        out[c] = top * (1 - ty) + bottom * ty;
    }
    return out;
}

} // namespace

// ---- Public: colour science, reference --------------------------------------------------------------------------

std::string_view chartEditionId(ChartEdition edition) { return edition == ChartEdition::AfterNov2014 ? "after-2014" : "before-2014"; }

std::optional<ChartEdition> chartEditionFromId(std::string_view id) {
    if (id == "after-2014") {
        return ChartEdition::AfterNov2014;
    }
    if (id == "before-2014") {
        return ChartEdition::BeforeNov2014;
    }
    return std::nullopt;
}

std::string_view chartPatchName(int index) { return index >= 0 && index < kChartPatches ? kNames[static_cast<std::size_t>(index)] : std::string_view{}; }

Rgb chartReferenceLab(int index, ChartEdition edition) { return referenceOf(edition).lab[static_cast<std::size_t>(std::clamp(index, 0, kChartPatches - 1))]; }
Rgb chartReferenceCode(int index, ChartEdition edition) { return referenceOf(edition).code[static_cast<std::size_t>(std::clamp(index, 0, kChartPatches - 1))]; }
Rgb chartTargetLab(int index, ChartEdition edition) { return referenceOf(edition).target[static_cast<std::size_t>(std::clamp(index, 0, kChartPatches - 1))]; }

double srgbDecode(double code) { return code <= 0.04045 ? code / 12.92 : std::pow((code + 0.055) / 1.055, 2.4); }
double srgbEncode(double linear) { return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055; }

Rgb srgbToLab(const Rgb& code) {
    const Rgb linear{srgbDecode(std::clamp(code[0], 0.0, 1.0)), srgbDecode(std::clamp(code[1], 0.0, 1.0)), srgbDecode(std::clamp(code[2], 0.0, 1.0))};
    return xyzToLab(mul(srgbToXyzD50(), linear));
}

double deltaE2000(const Rgb& lab1, const Rgb& lab2) {
    const double l1 = lab1[0], a1 = lab1[1], b1 = lab1[2], l2 = lab2[0], a2 = lab2[1], b2 = lab2[2];
    const double c1 = std::hypot(a1, b1), c2 = std::hypot(a2, b2);
    const double cBar = (c1 + c2) / 2.0;
    const double cBar7 = std::pow(cBar, 7.0);
    const double g = 0.5 * (1.0 - std::sqrt(cBar7 / (cBar7 + std::pow(25.0, 7.0))));
    const double a1p = (1.0 + g) * a1, a2p = (1.0 + g) * a2;
    const double c1p = std::hypot(a1p, b1), c2p = std::hypot(a2p, b2);
    const double h1p = hueAngle(b1, a1p), h2p = hueAngle(b2, a2p);
    const double dLp = l2 - l1, dCp = c2p - c1p;
    double dhp = 0.0;
    if (c1p * c2p != 0.0) {
        dhp = h2p - h1p;
        if (dhp > 180.0) {
            dhp -= 360.0;
        } else if (dhp < -180.0) {
            dhp += 360.0;
        }
    }
    const double dHp = 2.0 * std::sqrt(c1p * c2p) * std::sin(radians(dhp / 2.0));
    const double lBar = (l1 + l2) / 2.0, cBarp = (c1p + c2p) / 2.0;
    double hBarp = h1p + h2p;
    if (c1p * c2p != 0.0) {
        if (std::abs(h1p - h2p) <= 180.0) {
            hBarp = (h1p + h2p) / 2.0;
        } else if (h1p + h2p < 360.0) {
            hBarp = (h1p + h2p + 360.0) / 2.0;
        } else {
            hBarp = (h1p + h2p - 360.0) / 2.0;
        }
    }
    const double t = 1.0 - 0.17 * std::cos(radians(hBarp - 30.0)) + 0.24 * std::cos(radians(2.0 * hBarp)) +
                     0.32 * std::cos(radians(3.0 * hBarp + 6.0)) - 0.20 * std::cos(radians(4.0 * hBarp - 63.0));
    const double dTheta = 30.0 * std::exp(-std::pow((hBarp - 275.0) / 25.0, 2.0));
    const double cBarp7 = std::pow(cBarp, 7.0);
    const double rc = 2.0 * std::sqrt(cBarp7 / (cBarp7 + std::pow(25.0, 7.0)));
    const double sl = 1.0 + 0.015 * (lBar - 50.0) * (lBar - 50.0) / std::sqrt(20.0 + (lBar - 50.0) * (lBar - 50.0));
    const double sc = 1.0 + 0.045 * cBarp, sh = 1.0 + 0.015 * cBarp * t;
    const double rt = -std::sin(radians(2.0 * dTheta)) * rc;
    const double x = dLp / sl, y = dCp / sc, z = dHp / sh;
    return std::sqrt(std::max(0.0, x * x + y * y + z * z + rt * y * z));
}

// ---- Public: geometry -------------------------------------------------------------------------------------------

ChartCorners rotatedCorners(const ChartCorners& corners, int quarterTurns) {
    ChartCorners out;
    const int n = ((quarterTurns % 4) + 4) % 4;
    for (int i = 0; i < 4; ++i) {
        out.p[static_cast<std::size_t>(i)] = corners.p[static_cast<std::size_t>((i + n) % 4)];
    }
    return out;
}

ChartCorners mirroredCorners(const ChartCorners& corners) {
    return {{corners.p[1], corners.p[0], corners.p[3], corners.p[2]}};
}

ChartCorners defaultChartCorners(double aspect) {
    aspect = std::clamp(aspect, 0.25, 6.0);
    const double shape = kChartWidth / kChartHeight;
    double w = 0.5;
    double h = w * aspect / shape;
    if (h > 0.7) {
        h = 0.7;
        w = h * shape / aspect;
    }
    const double x0 = 0.5 - w / 2.0, x1 = 0.5 + w / 2.0, y0 = 0.5 - h / 2.0, y1 = 0.5 + h / 2.0;
    return {{ChartPoint{x0, y0}, ChartPoint{x1, y0}, ChartPoint{x1, y1}, ChartPoint{x0, y1}}};
}

ChartPoint chartToPicture(const ChartCorners& corners, double x, double y) {
    const auto map = chartMap(corners);
    if (!map) {
        return {};
    }
    const Pt p = applyH(*map, {x, y});
    return {p.x, p.y};
}

std::array<ChartPoint, 4> chartPatchQuad(const ChartCorners& corners, int index, double fraction) {
    std::array<ChartPoint, 4> out{};
    const auto map = chartMap(corners);
    if (!map || index < 0 || index >= kChartPatches) {
        return out;
    }
    const double cx = kChartCellMiddle + (index % kChartColumns), cy = kChartCellMiddle + (index / kChartColumns);
    const double half = std::clamp(fraction, 0.05, 1.0) * kChartPatchSide / 2.0;
    const Pt pts[4] = {{cx - half, cy - half}, {cx + half, cy - half}, {cx + half, cy + half}, {cx - half, cy + half}};
    for (int i = 0; i < 4; ++i) {
        const Pt p = applyH(*map, pts[i]);
        out[static_cast<std::size_t>(i)] = {p.x, p.y};
    }
    return out;
}

bool chartCornersPlausible(const ChartCorners& corners) {
    double sign = 0.0, area = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const ChartPoint &a = corners.p[i], &b = corners.p[(i + 1) % 4], &c = corners.p[(i + 2) % 4];
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || a.x < -0.5 || a.x > 1.5 || a.y < -0.5 || a.y > 1.5) {
            return false;
        }
        const double cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (std::abs(cross) < 1e-9) {
            return false;
        }
        if (sign == 0.0) {
            sign = cross > 0 ? 1.0 : -1.0;
        } else if ((cross > 0 ? 1.0 : -1.0) != sign) {
            return false;
        }
        if (std::hypot(b.x - a.x, b.y - a.y) < 0.01) {
            return false;
        }
        area += a.x * b.y - b.x * a.y;
    }
    return std::abs(area) / 2.0 > 0.002;
}

// ---- Public: detection and measuring ----------------------------------------------------------------------------

std::optional<ChartDetection> detectColorChecker(const ColorImage& image) {
    if (image.width < 48 || image.height < 32 ||
        image.rgba.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return std::nullopt;
    }
    const int longSide = std::max(image.width, image.height);
    std::optional<ChartDetection> best;
    // Two scales (a small chart needs the bigger one) and three flatness limits (noisy pictures need the looser ones).
    int lastK = 0;
    for (const int target : {512, 1024}) {
        const int k = std::max(1, static_cast<int>(std::lround(static_cast<double>(longSide) / target)));
        if (k == lastK) {
            continue; // a small picture has one scale only
        }
        lastK = k;
        const WorkImage work = downscale(image, k);
        if (work.width < 32 || work.height < 24) {
            continue;
        }
        for (const double threshold : {0.025, 0.05, 0.09}) {
            const auto found = detectIn(work, threshold, ChartEdition::AfterNov2014);
            if (found && (!best || found->confidence > best->confidence)) {
                best = found;
            }
        }
        if (best && best->confidence > 0.6) {
            break;
        }
    }
    return best;
}

ChartMeasurement measureChart(const ColorImage& image, const ChartCorners& corners) {
    ChartMeasurement out;
    const auto map = chartMap(corners);
    if (!map || image.width < 2 || image.height < 2 ||
        image.rgba.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return out;
    }
    constexpr int kTaps = 13;
    constexpr double kMiddle = 0.5; // the middle half of the patch
    for (int index = 0; index < kChartPatches; ++index) {
        const double cx = kChartCellMiddle + (index % kChartColumns), cy = kChartCellMiddle + (index / kChartColumns);
        std::vector<std::array<double, 4>> taps; // luma, r, g, b
        bool inside = true;
        for (int j = 0; j < kTaps; ++j) {
            for (int i = 0; i < kTaps; ++i) {
                const double u = (static_cast<double>(i) / (kTaps - 1) - 0.5) * kMiddle * kChartPatchSide;
                const double v = (static_cast<double>(j) / (kTaps - 1) - 0.5) * kMiddle * kChartPatchSide;
                const Pt p = applyH(*map, {cx + u, cy + v});
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
                    inside = false;
                    continue;
                }
                const Rgb c = sampleBilinear(image, p.x * image.width, p.y * image.height);
                taps.push_back({luma(c), c[0], c[1], c[2]});
            }
        }
        PatchMeasure& m = out.patches[static_cast<std::size_t>(index)];
        if (taps.size() < static_cast<std::size_t>(kTaps * kTaps) / 2) {
            continue;
        }
        std::stable_sort(taps.begin(), taps.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
        const std::size_t trim = taps.size() * 15 / 100;
        const std::size_t first = trim, last = taps.size() - trim;
        Rgb mean{0, 0, 0};
        double lumaMean = 0.0;
        for (std::size_t i = first; i < last; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                mean[c] += taps[i][c + 1] / static_cast<double>(last - first);
            }
            lumaMean += taps[i][0] / static_cast<double>(last - first);
        }
        double variance = 0.0;
        for (std::size_t i = first; i < last; ++i) {
            variance += (taps[i][0] - lumaMean) * (taps[i][0] - lumaMean) / static_cast<double>(last - first);
        }
        m.display = mean;
        m.spread = std::sqrt(variance);
        const double top = std::max({mean[0], mean[1], mean[2]});
        m.usable = inside && top < 0.985 && top > 0.01 && m.spread < 0.12;
        out.usable += m.usable ? 1 : 0;
    }
    return out;
}

// ---- Public: the fit --------------------------------------------------------------------------------------------

namespace {

/// Evaluates the picture's patches through a grade.
class Patches {
public:
    Patches(const ChartMeasurement& m, ChartEdition edition, GradeEncoding encoding) : m_encoding(encoding) {
        const Reference& ref = referenceOf(edition);
        for (int i = 0; i < kChartPatches; ++i) {
            const auto k = static_cast<std::size_t>(i);
            m_linear[k] = decode709(m.patches[k].display);
            m_used[k] = m.patches[k].usable;
            m_target[k] = ref.target[k];
            m_code[k] = ref.code[k];
            // The reference as the grade's working values (what the curves and hue points are measured in).
            m_working[k] = encodeWorking(decode709(ref.code[k]), encoding);
            m_weight[k] = i >= 18 ? 2.0 : (i < 2 ? 1.5 : 1.0);
        }
    }

    bool used(int i) const { return m_used[static_cast<std::size_t>(i)]; }
    const Rgb& linear(int i) const { return m_linear[static_cast<std::size_t>(i)]; }
    const Rgb& code(int i) const { return m_code[static_cast<std::size_t>(i)]; }
    const Rgb& working(int i) const { return m_working[static_cast<std::size_t>(i)]; }
    GradeEncoding encoding() const { return m_encoding; }

    /// The monitor's code values of patch i through `spec`, clamped.
    Rgb shown(const GradeSpec& spec, int i) const {
        const Rgb e = encode709(gradePixel(spec, linear(i)));
        return {std::clamp(e[0], 0.0, 1.0), std::clamp(e[1], 0.0, 1.0), std::clamp(e[2], 0.0, 1.0)};
    }

    double error(const GradeSpec& spec, int i) const {
        return deltaE2000(srgbToLab(shown(spec, i)), m_target[static_cast<std::size_t>(i)]);
    }

    /// Weighted mean delta E over the used patches.
    double cost(const GradeSpec& spec) const {
        double sum = 0.0, weights = 0.0;
        for (int i = 0; i < kChartPatches; ++i) {
            if (used(i)) {
                sum += m_weight[static_cast<std::size_t>(i)] * error(spec, i);
                weights += m_weight[static_cast<std::size_t>(i)];
            }
        }
        return weights > 0.0 ? sum / weights : 0.0;
    }

private:
    GradeEncoding m_encoding;
    std::array<Rgb, kChartPatches> m_linear{}, m_target{}, m_code{}, m_working{};
    std::array<bool, kChartPatches> m_used{};
    std::array<double, kChartPatches> m_weight{};
};

GradeSpec specOf(const GradeSpec& base, const GradeParams& params, std::shared_ptr<const BakedCurves> baked) {
    GradeSpec s = base;
    s.params = params;
    s.curves = std::move(baked);
    return s;
}

GradeSpec specOf(const GradeSpec& base, const GradeParams& params, const GradeCurves& curves) {
    return specOf(base, params, bakeCurves(curves));
}

double round1(double v, double unit) { return std::round(v / unit) * unit; }

/// The curve points of one channel from (x, y) nodes: anchors at both ends, increasing x, non-decreasing y, in [0, 1].
std::vector<EffectPoint> curveFromNodes(std::vector<EffectPoint> nodes) {
    std::stable_sort(nodes.begin(), nodes.end(), [](const EffectPoint& a, const EffectPoint& b) { return a.x < b.x; });
    std::vector<EffectPoint> points;
    points.push_back({0.0, 0.0});
    for (const EffectPoint& n : nodes) {
        const double x = std::clamp(n.x, 0.0, 1.0), y = std::clamp(n.y, 0.0, 1.0);
        if (x < 0.015 || x > 0.985) {
            continue;
        }
        if (x - points.back().x < 0.02) {
            continue;
        }
        points.push_back({x, std::max(y, points.back().y + 0.002)});
    }
    if (1.0 - points.back().x >= 0.02) {
        points.push_back({1.0, std::max(1.0, points.back().y + 0.002)});
    }
    for (EffectPoint& p : points) {
        p.y = std::clamp(p.y, 0.0, 1.0);
    }
    return points;
}

} // namespace

ChartMatchResult solveChartMatch(const ChartMeasurement& measurement, const GradeSpec& base, const GradeCurves& baseCurves,
                                 const ChartOptions& options) {
    ChartMatchResult result;
    result.params = base.params;
    result.curves = baseCurves;
    const Patches patches(measurement, options.edition, base.encoding);
    int neutrals = 0;
    for (int i = 0; i < kChartPatches; ++i) {
        result.used += patches.used(i) ? 1 : 0;
        neutrals += i >= 18 && patches.used(i) ? 1 : 0;
    }
    if (result.used < 12 || neutrals < 3) {
        result.error = result.used < 12 ? std::format("only {} of the 24 patches can be read (the rest are clipped, black or uneven): "
                                                      "expose the chart so no patch clips",
                                                      result.used)
                                        : "fewer than three of the grey patches can be read";
        return result;
    }

    // The grey row has to run white to black, whatever the cast: otherwise the chart is turned, mirrored or not the chart.
    {
        double previous = 2.0;
        for (int i = 18; i < kChartPatches; ++i) {
            if (!patches.used(i)) {
                continue;
            }
            const double l = luma(measurement.patches[static_cast<std::size_t>(i)].display);
            if (l > previous + 0.01) {
                result.error = "the grey patches do not run from white to black: the chart is turned or mirrored here (rotate or mirror "
                                "it, or put the corners on the 24 patches)";
                return result;
            }
            previous = l;
        }
    }

    // What the match replaces, set to neutral: the "before" picture.
    GradeParams neutral = base.params;
    neutral.basic = true;
    neutral.curves = true;
    neutral.temperature = 0;
    neutral.tint = 0;
    neutral.exposure = 0;
    neutral.contrast = 0;
    neutral.saturation = 100;
    GradeCurves cleared = baseCurves;
    cleared.master.clear();
    cleared.red.clear();
    cleared.green.clear();
    cleared.blue.clear();
    cleared.hueSat.clear();
    cleared.hueHue.clear();
    cleared.hueLuma.clear();
    const auto clearedBaked = bakeCurves(cleared);
    const GradeSpec beforeSpec = specOf(base, neutral, clearedBaked);

    // 1. The global fit.
    const auto globalSpec = [&](const std::vector<double>& v) {
        GradeParams p = neutral;
        p.temperature = v[0];
        p.tint = v[1];
        p.exposure = v[2];
        p.contrast = v[3];
        p.saturation = options.colours ? v[4] : 100.0;
        return specOf(base, p, clearedBaked);
    };
    const auto globalCost = [&](const std::vector<double>& v) {
        return patches.cost(globalSpec(v)) + 1e-5 * (v[0] * v[0] + v[1] * v[1] + 25.0 * v[2] * v[2] + v[3] * v[3]) / 100.0;
    };
    const std::vector<double> lo{-100.0, -100.0, -5.0, -100.0, 0.0}, hi{100.0, 100.0, 5.0, 100.0, 200.0};
    const std::vector<double> step{20.0, 20.0, 0.4, 15.0, 15.0};
    std::vector<double> best = minimize(globalCost, {0.0, 0.0, 0.0, 0.0, 100.0}, step, lo, hi, 450);
    {
        // A second start: balance the mid grey patch, then bring its brightness to the reference.
        int grey = -1;
        for (const int candidate : {21, 20, 22, 19}) {
            if (patches.used(candidate)) {
                grey = candidate;
                break;
            }
        }
        if (grey >= 0) {
            const Rgb lin = patches.linear(grey);
            const auto castCost = [&](const std::vector<double>& v) {
                const Rgb balanced = mul(whiteBalanceMatrix(v[0], v[1]), lin);
                const Rgb lab = linearToOklab({std::max(balanced[0], 0.0), std::max(balanced[1], 0.0), std::max(balanced[2], 0.0)});
                return lab[1] * lab[1] + lab[2] * lab[2];
            };
            const auto wb = minimize(castCost, {0.0, 0.0}, {25.0, 25.0}, {-100.0, -100.0}, {100.0, 100.0}, 200);
            const Rgb balanced = mul(whiteBalanceMatrix(wb[0], wb[1]), lin);
            const double now = luma(balanced), want = luma(decode709(patches.code(grey)));
            const double stops = now > 1e-6 && want > 1e-6 ? std::clamp(std::log2(want / now), -4.0, 4.0) : 0.0;
            const std::vector<double> second = minimize(globalCost, {wb[0], wb[1], stops, 0.0, 100.0}, step, lo, hi, 450);
            if (globalCost(second) < globalCost(best)) {
                best = second;
            }
        }
    }
    best[0] = round1(best[0], 0.1);
    best[1] = round1(best[1], 0.1);
    best[2] = round1(best[2], 0.01);
    best[3] = round1(best[3], 0.1);
    best[4] = round1(best[4], 0.1);
    GradeParams params = globalSpec(best).params;
    GradeCurves curves = cleared;
    const double costNone = patches.cost(beforeSpec);
    double current = patches.cost(specOf(base, params, clearedBaked));
    if (current >= costNone) { // nothing in the global fit helps: start from the unchanged picture
        params = neutral;
        current = costNone;
    }

    // 2. Per-channel curves from the grey row: the cast that depends on brightness.
    {
        GradeParams pre = params;
        pre.curves = false;
        pre.wheels = false;
        pre.hsl = false;
        pre.vignette = false;
        const GradeSpec preSpec = specOf(base, pre, clearedBaked);
        std::vector<int> row;
        for (int i = 18; i < kChartPatches; ++i) {
            if (patches.used(i)) {
                row.push_back(i);
            }
        }
        std::array<std::vector<EffectPoint>, 3> nodes;
        for (const int i : row) {
            const Rgb e = gradeMain(preSpec, patches.linear(i));
            for (std::size_t c = 0; c < 3; ++c) {
                nodes[c].push_back({e[c], e[c]});
            }
        }
        const auto build = [&](const std::array<std::vector<EffectPoint>, 3>& n) {
            GradeCurves out = cleared;
            out.red = curveFromNodes(n[0]);
            out.green = curveFromNodes(n[1]);
            out.blue = curveFromNodes(n[2]);
            return out;
        };
        for (int iteration = 0; iteration < 3; ++iteration) {
            const GradeSpec trial = specOf(base, params, build(nodes));
            for (std::size_t k = 0; k < row.size(); ++k) {
                const Rgb f = encodeWorking(gradePixel(trial, patches.linear(row[k])), base.encoding);
                for (std::size_t c = 0; c < 3; ++c) {
                    nodes[c][k].y += patches.working(row[k])[c] - f[c];
                }
            }
        }
        std::array<bool, 3> needed{};
        for (std::size_t c = 0; c < 3; ++c) {
            for (const EffectPoint& n : nodes[c]) {
                needed[c] = needed[c] || std::abs(n.y - n.x) > 0.005;
            }
            if (!needed[c]) {
                nodes[c].clear();
            }
        }
        GradeCurves withCurves = cleared;
        withCurves.red = needed[0] ? curveFromNodes(nodes[0]) : std::vector<EffectPoint>{};
        withCurves.green = needed[1] ? curveFromNodes(nodes[1]) : std::vector<EffectPoint>{};
        withCurves.blue = needed[2] ? curveFromNodes(nodes[2]) : std::vector<EffectPoint>{};
        const double after = patches.cost(specOf(base, params, withCurves));
        if ((needed[0] || needed[1] || needed[2]) && after < current - 1e-4) {
            curves = withCurves;
            current = after;
            result.usedRgbCurves = true;
        }
    }

    // 3. The saturation again, now that the grey row is right.
    if (options.colours) {
        const auto curvesBaked = bakeCurves(curves);
        const auto satCost = [&](double s) {
            GradeParams p = params;
            p.saturation = s;
            return patches.cost(specOf(base, p, curvesBaked));
        };
        double a = 20.0, b = 200.0;
        const double phi = (std::sqrt(5.0) - 1.0) / 2.0;
        double c = b - phi * (b - a), d = a + phi * (b - a);
        double fc = satCost(c), fd = satCost(d);
        for (int i = 0; i < 28; ++i) {
            if (fc < fd) {
                b = d;
                d = c;
                fd = fc;
                c = b - phi * (b - a);
                fc = satCost(c);
            } else {
                a = c;
                c = d;
                fc = fd;
                d = a + phi * (b - a);
                fd = satCost(d);
            }
        }
        const double s = round1((a + b) / 2.0, 0.1);
        if (satCost(s) < current - 1e-4) {
            params.saturation = s;
            current = satCost(s);
        }
    }

    // 4. Hue vs hue / saturation / luma at the hues of the coloured patches.
    std::array<double, kChartPatches> heldOut{};
    std::array<bool, kChartPatches> heldOutSet{};
    if (options.colours) {
        GradeParams pre = params;
        pre.wheels = false;
        pre.hsl = false;
        pre.vignette = false;
        const GradeSpec specNow = specOf(base, params, curves);
        const GradeSpec preSpec = specOf(base, pre, curves);
        struct Point {
            double hue;
            double sat, turn, lum;
            int patch;
        };
        std::vector<Point> points;
        for (int i = 0; i < 18; ++i) {
            if (!patches.used(i)) {
                continue;
            }
            const Rgb e = gradeMain(preSpec, patches.linear(i));
            const Hsv hsv = toHsv(e);
            if (hsv.s < 0.08 || hsv.v < 0.05) {
                continue;
            }
            // What the stages after the hue curves still do is carried over by the residual they leave.
            const Rgb f = encodeWorking(gradePixel(specNow, patches.linear(i)), base.encoding);
            Rgb want{0, 0, 0};
            for (std::size_t c = 0; c < 3; ++c) {
                want[c] = e[c] + (patches.working(i)[c] - f[c]);
            }
            const double yE = luma(e), yT = luma(want);
            const Rgb chromaT{want[0] - yT, want[1] - yT, want[2] - yT};
            double bestTurn = 0.0, bestFactor = 1.0, bestError = 1e18;
            for (int s = -60; s <= 60; ++s) {
                const double turn = s * 0.002;
                const Rgb r = rotateHue(e, turn);
                const double yR = luma(r);
                const Rgb chromaR{r[0] - yR, r[1] - yR, r[2] - yR};
                const double den = chromaR[0] * chromaR[0] + chromaR[1] * chromaR[1] + chromaR[2] * chromaR[2];
                if (den < 1e-9) {
                    continue;
                }
                const double factor = std::clamp((chromaR[0] * chromaT[0] + chromaR[1] * chromaT[1] + chromaR[2] * chromaT[2]) / den, 0.05, 1.95);
                double err = 0.0;
                for (std::size_t c = 0; c < 3; ++c) {
                    err += std::pow(factor * chromaR[c] - chromaT[c], 2.0);
                }
                if (err < bestError) {
                    bestError = err;
                    bestTurn = turn;
                    bestFactor = factor;
                }
            }
            const double dl = yT - yE;
            const double lumY = hsv.s > 0.08 ? std::clamp(0.5 + dl / (0.5 * hsv.s), 0.0, 1.0) : 0.5;
            points.push_back({hsv.h, bestFactor / 2.0, 0.5 + bestTurn, lumY, i});
        }
        // The hue curves from a set of points: patches within `gap` turns of hue share one point (their average), at
        // `damping` of the full correction.
        const auto hueCurvesFrom = [&](std::vector<Point> list, double damping, double gap, int* count) {
            std::stable_sort(list.begin(), list.end(), [](const Point& a, const Point& b) { return a.hue < b.hue; });
            std::vector<Point> merged;
            std::vector<double> members;
            for (const Point& p : list) {
                if (!merged.empty() && p.hue - merged.back().hue < gap) {
                    const double n = members.back();
                    Point& m = merged.back();
                    m.hue = (m.hue * n + p.hue) / (n + 1.0);
                    m.sat = (m.sat * n + p.sat) / (n + 1.0);
                    m.turn = (m.turn * n + p.turn) / (n + 1.0);
                    m.lum = (m.lum * n + p.lum) / (n + 1.0);
                    members.back() += 1.0;
                } else {
                    merged.push_back(p);
                    members.push_back(1.0);
                }
            }
            if (count) {
                *count = static_cast<int>(merged.size());
            }
            GradeCurves trial = curves;
            trial.hueSat.clear();
            trial.hueHue.clear();
            trial.hueLuma.clear();
            if (merged.size() < 2) {
                return trial; // a periodic curve needs two points
            }
            for (const Point& p : merged) {
                const double x = std::clamp(p.hue, 0.0, 0.9999);
                trial.hueSat.push_back({x, std::clamp(0.5 + damping * (p.sat - 0.5), 0.0, 1.0)});
                trial.hueHue.push_back({x, std::clamp(0.5 + damping * (p.turn - 0.5), 0.0, 1.0)});
                trial.hueLuma.push_back({x, std::clamp(0.5 + damping * (p.lum - 0.5), 0.0, 1.0)});
            }
            return trial;
        };
        // Leave-one-out: the error at each coloured patch of the curves made from the other patches. The points are kept
        // only when that - the error between the patches, which is what the picture's other colours get - falls clearly,
        // and the damping is the one that does best by it: three numbers per patch can always reproduce that patch.
        const auto leaveOneOut = [&](double damping, double gap, std::array<double, kChartPatches>* perPatch) {
            double sum = 0.0;
            for (std::size_t k = 0; k < points.size(); ++k) {
                std::vector<Point> others;
                for (std::size_t j = 0; j < points.size(); ++j) {
                    if (j != k) {
                        others.push_back(points[j]);
                    }
                }
                const double e = patches.error(specOf(base, params, hueCurvesFrom(others, damping, gap, nullptr)), points[k].patch);
                if (perPatch) {
                    (*perPatch)[static_cast<std::size_t>(points[k].patch)] = e;
                }
                sum += e;
            }
            return sum / static_cast<double>(points.size());
        };
        if (points.size() >= 5) {
            double without = 0.0;
            for (const Point& p : points) {
                without += patches.error(specNow, p.patch) / static_cast<double>(points.size());
            }
            double bestLoo = without - 0.25, bestDamping = 0.0, bestGap = 0.0;
            for (const double gap : {0.015, 0.03, 0.05}) {
                for (const double damping : {1.0, 0.75, 0.5, 0.3}) {
                    const double loo = leaveOneOut(damping, gap, nullptr);
                    if (loo < bestLoo) {
                        bestLoo = loo;
                        bestDamping = damping;
                        bestGap = gap;
                    }
                }
            }
            if (bestDamping > 0.0) {
                int count = 0;
                const GradeCurves trial = hueCurvesFrom(points, bestDamping, bestGap, &count);
                const double cost = patches.cost(specOf(base, params, trial));
                if (cost < current - 1e-3) {
                    curves = trial;
                    current = cost;
                    result.hueCurvePoints = count;
                    leaveOneOut(bestDamping, bestGap, &heldOut);
                    for (const Point& p : points) {
                        heldOutSet[static_cast<std::size_t>(p.patch)] = true;
                    }
                }
            }
        }
    }

    // The result and its numbers.
    params.basic = true;
    params.curves = true;
    result.params = params;
    result.curves = curves;
    const GradeSpec finalSpec = specOf(base, params, curves);
    double sumBefore = 0.0, sumAfter = 0.0, sumHeldOut = 0.0;
    for (int i = 0; i < kChartPatches; ++i) {
        ChartPatchReport& r = result.patches[static_cast<std::size_t>(i)];
        r.index = i;
        r.name = std::string(chartPatchName(i));
        r.reference = patches.code(i);
        r.used = patches.used(i);
        r.before = patches.shown(beforeSpec, i);
        r.after = patches.shown(finalSpec, i);
        r.deltaBefore = patches.error(beforeSpec, i);
        r.deltaAfter = patches.error(finalSpec, i);
        if (r.used) {
            sumBefore += r.deltaBefore;
            sumAfter += r.deltaAfter;
            sumHeldOut += heldOutSet[static_cast<std::size_t>(i)] ? std::max(r.deltaAfter, heldOut[static_cast<std::size_t>(i)]) : r.deltaAfter;
            result.maxBefore = std::max(result.maxBefore, r.deltaBefore);
            result.maxAfter = std::max(result.maxAfter, r.deltaAfter);
        }
    }
    result.meanBefore = sumBefore / result.used;
    result.meanAfter = sumAfter / result.used;
    result.holdOutMean = sumHeldOut / result.used;
    result.improved = result.meanAfter < result.meanBefore - 0.05;
    if (result.used < kChartPatches) {
        result.warnings.push_back(std::format("{} patches could not be read (clipped, black or uneven) and were left out", kChartPatches - result.used));
    }
    result.summary = std::format("temperature {:+.1f}, tint {:+.1f}, exposure {:+.2f}, contrast {:+.1f}, saturation {:.1f}", params.temperature,
                                 params.tint, params.exposure, params.contrast, params.saturation);
    if (result.usedRgbCurves) {
        result.summary += "; RGB curves on the grey row";
    }
    if (result.hueCurvePoints > 0) {
        result.summary += std::format("; hue curves at {} hues", result.hueCurvePoints);
    }
    result.summary += std::format("; mean delta E {:.1f} -> {:.1f} (worst {:.1f} -> {:.1f})", result.meanBefore, result.meanAfter,
                                  result.maxBefore, result.maxAfter);
    if (result.hueCurvePoints > 0) {
        result.summary += std::format(", about {:.1f} between the patches", result.holdOutMean);
    }
    result.ok = true;
    return result;
}

} // namespace wabcut::color
