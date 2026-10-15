#pragma once

// AI voice-over, dubbing and narration (AI-12, docs/ai.md "Going global"): the lines to speak, and the edit that
// places the generated speech:
//   - dub lines are caption cues (a translated track, usually): their text without "[bleep]" (a bleeped word is never
//     voiced: the line just leaves it out) and their timing as the slot to fit (TimeFit.h);
//   - narration lines are a text's sentences (or lines), one after another, or one per clip of a rough cut (AI-11);
//   - the speech is ONE generated audio file (the lines one after another, each stretched as fitted, MediaItem::generated
//     with kind "audio" and the provider, model and a hash of the text), one clip per line on a new audio track with the
//     Dialogue role ("Dub (ไทย)", "Narration"), and the original dialogue ducked under the new lines (audio.autoDuck's
//     envelope, Ducking.h). All of it is one edit, one undo step.

#include <wabcut/ai/dub/TimeFit.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::dub {

struct DubLine {
    std::string text;         ///< what is said (no "[bleep]", one line)
    Ticks start{};            ///< the slot
    Ticks duration{};
    SpeakerId speaker;
    std::size_t source = 0;   ///< the cue's (or sentence's) index
};

/// Lines from caption cues: bleeps and line breaks out, cues with nothing left to say skipped.
std::vector<DubLine> linesFromCaptions(const CaptionTrack& track);

/// The text a line speaks: "[bleep]" removed, runs of spaces and line breaks one space, trimmed.
std::string speakableText(std::string_view text);

/// Narration: `text` as lines (each non-empty line; a long line is split into its sentences), slotless (duration 0).
std::vector<DubLine> narrationLines(std::string_view text);

/// Where the beats of a rough cut (AI-11, Sequence::derivedFrom) play: per beat with its chosen take on the timeline,
/// the span of its clips, in time order. Empty for any other sequence.
std::vector<TimeRange> roughCutBeats(const Sequence& sequence);

/// Lays narration out: one after another from `at` with `gap` between them when `clips` is empty, else line i on
/// clips[i] (the rough cut's beats) when there are as many clips as lines (otherwise one after another, and a warning).
struct NarrationLayout {
    std::vector<Slot> targets; ///< where each line goes ("slots" is a Qt keyword in app code)
    bool onClips = false;
    std::string warning;
};
NarrationLayout layoutNarration(std::span<const DubLine> lines, std::span<const Ticks> lengths, Ticks at, Ticks gap,
                                std::span<const TimeRange> clips);

/// The generated audio: one media item and where each line is in it (media time).
struct RenderedDub {
    MediaItem item;
    std::vector<TimeRange> segments;
};

struct AssemblyOptions {
    std::string trackName = "Dub";
    double duckDb = -18.0;                          ///< under the new lines; 0 = no ducking
    std::optional<std::vector<TrackId>> duckTracks; ///< else every other unlocked audio track with one of duckRoles
    std::vector<TrackRole> duckRoles{TrackRole::Dialogue, TrackRole::None};
};

struct Assembly {
    ProjectState state;
    MediaId media;
    TrackId track;
    std::vector<ClipId> clips;
    std::size_t duckedClips = 0;
};

/// The edit: the media item, the track (Dialogue role), a clip per fitted line, the ducking.
std::expected<Assembly, Error> assembleDub(const ProjectState& state, SequenceId sequence, const RenderedDub& audio,
                                           std::span<const FittedLine> fitted, const AssemblyOptions& options);

} // namespace wabcut::ai::dub
