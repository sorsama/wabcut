#pragma once

// MediaTranscript (P0 round, the source clip transcript): what one media item's audio stream says, in SOURCE time - the
// Transcript panel's Source mode (the clip in the Source Monitor: select a soundbite, then Insert or Overwrite it) and
// transcript.mediaWords read it.
//
// The words are the provider's transcript of the stream with the project's corrections on top (C.7, TranscriptEdits),
// in time order. A word whose source range overlaps any enabled audio redaction of the media (bleep or mute, whatever
// clips the redaction applies to: no clip is involved here) reads as kRedactedWordText, so redacted speech stays private
// in the source view too.

#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::ai::edit {

/// One word of a media stream.
struct MediaWord {
    TimeRange source;             ///< the transcript word's [t0, t1) in the media
    std::uint32_t word = 0;       ///< index into MediaTranscript::transcript()->words
    std::optional<int> speaker;   ///< the transcript's diarization label
    float confidence = 0.0f;
    bool redacted = false;        ///< inside an audio redaction of the media: its text reads as kRedactedWordText

    bool operator==(const MediaWord&) const = default;
};

class MediaTranscript {
public:
    MediaTranscript() = default;

    /// The words of `media`'s audio stream `audioStream` (an index into MediaInfo::audio) in `state`; empty when the
    /// item is unknown, a generator, has no such stream, or `provider` has no transcript of it.
    static MediaTranscript build(const ProjectState& state, MediaId media, int audioStream, const TranscriptProvider& provider);

    MediaId media() const { return m_media; }
    int audioStream() const { return m_audioStream; }
    std::span<const MediaWord> words() const { return m_words; }
    std::size_t size() const { return m_words.size(); }
    bool empty() const { return m_words.empty(); }
    const MediaWord& operator[](std::size_t i) const { return m_words[i]; }
    /// The transcript the words come from (corrections applied), or null.
    const std::shared_ptr<const Transcript>& transcript() const { return m_transcript; }

    /// Word i's text (kRedactedWordText when it is redacted).
    std::string_view text(std::size_t i) const;
    /// The transcript's language ("" without one).
    std::string_view language() const;
    /// Words first..last (inclusive) as text, joined the way the language writes them (no spaces in Thai).
    std::string textOf(std::size_t first, std::size_t last) const;
    /// [begin, end): from the first to the last word that overlaps `range` (a zero-length word: its instant inside it).
    /// Words may overlap each other, so a word inside the span need not overlap `range` itself.
    std::pair<std::size_t, std::size_t> wordsIn(TimeRange range) const;
    /// The word playing at `t`, else the nearest one (by distance to its range); nullopt when empty.
    std::optional<std::size_t> wordAt(Ticks t) const;
    /// Same media, stream, words and texts (a rebuild that changed nothing the reader sees).
    bool sameAs(const MediaTranscript& other) const;

private:
    MediaId m_media;
    int m_audioStream = 0;
    std::shared_ptr<const Transcript> m_transcript;
    std::vector<MediaWord> m_words;
};

} // namespace wabcut::ai::edit
