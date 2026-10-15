#pragma once

// Time remapping (Premiere's Time Remapping: speed ramps, frame-hold and reverse segments; C.6). A remapped clip's
// content holds a remap curve (ClipContent::timeRemap, the keyframe Curve of Parameters.h) from curve time to source
// time:
//   * key times are curve times. A remapped clip's curve time runs with the timeline from its source In
//     (Clip::curveTimeAt = sourceIn + (t - start), like a freeze frame's), so its sourceIn is a curve time, its speed
//     stays 100 % and trims, splits and slips move along the curve. Both halves of a razor cut share the curve.
//   * key values are source *positions* in whole ticks (exact in a double up to kMaxRemapSource).
// Between two keys the curve is a segment that plays forward (the next key's position is later), in reverse
// (earlier) or holds (the same position, or a key with Hold interpolation: a step). Before the first key and after
// the last one it holds that key's position. A segment whose two sides are linear (Curve's rule) is *straight*: a
// constant speed evaluated in exact integer arithmetic,
//     position(c) = a + (c - a.time) * (b - a) / (b.time - a.time), rounded toward a,
// so a straight segment of speed n/d matches Clip::sourceTimeAt of a clip at that speed. Other segments are ramps
// (ease and Bezier sides): the curve value rounded toward the segment's start key, clamped inside the segment.
//
// The source time shown at a curve time is the position in forward segments and holds, and the position minus one
// tick in reverse segments: reverse plays the frame that ends at the position (Clip::sourceTimeAt of a reversed
// clip does the same). So the frame at a key is exactly the key's position (forward, hold) or the frame before it
// (reverse), every segment is monotonic (non-decreasing forward, non-increasing in reverse, constant in a hold),
// and a forward or reverse segment only ever shows source times between its two keys' positions.
//
// Audio (A.10, playback's TrackRenderer.h "Speed"): straight forward segments at exactly 100 % (RemapSegment::isUnitySpeed,
// Clip::unitySpeedRanges) read the media straight; ramps, other speeds and reverse play along remapPositionAt, pitch
// kept or varispeed (Clip::maintainPitch); holds are silent.

#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wabcut {

/// Remap key positions are whole ticks in [0, kMaxRemapSource] (a double holds them exactly: ~9.8 hours).
inline constexpr std::int64_t kMaxRemapSource = std::int64_t{1} << 53;
/// At most this many keys in one remap (project files are untrusted; evaluation walks the keys).
inline constexpr std::size_t kMaxRemapKeys = 10'000;

enum class RemapMotion : std::uint8_t { Hold, Forward, Reverse };

/// One piece of a remap curve: between two keys, or the hold before the first / after the last key.
struct RemapSegment {
    Ticks from;       ///< curve time where it starts (the lowest Ticks for the hold before the first key)
    Ticks to;         ///< curve time where it ends, exclusive (the highest Ticks after the last key)
    Ticks sourceFrom; ///< position at `from` (its key's value)
    Ticks sourceTo;   ///< position at `to`
    RemapMotion motion = RemapMotion::Hold;
    /// A constant speed (both sides linear): (sourceTo - sourceFrom) / (to - from), exact.
    bool straight = true;

    /// Plays its media forwards at exactly 100 %: the only segments with sound.
    bool isUnitySpeed() const {
        return straight && motion == RemapMotion::Forward && sourceTo - sourceFrom == to - from;
    }
    bool operator==(const RemapSegment&) const = default;
};

/// Every segment of `remap`, in curve-time order: the hold before the first key, one per pair of keys, the hold
/// after the last key.
std::vector<RemapSegment> remapSegments(const Curve& remap);
/// The segment containing curve time `c`.
RemapSegment remapSegmentAt(const Curve& remap, Ticks c);
/// The source position at curve time `c` (the key's value at a key).
Ticks remapPositionAt(const Curve& remap, Ticks c);
/// The source time shown at curve time `c` (see the file comment).
Ticks remapSourceAt(const Curve& remap, Ticks c);
/// The lowest and highest source times shown over curve times [from, to); from < to.
std::pair<Ticks, Ticks> remapShownRange(const Curve& remap, Ticks from, Ticks to);
/// Why `remap` is not a valid remap curve, or nullopt: at least one key, well formed (Curve::isWellFormed), whole
/// tick positions in [0, kMaxRemapSource], and ramps whose Bezier controls keep the segment monotonic.
std::optional<std::string> remapProblem(const Curve& remap);

} // namespace wabcut
