// Sources and citations (0.7, card 177): the list and its text forms. See SourceList.h.

#include <wabcut/command/SourceList.h>

#include <wabcut/model/Plural.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace wabcut::command {

namespace {

std::string lowerOf(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// `text` with a closing full stop (not doubled after ? ! or .).
std::string sentence(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    if (text.empty()) {
        return text;
    }
    const char last = text.back();
    if (last != '.' && last != '?' && last != '!') {
        text += '.';
    }
    return text;
}

constexpr std::array<const char*, 12> kMonths{"January", "February", "March",     "April",   "May",      "June",
                                              "July",    "August",   "September", "October", "November", "December"};

bool digits(std::string_view text, std::size_t from, std::size_t count) {
    if (from + count > text.size()) {
        return false;
    }
    for (std::size_t i = from; i < from + count; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    return true;
}

/// "(2024, February 1)" from "2024-02-01", "(2024)" from "2024", "(March 2024)" from "2024-03"; anything else as written
/// in brackets; "(n.d.)" for nothing.
std::string apaDate(std::string_view date) {
    if (date.empty()) {
        return "(n.d.)";
    }
    if (digits(date, 0, 4)) {
        const std::string year(date.substr(0, 4));
        if (date.size() == 4) {
            return "(" + year + ")";
        }
        if (date.size() >= 7 && date[4] == '-' && digits(date, 5, 2)) {
            const int month = std::stoi(std::string(date.substr(5, 2)));
            if (month >= 1 && month <= 12) {
                if (date.size() >= 10 && date[7] == '-' && digits(date, 8, 2)) {
                    return "(" + year + ", " + kMonths[static_cast<std::size_t>(month - 1)] + " " +
                           std::to_string(std::stoi(std::string(date.substr(8, 2)))) + ")";
                }
                return "(" + year + ", " + kMonths[static_cast<std::size_t>(month - 1)] + ")";
            }
        }
    }
    return "(" + std::string(date) + ")";
}

std::string address(const SourceEntry& e) { return !e.url.empty() ? e.url : e.citation.archive; }

/// "Title, Author, Publisher, date" with the parts that exist.
std::string reference(const SourceEntry& e) {
    std::string out;
    for (const std::string* part : {&e.citation.title, &e.citation.author, &e.citation.publisher, &e.citation.date}) {
        if (!part->empty()) {
            out += (out.empty() ? "" : ", ") + *part;
        }
    }
    return out;
}

std::string apaEntry(const SourceEntry& e, bool markdown) {
    const std::string title = e.citation.title.empty() ? address(e) : e.citation.title;
    const std::string shownTitle = markdown && !e.citation.title.empty() ? "*" + title + "*" : title;
    std::string out;
    if (!e.citation.author.empty()) {
        out = sentence(e.citation.author) + " " + apaDate(e.citation.date) + ". " + sentence(shownTitle);
    } else {
        out = sentence(shownTitle) + " " + apaDate(e.citation.date) + ".";
    }
    if (!e.citation.publisher.empty()) {
        out += " " + sentence(e.citation.publisher);
    }
    if (const std::string where = address(e); !where.empty() && where != title) {
        out += " " + where;
    }
    return out;
}

std::string sortKey(const SourceEntry& e) {
    return lowerOf(!e.citation.author.empty() ? e.citation.author : (e.citation.title.empty() ? address(e) : e.citation.title));
}

} // namespace

std::vector<SourceEntry> sourceEntries(const Sequence& sequence) {
    std::vector<SourceEntry> out;
    for (std::size_t i = 0; i < sequence.markers.size(); ++i) {
        const Marker& m = sequence.markers[i];
        if (m.type != MarkerType::Source) {
            continue;
        }
        SourceEntry e;
        e.marker = i;
        e.start = m.start;
        e.duration = m.duration;
        e.claim = m.name;
        e.note = m.comment;
        e.url = m.url;
        if (m.citation) {
            e.citation = *m.citation;
        }
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const SourceEntry& a, const SourceEntry& b) { return a.start < b.start; });
    int number = 1;
    for (SourceEntry& e : out) {
        e.number = number++;
    }
    return out;
}

std::optional<SourceFormat> sourceFormatFromName(std::string_view name) {
    const std::string lower = lowerOf(name);
    if (lower == "youtube") {
        return SourceFormat::YouTube;
    }
    if (lower == "plain" || lower == "text") {
        return SourceFormat::Plain;
    }
    if (lower == "apa") {
        return SourceFormat::Apa;
    }
    if (lower == "markdown" || lower == "md") {
        return SourceFormat::Markdown;
    }
    return std::nullopt;
}

std::string_view sourceFormatName(SourceFormat format) {
    switch (format) {
    case SourceFormat::YouTube:
        return "youtube";
    case SourceFormat::Plain:
        return "plain";
    case SourceFormat::Apa:
        return "apa";
    case SourceFormat::Markdown:
        return "markdown";
    }
    return "plain";
}

std::string clockText(Ticks t) {
    const std::int64_t total = std::max<std::int64_t>(0, static_cast<std::int64_t>(t.toSeconds()));
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total / 60) % 60;
    const std::int64_t seconds = total % 60;
    const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    return hours > 0 ? std::to_string(hours) + ":" + two(minutes) + ":" + two(seconds) : std::to_string(minutes) + ":" + two(seconds);
}

