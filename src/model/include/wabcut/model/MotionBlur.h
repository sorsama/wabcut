#pragma once

// Motion blur (0.7, docs/effects.md "Motion Blur"): a clip's Motion (position, scale, rotation - and so everything the
// Animations, Zoom Punch, Shake and tracked moves key there) smeared over the time the camera shutter is open, like
// Premiere's After Effects cousin's Shutter Angle and Shutter Phase.
//
// The effect "wabcut.blur.motion" (Blur & Sharpen) is only a switch and three numbers on the clip: it does nothing to
// the picture itself. The evaluator (playback/video/Evaluate.cpp) reads it, evaluates the clip's Motion at several instants
// of the shutter interval (curve time, so a speed ramp that makes the keys race also makes the blur longer), and the
// compositor draws the layer once per instant and averages the draws (Compositor.cpp, "blur samples"); the CPU reference
// of that average is render/reference/MotionBlur.h. This file is the one place that says WHICH instants, so the evaluator,
// the reference and the tests agree.
//
//   shutter interval   [phase, phase + angle] degrees of one frame (360 degrees = the whole frame time), measured from the
//                      frame's instant: the defaults (180, -90) open the shutter a quarter frame before the frame instant
//                      and close it a quarter after, centred on it
//   sample i of n      offset = (phase + angle * (i + 0.5) / n) / 360 frames, equal weights 1 / n
//   angle 0            no blur: the layer is drawn once at the frame instant, byte for byte as without the effect

#include <wabcut/model/Effects.h>
#include <wabcut/model/Time.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace wabcut {

inline constexpr std::string_view kMotionBlurEffect = "wabcut.blur.motion";

/// The most instants one layer is drawn at.
inline constexpr int kMaxBlurSamples = 64;

struct ShutterSettings {
    double angle = 180.0; ///< degrees of the frame the shutter stays open: 0..720
    double phase = -90.0; ///< where it opens relative to the frame's instant, degrees of a frame: -360..360
    int samples = 16;        ///< at most this many instants in an export (Quality): 2..kMaxBlurSamples
    int previewSamples = 8;  ///< at most this many in the preview (Preview Quality); never more than `samples`

    /// The instants cap for a preview (the monitors) or an export.
    int cap(bool preview) const { return preview ? std::min(previewSamples, samples) : samples; }

    bool operator==(const ShutterSettings&) const = default;
};

/// The settings of one Motion Blur instance at `curveTime`, kept inside their ranges.
ShutterSettings shutterSettingsOf(const EffectInstance& instance, Ticks curveTime);

/// The clip's blur: its first enabled, evaluable Motion Blur instance whose angle is above 0 at `curveTime`; none when it
/// has no such instance (nothing is blurred then).
std::optional<ShutterSettings> motionBlurOf(std::span<const EffectInstance> stack, Ticks curveTime);

/// The `count` instants of the shutter, as offsets from the frame's instant in frames (add offset * frameDuration to the
/// timeline time); count < 1 gives none, a settings with no angle gives the one instant 0.
std::vector<double> shutterOffsets(const ShutterSettings& settings, int count);

/// How many instants a layer needs when the farthest point of its frame travels `travelPixels` (sequence pixels, canvas
/// scale) along its path through the shutter: one per pixel and a half of the smear, between 2 and `cap` (cap(preview)); 1
/// (no blur needed) when it moves less than a quarter pixel. Pure geometry in sequence pixels, so a preview at a reduced size
/// and an export decide alike.
int blurSampleCount(const ShutterSettings& settings, double travelPixels, bool preview);

} // namespace wabcut
