#pragma once

// Auto-ducking: music turned down under speech, as plain volume keyframes on the music clips (editable afterwards,
// one undo step through audio.autoDuck, previewable with dryRun). Pure.
//
// The envelope per speech run: the music ramps down by `amount` over `attack` so it is fully down when the speech
// starts, stays down until `hold` after the speech ends, then ramps back over `release`. Speech runs closer than
// attack + release (after the hold) are bridged, so the music does not pump between sentences; speech shorter than
// minSpeech is ignored. Ramps are linear in dB.

#include <wabcut/model/Clip.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <optional>
#include <span>
#include <vector>

namespace wabcut::audio::edit {

struct DuckingOptions {
    double amountDb = -12.0; ///< how far the music goes down (negative dB)
    Ticks attack{kTicksPerSecond / 4};
    Ticks release{kTicksPerSecond * 3 / 4};
    Ticks hold{kTicksPerSecond / 4};
    Ticks minSpeech{kTicksPerSecond * 12 / 100};
};

/// A.10, "Game under voice" (audio.autoDuck "preset": "gameUnderVoice"; the tracks: GameAudio.h): game audio under a
/// streamer's mic. -10 dB, 0.12 s attack, 0.35 s hold, 0.8 s release, speech from 0.15 s. The game is the content, so it
/// goes down less than a music bed and comes back quickly; pauses shorter than attack + hold + release (1.27 s) stay
/// down. Reasons in Ducking.cpp.
DuckingOptions gameUnderVoiceDucking();

/// One ducked stretch of the timeline: down-ramp [downStart, speech.start), down [speech.start, upStart),
/// up-ramp [upStart, upEnd).
struct DuckRegion {
    TimeRange speech; ///< the (bridged) speech it covers
    Ticks downStart;
    Ticks upStart;
    Ticks upEnd;
};

/// The regions for speech ranges (any order; overlapping ranges are merged).
std::vector<DuckRegion> duckRegions(std::span<const TimeRange> speech, const DuckingOptions& options);

/// The duck's gain change (dB, from amountDb to 0) at timeline time `t`.
double duckOffsetDb(std::span<const DuckRegion> regions, const DuckingOptions& options, Ticks t);

/// A music clip's volume with the duck applied over the clip's part of `range` (nullopt: the whole clip), as linear
/// keyframes in curve time: the clip's level there (its constant, or its curve's value where the range starts)
/// plus the duck at every envelope corner. Like Premiere's Auto Ducking it replaces the volume keyframes inside
/// that span; keys outside it are kept. nullopt when the clip has no part in the range.
std::optional<ParamValue> duckedVolume(const Clip& clip, std::span<const DuckRegion> regions,
                                       const DuckingOptions& options, std::optional<TimeRange> range = std::nullopt);

} // namespace wabcut::audio::edit