std::string citationCaption(const SourceEntry& e) {
    std::string out = "Source: " + (e.citation.title.empty() ? address(e) : e.citation.title);
    if (!e.citation.author.empty()) {
        out += " - " + e.citation.author;
    }
    return out;
}

SourceListText formatSources(std::span<const SourceEntry> entries, SourceFormat format, const SourceListOptions& options) {
    SourceListText out;
    std::vector<const SourceEntry*> shown;
    std::string pending;
    for (const SourceEntry& e : entries) {
        if (e.needed()) {
            ++out.uncited;
            pending += (pending.empty() ? "" : "; ") + clockText(e.start) + " " + (e.claim.empty() ? std::string("(no claim written)") : e.claim);
            if (!options.includeUncited) {
                continue;
            }
        } else {
            ++out.cited;
        }
        shown.push_back(&e);
    }
    if (out.uncited > 0) {
        out.warnings.push_back(counted(out.uncited, "claim") + (out.uncited == 1 ? " still needs a source: " : " still need a source: ") + pending);
    }
    if (shown.empty()) {
        return out;
    }
    const auto claimOf = [](const SourceEntry& e) { return e.claim.empty() ? std::string("(no claim written)") : e.claim; };
    std::string text;
    switch (format) {
    case SourceFormat::YouTube: {
        text = "Sources\n";
        for (const SourceEntry* e : shown) {
            text += clockText(e->start) + " " + claimOf(*e);
            if (e->needed() && !e->cited()) {
                text += " [source needed]\n";
                continue;
            }
            if (const std::string ref = reference(*e); !ref.empty()) {
                text += " - " + ref;
            }
            if (const std::string where = address(*e); !where.empty() && where != e->citation.title) {
                text += " " + where;
            }
            text += "\n";
        }
        break;
    }
    case SourceFormat::Plain:
    case SourceFormat::Markdown: {
        const bool md = format == SourceFormat::Markdown;
        if (md) {
            text = "# Sources\n\n";
        }
        int n = 1;
        for (const SourceEntry* e : shown) {
            text += std::to_string(n++) + ". " + (md ? "**" + clockText(e->start) + "** " : "[" + clockText(e->start) + "] ") + claimOf(*e);
            if (e->needed() && !e->cited()) {
                text += " [source needed]\n";
                continue;
            }
            text += md ? " - " : " - ";
            std::string ref = reference(*e);
            if (md && !e->url.empty()) {
                const std::string label = e->citation.title.empty() ? e->url : e->citation.title;
                ref = "[" + label + "](" + e->url + ")";
                for (const std::string* part : {&e->citation.author, &e->citation.publisher, &e->citation.date}) {
                    if (!part->empty()) {
                        ref += ", " + *part;
                    }
                }
            } else if (!md && !e->url.empty()) {
                ref += (ref.empty() ? "" : ". ") + e->url;
            }
            text += ref;
            if (!e->citation.archive.empty()) {
                text += md ? " ([archive](" + e->citation.archive + "))" : " (archive: " + e->citation.archive + ")";
            }
            text += "\n";
        }
        break;
    }
    case SourceFormat::Apa: {
        // One entry per distinct source, alphabetical.
        std::vector<std::pair<std::string, std::string>> lines;
        for (const SourceEntry* e : shown) {
            if (!e->cited()) {
                continue;
            }
            lines.emplace_back(sortKey(*e), apaEntry(*e, false));
        }
        std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::set<std::string> seen;
        text = "References\n\n";
        for (const auto& [key, line] : lines) {
            if (seen.insert(line).second) {
                text += line + "\n";
            }
        }
        if (seen.empty()) {
            text.clear();
        }
        break;
    }
    }
    out.text = std::move(text);
    return out;
}

} // namespace wabcut::command
