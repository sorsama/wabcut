#pragma once

// Fixtures of the subject-analysis tests (AI-9): pictures are flat grey with plain colour blocks (a red block is a
// face, a green one a person, for the scripted detectors), videos are functions of time, and a scripted detector finds
// the blocks. No model, no decoder, no download.

#include <wabcut/ai/vision/Subjects.h>

#include <algorithm>
#include <cmath>
#include <expected>
#include <functional>
#include <optional>
#include <vector>

namespace wabcut::ai::vision::test {

/// A block of colour at normalised coordinates.
struct Block {
    Box box; ///< 0..1
    std::uint8_t r = 255;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

inline Block face(float x, float y, float w, float h) { return Block{Box{x, y, w, h}, 255, 0, 0}; }
inline Block person(float x, float y, float w, float h) { return Block{Box{x, y, w, h}, 0, 255, 0}; }

inline Image canvas(int width, int height, const std::vector<Block>& blocks, std::uint8_t grey = 96) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, grey);
    for (const Block& block : blocks) {
        const int x0 = std::clamp(static_cast<int>(std::lround(block.box.x * static_cast<float>(width))), 0, width);
        const int x1 = std::clamp(static_cast<int>(std::lround((block.box.x + block.box.w) * static_cast<float>(width))), 0, width);
        const int y0 = std::clamp(static_cast<int>(std::lround(block.box.y * static_cast<float>(height))), 0, height);
        const int y1 = std::clamp(static_cast<int>(std::lround((block.box.y + block.box.h) * static_cast<float>(height))), 0, height);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3;
                image.rgb[at] = block.r;
                image.rgb[at + 1] = block.g;
                image.rgb[at + 2] = block.b;
            }
        }
    }
    return image;
}

/// Bounding box (pixels) of one plain colour, as the scripted helper finds it.
inline std::optional<Box> colourBox(const Image& image, int r, int g, int b) {
    int x0 = image.width;
    int y0 = image.height;
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 3;
            if (std::abs(image.rgb[at] - r) < 48 && std::abs(image.rgb[at + 1] - g) < 48 && std::abs(image.rgb[at + 2] - b) < 48) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0) {
        return std::nullopt;
    }
    return Box{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0 + 1), static_cast<float>(y1 - y0 + 1)};
}

/// Finds the red block (a face) and the green block (a person) of each picture.
class BlockDetector : public SubjectDetector {
public:
    std::expected<std::vector<FrameDetections>, Error> detect(std::span<const Image> frames, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        ++calls;
        batchSizes.push_back(frames.size());
        std::vector<FrameDetections> out;
        for (const Image& frame : frames) {
            FrameDetections found;
            if (const auto box = colourBox(frame, 255, 0, 0)) {
                found.faces.push_back(Detection{*box, 0.9f, 0, {}});
            }
            if (const auto box = colourBox(frame, 0, 255, 0)) {
                found.people.push_back(Detection{*box, 0.8f, kCocoPerson, {}});
            }
            out.push_back(std::move(found));
        }
        return out;
    }
    std::string modelVersion() const override { return "blocks/1"; }

    int calls = 0;
    std::vector<std::size_t> batchSizes;
};

/// A video made by a function of time; records which times were asked for.
class FunctionVideo : public TimedFrameSource {
public:
    FunctionVideo(Ticks length, std::function<Image(Ticks)> make) : m_length(length), m_make(std::move(make)) {}
    std::expected<Image, Error> frameAt(Ticks time) override {
        if (time >= m_length) {
            return std::unexpected(Error(ErrorCode::NotFound, "past the end"));
        }
        asked.push_back(time);
        return m_make(time);
    }
    std::vector<Ticks> asked;

private:
    Ticks m_length;
    std::function<Image(Ticks)> m_make;
};

inline Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

inline analysis::ShotAnalysis shotsOf(Ticks length, std::vector<double> cutSeconds = {}) {
    analysis::ShotAnalysis shots;
    shots.start = Ticks{0};
    shots.end = length;
    for (const double c : cutSeconds) {
        analysis::ShotCut cut;
        cut.time = seconds(c);
        cut.confidence = 1.0f;
        shots.cuts.push_back(cut);
    }
    return shots;
}

} // namespace wabcut::ai::vision::test
