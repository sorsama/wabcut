#pragma once

// Captions from a SequenceTranscript: cues of at most maxLines lines of at most maxCharsPerLine display characters
// and at most maxDuration, in timeline time, stored as a model CaptionTrack (Sequence::captionTracks) and written
// as SRT or WebVTT.
//
// Cues break at pauses, speaker changes and, once half full, sentence ends. Lines break only between words and are
// balanced; unspaced languages (Thai) join their words without spaces and break between the word units AI-2's word
// building produced (never inside a unit, never at a space that is not there). Display length counts code points
// that take a cell: Thai vowels above/below and tone marks (and other combining marks) count 0.
//
// SRT/VTT follow ai::toSrt/ai::toVtt (Captions.h) exactly - numbering, millisecond rounding, VTT escaping - but keep
// the cue's line breaks, which those transcript writers flatten into one line.

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Sequence.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::edit {

struct CaptionOptions {
    int maxCharsPerLine = 42;
    int maxLines = 2;
    Ticks maxDuration = Ticks::fromSeconds(7);
    Ticks minDuration = Ticks{kTicksPerSecond * 7 / 10};
    /// A longer pause between two words starts a new cue.
    Ticks maxPause = Ticks::fromSeconds(1);
    bool breakOnSpeakerChange = true;
    /// Once a cue is half full, a sentence end closes it.
    bool splitOnPunctuation = true;
    /// The least time between a cue's end and the next one's start (a cue is shortened for it, never below its words
    /// when there is room).
    Ticks minGap{};
    /// Line-break rules (CaptionText.h; the app's ICU / Qt breaker): lines may also break at its opportunities inside
    /// words. Null: between transcript words only.
    const LineBreaker* breaker = nullptr;
};

/// Display width of UTF-8 text (combining marks count 0).
std::size_t displayLength(std::string_view text);

/// Joins words into one line the way `language` writes them (spaces only for space-delimited languages, never
/// before trailing punctuation; Latin words inside unspaced text still get one).
std::string joinWords(std::span<const std::string> words, std::string_view language);

/// Splits words into at most `maxLines` lines (fewest lines that fit, then the most balanced, preferring breaks
/// after punctuation and a shorter first line). nullopt when they do not fit.
std::optional<std::vector<std::string>> breakLines(std::span<const std::string> words, std::string_view language,
                                                   int maxCharsPerLine, int maxLines);

/// `words` as caption text: joined the way `language` writes them and wrapped into options.maxLines lines of
/// options.maxCharsPerLine characters (breaking between the words, and at options.breaker's opportunities); one line
/// when they do not fit.
std::string captionText(std::span<const std::string> words, std::string_view language, const CaptionOptions& options);

/// The caption track of a sequence's transcript (id unset; set it with edit::setCaptionTrack). Its language is the
/// language most words have; each cue's speaker is the project speaker of most of its words.
CaptionTrack buildCaptions(const SequenceTranscript& transcript, const CaptionOptions& options = {},
                           std::string name = "Captions");

std::string toSrt(const CaptionTrack& captions);
std::string toVtt(const CaptionTrack& captions);

} // namespace wabcut::ai::edit
