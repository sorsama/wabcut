#pragma once

// The names wabcut-embed knows the 0.7 side-product models by (ModelDescription::key in VideoModels.cpp). Header-only so the
// helper, which does not link the model store, shares them with the library.

#include <string_view>

namespace wabcut::ai::video {

/// RIFE 4.25 lite (Practical-RIFE, MIT): two pictures and a timestep in, the frame between them out.
inline constexpr std::string_view kKeyInterpolate = "rife-v425-lite";
/// Depth Anything V2 Small (Apache-2.0): one picture in, a relative inverse depth map out.
inline constexpr std::string_view kKeyDepth = "depth-anything-v2-small";

/// YuNet (OpenCV Zoo, MIT): finds the faces of a picture. BiSeNet face parsing (ResNet-18, MIT): labels the pixels of a 512 x 512
/// face crop (skin, nose, eyes, brows, ears, mouth, lips, hair, neck, cloth, ...). AI Face Retouch runs both in one request.
inline constexpr std::string_view kKeyFaceDetect = "yunet-face-detect";
inline constexpr std::string_view kKeyFaceParse = "bisenet-face-parse";
/// MediaPipe's face landmark network (Apache-2.0): 478 landmarks, the eyelids and the irises among them, from a 256 x 256 face crop. AI Eye
/// Contact runs it, with the detector, in one request (ai/video/Gaze.h).
inline constexpr std::string_view kKeyFaceLandmarks = "mediapipe-face-landmarks";

/// RIFE works on pictures whose sides are multiples of this (the network downsamples by 32 twice in its coarsest scale).
inline constexpr int kInterpolateMultiple = 64;
/// The longest side RIFE runs at: larger pictures are interpolated at this size and the result is scaled back up.
inline constexpr int kInterpolateMaxSide = 1920;
/// Depth Anything's patch size: its input sides are multiples of this.
inline constexpr int kDepthMultiple = 14;
/// The shorter side of the picture the depth model looks at (the model's native 518).
inline constexpr int kDepthShortSide = 518;
/// The longest side of the picture the face models look at, and of the face map they make (larger frames are looked at reduced).
inline constexpr int kFaceMaxSide = 1280;
/// The face parser's input (a square crop around one face) and the faces looked at per picture.
inline constexpr int kFaceParseSize = 512;
inline constexpr int kFaceMaxFaces = 8;

} // namespace wabcut::ai::video
