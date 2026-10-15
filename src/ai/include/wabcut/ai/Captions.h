#pragma once

// SubRip (.srt) and WebVTT (.vtt) output from a Transcript: one cue per segment, at source times.

#include <wabcut/ai/Transcript.h>

#include <string>

namespace wabcut::ai {

/// "1\n00:00:01,250 --> 00:00:03,000\nHello world\n\n2\n..." (times rounded to the nearest millisecond; empty
/// segments skipped; text taken from the segment's words, so user edits show).
std::string toSrt(const Transcript& transcript);

/// "WEBVTT\n\n00:00:01.250 --> 00:00:03.000\nHello world\n\n..." with '&', '<' and '>' escaped.
std::string toVtt(const Transcript& transcript);

/// The text of a segment from its words, joined the way the transcript's language writes them (spaces only
/// between words of space-delimited languages; punctuation never gets a space before it).
std::string segmentWordsText(const Transcript& transcript, const TranscriptSegment& segment);

} // namespace wabcut::ai
