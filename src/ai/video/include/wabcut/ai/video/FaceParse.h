#pragma once

// AI Face Retouch's face map (0.7, docs/ai.md "AI Face Retouch"): the parts of the work around the face-parsing network that are plain
// arithmetic, in pure C++ so the editor's tests cover them on fixture tensors. YuNet finds the faces of a picture; for each, a square
// crop round it goes through BiSeNet face parsing (ResNet-18, 512 x 512, ImageNet mean and deviation) which labels every pixel with one
// of 19 classes; the labels become three soft coverage planes that are pasted back into a map of the whole picture.
//
// Classes (CelebAMask-HQ): 0 background, 1 skin, 2 l_brow, 3 r_brow, 4 l_eye, 5 r_eye, 6 eye_g (glasses), 7 l_ear, 8 r_ear, 9 ear_r
// (earring), 10 nose, 11 mouth (the inside: teeth, tongue, gums), 12 u_lip, 13 l_lip, 14 neck, 15 neck_l (necklace), 16 cloth, 17 hair,
// 18 hat. The planes, from the classes' softmax probabilities p:
//   skin  = p1 + p10 + p7 + p8 + 0.8 p14   (face, nose, ears; the neck a little less so the jaw blends)
//   mouth = p11
//   eyes  = p4 + p5
// Brows, glasses, lips, hair, hat, cloth and background are in none of them: they are left alone.

#include <wabcut/ai/video/Picture.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace wabcut::ai::video {

/// A face box in the pixels of the picture it was found in.
struct FaceBox {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

/// A square window on the picture (centre and side in its pixels; it may reach outside it).
struct FaceCrop {
    double cx = 0.0;
    double cy = 0.0;
    double side = 0.0;
};

/// The window the parser looks at for a face: 1.5 x the larger side of the box (the face fills the network's picture as it did in
/// the images it was trained on), centred on the box and moved up by 3 % of the window (the forehead and hair above the box),
/// at least 96 pixels.
FaceCrop faceCropFor(const FaceBox& box);

/// The parser's input, 3 x 512 x 512 float32 planar RGB: the crop sampled bilinearly from `picture` (edge pixels repeated outside it),
/// 0..1, minus the ImageNet mean and over its deviation. Empty for an invalid picture or crop.
std::vector<float> faceParseInput(const Picture& picture, const FaceCrop& crop);

/// The three coverage planes of one face, each 512 x 512 bytes (0..255), rows top-down, in the crop's own pixels.
struct FaceClasses {
    std::vector<std::uint8_t> skin;
    std::vector<std::uint8_t> mouth;
    std::vector<std::uint8_t> eyes;
};

/// The planes from the network's output `logits` (19 x 512 x 512 floats, class-major): the softmax over the classes, summed as above.
/// An output of the wrong size gives empty planes.
FaceClasses faceClassesOf(std::span<const float> logits);

/// A map of a whole picture: skin, mouth and eyes coverage, interleaved (width x height x 3 bytes), rows top-down.
struct FaceMapBytes {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> coverage;

    FaceMapBytes() = default;
    FaceMapBytes(int w, int h) : width(w), height(h), coverage(static_cast<std::size_t>(std::max(w, 0)) * static_cast<std::size_t>(std::max(h, 0)) * 3, 0) {}
    bool valid() const { return width > 0 && height > 0 && coverage.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3; }
};

/// Pastes a face into the map: every map pixel inside the crop takes the plane values at its place (bilinear), faded to nothing over
/// the outer 24 pixels of the crop so two faces' windows never leave a seam, and keeps the larger of that and what the map already
/// held there.
void pasteFace(FaceMapBytes& map, const FaceClasses& face, const FaceCrop& crop);

/// A picture of the map (R skin, G mouth, B eyes, 16 bits: byte x 257), the form a helper request answers in.
Picture faceMapPicture(const FaceMapBytes& map);
/// The map back from such a picture (each value / 257, rounded); empty (invalid) when the picture is.
FaceMapBytes faceMapFromPicture(const Picture& picture);

} // namespace wabcut::ai::video
