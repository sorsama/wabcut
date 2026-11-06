#include <wabcut/render/FaceMap.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::render {

namespace {

/// The IEC 61966-2-1 encoding of the linear value i / 255, rounded to a byte.
const std::array<std::uint8_t, 256>& encodeTable() {
    static const std::array<std::uint8_t, 256> table = [] {
        std::array<std::uint8_t, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double l = static_cast<double>(i) / 255.0;
            const double e = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
            t[i] = static_cast<std::uint8_t>(std::lround(std::clamp(e, 0.0, 1.0) * 255.0));
        }
        return t;
    }();
    return table;
}

} // namespace

std::uint8_t faceMapByte(std::uint8_t coverage) {
    return encodeTable()[coverage];
}

std::shared_ptr<const TitleRaster> makeFaceMapRaster(int width, int height, std::span<const std::uint8_t> coverage) {
    const std::size_t pixels = static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0));
    std::vector<std::uint8_t> rgba(pixels * 4, 0);
    const bool matches = coverage.size() == pixels * 3;
    const auto& table = encodeTable();
    for (std::size_t i = 0; i < pixels; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            rgba[4 * i + c] = matches ? table[coverage[3 * i + c]] : 0;
        }
        rgba[4 * i + 3] = 255;
    }
    return std::make_shared<const TitleRaster>(width, height, std::move(rgba), TitleLayoutInfo{});
}

} // namespace wabcut::render
