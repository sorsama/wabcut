#pragma once

// p02 audio (docs/audio.md "Profanity"): Premiere's Speech to Text "Hide profanity": profane words of transcripts and
// captions shown as their first letter and asterisks ("f***"), and (Wabcut) the same words bleeped or muted in the
// sound (audio.bleepProfanity, through source-anchored audio redactions).
//
// The built-in lists are English and Thai, editable per request: words added (a trailing '*' matches every word that
// starts with it: "fuck*" catches "fucking") and words allowed. English words match case-insensitively, whole, with
// the punctuation around them ignored. Thai has no spaces between words: a Thai entry matches a whole transcript word,
// or with a trailing '*' anywhere inside one (only for entries that do not occur inside ordinary words: "หี" is in
// "หีบ", so it matches whole words only).

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::edit {

class ProfanityFilter {
public:
    /// The built-in English and Thai lists, then `extra` words added and `allowed` words removed (both as the lists
    /// write them; bounded: at most kMaxWords of at most kMaxWordBytes each are taken).
    explicit ProfanityFilter(std::span<const std::string> extra = {}, std::span<const std::string> allowed = {});

    static constexpr std::size_t kMaxWords = 2000;
    static constexpr std::size_t kMaxWordBytes = 64;

    /// The built-in entries of "en" or "th" (empty for other languages).
    static std::span<const std::string_view> builtin(std::string_view language);

    /// Whether one word (a transcript word, punctuation and all) is profane.
    bool isProfane(std::string_view word) const;
    /// `word` with every letter after its first replaced by '*' when it is profane (a profane part of a Thai word only),
    /// punctuation around it kept; otherwise `word` as it is.
    std::string mask(std::string_view word) const;
    /// Every profane word of `text` masked: space-separated words one by one, and the '*' Thai entries inside unspaced
    /// Thai runs.
    std::string maskText(std::string_view text) const;
    std::size_t size() const { return m_entries.size(); }

private:
    struct Entry {
        std::string word; ///< lowercased (ASCII), without the trailing '*'
        bool prefix = false;
        bool thai = false;
    };
    /// Where `word`'s profane part is: [begin, end) bytes, or begin == end when it is clean.
    std::pair<std::size_t, std::size_t> find(std::string_view word) const;

    std::vector<Entry> m_entries;
};

} // namespace wabcut::ai::edit
