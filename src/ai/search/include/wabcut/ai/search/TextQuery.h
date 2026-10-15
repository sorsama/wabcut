#pragma once

// The transcript side of smart search: what a query looks for in the words people say.
//
// Phrases are indexed in an SQLite FTS5 table with the trigram tokenizer, which matches any substring of three or
// more characters. That makes every script searchable without a word segmenter, Thai included (written without
// spaces between words): a phrase is indexed as its words joined by spaces and, for languages written without
// spaces, also joined without them, so "แมววิ่ง" finds the words แมว + วิ่ง. Matching is case-insensitive.
//
// A term of one or two characters cannot use a trigram index. Such terms are matched with LIKE '%term%' over the same
// rows, and only when the query has no longer term (a short word next to a long one would match nearly everything):
// the documented fallback for short Thai words such as รถ.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::search {

inline constexpr std::size_t kMaxQueryTerms = 16;
inline constexpr std::size_t kMaxTermCodePoints = 64;

struct TextQuery {
    std::vector<std::string> terms;      ///< lowercased, in query order, without duplicates
    std::string match;                   ///< FTS5 MATCH expression over the terms of 3+ characters; "" = none
    std::vector<std::string> likeTerms;  ///< when no term has 3+ characters: LIKE patterns ('%' + escaped + '%')

    bool empty() const { return match.empty() && likeTerms.empty(); }
};

/// Splits `query` into terms at whitespace and punctuation (a "quoted phrase" stays one term), lowercases them and
/// builds the FTS5 expression: every term a quoted trigram string (a substring match), joined with OR, so phrases
/// that contain more of the terms rank higher (bm25) while a partial match still counts.
TextQuery buildTextQuery(std::string_view query);

/// `text` for a LIKE pattern with ESCAPE '\': %, _ and \ escaped.
std::string escapeLike(std::string_view text);

/// Number of code points of UTF-8 text (an invalid byte counts as one).
std::size_t codePoints(std::string_view text);

/// Whether a language is written without spaces between words (Thai, Lao, Khmer, Burmese, Chinese, Japanese).
bool writtenWithoutSpaces(std::string_view language);

/// Indices of the words that contain one of the query's terms (case-insensitive), or that a term spans when it runs
/// over several words joined without spaces ("แมววิ่ง" marks แมว and วิ่ง).
std::vector<std::size_t> matchedWords(std::span<const std::string> words, const TextQuery& query);

} // namespace wabcut::ai::search
