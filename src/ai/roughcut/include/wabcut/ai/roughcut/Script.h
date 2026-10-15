#pragma once

// Scripts and outlines as beats (AI-11, "paste your script, get a first cut"). A beat is one thing the edit should say,
// in order: a script line, a sentence of a paragraph, or an outline bullet. parseScript() reads plain text and the
// Markdown people paste from docs and notes apps:
//
//   # Title / ## Section     the first heading names the script; every heading starts a section (a sequence marker)
//   - bullet, * bullet, 1.    an outline beat (loose wording: matched by meaning as well as words)
//   NAME: line                a speaker label is dropped from what is matched and kept as a hint
//   [B-roll: drone shot]      a visual cue (a whole line, or inline): a B-roll query for the beat it belongs to;
//   (cut to the product)      bracketed stage directions are never matched as speech
//   **bold**, _italic_, `code`, > quotes   decoration is dropped
//
// Prose lines with several sentences become one beat per sentence (the take is chosen per sentence), unless asked to
// keep lines whole. Thai and other unspaced scripts are kept per line (they rarely mark sentence ends).

#include <wabcut/model/Error.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::roughcut {

struct ScriptBeat {
    std::size_t index = 0;            ///< 0-based, in script order
    std::string text;                 ///< what should be said (decoration, labels and cues removed)
    std::string section;              ///< the heading it is under ("" = none)
    std::string speaker;              ///< a speaker label, when the line had one
    std::vector<std::string> broll;   ///< visual cues for this beat ("drone shot of the coast")
    bool outline = false;             ///< a bullet: loose wording, matched by meaning too
    std::size_t line = 0;             ///< 1-based line of the input it came from

    bool operator==(const ScriptBeat&) const = default;
};

enum class ScriptSplit {
    Auto,      ///< bullets and short lines whole, prose split into sentences
    Lines,     ///< one beat per line
    Sentences, ///< every line split into sentences
};

struct ScriptOptions {
    ScriptSplit split = ScriptSplit::Auto;
    std::size_t maxBeats = 400;       ///< more is refused (InvalidArgument), so a pasted book cannot stall the editor
    std::size_t maxBytes = 256 * 1024;
};

struct Script {
    std::string title;                ///< the first heading, else ""
    std::vector<ScriptBeat> beats;
    std::string language;             ///< "th" when Thai dominates the letters, else "" (unknown / spaced)
    std::vector<std::string> warnings;
};

/// Parses a script or outline (UTF-8; invalid bytes are replaced). Errors: InvalidArgument when it is larger than
/// maxBytes or has more than maxBeats beats. A script with nothing to say gives no beats (not an error).
std::expected<Script, Error> parseScript(std::string_view text, const ScriptOptions& options = {});

/// The sentences of a line of prose: split after . ? ! … (and the CJK forms) followed by a space or the end;
/// fragments of fewer than three words join the sentence before them.
std::vector<std::string> splitProse(std::string_view line);

} // namespace wabcut::ai::roughcut
