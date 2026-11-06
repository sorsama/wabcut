#pragma once

// Warp Stabilizer's "Gyro (camera metadata)" method (0.7; docs/motion.md "Stabilize with the camera's gyro"): the camera's own
// gyroscope instead of the picture's motion.
//
// The rotation the gyro recorded (wabcut/gyro/Gyro.h) is turned into the same thing the image analysis ends in: a
// compensating transform per frame (Compensation, a corner pin) that the Warp Stabilizer keys and the renderer applies - so
// Smoothness (also keyframed), Result, Framing, Maximum Scale, Additional Scale, the action-safe margin and Crop Less /
// Smooth More all mean what they mean for the picture-based methods, and preview and export draw the same keys.
//
//   1. Frame i is read at its time + the sync offset + half the sensor's read-out (the middle line's exposure).
//   2. The camera's orientation there is the integral of the angular velocity (wabcut/gyro/Rotation.h).
//   3. The path is smoothed in rotation space, not on the picture: for every frame the Gaussian-weighted local linear fit
//      (the one the picture-based path uses) of the tangent vectors log(q_i^-1 q_j) of the frames around it gives how far
//      the smoothed camera is from the real one, whatever the total turning (a corner pin of the picture would break down
//      past ~70 degrees). Smoothness 0 leaves a frame exactly as shot; No Motion locks the camera to the first frame.
//   4. The correction is the rotation as a homography on the picture, K E^T K^-1 with the lens's focal length in pixels K.
//   5. A rolling-shutter camera (the file gives its read-out time) also has its lines' skew taken out first, the way
//      Rolling Shutter Repair does it from the angular velocity of the frame.
//   6. Framing (auto scale, crop edges) is the picture-based path's (frameCompensations).
//
// Three things the file may not tell, and how they are found: the sync offset (the clocks of the gyro and the picture),
// the lens's field of view, and - for a camera whose axis table was not checked against footage - the axis order. Auto-sync
// measures them against the picture's own motion on a few stretches of the clip (fitGyroToPictures); with it off, or when
// the clip has too little motion, the file's values and the camera's usual field of view stand in.

#include <wabcut/ai/stabilize/Stabilize.h>
#include <wabcut/gyro/Gyro.h>

#include <span>
#include <string>
#include <vector>

namespace wabcut::ai::stabilize {

/// The 24 ways the gyro's axes can be turned (signed permutations with determinant +1) into the picture's; index 0 is "as the
/// camera recorded them".
inline constexpr int kGyroAxisCount = 24;
gyro::Vec3 applyGyroAxes(int index, const gyro::Vec3& v);
/// "as recorded" for 0, else where the camera's x, y, z go ("x -> -y, y -> x, z -> z").
std::string describeGyroAxes(int index);

/// What Effect Controls asks of the method (Gyro Offset, Gyro Field of View, Gyro Read-out, Gyro Sync).
struct GyroOptions {
    double offsetSeconds = 0.0;        ///< added to the sync Auto-sync finds: the gyro is read this much after the frame
    double horizontalFovDegrees = 0.0; ///< the lens's horizontal field of view; 0 = automatic
    double readoutSeconds = 0.0;       ///< the sensor's read-out time; 0 = the file's, if it has one
    bool autoSync = true;
};

/// A stretch of consecutive frames whose pictures' motion was measured (a Similarity analysis): what the gyro is compared with.
struct ImageMotionWindow {
    std::vector<Ticks> times;  ///< the frames' media times
    MotionAnalysis motion;     ///< analyzeMotion() of those frames: frames[k].motion takes frame k-1 to frame k
};

/// The result of comparing the gyro with the pictures.
struct GyroFit {
    bool measured = false;      ///< there was enough motion in the pictures to compare
    bool usable = false;        ///< ... and the gyro matched it well enough to take its numbers
    double offsetSeconds = 0.0; ///< the gyro is read this much after the frame for the best match
    double focalPixels = 0.0;   ///< the lens's focal length in source pixels
    double horizontalFovDegrees = 0.0;
    double match = 0.0;         ///< 0..1: how well the gyro's rotation explains the pictures' motion (cosine similarity)
    int axes = 0;               ///< the axis turn that fits best (0: as recorded)
    int pairs = 0;              ///< frame pairs compared
    std::string note;           ///< what happened, for the user
};

/// Compares the gyro with the pictures' motion: searches the sync offset (+-0.5 s) and, for every axis turn, how well the
/// rotation integrated over each frame interval explains the measured image motion; the focal length is the least squares
/// scale between them. `sourceWidth` is the clip's picture width (the windows were measured at their own analysis width).
GyroFit fitGyroToPictures(const gyro::GyroTrack& track, std::span<const ImageMotionWindow> windows, int sourceWidth);

/// The numbers a run works with: the user's options, the fit and the file's facts put together.
struct GyroSetup {
    double offsetSeconds = 0.0;
    double focalPixels = 1000.0;
    double horizontalFovDegrees = 90.0;
    std::string fovFrom;        ///< "set" (the user's), "picture" (Auto-sync), "file" (lens data), "camera" (the camera's usual)
    double readoutSeconds = 0.0; ///< 0: global shutter or unknown
    int axes = 0;
    bool synced = false;        ///< Auto-sync's numbers are in use
    GyroFit fit;
};
GyroSetup resolveGyroSetup(const gyro::GyroTrack& track, const GyroOptions& options, const GyroFit& fit, int sourceWidth);

/// The horizontal field of view a camera's pictures usually have when nothing better is known.
double defaultHorizontalFov(gyro::Camera camera);

/// The compensating transform per frame of `frameTimes` (strictly increasing, media time) from the gyro, in the
/// Perspective form (corner offsets) the Warp Stabilizer keys. Frames the gyro does not cover are left as shot.
/// `options` are the picture-based path's: smoothness (or smoothnessAt per frame), noMotion, framing, scales, margins.
Compensation stabilizeFromGyro(const gyro::GyroTrack& track, std::span<const Ticks> frameTimes, const GyroSetup& setup,
                               const SmoothOptions& options, int sourceWidth, int sourceHeight);

/// "key=value;..." for the effect's data entry "gyro": the camera, what was found in the file and the numbers in use. Read
/// back by the Effect Controls header (the bridge); values never contain ';' or '='.
std::string gyroSummaryText(const gyro::GyroTrack& track, const GyroSetup& setup, bool autoSyncAsked);

} // namespace wabcut::ai::stabilize
