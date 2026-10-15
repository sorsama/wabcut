#pragma once

// Audio redactions and transcripts (review P1-3): what a project's source redactions (model/Masks.h, I.6) bleep or
// mute must not reach a transcriber, nor come back as text.
//
//   audioRedactionRanges()  the media times a project's enabled audio redactions cover for one media item: their word
//                           ranges, else their whole source range. Clip exceptions are ignored: a transcript belongs to
//                           the media, so a range any clip redacts is redacted for all (the safe direction).
//   redactTranscript()      replaces every word overlapping those ranges by kRedactedWord (times kept, confidence 0)
//                           and rebuilds the text of the segments that had one.
//
// TranscribeService silences the ranges in the audio before any transcriber (local whisper or a cloud upload) gets it,
// and redacts every result, cached ones included.

#include <wabcut/ai/Transcript.h>
#include <wabcut/model/ProjectState.h>

#include <span>
#include <string_view>
#include <vector>

namespace wabcut::ai {

// The one redacted-word token of the whole app: transcripts, captions, burn-in, sidecars and MCP text all read it
// (ai::edit::kRedactedWordText, export/playback kRedactedWordToken mirror it; tests pin them equal).
inline constexpr std::string_view kRedactedWord = "[bleep]";

/// Sorted, merged media-time ranges of `media`'s enabled redactions that mute or bleep.
std::vector<TimeRange> audioRedactionRanges(const ProjectState& project, MediaId media);

/// Replaces the words of `transcript` that overlap `ranges` (media time) by kRedactedWord. Returns how many.
std::size_t redactTranscript(Transcript& transcript, std::span<const TimeRange> ranges);

} // namespace wabcut::ai
