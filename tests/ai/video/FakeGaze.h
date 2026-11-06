#pragma once

// A synthetic face for the AI Eye Contact tests (0.7): the landmark subset the table carries (ai/video/Gaze.h), for a face whose eyes are
// level (or rolled by a given angle) and whose irises look down by a given share of the eye's width. Used by the fake backend and by the
// scripted helper (tests/ai/search/fake_embed.cpp), so a "model run" gives measurements the tests know exactly.

#include <wabcut/ai/video/Gaze.h>

#include <cmath>
#include <numbers>
#include <span>

namespace wabcut::ai::video::test {

struct FakeFaceSpec {
    double down = 0.12;        ///< the iris below the middle of the opening, in eye widths
    double right = 0.0;        ///< ... and to the right of the middle between the corners
    double rollDegrees = 0.0;  ///< the whole face turned in the picture's plane
    double upper = 0.012;      ///< the lids' heights above / below the corner line, fractions of the picture's width
    double lower = 0.007;
    double nose = 0.0;         ///< the nose's place between the cheeks: 0 = in the middle, 1 = on a cheek
};

/// The landmarks of one face in a `width` x `height` picture, in table order. The eyes are 0.045 of the width half wide, the iris radius is
/// 0.018 of the width, the face's centre is at (0.5, 0.45) of the picture.
inline GazeLandmarkFace fakeLandmarkFace(int width, int height, const FakeFaceSpec& spec = {}) {
    const double w = width;
    const double h = height;
    const double fx = 0.5 * w;
    const double fy = 0.45 * h;
    const double a = 0.045 * w;
    const double up = spec.upper * w;
    const double lo = spec.lower * w;
    const double r = 0.018 * w;
    const double roll = spec.rollDegrees * std::numbers::pi / 180.0;
    const double c = std::cos(roll);
    const double s = std::sin(roll);
    GazeLandmarkFace face;
    face.points.resize(kGazeLandmarkSlots);
    const auto put = [&](int slot, double x, double y) {
        const double dx = x - fx;
        const double dy = y - fy;
        face.points[static_cast<std::size_t>(slot)] = {fx + c * dx - s * dy, fy + s * dx + c * dy, 0.0};
    };
    const auto eye = [&](int first, double cx, double cy, bool rightEye) {
        // Corners (the picture's left one first) and the two lids, seven points each between the corners.
        const int leftCorner = first;
        const int rightCorner = first + 8;
        put(leftCorner, cx - a, cy);
        put(rightCorner, cx + a, cy);
        for (int k = 0; k < 7; ++k) {
            const double u = -0.75 + 0.25 * k;
            const double k2 = 1.0 - u * u;
            put(first + 1 + k, cx + u * a, cy + lo * k2);                  // the lower lid, left to right
            const int upperSlot = rightEye ? first + 15 - k : first + 9 + (6 - k); // the upper lid, running back from the right corner
            put(upperSlot, cx + u * a, cy - up * k2);
        }
    };
    eye(0, fx - 0.10 * w, fy - 0.05 * h, true);
    eye(16, fx + 0.10 * w, fy - 0.05 * h, false);
    const double middle = 0.5 * (lo - up);
    for (int e = 0; e < 2; ++e) {
        const double cx = fx + (e == 0 ? -0.10 : 0.10) * w;
        const double cy = fy - 0.05 * h;
        const double ix = cx + spec.right * 2.0 * a;
        const double iy = cy + middle + spec.down * 2.0 * a;
        const int base = 32 + 5 * e;
        put(base, ix, iy);
        put(base + 1, ix + r, iy);
        put(base + 2, ix, iy - r);
        put(base + 3, ix - r, iy);
        put(base + 4, ix, iy + r);
    }
    put(42, fx + spec.nose * 0.22 * w, fy + 0.1 * h);
    put(43, fx - 0.22 * w, fy + 0.05 * h);
    put(44, fx + 0.22 * w, fy + 0.05 * h);
    face.box = {fx - 0.22 * w, fy - 0.30 * h, 0.44 * w, 0.60 * h};
    face.presence = 0.99;
    return face;
}

/// The table a model run answers in, with `faces` copies of the face (0: a picture with no face in it).
inline Picture fakeGazeTable(int width, int height, const FakeFaceSpec& spec = {}, int faces = 1) {
    std::vector<GazeLandmarkFace> list;
    for (int i = 0; i < faces; ++i) {
        list.push_back(fakeLandmarkFace(width, height, spec));
    }
    return gazeTablePicture(list, width, height);
}

} // namespace wabcut::ai::video::test
