#pragma once

// AI Eye Contact's measurements (0.7, docs/ai.md "AI Eye Contact"): the parts of the work around the landmark network that are plain
// arithmetic, in pure C++ so the editor's tests cover them on fixture tensors. YuNet finds the faces of a picture (as for AI Face Retouch);
// for each, a square window rolled so the eyes are level goes through MediaPipe's face-mesh landmark network (Apache-2.0: 478 landmarks, the
// face mesh and ten iris points; input 1 x 256 x 256 x 3 float32 RGB 0..1, output 1434 = 478 x (x, y, z) in input pixels and a face-presence
// logit). A fixed subset of the landmarks (the eyelid contours, the irises, the nose tip and the cheeks) goes back to the editor as a small
// table, and the editor turns it into the GazeFace measurements the effect works from (model/EyeContact.h) and stores them per frame.
//
// Mesh indices (MediaPipe's canonical face mesh, checked on a portrait: the eyelid contours and iris points lie on the eyes):
//   the subject's right eye (the picture's left one): corners 33 (outer) and 133 (inner); upper lid 246 161 160 159 158 157 173, lower lid
//   7 163 144 145 153 154 155; iris centre 468 and limbus 469 470 471 472;
//   the subject's left eye (the picture's right one): corners 362 (inner) and 263 (outer); upper lid 466 388 387 386 385 384 398, lower lid
//   249 390 373 374 380 381 382; iris centre 473 and limbus 474 475 476 477;
//   1 the nose tip, 234 and 454 the cheeks.

#include <wabcut/ai/video/FaceParse.h>
#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/EyeContact.h>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace wabcut::ai::video {

/// The landmark network's input side, and how many landmarks it makes.
inline constexpr int kGazeCropSize = 256;
inline constexpr int kGazeLandmarkCount = 478;

/// A square window on the picture, rolled by `angle` (radians: the line from the picture's left eye to its right one), `side` pixels wide.
struct GazeCrop {
    double cx = 0.0;
    double cy = 0.0;
    double side = 0.0;
    double angle = 0.0;
};

/// The window for a face: 1.5 x the larger side of the box, centred on it, rolled by the line between YuNet's two eye landmarks
/// (`leftEye` is the one on the picture's left).
GazeCrop gazeCropFor(const FaceBox& box, std::array<double, 2> leftEye, std::array<double, 2> rightEye);

/// The network's input: the crop sampled bilinearly from `picture` (edge pixels repeated outside it) as 256 x 256 x 3 floats, NHWC, 0..1.
/// Crop pixel (u, v) lies at `centre + R(angle) ((u - 128) k, (v - 128) k)`, k = side / 256. Empty for an invalid picture or crop.
std::vector<float> gazeInput(const Picture& picture, const GazeCrop& crop);

struct LandmarkPoint {
    double x = 0.0; ///< picture pixels
    double y = 0.0;
    double z = 0.0; ///< the network's depth, in crop pixels (kept for the face's turn)
};
/// The 478 landmarks of the network's output (1434 floats) in the picture's pixels. Empty for an output of the wrong size.
std::vector<LandmarkPoint> gazeLandmarksOf(std::span<const float> output, const GazeCrop& crop);

/// The landmarks that go back to the editor, in table order: right eye contour (16), left eye contour (16), the ten iris points, the nose
/// tip and the two cheeks. kGazeSlotCount is the width of a table row (45 landmarks, then the face's box and scores).
inline constexpr int kGazeLandmarkSlots = 45;
inline constexpr int kGazeSlotCount = 48;
std::span<const int> gazeLandmarkIndices();

/// One face of the table: its landmark subset (picture pixels), the detector's box and the network's face presence (0..1).
struct GazeLandmarkFace {
    std::vector<LandmarkPoint> points; ///< kGazeLandmarkSlots, in table order
    FaceBox box;                        ///< picture pixels
    double presence = 0.0;
};

/// The table a helper request answers in: a picture kGazeSlotCount wide and one row per face (at least one row; the first slot of the last
/// pixel of a row says whether it holds a face), 16 bits per channel. Positions are stored as fractions of the working picture
/// ((v + 0.5) / 2 of its width and height, so -0.5 .. 1.5 fits) and the working size as two whole numbers.
Picture gazeTablePicture(std::span<const GazeLandmarkFace> faces, int workWidth, int workHeight);
/// The faces back from such a table, and the working size it was made for. Errors: DecoderError (not a valid table).
struct GazeTable {
    int workWidth = 0;
    int workHeight = 0;
    std::vector<GazeLandmarkFace> faces;
};
std::expected<GazeTable, Error> gazeTableFromPicture(const Picture& table);

/// A face's measurements from its landmarks (model/EyeContact.h for the geometry): each eye's corners, lid heights and iris, the face's turn
/// from the nose tip between the cheeks, its size and score. `workWidth` / `workHeight` normalise the lengths.
GazeFace gazeFaceOf(const GazeLandmarkFace& face, int workWidth, int workHeight);

/// The stored form of a frame's measurements: one 32-bit float plane (face count, then kGazeFaceFloats per face, at most kGazeMaxFaces of
/// them, largest first) with the frame's timing. Errors: DecoderError (not a valid table).
inline constexpr int kGazeFaceFloats = 24;
std::expected<StoredFrame, Error> gazeStoredFrame(const Picture& table, Ticks pts, Ticks duration);
/// The faces of a stored frame; empty for a frame that is not one.
std::vector<GazeFace> gazeFacesOfStored(const StoredFrame& frame);

} // namespace wabcut::ai::video
