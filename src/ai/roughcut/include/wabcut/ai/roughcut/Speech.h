#pragma once

// What was said, prepared for matching and for judging takes (AI-11). Shared by the rough cut from a script
// (RoughCut.h) and retake comparison (Takes.h).
//
//   SpeechSource   one run of transcribed speech: a media item's audio stream (times are its source times) or a
//                  sequence (times are timeline times, words carry their clips). Each source has its words
//                  (edit::SequenceTranscript), the transcript review (fillers and long pauses, AI-3), its sentences
//                  (AI-9's splitter: punctuation, pauses, speaker changes; Thai included) and its matching units
//                  (TextMatch.h), each unit pointing back at its word.
//   SpeechCorpus   the sources of a scope. Media sources are mapped through a scratch sequence per media item (one
//                  clip at 0 at 100 %), so every AI-3 tool reads them in source time; the scratch never reaches the
//                  project.
//   TakeQuality    how good one attempt is: how well it says what it should, fillers, hesitations and false starts,
//                  complete sentence or not, level / clipping / noise floor (TakeAudioProbe), pace and ASR
//                  confidence, combined into one deterministic 0..1 score with the reasons in words.

#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/roughcut/TextMatch.h>
#include <wabcut/ai/shorts/Shorts.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::ai::roughcut {

/// Level measurements of a stretch of audio.
struct AudioStats {
    std::optional<double> rmsDb;  ///< dBFS
    std::optional<double> peakDb; ///< dBFS of the loudest sample (clipping near 0)
};

/// Measures stretches of a media audio stream (media time) for take quality.
class TakeAudioProbe {
public:
    virtual ~TakeAudioProbe() = default;
    virtual AudioStats stats(const MediaItem& media, int audioStream, TimeRange source) const = 0;
    /// Every range stats() is about to be asked for, first (a probe may measure them in one pass per file).
    virtual void prefetch(std::span<const edit::AudioEnergyProbe::Range> ranges) const { (void)ranges; }
};

/// RMS only, from the transcript review's energy probe (edit::AudioEnergyProbe).
class EnergyTakeProbe final : public TakeAudioProbe {
public:
    explicit EnergyTakeProbe(std::shared_ptr<edit::AudioEnergyProbe> energy) : m_energy(std::move(energy)) {}
    AudioStats stats(const MediaItem& media, int audioStream, TimeRange source) const override;
    void prefetch(std::span<const edit::AudioEnergyProbe::Range> ranges) const override;

private:
    std::shared_ptr<edit::AudioEnergyProbe> m_energy;
};

struct SpeechSource {
    enum class Kind { Media, Sequence };
    Kind kind = Kind::Media;
    MediaId media;                  ///< Media: the item; Sequence: invalid
    int audioStream = 0;            ///< Media: the stream (index into MediaInfo::audio)
    SequenceId sequence;            ///< Sequence: the real sequence; Media: the scratch one
    std::string name;
    std::string language;           ///< of the first transcript
    edit::SequenceTranscript transcript;
    std::vector<edit::ReviewItem> review;
    std::vector<shorts::Sentence> sentences;
    std::vector<Unit> units;        ///< every word's units in order; Unit::source = word index
    std::vector<std::uint32_t> unitStart; ///< per word: its first unit (size = words + 1)
    std::vector<std::uint8_t> filler;     ///< per word: 1 inside a filler of the review
    double wordsPerSecond = 0.0;    ///< median pace of its sentences
};

struct SpeechCorpus {
    /// The state the sources' transcripts map through (the project plus one scratch sequence per media source).
    std::shared_ptr<const ProjectState> state;
    std::vector<SpeechSource> sources;
    std::size_t words = 0;
    std::optional<double> medianRmsDb; ///< of the takes measured so far (set by the callers that measure)
    std::vector<std::string> warnings;
};

struct CorpusOptions {
    edit::ReviewOptions review;              ///< fillers and pauses (0.8 s by default)
    Ticks hesitation = Ticks{kTicksPerSecond * 7 / 10}; ///< a pause inside a take this long counts against it
};

