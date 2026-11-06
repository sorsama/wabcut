#pragma once

// Sources and citations (0.7, card 177): the claims of a sequence that cite a source, as a list by time and as text to
// publish - a YouTube description block with timestamps, a numbered list, an APA-style bibliography, Markdown. A source is a
// marker of type Source (Sequence.h MarkerCitation): the marker's name is the claim, its url the source's address.

#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::command {

struct SourceEntry {
    std::size_t marker = 0; ///< index into Sequence::markers
    int number = 1;         ///< 1-based, in time order
    Ticks start;
    Ticks duration;
    std::string claim;      ///< the marker's name
    std::string note;       ///< the marker's comment
    std::string url;
    MarkerCitation citation;

    /// There is something to cite: a title or an address.
    bool cited() const { return !citation.title.empty() || !url.empty(); }
    /// A claim that still wants a source: marked so, or with nothing to cite.
    bool needed() const { return citation.needed || !cited(); }
};

/// The sequence's Source markers in time order (equal times keep their marker order).
std::vector<SourceEntry> sourceEntries(const Sequence& sequence);

enum class SourceFormat { YouTube, Plain, Apa, Markdown };

std::optional<SourceFormat> sourceFormatFromName(std::string_view name);
std::string_view sourceFormatName(SourceFormat format);

struct SourceListOptions {
    /// List the claims that still want a source too, marked "[source needed]" (else they are only counted and named in
    /// the warnings).
    bool includeUncited = false;
};

struct SourceListText {
    std::string text;                  ///< the list; empty when nothing is cited
    int cited = 0;                     ///< claims with a source in the text
    int uncited = 0;                   ///< claims that still want one
    std::vector<std::string> warnings; ///< "2 claims still need a source: 0:12 ..."
};

/// The list in `format`. YouTube: "Sources" and a line per cited claim, "m:ss claim - title, author (publisher, date)
/// address", ready to paste into a description; Plain: a numbered list by time; Markdown: the same with links; Apa: the
/// bibliography - one entry per distinct source (the same source cited twice once), alphabetical, "Author. (2024, February 1).
/// Title. Publisher. https://address".
SourceListText formatSources(std::span<const SourceEntry> entries, SourceFormat format, const SourceListOptions& options = {});

/// "0:42", "1:02:03" (hours only when there are some): the form YouTube reads as a timestamp.
std::string clockText(Ticks t);

/// The lower-third text of a citation: "Source: Title - Author" (the title or the address, then the author).
std::string citationCaption(const SourceEntry& entry);

} // namespace wabcut::command
