#pragma once

// Stream timestamps <-> source ticks: the one conversion between container time bases and Wabcut time.

#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>

namespace wabcut::media {

/// `pts` in `timeBase` units -> ticks, rounded to nearest (exact halves away from zero, as FFmpeg's
/// AV_ROUND_NEAR_INF). Exact for every time base that divides a second into whole ticks (all common container
/// time bases). Uses the checked 128-bit arithmetic of Time.h: std::nullopt for an invalid time base or a
/// result outside int64, never a wrapped value.
std::optional<Ticks> streamToTicks(std::int64_t pts, TimeBase timeBase);

/// Ticks -> `timeBase` units: the last unit whose start, as streamToTicks() reports it, is at or before `t`
/// (the unit that contains `t`). ticksToStream(streamToTicks(p)) == p for every time base. std::nullopt as
/// for streamToTicks().
std::optional<std::int64_t> ticksToStream(Ticks t, TimeBase timeBase);

} // namespace wabcut::media
