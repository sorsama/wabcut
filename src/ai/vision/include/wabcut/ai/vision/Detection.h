#pragma once

// Face and person detection for auto reframe (AI-9, docs/ai.md "Auto reframe"): the networks' input and output.
// The networks run out of process (wabcut-embed, EmbedProtocol.h); this is what goes in and comes out, in pure C++,
// so it is tested on fixture tensors.
//
//   YuNet (OpenCV Zoo face_detection_yunet_2023mar, MIT): a 640 x 640 BGR 0..255 input with the picture letterboxed
//     into its top-left corner over black; per stride (8, 16, 32) a class and an objectness score, a box (grid
//     offsets and log sizes) and five landmarks, decoded like OpenCV's FaceDetectorYN.
//   YOLOX (Megvii, Apache-2.0; tiny/nano of release 0.1.1rc0): a 416 x 416 BGR 0..255 input letterboxed over grey
//     114, not normalised (the released weights were trained without the "legacy" mean/std normalisation the demo
//     code of that tag still applies: with it, nothing is found); 3549 x 85 raw predictions (grid offsets, log sizes,
//     objectness, 80 COCO class scores), decoded like the release's demo_postprocess.
// Boxes come out in the source picture's pixels.

#include <wabcut/ai/search/Embedding.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace wabcut::ai::vision {

using search::Image;

inline constexpr int kYuNetSize = 640;
inline constexpr int kYoloxSize = 416;
inline constexpr int kCocoPerson = 0;
inline constexpr int kYoloxClasses = 80;

struct Box {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    float area() const { return w > 0.0f && h > 0.0f ? w * h : 0.0f; }
    bool operator==(const Box&) const = default;
};

/// Intersection over union (0 for disjoint or empty boxes).
float iou(const Box& a, const Box& b);
/// `b` scaled by 1/width, 1/height (picture pixels to 0..1).
Box normalized(const Box& b, float width, float height);

struct Detection {
    Box box;
    float score = 0.0f;
    int classId = 0;
    std::vector<float> landmarks; ///< YuNet: 5 x (x, y) in the same pixels as `box`
};

/// How a picture sits inside a network's square input: scaled by `scale` into the top-left corner.
struct Letterbox {
    float scale = 1.0f;
    int inputWidth = 0;
    int inputHeight = 0;
    int imageWidth = 0;
    int imageHeight = 0;

    /// A box in network input pixels as picture pixels, clipped to the picture.
    Box toImage(const Box& input) const;
};

/// The largest scale that fits `imageWidth` x `imageHeight` into the input (aspect kept).
Letterbox letterboxFor(int imageWidth, int imageHeight, int inputWidth, int inputHeight);

/// The networks' input tensors (NCHW float32) and the letterbox used.
std::vector<float> yunetInput(const Image& image, Letterbox& letterbox);
std::vector<float> yoloxInput(const Image& image, Letterbox& letterbox);

/// YuNet's twelve outputs: per stride 8, 16, 32 (index 0, 1, 2).
struct YuNetOutputs {
    std::array<std::span<const float>, 3> cls;  ///< rows x cols scores
    std::array<std::span<const float>, 3> obj;
    std::array<std::span<const float>, 3> bbox; ///< 4 per cell
    std::array<std::span<const float>, 3> kps;  ///< 10 per cell
};

/// Faces, after the score threshold and non-maximum suppression, in picture pixels, best first.
std::vector<Detection> decodeYuNet(const YuNetOutputs& outputs, const Letterbox& letterbox, float scoreThreshold = 0.6f,
                                   float nmsThreshold = 0.3f);

/// Objects of class `onlyClass` (-1 = all classes), same contract. `output` is the 3549 x 85 prediction.
std::vector<Detection> decodeYolox(std::span<const float> output, const Letterbox& letterbox, int onlyClass = kCocoPerson,
                                   float scoreThreshold = 0.35f, float nmsThreshold = 0.45f);

/// Greedy non-maximum suppression per class: the best box wins over every box it overlaps by more than
/// `iouThreshold`. Best first.
std::vector<Detection> nonMaximumSuppression(std::vector<Detection> detections, float iouThreshold);

} // namespace wabcut::ai::vision
