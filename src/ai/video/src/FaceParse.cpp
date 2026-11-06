#include <wabcut/ai/video/FaceParse.h>

#include <wabcut/ai/video/ModelKeys.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::ai::video {

namespace {

constexpr int kClasses = 19;
constexpr int kSize = kFaceParseSize;
constexpr std::size_t kPlane = static_cast<std::size_t>(kSize) * static_cast<std::size_t>(kSize);
constexpr std::array<float, 3> kMean{0.485f, 0.456f, 0.406f};
constexpr std::array<float, 3> kDeviation{0.229f, 0.224f, 0.225f};
constexpr double kFeather = 24.0; ///< pixels of the crop faded at its edge when pasted

/// How much class `c` counts towards each plane.
struct Weights {
    float skin;
    float mouth;
    float eyes;
};
constexpr Weights weightOf(int c) {
    switch (c) {
    case 1: return {1.0f, 0.0f, 0.0f};  // skin
    case 10: return {1.0f, 0.0f, 0.0f}; // nose
    case 7:
    case 8: return {1.0f, 0.0f, 0.0f};  // ears
    case 14: return {0.8f, 0.0f, 0.0f}; // neck
    case 11: return {0.0f, 1.0f, 0.0f}; // mouth (inside)
    case 4:
    case 5: return {0.0f, 0.0f, 1.0f};  // eyes
    default: return {0.0f, 0.0f, 0.0f};
    }
}

std::uint8_t toByte(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

FaceCrop faceCropFor(const FaceBox& box) {
    FaceCrop crop;
    crop.side = std::max(96.0, 1.5 * std::max(box.w, box.h));
    crop.cx = box.x + box.w / 2.0;
    crop.cy = box.y + box.h / 2.0 - 0.03 * crop.side;
    return crop;
}

std::vector<float> faceParseInput(const Picture& picture, const FaceCrop& crop) {
    if (!picture.valid() || !(crop.side > 0.0)) {
        return {};
    }
    std::vector<float> input(kPlane * 3);
    const double left = crop.cx - crop.side / 2.0;
    const double top = crop.cy - crop.side / 2.0;
    const double step = crop.side / kSize;
    const auto at = [&](int x, int y, std::size_t channel) {
        const int cx = std::clamp(x, 0, picture.width - 1);
        const int cy = std::clamp(y, 0, picture.height - 1);
        return static_cast<double>(picture.rgb[(static_cast<std::size_t>(cy) * static_cast<std::size_t>(picture.width) + static_cast<std::size_t>(cx)) * 3 + channel]) /
               65535.0;
    };
    for (int j = 0; j < kSize; ++j) {
        const double sy = std::clamp(top + (j + 0.5) * step - 0.5, 0.0, picture.height - 1.0);
        const int y0 = static_cast<int>(std::floor(sy));
        const double fy = sy - y0;
        for (int i = 0; i < kSize; ++i) {
            const double sx = std::clamp(left + (i + 0.5) * step - 0.5, 0.0, picture.width - 1.0);
            const int x0 = static_cast<int>(std::floor(sx));
            const double fx = sx - x0;
            for (std::size_t c = 0; c < 3; ++c) {
                const double topRow = at(x0, y0, c) * (1.0 - fx) + at(x0 + 1, y0, c) * fx;
                const double bottomRow = at(x0, y0 + 1, c) * (1.0 - fx) + at(x0 + 1, y0 + 1, c) * fx;
                const double v = topRow * (1.0 - fy) + bottomRow * fy;
                input[c * kPlane + static_cast<std::size_t>(j) * kSize + static_cast<std::size_t>(i)] =
                    (static_cast<float>(v) - kMean[c]) / kDeviation[c];
            }
        }
    }
    return input;
}

FaceClasses faceClassesOf(std::span<const float> logits) {
    FaceClasses out;
    if (logits.size() != kPlane * kClasses) {
        return out;
    }
    std::vector<float> peak(kPlane, -1e30f);
    for (int c = 0; c < kClasses; ++c) {
        const float* plane = logits.data() + static_cast<std::size_t>(c) * kPlane;
        for (std::size_t i = 0; i < kPlane; ++i) {
            peak[i] = std::max(peak[i], std::isfinite(plane[i]) ? plane[i] : -1e30f);
        }
    }
    std::vector<float> sum(kPlane, 0.0f);
    std::vector<float> skin(kPlane, 0.0f);
    std::vector<float> mouth(kPlane, 0.0f);
    std::vector<float> eyes(kPlane, 0.0f);
    for (int c = 0; c < kClasses; ++c) {
        const float* plane = logits.data() + static_cast<std::size_t>(c) * kPlane;
        const Weights w = weightOf(c);
        for (std::size_t i = 0; i < kPlane; ++i) {
            const float d = (std::isfinite(plane[i]) ? plane[i] : -1e30f) - peak[i];
            if (d < -16.0f) {
                continue; // e^-16 is 1e-7 of the peak: nothing
            }
            const float e = std::exp(d);
            sum[i] += e;
            skin[i] += w.skin * e;
            mouth[i] += w.mouth * e;
            eyes[i] += w.eyes * e;
        }
    }
    out.skin.resize(kPlane);
    out.mouth.resize(kPlane);
    out.eyes.resize(kPlane);
    for (std::size_t i = 0; i < kPlane; ++i) {
        const float inv = sum[i] > 0.0f ? 1.0f / sum[i] : 0.0f;
        out.skin[i] = toByte(skin[i] * inv);
        out.mouth[i] = toByte(mouth[i] * inv);
        out.eyes[i] = toByte(eyes[i] * inv);
    }
    return out;
}

void pasteFace(FaceMapBytes& map, const FaceClasses& face, const FaceCrop& crop) {
    if (!map.valid() || face.skin.size() != kPlane || face.mouth.size() != kPlane || face.eyes.size() != kPlane || !(crop.side > 0.0)) {
        return;
    }
    const double left = crop.cx - crop.side / 2.0;
    const double top = crop.cy - crop.side / 2.0;
    const double scale = kSize / crop.side;
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int x1 = std::min(map.width, static_cast<int>(std::ceil(left + crop.side)));
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int y1 = std::min(map.height, static_cast<int>(std::ceil(top + crop.side)));
    const std::array<const std::vector<std::uint8_t>*, 3> planes{&face.skin, &face.mouth, &face.eyes};
    for (int y = y0; y < y1; ++y) {
        const double v = ((y + 0.5) - top) * scale - 0.5; // the plane's row at this map row
        if (v < -0.5 || v > kSize - 0.5) {
            continue;
        }
        for (int x = x0; x < x1; ++x) {
            const double u = ((x + 0.5) - left) * scale - 0.5;
            if (u < -0.5 || u > kSize - 0.5) {
                continue;
            }
            const double edge = std::min(std::min(u + 0.5, kSize - 0.5 - u), std::min(v + 0.5, kSize - 0.5 - v)); // texels to the nearest edge
            const double fade = std::clamp(edge / kFeather, 0.0, 1.0);
            const double window = fade * fade * (3.0 - 2.0 * fade);
            if (window <= 0.0) {
                continue;
            }
            const int ix = static_cast<int>(std::floor(u));
            const int iy = static_cast<int>(std::floor(v));
            const double fx = u - ix;
            const double fy = v - iy;
            for (std::size_t c = 0; c < 3; ++c) {
                const auto& plane = *planes[c];
                const auto sample = [&](int sx, int sy) {
                    return static_cast<double>(plane[static_cast<std::size_t>(std::clamp(sy, 0, kSize - 1)) * kSize + static_cast<std::size_t>(std::clamp(sx, 0, kSize - 1))]);
                };
                const double topRow = sample(ix, iy) * (1.0 - fx) + sample(ix + 1, iy) * fx;
                const double bottomRow = sample(ix, iy + 1) * (1.0 - fx) + sample(ix + 1, iy + 1) * fx;
                const double value = (topRow * (1.0 - fy) + bottomRow * fy) * window;
                std::uint8_t& dst = map.coverage[(static_cast<std::size_t>(y) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(x)) * 3 + c];
                dst = std::max(dst, static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0))));
            }
        }
    }
}

Picture faceMapPicture(const FaceMapBytes& map) {
    Picture out;
    if (!map.valid()) {
        return out;
    }
    out.width = map.width;
    out.height = map.height;
    out.rgb.resize(map.coverage.size());
    for (std::size_t i = 0; i < map.coverage.size(); ++i) {
        out.rgb[i] = static_cast<std::uint16_t>(map.coverage[i] * 257u);
    }
    return out;
}

FaceMapBytes faceMapFromPicture(const Picture& picture) {
    FaceMapBytes out;
    if (!picture.valid()) {
        return out;
    }
    out.width = picture.width;
    out.height = picture.height;
    out.coverage.resize(picture.rgb.size());
    for (std::size_t i = 0; i < picture.rgb.size(); ++i) {
        out.coverage[i] = static_cast<std::uint8_t>((static_cast<unsigned>(picture.rgb[i]) + 128u) / 257u);
    }
    return out;
}

} // namespace wabcut::ai::video
