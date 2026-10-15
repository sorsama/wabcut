#pragma once

// SequenceTranscript: what a sequence says, in timeline order - the basis of transcript-first editing ("edit the
// video by editing its text"), the review of fillers and silences, and captions.
//
// Transcripts are made per media audio stream at source times (wabcut/ai/Transcript.h). A sequence shows them
// through its audio clips: a word is on the timeline when its midpoint lies inside a clip's source window
// [sourceIn, sourceOut), at the timeline position the clip's speed and direction give it (Clip::sourceTimeAt's
// inverse); words trimmed away are not there. Video clips contribute nothing: a linked A/V pair is heard through
// its audio clip, and selections of words map back to the whole link group (clipsFor). When one link group holds
// two audio clips of the same media stream (a stereo pair split over two tracks), its words count once.
//
// Nested and multicam clips (C.8) are read through (C.17): an audio clip that plays a sequence shows the words of that
// sequence's audio tracks it mixes - a multicam clip only its active audio angles' tracks - mapped from the nested
// sequence's time onto the timeline through the clip's sourceIn, speed, direction and remap, like media words. Such a
// word belongs to the outer clip (TimelineWord::clip), so selections, seeking and cuts act on that clip; its source is
// still the leaf media's word.
//
// update() keeps it current after an edit from the edit's ChangeSet: clips that only moved shift their words,
// changed clips are mapped again, everything else is untouched (a change inside a sequence it reads through: rebuilt).
//
// Redacted speech stays private: a word whose media range overlaps an enabled audio redaction (Masks.h: bleep or
// mute; its audioRanges, else its source range) of that media, on a clip it applies to, reads as kRedactedWordText
// wherever text comes from here (transcript.text / words / search, captions, corrections).

#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wabcut::ai::edit {

/// Supplies the transcripts of media audio streams.
class TranscriptProvider {
public:
    virtual ~TranscriptProvider() = default;
    /// The transcript of `media`'s audio stream `audioStream` (an index into MediaInfo::audio, like
    /// StreamRef::index), or null when there is none.
    virtual std::shared_ptr<const Transcript> transcript(const MediaItem& media, int audioStream) const = 0;
};

/// Transcripts handed over explicitly (tests, the CLI's --transcript, an app that already holds them).
class MapTranscriptProvider : public TranscriptProvider {
public:
    void add(MediaId media, int audioStream, std::shared_ptr<const Transcript> transcript);
    std::shared_ptr<const Transcript> transcript(const MediaItem& media, int audioStream) const override;
    bool empty() const { return m_transcripts.empty(); }

private:
    std::map<std::pair<std::uint64_t, int>, std::shared_ptr<const Transcript>> m_transcripts;
};

/// What a redacted word reads as (the token captions show for bleeped speech).
inline constexpr std::string_view kRedactedWordText = wabcut::ai::kRedactedWord;

/// Whether media range `source` of `media` overlaps an enabled audio redaction (bleep or mute) of it; with a clip,
/// only the redactions that apply to that clip (Redaction::appliesTo). `redactions` are matched by their `media` as
/// given (pass redaction roots on both sides, model/RedactionMedia.h, or use the overload below).
bool isAudioRedacted(std::span<const Redaction> redactions, MediaId media, TimeRange source,
                     std::optional<ClipId> clip = std::nullopt);
/// The same over `state`'s redactions covering `media` (its subclip master's and stem source's too: review wave 5).
bool isAudioRedacted(const ProjectState& state, MediaId media, TimeRange source, std::optional<ClipId> clip = std::nullopt);

/// One word as the sequence plays it.
struct TimelineWord {
    TimeRange timeline;            ///< on the timeline, inside its clip
    TimeRange source;              ///< in the media: the transcript word's [t0, t1)
    ClipId clip;                   ///< the audio clip it is heard through
    TrackId track;
    LinkGroupId linkGroup;         ///< the clip's link group (its video partner), if any
    std::uint16_t trackOrder = 0;  ///< 0 = A1, 1 = A2, ...
    std::uint32_t transcript = 0;  ///< index into SequenceTranscript::transcripts()
    std::uint32_t word = 0;        ///< index into that transcript's words
    std::optional<int> speaker;    ///< the transcript's diarization label
    SpeakerId speakerId;           ///< the project speaker (ProjectState::speakers, C.7), when one is known
    float confidence = 0.0f;
    bool redacted = false;         ///< inside an audio redaction: its text reads as kRedactedWordText
};

