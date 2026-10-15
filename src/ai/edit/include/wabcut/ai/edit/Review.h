#pragma once

// The transcript review: filler words and silences found in a SequenceTranscript, as a list the UI shows with a
// keep/remove choice per item and "remove all". Removing goes through planDelete (TextEdits.h): fillers as word
// ranges with the usual padding, silences as pauses that keep some room tone on both sides.
//
// Fillers come from a per-language lexicon (data/fillers.<language>.txt, embedded at build time; FillerLexicon::parse
// reads more). Context-dependent fillers ("like", "you know", Thai "แบบ") only count in disfluent context: a pause
// next to them, a comma after them, or said twice. Thai and other unspaced languages match entries against
// consecutive word units joined without spaces.
//
// Silences are pauses between consecutive words longer than a threshold, checked against the audio when an
// AudioEnergyProbe is given (a pause with music or noise in it is reported with low confidence).

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/edit/TextEdits.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::edit {

struct FillerLexicon {
    struct Entry {
        std::string text;                ///< as written in the lexicon
        std::vector<std::string> words;  ///< normalised; one element for unspaced languages
        bool contextual = false;         ///< a filler only in disfluent context
    };

    std::string language;
    std::vector<Entry> entries;

    /// Parses the lexicon format (see data/fillers.en.txt). Errors: InvalidArgument for a bad line.
    static std::expected<FillerLexicon, Error> parse(std::string_view language, std::string_view text);
    /// The built-in lexicon of a language, or null.
    static const FillerLexicon* builtin(std::string_view language);
    static std::vector<std::string> builtinLanguages();
};

/// Measures how loud a stretch of a media audio stream is.
class AudioEnergyProbe {
public:
    struct Range {
        const MediaItem* media = nullptr;
        int audioStream = 0;
        TimeRange source;
    };

    virtual ~AudioEnergyProbe() = default;
    /// RMS level of `source` (media time) in dBFS (-inf for digital silence), or nullopt when it cannot be read.
    virtual std::optional<double> rmsDb(const MediaItem& media, int audioStream, TimeRange source) const = 0;
    /// Every range review() is about to ask rmsDb() for, first: a probe may measure them in one pass per media file
    /// (ServiceEnergyProbe does, with AudioEnergyService). The default does nothing.
    virtual void prefetch(std::span<const Range> ranges) const { (void)ranges; }
};

enum class ReviewKind { Filler, Silence };

std::string_view toString(ReviewKind kind);

struct ReviewItem {
    ReviewKind kind = ReviewKind::Filler;
    TimeRange range;              ///< on the timeline: the filler's words, or the pause between two words
    std::size_t firstWord = 0;    ///< filler: its words; silence: the word before the pause
    std::size_t lastWord = 0;     ///< filler: its last word; silence: the word after the pause
    float confidence = 0.0f;      ///< 0..1: how sure it should go
    std::string label;            ///< the filler as said, or "1.4 s pause"
    std::string context;          ///< "...so we went [um] to the..." / "...to the [1.4 s] store..."
    std::optional<double> rmsDb;  ///< silence: the loudest level measured in the pause
};

struct ReviewOptions {
    bool fillers = true;
    /// Pauses at least this long are silences; nullopt = do not look for silences.
    std::optional<Ticks> silenceThreshold = Ticks{kTicksPerSecond * 8 / 10};
    /// A pause louder than this (dBFS) has sound in it (music, breathing, noise): low confidence.
    double silenceDb = -40.0;
    /// A pause at least this long next to a contextual filler makes it disfluent.
    Ticks disfluentPause = Ticks{kTicksPerSecond * 15 / 100};
    /// Lexicons to use instead of / in addition to the built-in ones, by language.
    std::vector<FillerLexicon> lexicons;
};

/// The review list in timeline order. `probe` may be null (no energy check).
std::vector<ReviewItem> review(const SequenceTranscript& transcript, const ProjectState& state,
                               const ReviewOptions& options = {}, const AudioEnergyProbe* probe = nullptr);

/// The deletion that removes `items` (fillers as words, silences as pauses with room tone).
DeleteRequest deleteRequestFor(std::span<const ReviewItem> items, const CutOptions& options = {});

} // namespace wabcut::ai::edit
