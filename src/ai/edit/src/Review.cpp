#include <wabcut/ai/edit/Review.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/WordBuilder.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace wabcut::ai::edit {

namespace detail {
/// The embedded data/fillers.<language>.txt files: (language, text). Generated at build time.
std::span<const std::pair<std::string_view, std::string_view>> embeddedLexicons();
} // namespace detail

namespace {

/// Index of the first byte of the last UTF-8 code point of `s` (s non-empty).
std::size_t lastCodePoint(std::string_view s) {
    std::size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

bool isLeadingPunctuation(std::string_view s) {
    static constexpr std::string_view kAscii = "\"'([{";
    if (s.empty()) {
        return false;
    }
    if (s.starts_with("\xE2\x80\x9C") || s.starts_with("\xE2\x80\x98") || s.starts_with("\xC2\xAB") ||
        s.starts_with("\xC2\xBF") || s.starts_with("\xC2\xA1")) {
        return true; // opening quotes, guillemet, inverted question and exclamation marks
    }
    return kAscii.find(s.front()) != std::string_view::npos;
}

/// Lower-case ASCII, punctuation around the word removed.
std::string normalize(std::string_view word) {
    std::string_view s = text::trimAsciiSpace(word);
    while (!s.empty()) {
        const std::size_t last = lastCodePoint(s);
        const std::string_view tail = s.substr(last);
        if (text::isTrailingPunctuation(tail) || tail == "\xE2\x80\x9D" || tail == "\xE2\x80\x99" || tail == "\xC2\xBB" ||
            tail == "-") {
            s = s.substr(0, last);
        } else {
            break;
        }
    }
    while (!s.empty() && isLeadingPunctuation(s)) {
        const int n = text::utf8SequenceAt(s, 0);
        s.remove_prefix(n > 0 ? static_cast<std::size_t>(n) : 1);
    }
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c); });
    return out;
}

bool endsWithComma(std::string_view word) {
    return word.ends_with(",") || word.ends_with("\xEF\xBC\x8C") || word.ends_with("\xE3\x80\x81"); // , ， 、
}

std::string seconds(Ticks t) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond));
    return buf;
}

std::string snippet(const SequenceTranscript& st, std::size_t before, std::size_t after, const std::string& middle) {
    constexpr std::size_t kSpan = 4;
    std::string out;
    if (before != static_cast<std::size_t>(-1)) {
        const std::size_t from = before >= kSpan - 1 ? before - (kSpan - 1) : 0;
        out = (from > 0 ? "..." : "") + st.textOf(from, before) + " ";
    }
    out += "[" + middle + "]";
    if (after < st.size()) {
        const std::size_t to = std::min(after + kSpan - 1, st.size() - 1);
        out += " " + st.textOf(after, to) + (to + 1 < st.size() ? "..." : "");
    }
    return out;
}

/// The loudest level of the audio under `gap` on `tracks` (parts without a clip count as silence).
std::optional<double> loudest(const ProjectState& state, const Sequence& seq, TimeRange gap,
                              std::initializer_list<TrackId> tracks, const AudioEnergyProbe& probe) {
    std::optional<double> level;
    std::vector<TrackId> seen;
    for (const TrackId id : tracks) {
        const Track* track = seq.findTrack(id);
        if (!track || std::ranges::find(seen, id) != seen.end()) {
            continue;
        }
        seen.push_back(id);
        for (std::size_t i = track->firstClipAtOrAfter(gap.start) > 0 ? track->firstClipAtOrAfter(gap.start) - 1 : 0;
             i < track->clips.size() && track->clips[i].start() < gap.end(); ++i) {
            const Clip& c = track->clips[i];
            const Ticks a = std::max(c.start(), gap.start);
            const Ticks b = std::min(c.end(), gap.end());
            if (b <= a) {
                continue;
            }
            const MediaItem* media = state.findMedia(c.media);
            if (!media) {
                continue;
            }
            // The media under the stretch (any speed, direction or remap).
            if (const auto db = probe.rmsDb(*media, c.stream.index, c.shownSourceRange(a, b))) {
                level = level ? std::max(*level, *db) : *db;
            }
        }
    }
    return level;
}

/// Records what loudest() would measure, measuring nothing (for AudioEnergyProbe::prefetch).
class RecordingProbe final : public AudioEnergyProbe {
public:
    std::optional<double> rmsDb(const MediaItem& media, int audioStream, TimeRange source) const override {
        ranges.push_back(Range{&media, audioStream, source});
        return std::nullopt;
    }
    mutable std::vector<Range> ranges;
};

} // namespace

