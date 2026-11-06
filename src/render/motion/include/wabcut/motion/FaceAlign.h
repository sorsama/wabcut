#pragma once

// Face-aware motion for Morph Cut (0.7, docs/motion.md "Morph Cut and faces"): the plain optical flow (Flow.h) between the two sides
// of a jump cut is estimated on the whole picture, and a talking head is the hardest part of it - cheeks and forehead have no
// texture to follow, the mouth opens and closes, and a head that moved by more than a patch loses its way - so the face ghosts.
// Where a face detector found the same face on both sides, its five landmarks (eyes, nose tip, mouth corners) give the head's
// motion directly: the similarity transform (shift, turn and scale) that carries one side's landmarks onto the other's. Inside the
// face's window the flow is then estimated again as a *residual* on the pictures aligned by that transform (so even a large move
// is within the estimator's reach), and each pixel takes, in proportion to the window's feathered weight, the flow - plain or
// face-aligned - whose pictures agree better there (the photometric error over a 5 x 5 neighbourhood). Outside the windows, and
// wherever the plain flow was already the better one, nothing changes; with no face on both sides the plain flow is returned
// as it is (the old Morph Cut).
//
// Pure C++, deterministic (the same inputs give the same flow on any number of threads), no detector in here: the faces come in
// as FaceFeatures in the pixels of the planes (the playback layer asks its face finder, wabcut/playback/video/MotionAnalysis.h).

#include <wabcut/motion/Flow.h>

#include <array>
#include <span>
#include <vector>

namespace wabcut::motion {

struct FacePoint {
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const FacePoint&) const = default;
};

/// One detected face in the pixels of the plane it was found in: its box and the detector's five landmarks (YuNet's order: the
/// eye on the picture's left, the eye on its right, the nose tip, the mouth's left and right corner).
struct FaceFeatures {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    std::array<FacePoint, 5> points{};
    float score = 1.0f;

    float centreX() const { return x + 0.5f * w; }
    float centreY() const { return y + 0.5f * h; }
    bool valid() const { return w > 1.0f && h > 1.0f && x == x && y == y; }
    /// The face in the pixels of a plane `sx` / `sy` times as large.
    FaceFeatures scaled(float sx, float sy) const;
    bool operator==(const FaceFeatures&) const = default;
};

/// x' = a x - b y + tx, y' = b x + a y + ty: a scale (|a + ib|), a turn (arg(a + ib)) and a shift.
struct Similarity {
    double a = 1.0;
    double b = 0.0;
    double tx = 0.0;
    double ty = 0.0;

    FacePoint map(FacePoint p) const;
    Similarity inverted() const;
    double scale() const;
    double turnDegrees() const;
};

/// The least-squares similarity carrying `from` onto `to` (Umeyama: no reflection). The identity for fewer than two pairs of
/// points or points that coincide.
Similarity similarityOf(std::span<const FacePoint> from, std::span<const FacePoint> to);

struct FaceMatch {
    int a = 0; ///< index into the first side's faces
    int b = 0; ///< index into the second side's
};
/// The faces that are the same person on both sides: greedy by the distance of the centres over the faces' size plus the
/// difference of their sizes (a head that moved by less than about one head and that is within a factor of two in size), one to one.
std::vector<FaceMatch> matchFaces(std::span<const FaceFeatures> a, std::span<const FaceFeatures> b);

struct FaceAlignOptions {
    double windowScale = 1.7; ///< the window is the face box this many times as large (forehead, hair, ears, neck)
    double feather = 0.45;    ///< the window's weight falls from 1 to 0 over this share of its radius
    int threads = 0;
};

struct FaceAlignStats {
    int faces = 0;             ///< faces whose window was re-estimated
    double errorBefore = 0.0;  ///< mean |A(x) - B(x + F(x))| inside the windows with the plain flow
    double errorAfter = 0.0;   ///< ... with the result
};

/// `plain` (estimateFlowPair of `a` and `b`) with the faces' windows improved as above. `a` and `b` are the planes the flow was
/// estimated on (any size: they are resampled to the pair's) and the faces are in the pixels of those planes (the same size for
/// both). Returns `plain` unchanged when no face matches.
FlowPair alignFaces(const Plane& a, const Plane& b, const FlowPair& plain, std::span<const FaceFeatures> facesA,
                    std::span<const FaceFeatures> facesB, const FaceAlignOptions& options = {}, FaceAlignStats* stats = nullptr);

} // namespace wabcut::motion
