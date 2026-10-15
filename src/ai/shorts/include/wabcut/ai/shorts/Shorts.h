#pragma once

// The shorts finder (AI-9, docs/ai.md "Shorts finder"): the 20-90 s moments of a long sequence (a podcast, an
// interview, a talk) that stand on their own as vertical shorts, scored locally and explained.
//
//   1. The transcript is split into sentences (punctuation; for Thai and unpunctuated speech, pauses and speaker
//      changes) and topic boundaries are found TextTiling-style: where the vocabulary of the sentences before and after
//      stops overlapping (TF-IDF cosine, or a text embedder when the host has one).
//   2. Every run of whole sentences of the right length is a candidate. It is scored on what makes a short work:
//        structure  starts at a topic's start and ends at its end (self-contained)
//        hook       a question, a hook phrase or dense speech in its first 3 seconds
//        topic      one topic throughout (no boundary inside), its sentences alike
//        energy     livelier than the sequence's average; bursts in pauses (laughter, applause)
//        visual     faces on screen, shot rhythm, motion (cached subject analyses)
//        clean      few fillers and silences (transcript review)
//        length     close to the target length
//        query      matches "about the pricing discussion", when asked
//      Text features rank all candidates; the media probes (energy, visual) refine only the best ones, so a 60-minute
//      transcript takes well under a second.
//   3. The best non-overlapping ones are kept, snapped outwards to the nearest cut (clip or shot boundary) within half
//      a second, else padded around their words, and given a title (their most telling sentence).
//
// An optional cloud re-rank (Rerank.h) refines the order and titles from the transcript text only.

#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/vision/Subjects.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::ai::shorts {

struct Sentence {
    std::size_t first = 0; ///< word indices in the SequenceTranscript, inclusive
    std::size_t last = 0;
    TimeRange range;       ///< timeline: first word's start to last word's end
    std::string text;
    std::optional<int> speaker;
};

/// The transcript as sentences. A sentence ends at . ? ! (and their Thai / CJK forms), at a pause of `pause` or a
/// speaker change, and is split at the longest pause inside once it reaches `maxLength`.
std::vector<Sentence> splitSentences(const edit::SequenceTranscript& transcript, Ticks pause = Ticks{kTicksPerSecond * 7 / 10},
                                     Ticks maxLength = Ticks{kTicksPerSecond * 15});

/// TF-IDF vectors over a set of documents (sentences); words lowercased, punctuation and stop words dropped.
class TextVectors {
public:
    explicit TextVectors(std::span<const std::string> documents);
    /// The (L2-normalised, sparse) vector of a text in this vocabulary.
    std::vector<std::pair<std::uint32_t, float>> vectorOf(std::string_view text) const;
    /// Of document i.
    const std::vector<std::pair<std::uint32_t, float>>& document(std::size_t i) const { return m_documents[i]; }
    /// The weight of each word of `text` (its TF-IDF), for picking a telling sentence.
    double informativeness(std::string_view text) const;
    std::size_t size() const { return m_documents.size(); }

    static float cosine(const std::vector<std::pair<std::uint32_t, float>>& a, const std::vector<std::pair<std::uint32_t, float>>& b);
    /// Lowercased word tokens without punctuation or stop words.
    static std::vector<std::string> tokens(std::string_view text);

private:
    std::map<std::string, std::uint32_t, std::less<>> m_ids;
    std::vector<double> m_idf;
    std::vector<std::vector<std::pair<std::uint32_t, float>>> m_documents;
};

struct Topics {
    std::vector<double> cohesion; ///< [i]: similarity of the sentences before i and from i (1 for i = 0)
    std::vector<bool> starts;     ///< [i]: a new topic starts at sentence i (true for i = 0)
};

/// TextTiling over sentence vectors: blocks of `window` sentences each side of every gap, boundaries at the deepest
/// valleys of cohesion (depth over mean - sigma / 2). `vectors` holds one vector per sentence.
Topics segmentTopics(std::span<const std::vector<std::pair<std::uint32_t, float>>> vectors, std::size_t window = 3);

struct ShortsOptions {
    double minSeconds = 20.0;
    double maxSeconds = 90.0;
    double targetSeconds = 45.0;
    std::size_t count = 5;
    std::string query;            ///< what the shorts should be about ("" = anything)
    double padding = 0.15;        ///< seconds around the words when no cut is near
    double snap = 0.5;            ///< seconds within which a cut is used
    double maxOverlap = 0.2;      ///< of the shorter one, between two picked shorts
    std::size_t refine = 40;      ///< candidates the media probes (energy, visual) look at
};

/// Each 0..1; what the score is made of.
struct ScoreParts {
    double structure = 0.0;
    double hook = 0.0;
    double topic = 0.0;
    double energy = 0.5; ///< 0.5 = unknown / average
    double visual = 0.5;
    double clean = 0.0;
    double length = 0.0;
    double query = 0.0;
};

struct ShortCandidate {
    TimeRange range;          ///< on the timeline, snapped (frame-aligned)
    std::size_t firstSentence = 0;
    std::size_t lastSentence = 0;
    std::size_t firstWord = 0;
    std::size_t lastWord = 0;
    double score = 0.0;       ///< 0..1
    ScoreParts parts;
    std::vector<std::string> reasons; ///< what stood out, in words
    std::string title;
    std::string hook;         ///< the opening sentence
    std::string text;         ///< the words (truncated)
    std::optional<double> cloudScore; ///< 0..1, after a re-rank
    std::string cloudReason;
};

/// What else the finder may look at (all optional).
struct ShortsSignals {
    const edit::AudioEnergyProbe* energy = nullptr;
    /// Subject analyses of the media (cached ones; null = none).
    std::function<const vision::SubjectAnalysis*(const MediaItem& media, int videoStream)> subjects;
    /// Sentence vectors from a text embedder instead of TF-IDF (dense, normalised); null = TF-IDF.
    std::function<std::optional<std::vector<float>>(const std::string& text)> embed;
};

struct ShortsReport {
    std::vector<ShortCandidate> shorts; ///< best first
    std::size_t sentences = 0;
    std::size_t topics = 0;
    std::size_t candidates = 0;
    double milliseconds = 0.0;
    std::vector<std::string> warnings;
};

/// Finds shorts in a sequence. `review` is the transcript review (fillers and silences); may be empty.
ShortsReport findShorts(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& transcript,
                        std::span<const edit::ReviewItem> review, const ShortsSignals& inputs, const ShortsOptions& options);

/// The cut points of a sequence (clip boundaries on video tracks, and shot cuts from `inputs.subjects`), sorted.
std::vector<Ticks> cutPoints(const ProjectState& state, SequenceId sequence, const ShortsSignals& inputs);

/// A title from a sentence: fillers and trailing punctuation dropped, at most `maxChars` (whole words), capitalised.
std::string titleFrom(std::string_view sentence, std::size_t maxChars = 60);

} // namespace wabcut::ai::shorts
