#include <wabcut/ai/vision/Saliency.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace wabcut::ai::vision {

namespace {

using Complex = std::complex<double>;

int nextPowerOfTwo(int n) {
    int p = 1;
    while (p < n) {
        p *= 2;
    }
    return p;
}

/// In-place iterative radix-2 FFT of `a` (size a power of two); `inverse` without the 1/n scale.
void fft(std::vector<Complex>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * std::numbers::pi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const Complex wlen(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

/// 2-D FFT of a rows x cols grid (both powers of two).
void fft2(std::vector<Complex>& grid, int rows, int cols, bool inverse) {
    std::vector<Complex> line(static_cast<std::size_t>(std::max(rows, cols)));
    for (int r = 0; r < rows; ++r) {
        line.assign(grid.begin() + static_cast<std::ptrdiff_t>(r) * cols, grid.begin() + static_cast<std::ptrdiff_t>(r + 1) * cols);
        fft(line, inverse);
        std::copy(line.begin(), line.end(), grid.begin() + static_cast<std::ptrdiff_t>(r) * cols);
    }
    line.resize(static_cast<std::size_t>(rows));
    for (int c = 0; c < cols; ++c) {
        for (int r = 0; r < rows; ++r) {
            line[static_cast<std::size_t>(r)] = grid[static_cast<std::size_t>(r) * cols + c];
        }
        fft(line, inverse);
        for (int r = 0; r < rows; ++r) {
            grid[static_cast<std::size_t>(r) * cols + c] = line[static_cast<std::size_t>(r)];
        }
    }
}

/// Mean over a (2 radius + 1)^2 window, edges clamped.
std::vector<double> boxBlur(const std::vector<double>& in, int rows, int cols, int radius) {
    std::vector<double> out(in.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            double sum = 0.0;
            int n = 0;
            for (int dr = -radius; dr <= radius; ++dr) {
                for (int dc = -radius; dc <= radius; ++dc) {
                    const int rr = std::clamp(r + dr, 0, rows - 1);
                    const int cc = std::clamp(c + dc, 0, cols - 1);
                    sum += in[static_cast<std::size_t>(rr) * cols + cc];
                    ++n;
                }
            }
            out[static_cast<std::size_t>(r) * cols + c] = sum / n;
        }
    }
    return out;
}

void normalize01(std::vector<float>& v) {
    if (v.empty()) {
        return;
    }
    const auto [lo, hi] = std::ranges::minmax_element(v);
    const float min = *lo;
    const float range = *hi - min;
    for (float& x : v) {
        x = range > 1e-12f ? (x - min) / range : 0.0f;
    }
}

} // namespace

GrayImage toGray(const Image& image, int width) {
    GrayImage out;
    if (!image.valid() || width <= 0) {
        return out;
    }
    const int w = std::min(width, image.width);
    const int h = std::max(1, static_cast<int>(std::lround(static_cast<double>(image.height) * w / image.width)));
    const Image small = search::resizeBilinear(image, w, h);
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        const std::uint8_t* px = &small.rgb[i * 3];
        out.pixels[i] = (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.0f;
    }
    return out;
}

std::vector<float> spectralResidual(const GrayImage& image) {
    if (!image.valid()) {
        return {};
    }
    const int rows = nextPowerOfTwo(image.height);
    const int cols = nextPowerOfTwo(image.width);
    std::vector<Complex> grid(static_cast<std::size_t>(rows) * cols);
    double mean = 0.0;
    for (const float p : image.pixels) {
        mean += p;
    }
    mean /= static_cast<double>(image.pixels.size());
    for (int r = 0; r < rows; ++r) { // pad with the mean (no artificial edges)
        for (int c = 0; c < cols; ++c) {
            const bool inside = r < image.height && c < image.width;
            grid[static_cast<std::size_t>(r) * cols + c] =
                inside ? Complex(image.pixels[static_cast<std::size_t>(r) * image.width + c], 0.0) : Complex(mean, 0.0);
        }
    }
    fft2(grid, rows, cols, false);
    std::vector<double> logAmplitude(grid.size());
    std::vector<double> phase(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
        logAmplitude[i] = std::log(std::abs(grid[i]) + 1e-9);
        phase[i] = std::arg(grid[i]);
    }
    const std::vector<double> average = boxBlur(logAmplitude, rows, cols, 1);
    for (std::size_t i = 0; i < grid.size(); ++i) {
        grid[i] = std::polar(std::exp(logAmplitude[i] - average[i]), phase[i]); // the spectral residual
    }
    fft2(grid, rows, cols, true);
    std::vector<double> map(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
        map[i] = std::norm(grid[i]);
    }
    // A Gaussian-like smoothing: three passes of a 3 x 3 mean.
    for (int pass = 0; pass < 3; ++pass) {
        map = boxBlur(map, rows, cols, 1);
    }
    std::vector<float> out(image.pixels.size());
    for (int r = 0; r < image.height; ++r) {
        for (int c = 0; c < image.width; ++c) {
            out[static_cast<std::size_t>(r) * image.width + c] = static_cast<float>(map[static_cast<std::size_t>(r) * cols + c]);
        }
    }
    normalize01(out);
    return out;
}

float motionEnergy(const GrayImage& a, const GrayImage& b) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        return 0.0f;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        sum += std::abs(a.pixels[i] - b.pixels[i]);
    }
    return static_cast<float>(sum / static_cast<double>(a.pixels.size()));
}

