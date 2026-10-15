#pragma once

// Loudness targets and the normalisation gain. Pure.

#include <wabcut/model/Effects.h>

#include <optional>
#include <span>
#include <string_view>

namespace wabcut::audio::edit {

/// A delivery target: integrated loudness and the true-peak ceiling (dBTP).
struct LoudnessPreset {
    std::string_view id;
    std::string_view name;
    double targetLufs;
    double truePeakDbtp;
};

/// youtube (-14 LUFS), spotify (-14, its playback reference), podcast (-16: Spotify and most podcast platforms),
/// applePodcasts (-16), broadcast (EBU R128: -23), atsc (A/85: -24, -2 dBTP). True-peak ceilings -1 dBTP unless
/// stated.
std::span<const LoudnessPreset> loudnessPresets();
const LoudnessPreset* findLoudnessPreset(std::string_view id);

inline constexpr std::string_view kGainEffect = "wabcut.audio.gain";
inline constexpr std::string_view kLimiterEffect = "wabcut.audio.limiter";

/// The stack's normalisation gain: its first instance (after an Essential Sound record, A.7) when that is an enabled
/// wabcut.audio.gain with a constant value (normalising adjusts it), else null (normalising inserts one at the top of
/// the stack, before dynamics and limiters).
const EffectInstance* normalizationGain(const EffectStack& stack);
/// Where normalising inserts a gain: 0, or 1 after an Essential Sound record.
std::size_t normalizationGainIndex(const EffectStack& stack);

/// The gain (dB) a normalisation gain moves to when `deltaDb` is added, within the Gain effect's range.
double nextNormalizationGainDb(double currentDb, double deltaDb);

} // namespace wabcut::audio::edit
