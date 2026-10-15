#include <wabcut/render/Matte.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace wabcut::render {

namespace {

std::mutex& sourceMutex() {
    static std::mutex m;
    return m;
}

std::shared_ptr<MatteSource>& sourceSlot() {
    static std::shared_ptr<MatteSource> s;
    return s;
}

} // namespace

void setMatteSource(std::shared_ptr<MatteSource> source) {
    std::lock_guard lock(sourceMutex());
    sourceSlot() = std::move(source);
}

std::shared_ptr<MatteSource> matteSource() {
    std::lock_guard lock(sourceMutex());
    return sourceSlot();
}

bool usableField(const MatteField& field) {
    if (field.width <= 0 || field.height <= 0 ||
        field.distance.size() != static_cast<std::size_t>(field.width) * static_cast<std::size_t>(field.height)) {
        return false;
    }
    return std::ranges::all_of(field.distance, [](float d) { return std::isfinite(d); });
}

std::uint16_t toHalf(float value) {
    std::uint32_t f = 0;
    std::memcpy(&f, &value, sizeof f);
    const std::uint32_t sign = (f >> 16) & 0x8000u;
    const std::uint32_t exponent = (f >> 23) & 0xFFu;
    std::uint32_t mantissa = f & 0x7FFFFFu;
    if (exponent == 0xFFu) { // inf / NaN
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    }
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00u); // overflow: inf
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<std::uint16_t>(sign); // underflow: zero
        }
        mantissa |= 0x800000u;
        const int shift = 14 - e;
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1u))) {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) {
        ++half; // may carry into the exponent: still the right rounding (up to the next power of two, or inf)
    }
    return static_cast<std::uint16_t>(sign | half);
}

double matteDistance(const MatteField& field, Vec2 p) {
    if (!usableField(field)) {
        return -1.0e4; // kMaskEmptyDistance: covers nothing (maskSpecAt never hands over such a field)
    }
    // Texel centres at (i + 0.5) / width; clamp to the edge texels (the GPU sampler's clamp-to-edge).
    const double fx = std::clamp(p.x * field.width - 0.5, 0.0, static_cast<double>(field.width - 1));
    const double fy = std::clamp(p.y * field.height - 0.5, 0.0, static_cast<double>(field.height - 1));
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = std::min(x0 + 1, field.width - 1);
    const int y1 = std::min(y0 + 1, field.height - 1);
    const double tx = fx - x0;
    const double ty = fy - y0;
    const auto at = [&](int x, int y) { return static_cast<double>(field.distance[static_cast<std::size_t>(y) * field.width + x]); };
    const double top = at(x0, y0) * (1.0 - tx) + at(x1, y0) * tx;
    const double bottom = at(x0, y1) * (1.0 - tx) + at(x1, y1) * tx;
    return top * (1.0 - ty) + bottom * ty;
}

} // namespace wabcut::render
