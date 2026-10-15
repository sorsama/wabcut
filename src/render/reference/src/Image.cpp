#include <wabcut/render/reference/Image.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::render::reference {

namespace {

// The BT.709 OETF constants (ColorPipeline.cpp kRec709Alpha / kRec709Beta).
constexpr double kRec709Alpha = 1.09929682680944;
constexpr double kRec709Beta = 0.018053968510807;

std::size_t extent(int v) { return v > 0 ? static_cast<std::size_t>(v) : 0; }

} // namespace

Image::Image(int w, int h) : width(std::max(w, 0)), height(std::max(h, 0)), rgba(extent(w) * extent(h) * 4, 0.0f) {}

Image Image::filled(int w, int h, const Pixel& p) {
    Image out(w, h);
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        std::copy(p.begin(), p.end(), out.rgba.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return out;
}

std::size_t Image::pixelCount() const { return extent(width) * extent(height); }

std::size_t Image::index(int x, int y) const {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
}

Pixel Image::at(int x, int y) const {
    const std::size_t i = index(x, y);
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

Rgba Image::atD(int x, int y) const {
    const std::size_t i = index(x, y);
    return {static_cast<double>(rgba[i]), static_cast<double>(rgba[i + 1]), static_cast<double>(rgba[i + 2]),
            static_cast<double>(rgba[i + 3])};
}

void Image::set(int x, int y, const Pixel& p) {
    const std::size_t i = index(x, y);
    std::copy(p.begin(), p.end(), rgba.begin() + static_cast<std::ptrdiff_t>(i));
}

void Image::setD(int x, int y, const Rgba& p) {
    set(x, y, {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), static_cast<float>(p[3])});
}

// ---- Transfer curves ----------------------------------------------------------------------------------------------

double toLinear(double encoded) {
    const double v = std::clamp(encoded, 0.0, 1.0);
    return v < 4.5 * kRec709Beta ? v / 4.5 : std::pow((v + (kRec709Alpha - 1.0)) / kRec709Alpha, 1.0 / 0.45);
}

double toEncoded(double linear) {
    const double l = std::clamp(linear, 0.0, 1.0);
    return l < kRec709Beta ? 4.5 * l : kRec709Alpha * std::pow(l, 0.45) - (kRec709Alpha - 1.0);
}

float toLinear(float encoded) { return static_cast<float>(toLinear(static_cast<double>(encoded))); }

float toEncoded(float linear) { return static_cast<float>(toEncoded(static_cast<double>(linear))); }

double srgbToLinear(double encoded) {
    const double v = std::clamp(encoded, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double linear) {
    const double l = std::clamp(linear, 0.0, 1.0);
    return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

double luma(const Rgb& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

// ---- Sampling -----------------------------------------------------------------------------------------------------

Rgba sampleBilinear(const Image& image, double x, double y) {
    if (image.empty()) {
        return {0.0, 0.0, 0.0, 0.0};
    }
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const double flx = std::floor(fx);
    const double fly = std::floor(fy);
    const double tx = fx - flx;
    const double ty = fy - fly;
    // Clamp in double before converting, so far-away points cannot overflow int.
    const auto texel = [&](double xi, double yi) {
        const int cx = static_cast<int>(std::clamp(xi, 0.0, static_cast<double>(image.width - 1)));
        const int cy = static_cast<int>(std::clamp(yi, 0.0, static_cast<double>(image.height - 1)));
        return image.atD(cx, cy);
    };
    const Rgba p00 = texel(flx, fly);
    const Rgba p10 = texel(flx + 1.0, fly);
    const Rgba p01 = texel(flx, fly + 1.0);
    const Rgba p11 = texel(flx + 1.0, fly + 1.0);
    Rgba out{};
    for (std::size_t c = 0; c < 4; ++c) {
        const double top = p00[c] + (p10[c] - p00[c]) * tx;
        const double bottom = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = top + (bottom - top) * ty;
    }
    return out;
}

// ---- Comparison ---------------------------------------------------------------------------------------------------

Difference compare(const Image& a, const Image& b) {
    const double infinity = std::numeric_limits<double>::infinity();
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return {infinity, 0.0};
    }
    Difference out;
    double sumSquares = 0.0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        const double d = std::abs(static_cast<double>(a.rgba[i]) - static_cast<double>(b.rgba[i]));
        out.maxAbs = std::max(out.maxAbs, std::isnan(d) ? infinity : d);
        sumSquares += d * d;
    }
    const double mse = a.rgba.empty() ? 0.0 : sumSquares / static_cast<double>(a.rgba.size());
    out.psnr = mse > 0.0 ? 10.0 * std::log10(1.0 / mse) : (std::isnan(mse) ? 0.0 : infinity);
    return out;
}

} // namespace wabcut::render::reference
