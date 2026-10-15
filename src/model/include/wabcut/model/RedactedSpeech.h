#pragma once

// Redacted speech on a sequence's timeline (review wave 5, A5-P1-4 / S5-P1-2): the one place that says where a
// project's audio redactions bleep or mute the sound of a sequence, so every reader of caption text that sends it
// somewhere - a voice provider (dub), a cloud model (translation, the assistant's captions.get), an MCP agent, a sidecar
// file - masks it the same way, whatever made the cue (a transcript, typing, an imported SRT). Burned-in captions and
// sidecars mask word by word where the karaoke timings are known (playback/CaptionRedaction.h); without word timings a
// cue over redacted speech is masked as a whole.

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// What a redacted word (or a cue whose words cannot be told apart) becomes: ai::kRedactedWord,
/// playback::kRedactedWordToken and ai::edit::kRedactedWordText mirror it.
inline constexpr std::string_view kRedactedSpeechToken = "[bleep]";

/// Whether any enabled redaction of `project` bleeps or mutes sound (the cheap test before redactedSpeechRanges).
bool hasSpeechRedactions(const ProjectState& project);

/// The timeline ranges of `sequence` whose sound a redaction of `project` bleeps or mutes: every enabled redaction with
/// audio (its audioRanges, else its whole source range) through every clip, video or audio, of its media - or of media
/// sharing its redaction root: subclips, stems (model/RedactionMedia.h) - that it applies to (only top-level clips
/// overlapping `within`, when given). Nested and multicam clips are followed into the tracks they read, to any depth,
/// and what plays inside maps out through each level's timing; a redaction excepting a nested clip excepts everything
/// inside it (review wave 5 E5-P1-2). Plain clips map through their speed and direction (a freeze frame of a redacted
/// moment is covered whole); a remapped clip maps its 100 % segments exactly and covers any other segment showing a
/// redacted moment whole (conservative: no word inside it can slip through), and a nest too deep or too wide to follow
/// is covered whole. Sorted, merged, never empty ranges.
std::vector<TimeRange> redactedSpeechRanges(const ProjectState& project, const Sequence& sequence,
                                            std::optional<TimeRange> within = std::nullopt);

/// Whether [start, end) overlaps one of the sorted, merged `ranges`.
bool overlapsRedactedSpeech(std::span<const TimeRange> ranges, Ticks start, Ticks end);

/// `cue`'s text as it may leave the computer when its words have no timings: `token` when the cue overlaps `ranges`,
/// else its text.
std::string outwardCueText(const CaptionCue& cue, std::span<const TimeRange> ranges, std::string_view token = kRedactedSpeechToken);

} // namespace wabcut
