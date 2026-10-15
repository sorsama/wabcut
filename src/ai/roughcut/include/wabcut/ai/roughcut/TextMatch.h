#pragma once

// Text matching for script-driven editing (AI-11, docs/ai.md "Creator superpowers"): how a line of a script is
// found in what was said, and how two attempts of the same sentence are compared. Pure, deterministic, no ICU.
//
//   units()         text -> matching units. Spaced scripts (Latin, Cyrillic, ...) give one unit per word: lowercased,
//                   punctuation dropped, apostrophes removed ("Don't" -> "dont"), small English numbers written as
//                   digits ("ten" -> "10"). Unspaced scripts (Thai, Lao, Khmer, Myanmar, CJK, kana) have no word
//                   boundaries a matcher could trust, so they give one unit per character cluster: a base character
//                   with its combining vowels and tone marks (Thai "ที่" is one unit). Mixed text switches per run;
//                   Thai digits become ASCII. Whitespace is never needed to split Thai.
//   unitSimilarity()  1 for equal units; for words, a Levenshtein ratio (counted from 0.75 up, words of 4+ letters);
//                   for clusters, 0.6 when only the tone/vowel marks differ (the commonest ASR slip in Thai).
//   align()         semi-global alignment of a query (a script beat) inside a target (transcript units): the whole
//                   query against any stretch of the target, with fuzzy matches, skipped query units (a word the
//                   speaker dropped) and skipped target units (a word they added). Target units marked as fillers
//                   are skipped for free, so "so um today" matches "so today" fully.
//   diffUnits()     the word-level difference between two attempts (LCS over units): equal / inserted / deleted runs.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::roughcut {

/// One matching unit of a text.
struct Unit {
    std::string text;         ///< normalised: a word, or a character cluster of an unspaced script
    std::uint32_t source = 0; ///< which input word it came from (the index passed to appendUnits)
    bool word = true;         ///< a word of a spaced script; false = a character cluster
    bool filler = false;      ///< the source word is a filler (skipped for free by align)

    bool operator==(const Unit&) const = default;
};

/// True for code points of scripts written without spaces between words (Thai, Lao, Khmer, Myanmar, CJK, kana).
bool isUnspacedCodePoint(char32_t c);

/// The units of `text`. Every unit gets `source` as its source index.
std::vector<Unit> units(std::string_view text, std::uint32_t source = 0);
/// Appends the units of `text` to `out` (source = `source`, filler = `filler`).
void appendUnits(std::vector<Unit>& out, std::string_view text, std::uint32_t source, bool filler = false);

/// The units of `text` joined into one normalised string (spaces between words, clusters joined): for display and keys.
std::string normalizedText(std::string_view text);

/// Similarity of two units in [0, 1] (see the file comment). Words below the fuzzy threshold give 0.
double unitSimilarity(const Unit& a, const Unit& b);

struct AlignOptions {
    double match = 2.0;        ///< per fully matching unit (scaled by the similarity for fuzzy ones)
    double mismatch = -1.0;    ///< a substitution
    double skipQuery = -1.0;   ///< a query unit the target does not have
    double skipTarget = -1.0;  ///< a target unit the query does not have (fillers: 0)
    double minSimilarity = 0.6; ///< below this two units do not match
};

struct Alignment {
    std::size_t begin = 0;      ///< target units [begin, end) the query aligned to
    std::size_t end = 0;
    double score = 0.0;         ///< the DP score (higher is better; <= 0 means nothing useful)
    double matched = 0.0;       ///< sum of the similarities of the matched pairs
    std::size_t queryMatched = 0; ///< query units matched (fuzzy included)
    std::size_t targetUnits = 0;  ///< non-filler target units in [begin, end)
    double coverage = 0.0;      ///< matched / query units: how much of the beat was said
    double precision = 0.0;     ///< matched / non-filler target units in the span: how little else was said
    /// The F1 of coverage and precision: the alignment's quality in [0, 1].
    double quality() const { return coverage + precision > 0.0 ? 2.0 * coverage * precision / (coverage + precision) : 0.0; }
    bool valid() const { return end > begin && queryMatched > 0; }
};

/// The best alignment of the whole `query` inside `target` (any stretch of it). O(|query| x |target|) time,
/// O(|target|) memory. An empty query or target gives an invalid alignment.
Alignment align(std::span<const Unit> query, std::span<const Unit> target, const AlignOptions& options = {});

/// One run of a diff between two unit sequences.
struct DiffRun {
    enum class Kind { Equal, Inserted, Deleted };
    Kind kind = Kind::Equal;
    std::size_t aBegin = 0; ///< units of `a` (Equal, Deleted)
    std::size_t aEnd = 0;
    std::size_t bBegin = 0; ///< units of `b` (Equal, Inserted)
    std::size_t bEnd = 0;

    bool operator==(const DiffRun&) const = default;
};

/// The shortest edit from `a` to `b` (an LCS over units; fuzzy-equal words count as equal), as runs in order.
/// A replaced word shows as Deleted then Inserted. Quadratic; meant for sentences (a few hundred units).
std::vector<DiffRun> diffUnits(std::span<const Unit> a, std::span<const Unit> b, double minSimilarity = 0.75);

/// A bag of features of a text for fast retrieval: words and word bigrams for spaced scripts, cluster bigrams and
/// trigrams for unspaced ones (fillers left out). Sorted by feature hash, each once; weights are term counts.
struct FeatureBag {
    std::vector<std::pair<std::uint64_t, float>> features;
};
FeatureBag featuresOf(std::span<const Unit> units);
/// Cosine of two bags (term counts as weights).
double cosine(const FeatureBag& a, const FeatureBag& b);

/// A 64-bit hash of a feature string (FNV-1a), stable across runs and platforms.
std::uint64_t featureHash(std::string_view text);

} // namespace wabcut::ai::roughcut
