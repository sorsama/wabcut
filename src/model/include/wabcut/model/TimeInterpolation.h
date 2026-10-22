#pragma once

// Time Interpolation (P.2, docs/motion.md): how a retimed video clip makes the frames that fall between its media's
// frames - Premiere's Clip > Video Options > Time Interpolation, also in the Speed/Duration dialog.
//
//   Frame Sampling  repeats or drops whole frames: the frame nearest below the source time (the default; what every
//                   clip did before P.2)
//   Frame Blending  the two frames around the source time, mixed by how far between them it lies
//   Optical Flow    the two frames warped along their motion (motion::estimateFlowPair) toward the source time and
//                   blended, each side weighted by how surely it sees the point (occlusions); sections with it need
//                   rendering for real-time playback (the render bar shows them red)
//
// It applies wherever a clip's video is retimed: a speed other than 100 %, reverse, time remapping (ramps), Rate
// Stretch, and a media frame rate other than the sequence's (24 or 30 fps footage in a 60 fps sequence). The setting
// lives in the clip's shared content (ClipContent::timeInterpolation): both halves of a razor cut keep it. The edit is
// edit::setTimeInterpolation (MotionOps.h).

#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace wabcut {

struct Clip;
struct MediaItem;

enum class TimeInterpolation : std::uint8_t { FrameSampling, FrameBlending, OpticalFlow };

/// "frameSampling", "frameBlending", "opticalFlow": what project files and the commands use.
std::string_view toString(TimeInterpolation mode);
std::optional<TimeInterpolation> timeInterpolationFromString(std::string_view text);
/// Premiere's menu labels: "Frame Sampling", "Frame Blending", "Optical Flow".
std::string_view displayName(TimeInterpolation mode);

/// Whether `clip`, a video clip of `item`, shows source times off its media's frame grid, so its Time Interpolation
/// decides the picture: a speed other than 100 %, reverse, a time remap, or a media frame rate other than
/// `sequenceRate`. False for freeze frames, still images, generated items and clips of sequences. For an item with an
/// interpreted frame rate (Interpret Footage, edit::conformSpeed), its conform speed is 100 % and the interpreted rate
/// is the media's: a 120 fps capture interpreted as 30 fps in a 30 fps sequence shows every frame, nothing retimed.
bool retimesFrames(const Clip& clip, const MediaItem& item, Rate sequenceRate);

} // namespace wabcut
