#pragma once

// Auto-mix of several voices (card 137, audio.autoMix): in a podcast, an interview or a co-op stream with one microphone
// per person, the microphones of the people who are not speaking are turned down, so room noise, breathing and bleed
// from the other voices stop piling up. Like auto-ducking (Ducking.h) it writes plain volume keyframes on the clips of
// each voice track (editable afterwards, one undo step, previewable with dryRun). Pure.
//
// Per voice track the question is "when must this microphone be down?": while another voice speaks and this one does
// not. Where both speak (talking over each other) nothing is lowered. The envelope is built so that a voice is never
// lowered under its own words:
//   * a voice stays "on" for `hold` after its last word (a breath, a pause between sentences);
//   * the ramp that brings a microphone back up ends `lead` before its speaker's first word, so the first syllable is at
//     full level (offline, the speech is known in advance), also when the speaker starts talking over someone;
//   * a ramp down that would run into the voice's own (held) speech starts after it instead;
//   * a gap between two lowered stretches shorter than attack + release is bridged, so a mic does not pump;
//   * a stretch too short to be lowered and raised again is left alone.
// With `gate` a microphone is also lowered while nobody speaks (a noise gate for each voice).

#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/model/Time.h>

#include <optional>
#include <span>
#include <vector>

namespace wabcut::audio::edit {

struct AutoMixOptions {
    double amountDb = -12.0;                     ///< how far a silent microphone goes down (negative dB)
    Ticks attack{kTicksPerSecond / 10};          ///< ramp down (linear in dB)
    Ticks release{kTicksPerSecond / 2};          ///< ramp up
    Ticks hold{kTicksPerSecond * 4 / 10};        ///< a voice stays on this long after its last word
    Ticks lead{kTicksPerSecond * 8 / 100};       ///< a microphone is back up this long before its speaker's first word
    Ticks minSpeech{kTicksPerSecond * 2 / 10};   ///< speech shorter than this (a cough, "mm") is not a voice
    bool gate = false;                           ///< also lower a microphone while nobody speaks

    bool operator==(const AutoMixOptions&) const = default;
};

/// The regions in which voice `voice` is lowered, from every voice's speech (`speech[k]`: ranges where voice k speaks,
/// any order, overlaps merged). `span` is the stretch the mix works on (the command's from / to, else the sequence); it
/// bounds the gate. Regions use the Ducking.h shape (ramp down, plateau = `speech`, ramp up) with their own ramp
/// lengths, so duckedVolume() writes them.
std::vector<DuckRegion> autoMixRegions(std::span<const std::vector<TimeRange>> speech, std::size_t voice,
                                       const AutoMixOptions& options, std::optional<TimeRange> span = std::nullopt);

/// The Ducking.h options a region list of this mix is written with (only the amount matters to duckedVolume()).
DuckingOptions duckingOptionsOf(const AutoMixOptions& options);

/// Sorts, drops empty ranges and merges ranges that overlap or touch.
std::vector<TimeRange> mergedRanges(std::vector<TimeRange> ranges);

} // namespace wabcut::audio::edit