class SequenceTranscript {
public:
    SequenceTranscript() = default;

    /// Maps the transcripts of every audio clip of `sequence` onto the timeline.
    static SequenceTranscript build(const ProjectState& state, SequenceId sequence, const TranscriptProvider& provider);

    /// Brings the mapping up to date with `next`, the state after an edit described by `changes` (the edit's
    /// ChangeSet, or diff(before, next)). Falls back to a full build when the edit changed tracks, links or media.
    void update(const ProjectState& next, const ChangeSet& changes, const TranscriptProvider& provider);

    SequenceId sequence() const { return m_sequence; }
    /// Every visible word, in timeline order (start, then track A1..An).
    std::span<const TimelineWord> words() const { return m_words; }
    std::size_t size() const { return m_words.size(); }
    bool empty() const { return m_words.empty(); }
    const TimelineWord& operator[](std::size_t i) const { return m_words[i]; }
    std::span<const std::shared_ptr<const Transcript>> transcripts() const { return m_transcripts; }

    /// Word i's text (kRedactedWordText when it is redacted).
    std::string_view text(std::size_t i) const;
    /// The language of the transcript word i comes from.
    std::string_view language(std::size_t i) const;
    /// Words first..last (inclusive) as text, joined the way their language writes them.
    std::string textOf(std::size_t first, std::size_t last) const;

    /// [begin, end) indices of the words that overlap `range` on the timeline.
    std::pair<std::size_t, std::size_t> wordsIn(TimeRange range) const;
    /// The word playing at `t`, else the nearest one (by distance to its range); nullopt when empty.
    std::optional<std::size_t> wordAt(Ticks t) const;
    /// The clips words first..last are heard through plus their linked partners (selections act on link groups),
    /// sorted by id. Clips no longer in `sequence` are skipped.
    std::vector<ClipId> clipsFor(std::size_t first, std::size_t last, const Sequence& sequence) const;

    /// The timeline extent of the clips that contribute words.
    std::optional<TimeRange> extent() const;

private:
    struct ClipEntry {
        bool nested = false;   ///< C.17: a nested or multicam audio clip read through (mapped again on any change)
        TimeRange timeline;
        Ticks sourceIn;
        Speed speed;
        bool reversed = false;
        bool remapped = false; ///< words map through the remap's 100 % segments (rebuilt on any change)
        MediaId media;
        MediaId redactionRoot; ///< whose redactions it hears (model/RedactionMedia.h)
        int audioStream = 0;
        TrackId track;
        std::uint16_t trackOrder = 0;
        LinkGroupId linkGroup;
        std::uint32_t transcript = 0;
        bool contributes = false; ///< false: a duplicate of another clip of its link group
    };

    std::optional<std::uint32_t> transcriptFor(const MediaItem& media, int audioStream, const ProjectState& state,
                                               const TranscriptProvider& provider);
    void addClip(const Clip& clip, const Track& track, std::uint16_t order, const ProjectState& state,
                 const TranscriptProvider& provider);
    void appendWords(const Clip& clip, const ClipEntry& entry);
    void addNestedClip(const Clip& clip, const Track& track, std::uint16_t order, const ProjectState& state,
                       const TranscriptProvider& provider);
    static SequenceTranscript buildTracks(const ProjectState& state, SequenceId sequence, const TranscriptProvider& provider,
                                          const std::vector<TrackId>* only, int depth);
    void appendWord(const Clip& clip, const ClipEntry& entry, std::uint32_t word, TimeRange timeline);
    void sortWords();

    SequenceId m_sequence;
    std::vector<TimelineWord> m_words;
    std::vector<std::shared_ptr<const Transcript>> m_transcripts; ///< with the project's corrections applied
    std::vector<std::optional<TranscriptEdits>> m_edits;           ///< per transcript: the project's edits of it
    std::vector<Ticks> m_longestWord; ///< per transcript, for the source-window search
    std::map<std::pair<std::uint64_t, int>, std::optional<std::uint32_t>> m_transcriptIndex;
    std::unordered_map<std::uint64_t, ClipEntry> m_clips; ///< audio clips with a transcript, by clip id
    std::vector<Redaction> m_redactions; ///< the project's enabled audio redactions (their areas dropped)
    int m_depth = 0;                      ///< nesting depth while building (nested sequences read through)
    bool m_readsNested = false;           ///< words come through nested or multicam clips
};

} // namespace wabcut::ai::edit
