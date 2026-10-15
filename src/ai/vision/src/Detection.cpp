#include <wabcut/ai/vision/Detection.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::vision {

float iou(const Box& a, const Box& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w);
    const float y1 = std::min(a.y + a.h, b.y + b.h);
    const float inter = std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0);
    const float uni = a.area() + b.area() - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

Box normalized(const Box& b, float width, float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return {};
    }
    return Box{b.x / width, b.y / height, b.w / width, b.h / height};
}

Box Letterbox::toImage(const Box& input) const {
    if (scale <= 0.0f) {
        return {};
    }
    const float x0 = std::clamp(input.x / scale, 0.0f, static_cast<float>(imageWidth));
    const float y0 = std::clamp(input.y / scale, 0.0f, static_cast<float>(imageHeight));
    const float x1 = std::clamp((input.x + input.w) / scale, 0.0f, static_cast<float>(imageWidth));
    const float y1 = std::clamp((input.y + input.h) / scale, 0.0f, static_cast<float>(imageHeight));
    return Box{x0, y0, x1 - x0, y1 - y0};
}

Letterbox letterboxFor(int imageWidth, int imageHeight, int inputWidth, int inputHeight) {
    Letterbox l;
    l.inputWidth = inputWidth;
    l.inputHeight = inputHeight;
    l.imageWidth = imageWidth;
    l.imageHeight = imageHeight;
    if (imageWidth > 0 && imageHeight > 0) {
        l.scale = std::min(static_cast<float>(inputWidth) / static_cast<float>(imageWidth),
                           static_cast<float>(inputHeight) / static_cast<float>(imageHeight));
    }
    return l;
}

namespace {

/// The picture scaled into the letterbox (at least 1 x 1).
Image scaledInto(const Image& image, const Letterbox& l) {
    const int w = std::clamp(static_cast<int>(std::lround(static_cast<float>(image.width) * l.scale)), 1, l.inputWidth);
    const int h = std::clamp(static_cast<int>(std::lround(static_cast<float>(image.height) * l.scale)), 1, l.inputHeight);
    return search::resizeBilinear(image, w, h);
}

} // namespace

std::vector<float> yunetInput(const Image& image, Letterbox& letterbox) {
    letterbox = letterboxFor(image.width, image.height, kYuNetSize, kYuNetSize);
    const std::size_t plane = static_cast<std::size_t>(kYuNetSize) * kYuNetSize;
    std::vector<float> input(plane * 3, 0.0f); // black padding
    if (!image.valid()) {
        return input;
    }
    const Image scaled = scaledInto(image, letterbox);
    for (int y = 0; y < scaled.height; ++y) {
        for (int x = 0; x < scaled.width; ++x) {
            const std::uint8_t* px = &scaled.rgb[(static_cast<std::size_t>(y) * scaled.width + x) * 3];
            const std::size_t at = static_cast<std::size_t>(y) * kYuNetSize + x;
            input[at] = px[2];             // B
            input[plane + at] = px[1];     // G
            input[2 * plane + at] = px[0]; // R
        }
    }
    return input;
}

std::vector<float> yoloxInput(const Image& image, Letterbox& letterbox) {
    letterbox = letterboxFor(image.width, image.height, kYoloxSize, kYoloxSize);
    const std::size_t plane = static_cast<std::size_t>(kYoloxSize) * kYoloxSize;
    std::vector<float> input(plane * 3, 114.0f); // grey padding
    if (!image.valid()) {
        return input;
    }
    const Image scaled = scaledInto(image, letterbox);
    for (int y = 0; y < scaled.height; ++y) {
        for (int x = 0; x < scaled.width; ++x) {
            const std::uint8_t* px = &scaled.rgb[(static_cast<std::size_t>(y) * scaled.width + x) * 3];
            const std::size_t at = static_cast<std::size_t>(y) * kYoloxSize + x;
            input[at] = px[2];             // B
            input[plane + at] = px[1];     // G
            input[2 * plane + at] = px[0]; // R
        }
    }
    return input;
}