// --- Lexicons ----------------------------------------------------------------------------------------------

std::expected<FillerLexicon, Error> FillerLexicon::parse(std::string_view language, std::string_view text) {
    FillerLexicon lexicon;
    lexicon.language = std::string(language);
    const bool unspaced = wordSplitFor(language) == WordSplit::PerUnit;
    std::size_t lineNo = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = text::trimAsciiSpace(line);
        if (line.empty()) {
            continue;
        }
        Entry entry;
        if (const std::size_t bar = line.find('|'); bar != std::string_view::npos) {
            const std::string_view condition = text::trimAsciiSpace(line.substr(bar + 1));
            if (condition != "context") {
                return std::unexpected(Error{ErrorCode::InvalidArgument, "fillers." + lexicon.language + " line " +
                                                                             std::to_string(lineNo) +
                                                                             ": unknown condition \"" +
                                                                             std::string(condition) + "\""});
            }
            entry.contextual = true;
            line = text::trimAsciiSpace(line.substr(0, bar));
        }
        if (!text::isValidUtf8(line)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "fillers." + lexicon.language + " line " + std::to_string(lineNo) + ": not UTF-8"});
        }
        entry.text = std::string(line);
        if (unspaced) {
            std::string joined;
            for (const char c : line) {
                if (c != ' ') {
                    joined += c;
                }
            }
            entry.words.push_back(normalize(joined));
        } else {
            std::string_view rest = line;
            while (!rest.empty()) {
                const std::size_t sp = rest.find(' ');
                const std::string_view w = rest.substr(0, sp);
                if (!w.empty()) {
                    entry.words.push_back(normalize(w));
                }
                rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
            }
        }
        lexicon.entries.push_back(std::move(entry));
    }
    return lexicon;
}

const FillerLexicon* FillerLexicon::builtin(std::string_view language) {
    static const std::map<std::string, FillerLexicon, std::less<>> lexicons = [] {
        std::map<std::string, FillerLexicon, std::less<>> out;
        for (const auto& [lang, text] : detail::embeddedLexicons()) {
            if (auto parsed = parse(lang, text)) {
                out.emplace(std::string(lang), std::move(*parsed));
            }
        }
        return out;
    }();
    const auto it = lexicons.find(language);
    return it == lexicons.end() ? nullptr : &it->second;
}

