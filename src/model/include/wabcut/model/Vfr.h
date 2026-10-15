#pragma once

// Variable-frame-rate sources (C.15, gaming captures). OBS, ShadowPlay, Game Bar and phone recordings are often VFR: the
// frames' presentation times do not lie on a fixed grid (VideoStreamInfo::variableFrameRate; frameRate is then the
// nominal or average rate). Wabcut's policy is to CONFORM BY TIMESTAMP to the sequence rate:
//
// * Sequence frame n of a clip shows the source frame whose presentation time is the latest at or before
//   clip.sourceTimeAt(n x sequence frame duration) (VfrConform::LatestAtOrBefore). Times are exact ticks; nothing counts
//   source frames at a nominal rate, so picture and sound stay in sync over hour-long VODs (no drift), and a dropped or
//   duplicated capture frame repeats or skips one frame where it happened instead of shifting everything after it.
// * Edits, trims, marks and frame stepping of a VFR stream use the SEQUENCE's frame grid (editingRate): a VFR stream has
//   no frame grid of its own. A constant-rate stream keeps its own grid.
// * Decoders and proxies honour the same rule: conformedSourceTimes() lists the exact source times to show.

#include <wabcut/model/Clip.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <vector>

namespace wabcut {

enum class VfrConform : std::uint8_t { LatestAtOrBefore };

/// The policy (one for now): VfrConform::LatestAtOrBefore.
constexpr VfrConform vfrConformPolicy() { return VfrConform::LatestAtOrBefore; }

/// The frame grid edits use for `stream` in a sequence of `sequenceRate`: its own rate when it is constant and valid,
/// else (VFR, or an unknown rate) the sequence's.
Rate editingRate(const VideoStreamInfo& stream, Rate sequenceRate);

/// The start of the editing-grid frame holding source time `t` (marks, match frame, stepping). Exact ticks.
Ticks snapToEditingGrid(Ticks t, const VideoStreamInfo& stream, Rate sequenceRate);

/// The exact source times `clip` shows for sequence frames [firstFrame, firstFrame + count) of a sequence at
/// `sequenceRate` (frames inside the clip; others are skipped): clip.sourceTimeAt(frame start). A VFR decoder shows the
/// latest source frame at or before each. At most 1,000,000 times.
std::vector<Ticks> conformedSourceTimes(const Clip& clip, Rate sequenceRate, std::int64_t firstFrame, std::int64_t count);

/// The nominal rate to show for a VFR stream whose average rate is `average`: the nearest of 23.976, 24, 25, 29.97,
/// 30, 48, 50, 59.94, 60, 90, 100, 119.88, 120, 144, 165, 240 within 2 %, else `average` itself (normalized).
Rate nominalRate(Rate average);

} // namespace wabcut
