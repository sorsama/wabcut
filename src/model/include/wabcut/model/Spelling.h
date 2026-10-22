#pragma once

// Spell checking (0.2 graphics, Premiere's spell check in captions and text): misspelled words of caption cues and
// graphic text layers, with suggestions, Add to Dictionary and Ignore.
//
//   * SpellChecker is the host's checker (the app and wabcut-cli: the operating system's, editor/SystemSpellChecker.h:
//     Windows' spell checking API; none elsewhere). It finds misspellings in byte ranges of UTF-8 text in a language
//     (BCP 47 or ISO 639-1, "" = the user's) and suggests corrections.
//   * PersonalSpelling is the user's side over a checker: words added to the dictionary (Wabcut's own word list, one
//     word per line in a UTF-8 file the host keeps with the user, never the operating system's dictionary) and words
//     ignored until the app quits. Added and ignored words are matched case-insensitively (Unicode simple folding).
//   * WordListSpellChecker checks against a fixed word list (tests, and hosts without a system checker).
//   * spellingOf() lists the misspellings of a caption track's cues or a graphic's text layers.
//
// Nothing here changes the project; corrections are ordinary edits (captions.updateCue, graphic.setText).

#include <wabcut/model/Graphics.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

struct CaptionTrack;

inline constexpr std::size_t kMaxSpellSuggestions = 8;
inline constexpr std::size_t kMaxPersonalWords = 10'000;
inline constexpr std::size_t kMaxSpellWordBytes = 256;

/// What the checker advises for a misspelling: pick a suggestion, Replace it with suggestions.front() (a known
/// autocorrection, "teh" -> "the"), or Delete it (a repeated word, "the the").
enum class SpellAction : std::uint8_t { Suggest, Replace, Delete };

struct SpellIssue {
    std::size_t start = 0; ///< byte range of the checked UTF-8 text
    std::size_t end = 0;
    std::string word;
    SpellAction action = SpellAction::Suggest;
    /// Best first, at most kMaxSpellSuggestions; filled by check() only when the checker gives them with the issue
    /// (Replace); otherwise ask suggest().
    std::vector<std::string> suggestions;

    bool operator==(const SpellIssue&) const = default;
};

class SpellChecker {
public:
    virtual ~SpellChecker() = default;
    /// Whether `language` can be checked ("" = the user's language).
    virtual bool supports(std::string_view language) const = 0;
    /// The misspelled words of `text`, left to right. A language that cannot be checked finds nothing.
    virtual std::vector<SpellIssue> check(std::string_view text, std::string_view language) const = 0;
    /// Corrections for `word`, best first, at most kMaxSpellSuggestions.
    virtual std::vector<std::string> suggest(std::string_view word, std::string_view language) const = 0;
};

/// A checker that knows nothing (hosts without a spell checker): supports no language.
const SpellChecker& noSpellChecker();

/// The words of `text` a word-list checker looks at: runs of letters, digits, marks and inner apostrophes ("don't";
/// U+2019 too), split at spaces, punctuation and digits-only runs are skipped. Thai and other unspaced scripts come out
/// as whole runs between spaces.
std::vector<std::pair<std::size_t, std::size_t>> spellWords(std::string_view text);

/// Checks against a word list, case-insensitively (a capitalised word is right when its lower-case form is listed).
/// Suggestions: listed words one or two edits away, nearest first. Every language uses the same list unless
/// `languages` names the ones it supports.
class WordListSpellChecker final : public SpellChecker {
public:
    explicit WordListSpellChecker(std::vector<std::string> words, std::vector<std::string> languages = {});
    bool supports(std::string_view language) const override;
    std::vector<SpellIssue> check(std::string_view text, std::string_view language) const override;
    std::vector<std::string> suggest(std::string_view word, std::string_view language) const override;

private:
    std::set<std::string> m_words; // folded
    std::vector<std::string> m_original;
    std::vector<std::string> m_languages;
};

/// The user's dictionary and ignored words over a checker. Thread-safe.
class PersonalSpelling final : public SpellChecker {
public:
    /// `dictionary` empty: added words are kept in memory only. The file is read now (a file that does not read or is
    /// too large leaves the list empty and is not written over until a word is added).
    explicit PersonalSpelling(std::shared_ptr<const SpellChecker> base, std::filesystem::path dictionary = {});

    bool supports(std::string_view language) const override;
    std::vector<SpellIssue> check(std::string_view text, std::string_view language) const override;
    std::vector<std::string> suggest(std::string_view word, std::string_view language) const override;

    /// Add to Dictionary: the word is right from now on, saved to the dictionary file. False: empty, too long, holds
    /// spaces or control characters, the list is full, or the file could not be written (the word is still added for
    /// this session).
    bool addWord(std::string_view word);
    /// Removes an added word; false when it was not there.
    bool removeWord(std::string_view word);
    /// Ignore All: the word is right until the app quits (not saved).
    void ignoreWord(std::string_view word);
    /// The added words, in the order they were added.
    std::vector<std::string> words() const;
    /// Whether the word was added or ignored.
    bool accepted(std::string_view word) const;
    const SpellChecker& base() const { return *m_base; }
    /// Replaces the checker underneath (tests; the app when the system checker becomes available).
    void setBase(std::shared_ptr<const SpellChecker> base);

private:
    bool saveLocked() const;

    mutable std::mutex m_mutex;
    std::shared_ptr<const SpellChecker> m_base;
    std::filesystem::path m_file;
    std::vector<std::string> m_words;  // as added
    std::set<std::string> m_accepted;  // folded: added and ignored
};

/// A misspelling in a caption cue or a graphic's text layer.
struct TextSpellIssue {
    std::size_t cue = 0;       ///< caption cue index (captions)
    std::uint32_t layer = 0;   ///< text layer id (graphics)
    SpellIssue issue;

    bool operator==(const TextSpellIssue&) const = default;
};

/// The misspellings of every cue of `track` in `language` ("" = the track's language, else the user's); with
/// `suggestions`, each issue gets the checker's suggestions. At most `limit` issues.
std::vector<TextSpellIssue> spellingOf(const CaptionTrack& track, const SpellChecker& checker, std::string_view language = {},
                                       bool suggestions = false, std::size_t limit = 10'000);
/// The misspellings of every text layer of `graphic` (groups included, bottom to top), each in its layer's language.
std::vector<TextSpellIssue> spellingOf(const Graphic& graphic, const SpellChecker& checker, std::string_view language = {},
                                       bool suggestions = false, std::size_t limit = 10'000);

std::string_view toString(SpellAction action);

} // namespace wabcut