std::vector<std::string> FillerLexicon::builtinLanguages() {
    std::vector<std::string> out;
    for (const auto& [lang, text] : detail::embeddedLexicons()) {
        out.emplace_back(lang);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string_view toString(ReviewKind kind) { return kind == ReviewKind::Filler ? "filler" : "silence"; }

// --- Review ------------------------------------------------------------------------------------------------

std::vector<ReviewItem> review(const SequenceTranscript& st, const ProjectState& state, const ReviewOptions& options,
                               const AudioEnergyProbe* probe) {
    std::vector<ReviewItem> items;
    const Sequence* seq = state.findSequence(st.sequence());
    if (!seq || st.empty()) {
        return items;
    }
    const std::size_t n = st.size();
    std::vector<std::string> normalized(n);
    for (std::size_t i = 0; i < n; ++i) {
        normalized[i] = normalize(st.text(i));
    }
    const auto lexiconFor = [&](std::string_view language) -> const FillerLexicon* {
        for (const FillerLexicon& l : options.lexicons) {
            if (l.language == language) {
                return &l;
            }
        }
        return FillerLexicon::builtin(language);
    };
    const auto pauseBefore = [&](std::size_t i) { return i == 0 || st[i].timeline.start - st[i - 1].timeline.end() >= options.disfluentPause; };
    const auto pauseAfter = [&](std::size_t j) { return j + 1 == n || st[j + 1].timeline.start - st[j].timeline.end() >= options.disfluentPause; };

    if (options.fillers) {
        // Matches entry `e` at word i: the number of words it spans, or 0.
        const auto matchAt = [&](const FillerLexicon::Entry& e, std::size_t i, bool unspaced) -> std::size_t {
            if (unspaced) {
                const std::string& target = e.words.front();
                std::string joined;
                std::size_t j = i;
                while (j < n && joined.size() < target.size() && st[j].transcript == st[i].transcript) {
                    joined += normalized[j];
                    ++j;
                    if (!target.starts_with(joined)) {
                        return 0;
                    }
                }
                return joined == target ? j - i : 0;
            }
            if (i + e.words.size() > n) {
                return 0;
            }
            for (std::size_t k = 0; k < e.words.size(); ++k) {
                if (normalized[i + k] != e.words[k] || st[i + k].transcript != st[i].transcript) {
                    return 0;
                }
            }
            return e.words.size();
        };
        for (std::size_t i = 0; i < n; ++i) {
            const std::string_view language = st.language(i);
            const FillerLexicon* lexicon = lexiconFor(language);
            if (!lexicon || normalized[i].empty()) {
                continue;
            }
            const bool unspaced = wordSplitFor(language) == WordSplit::PerUnit;
            const FillerLexicon::Entry* best = nullptr;
            std::size_t span = 0;
            for (const FillerLexicon::Entry& e : lexicon->entries) {
                if (const std::size_t m = matchAt(e, i, unspaced); m > span || (m == span && m > 0 && best &&
                                                                                 e.text.size() > best->text.size())) {
                    best = &e;
                    span = m;
                }
            }
            if (!best || span == 0) {
                continue;
            }
            const std::size_t last = i + span - 1;
            const bool before = pauseBefore(i);
            const bool after = pauseAfter(last);
            const bool comma = endsWithComma(st.text(last));
            const bool repeated = last + span < n && matchAt(*best, last + 1, unspaced) == span;
            if (best->contextual && !(before || after || comma || repeated)) {
                continue;
            }
            float confidence = best->contextual ? 0.6f : 0.9f;
            if (best->contextual && before && after) {
                confidence += 0.15f;
            }
            if (st[i].confidence < 0.4f) {
                confidence -= 0.1f;
            }
            const Ticks length = st[last].timeline.end() - st[i].timeline.start;
            if (length < Ticks{kTicksPerSecond * 6 / 100}) {
                confidence -= 0.2f; // 60 ms: probably a recognition artefact
            }
            ReviewItem item;
            item.kind = ReviewKind::Filler;
            item.range = TimeRange::fromStartEnd(st[i].timeline.start, st[last].timeline.end());
            item.firstWord = i;
            item.lastWord = last;
            item.confidence = std::clamp(confidence, 0.05f, 0.99f);
            item.label = st.textOf(i, last);
            item.context = snippet(st, i == 0 ? static_cast<std::size_t>(-1) : i - 1, last + 1, item.label);
            items.push_back(std::move(item));
            i = last;
        }
    }

    if (options.silenceThreshold && probe) {
        // The pauses' audio, handed to the probe up front so it can measure it in one pass.
        RecordingProbe recorder;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Ticks a = st[i].timeline.end();
            const Ticks b = st[i + 1].timeline.start;
            if (b - a >= *options.silenceThreshold) {
                (void)loudest(state, *seq, TimeRange::fromStartEnd(a, b), {st[i].track, st[i + 1].track}, recorder);
            }
        }
        if (!recorder.ranges.empty()) {
            probe->prefetch(recorder.ranges);
        }
    }
    if (options.silenceThreshold) {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Ticks a = st[i].timeline.end();
            const Ticks b = st[i + 1].timeline.start;
            if (b - a < *options.silenceThreshold) {
                continue;
            }
            ReviewItem item;
            item.kind = ReviewKind::Silence;
            item.range = TimeRange::fromStartEnd(a, b);
            item.firstWord = i;
            item.lastWord = i + 1;
            item.label = seconds(b - a) + " pause";
            item.confidence = 0.7f;
            if (probe) {
                item.rmsDb = loudest(state, *seq, item.range, {st[i].track, st[i + 1].track}, *probe);
                // Unmeasured (no clip under it, unreadable media): as without a probe.
                item.confidence = !item.rmsDb ? 0.7f : (*item.rmsDb <= options.silenceDb ? 0.9f : 0.3f);
            }
            item.context = snippet(st, i, i + 1, seconds(b - a));
            items.push_back(std::move(item));
        }
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const ReviewItem& x, const ReviewItem& y) { return x.range.start < y.range.start; });
    return items;
}

DeleteRequest deleteRequestFor(std::span<const ReviewItem> items, const CutOptions& options) {
    DeleteRequest request;
    request.options = options;
    for (const ReviewItem& item : items) {
        if (item.kind == ReviewKind::Filler) {
            request.words.push_back(WordRange{item.firstWord, item.lastWord});
        } else {
            request.gapsAfter.push_back(item.firstWord);
        }
    }
    return request;
}

} // namespace wabcut::ai::edit
