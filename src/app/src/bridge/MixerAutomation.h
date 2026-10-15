#pragma once

// Audio Track Mixer automation (M5l): what a playback pass with the faders writes into a track's (or the master's)
// fader or pan curve, Premiere's Read / Touch / Latch / Write modes.
//
//   * Read: the fader follows the automation; nothing is written while playing.
//   * Touch: writes while the fader is held; on release it returns to the automation that was there.
//   * Latch: starts writing when the fader is touched and keeps writing its last value until playback stops.
//   * Write: writes from the start of playback to its end, touched or not.
//
// The bridge records the stretches written during a pass (AutomationSegment); applyAutomationPass() turns them into the
// new parameter value, which one setParameter commits at stop: one undo step per pass. Pure, so tests check the keys.

#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <QString>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace wabcut::app {

enum class AutomationMode : std::uint8_t { Read, Touch, Latch, Write };

/// "read" / "touch" / "latch" / "write".
QString automationModeName(AutomationMode mode);
std::optional<AutomationMode> automationModeFromName(const QString& name);

/// One stretch of a pass where the fader wrote: [start, end] in timeline time and the fader's values in it
/// (time ascending, inside [start, end]; the first at `start`).
struct AutomationSegment {
    Ticks start;
    Ticks end;
    std::vector<std::pair<Ticks, double>> samples;
};

/// The gap between a written stretch and the automation around it: the old value is kept up to one gap before the
/// stretch and comes back one gap after it (a short ramp, not a jump).
inline constexpr Ticks kAutomationAnchorGap{kTicksPerSecond / 25};

/// `existing` with each segment written over it: keys inside [start - gap, end + gap] are replaced by the segment's
/// samples (thinned: a sample within `tolerance` of the straight line between its kept neighbours is dropped), anchored
/// to the old automation one gap before and after. Keys outside stay. A segment that starts within one gap of 0 has no
/// leading anchor. Returns a curve unless nothing was written.
ParamValue applyAutomationPass(const ParamValue& existing, std::span<const AutomationSegment> segments,
                               double tolerance = 0.05);

/// The samples without the ones a straight line between their kept neighbours predicts within `tolerance` (the first
/// and the last are kept).
std::vector<std::pair<Ticks, double>> thinAutomation(std::span<const std::pair<Ticks, double>> samples,
                                                      double tolerance);

} // namespace wabcut::app