std::optional<SalientRegion> salientRegion(const GrayImage& current, const GrayImage* previous, float motionWeight) {
    std::vector<float> map = spectralResidual(current);
    if (map.empty()) {
        return std::nullopt;
    }
    const bool moved = previous && motionWeight > 0.0f && motionEnergy(current, *previous) > 0.0f;
    if (!moved) { // a flat picture has no salient part (its residual is numerical noise)
        double mean = 0.0;
        for (const float v : current.pixels) {
            mean += v;
        }
        mean /= static_cast<double>(current.pixels.size());
        double variance = 0.0;
        for (const float v : current.pixels) {
            variance += (v - mean) * (v - mean);
        }
        if (variance / static_cast<double>(current.pixels.size()) < 1e-5) {
            return std::nullopt;
        }
    }
    if (moved) {
        std::vector<float> motion(map.size());
        for (std::size_t i = 0; i < map.size(); ++i) {
            motion[i] = std::abs(current.pixels[i] - previous->pixels[i]);
        }
        normalize01(motion);
        for (std::size_t i = 0; i < map.size(); ++i) {
            map[i] += motionWeight * motion[i];
        }
        normalize01(map);
    }
    double mean = 0.0;
    float peak = 0.0f;
    for (const float v : map) {
        mean += v;
        peak = std::max(peak, v);
    }
    mean /= static_cast<double>(map.size());
    if (mean <= 1e-6 || peak <= 0.0f) {
        return std::nullopt;
    }
    const float threshold = static_cast<float>(std::min(3.0 * mean, 0.5 * peak + 0.5 * mean));
    double weight = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    for (int r = 0; r < current.height; ++r) {
        for (int c = 0; c < current.width; ++c) {
            const float v = map[static_cast<std::size_t>(r) * current.width + c];
            if (v >= threshold) {
                weight += v;
                sx += v * (c + 0.5);
                sy += v * (r + 0.5);
            }
        }
    }
    if (weight <= 0.0) {
        return std::nullopt;
    }
    const double cx = sx / weight;
    const double cy = sy / weight;
    double vx = 0.0;
    double vy = 0.0;
    for (int r = 0; r < current.height; ++r) {
        for (int c = 0; c < current.width; ++c) {
            const float v = map[static_cast<std::size_t>(r) * current.width + c];
            if (v >= threshold) {
                vx += v * (c + 0.5 - cx) * (c + 0.5 - cx);
                vy += v * (r + 0.5 - cy) * (r + 0.5 - cy);
            }
        }
    }
    const double halfW = std::max(1.5 * std::sqrt(vx / weight), 1.0);
    const double halfH = std::max(1.5 * std::sqrt(vy / weight), 1.0);
    const double x0 = std::clamp(cx - halfW, 0.0, static_cast<double>(current.width));
    const double y0 = std::clamp(cy - halfH, 0.0, static_cast<double>(current.height));
    const double x1 = std::clamp(cx + halfW, 0.0, static_cast<double>(current.width));
    const double y1 = std::clamp(cy + halfH, 0.0, static_cast<double>(current.height));
    SalientRegion region;
    region.box = normalized(Box{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0), static_cast<float>(y1 - y0)},
                            static_cast<float>(current.width), static_cast<float>(current.height));
    region.strength = static_cast<float>(peak / mean);
    return region;
}

} // namespace wabcut::ai::vision