/// The speech of `media` (each item's first audio stream that has a transcript; items without one are named in the
/// warnings). `energy` (optional) checks pauses for the review.
SpeechCorpus mediaCorpus(const ProjectState& state, std::span<const MediaId> media, const edit::TranscriptProvider& transcripts,
                         const edit::AudioEnergyProbe* energy, const CorpusOptions& options = {});

/// Adds the speech of a sequence (timeline times) to `corpus` (whose state must be `state` or carry its sequences).
void addSequenceSource(SpeechCorpus& corpus, const ProjectState& state, SequenceId sequence, const edit::TranscriptProvider& transcripts,
                       const edit::AudioEnergyProbe* energy, const CorpusOptions& options = {});

/// Builds the units, fillers, sentences and pace of a source whose transcript and review are set.
void prepareSource(SpeechSource& source);

/// A stretch of words of one source: words first..last (indices into its transcript).
struct WordSpan {
    std::size_t source = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    bool operator==(const WordSpan&) const = default;
};

/// The span's times on its source's axis (first word's start to last word's end).
TimeRange spanRange(const SpeechCorpus& corpus, const WordSpan& span);
/// The span's text as said.
std::string spanText(const SpeechCorpus& corpus, const WordSpan& span);
/// The media stretch a span plays (Media sources: its range; Sequence sources: when all its words come from one clip of
/// one media at 100 % forward, that media range), or nullopt.
struct MediaRange {
    MediaId media;
    int audioStream = 0;
    TimeRange source;
    bool operator==(const MediaRange&) const = default;
};
std::optional<MediaRange> mediaRangeOf(const SpeechCorpus& corpus, const WordSpan& span);
/// mediaRangeOf() plus up to `handle` of media each side, at most half the pause to the neighbouring word (so a clip of
/// it never starts or ends inside another word).
std::optional<MediaRange> mediaRangeWithHandles(const SpeechCorpus& corpus, const WordSpan& span, Ticks handle);

struct TakeQuality {
    double match = 1.0;           ///< 0..1: how well it says what it should (alignment quality)
    std::size_t fillers = 0;
    std::size_t hesitations = 0;  ///< pauses >= CorpusOptions::hesitation inside it
    std::size_t falseStarts = 0;  ///< repeated words at its start ("so so we", "I- I think")
    bool startsSentence = false;
    bool endsSentence = false;
    std::optional<double> rmsDb;
    std::optional<double> peakDb;
    std::optional<double> noiseDb; ///< the quietest pause around it
    bool clipped = false;
    double wordsPerSecond = 0.0;
    double confidence = 0.0;      ///< mean ASR confidence
    double score = 0.0;           ///< 0..1, combined (see takeScore)
    std::vector<std::string> reasons;
};

/// Measures a span (no audio yet: see measureAudio). `match` is the caller's alignment quality.
TakeQuality assessSpan(const SpeechCorpus& corpus, const WordSpan& span, double match, const CorpusOptions& options = {});

/// Adds level, peak and noise floor to `quality` for `span` (media spans only; sequence spans through their clip).
void measureAudio(const SpeechCorpus& corpus, const WordSpan& span, const TakeAudioProbe& probe, TakeQuality& quality);
/// The ranges measureAudio() will ask the probe for (to prefetch them in one pass).
std::vector<edit::AudioEnergyProbe::Range> audioRanges(const SpeechCorpus& corpus, const WordSpan& span);

/// The combined score: 0.40 match + 0.20 clean (fillers, hesitations, false starts) + 0.15 audio (level against
/// `medianRmsDb`, signal over the noise floor, clipping) + 0.15 complete sentences + 0.05 confidence + 0.05 pace (close
/// to the source's usual pace). Sets quality.score and quality.reasons.
void scoreTake(TakeQuality& quality, std::optional<double> medianRmsDb, double usualWordsPerSecond);

/// Units [begin, end) of a source -> the words they cover (first, last).
WordSpan wordsOfUnits(const SpeechSource& source, std::size_t sourceIndex, std::size_t beginUnit, std::size_t endUnit);

} // namespace wabcut::ai::roughcut