std::vector<Detection> nonMaximumSuppression(std::vector<Detection> detections, float iouThreshold) {
    std::ranges::stable_sort(detections, [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> kept;
    for (Detection& d : detections) {
        const bool suppressed = std::ranges::any_of(kept, [&](const Detection& k) {
            return k.classId == d.classId && iou(k.box, d.box) > iouThreshold;
        });
        if (!suppressed) {
            kept.push_back(std::move(d));
        }
    }
    return kept;
}

std::vector<Detection> decodeYuNet(const YuNetOutputs& outputs, const Letterbox& letterbox, float scoreThreshold,
                                   float nmsThreshold) {
    static constexpr int kStrides[3] = {8, 16, 32};
    std::vector<Detection> found;
    for (int s = 0; s < 3; ++s) {
        const int stride = kStrides[s];
        const int cols = letterbox.inputWidth / stride;
        const int rows = letterbox.inputHeight / stride;
        const std::size_t cells = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
        const auto& cls = outputs.cls[static_cast<std::size_t>(s)];
        const auto& obj = outputs.obj[static_cast<std::size_t>(s)];
        const auto& bbox = outputs.bbox[static_cast<std::size_t>(s)];
        const auto& kps = outputs.kps[static_cast<std::size_t>(s)];
        if (cls.size() < cells || obj.size() < cells || bbox.size() < cells * 4 || kps.size() < cells * 10) {
            continue; // a malformed output: nothing from this stride
        }
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const std::size_t idx = static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c);
                const float score = std::sqrt(std::clamp(cls[idx], 0.0f, 1.0f) * std::clamp(obj[idx], 0.0f, 1.0f));
                if (!(score >= scoreThreshold)) {
                    continue;
                }
                const float cx = (static_cast<float>(c) + bbox[idx * 4 + 0]) * static_cast<float>(stride);
                const float cy = (static_cast<float>(r) + bbox[idx * 4 + 1]) * static_cast<float>(stride);
                const float w = std::exp(std::min(bbox[idx * 4 + 2], 10.0f)) * static_cast<float>(stride);
                const float h = std::exp(std::min(bbox[idx * 4 + 3], 10.0f)) * static_cast<float>(stride);
                Detection d;
                d.box = letterbox.toImage(Box{cx - w * 0.5f, cy - h * 0.5f, w, h});
                d.score = score;
                for (int n = 0; n < 5; ++n) {
                    d.landmarks.push_back((kps[idx * 10 + 2 * n] + static_cast<float>(c)) * static_cast<float>(stride) / letterbox.scale);
                    d.landmarks.push_back((kps[idx * 10 + 2 * n + 1] + static_cast<float>(r)) * static_cast<float>(stride) / letterbox.scale);
                }
                if (d.box.area() > 0.0f && std::isfinite(d.box.x) && std::isfinite(d.box.w)) {
                    found.push_back(std::move(d));
                }
            }
        }
    }
    return nonMaximumSuppression(std::move(found), nmsThreshold);
}

std::vector<Detection> decodeYolox(std::span<const float> output, const Letterbox& letterbox, int onlyClass,
                                   float scoreThreshold, float nmsThreshold) {
    static constexpr int kStrides[3] = {8, 16, 32};
    static constexpr std::size_t kRow = 5 + kYoloxClasses;
    std::vector<Detection> found;
    std::size_t row = 0;
    for (const int stride : kStrides) {
        const int wsize = letterbox.inputWidth / stride;
        const int hsize = letterbox.inputHeight / stride;
        for (int gy = 0; gy < hsize; ++gy) {
            for (int gx = 0; gx < wsize; ++gx, ++row) {
                if ((row + 1) * kRow > output.size()) {
                    return nonMaximumSuppression(std::move(found), nmsThreshold);
                }
                const float* p = output.data() + row * kRow;
                int bestClass = onlyClass;
                float classScore = 0.0f;
                if (onlyClass >= 0 && onlyClass < kYoloxClasses) {
                    classScore = p[5 + onlyClass];
                } else {
                    for (int k = 0; k < kYoloxClasses; ++k) {
                        if (p[5 + k] > classScore) {
                            classScore = p[5 + k];
                            bestClass = k;
                        }
                    }
                }
                const float score = p[4] * classScore;
                if (!(score >= scoreThreshold)) {
                    continue;
                }
                const float cx = (p[0] + static_cast<float>(gx)) * static_cast<float>(stride);
                const float cy = (p[1] + static_cast<float>(gy)) * static_cast<float>(stride);
                const float w = std::exp(std::min(p[2], 10.0f)) * static_cast<float>(stride);
                const float h = std::exp(std::min(p[3], 10.0f)) * static_cast<float>(stride);
                Detection d;
                d.box = letterbox.toImage(Box{cx - w * 0.5f, cy - h * 0.5f, w, h});
                d.score = score;
                d.classId = bestClass;
                if (d.box.area() > 0.0f && std::isfinite(d.box.x) && std::isfinite(d.box.w)) {
                    found.push_back(std::move(d));
                }
            }
        }
    }
    return nonMaximumSuppression(std::move(found), nmsThreshold);
}

} // namespace wabcut::ai::vision
